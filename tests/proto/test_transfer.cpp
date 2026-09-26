#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

#include "fuse/proto/block.hpp"
#include "fuse/proto/control.hpp"
#include "fuse/proto/udp.hpp"
#include "fuse/transfer.hpp"

namespace {

// Ports are picked per-test to avoid collisions between concurrently
// running cases; the range must be free for `lanes` consecutive ports.
uint16_t next_port() {
    static std::atomic<uint16_t> p{41000};
    return p.fetch_add(64);
}

std::vector<uint8_t> make_payload(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 31 + (i >> 8));
    return v;
}

// Runs a full transfer with the receiver started first, and returns both
// sides' status.
struct Outcome {
    fuse::TransferStatus send_status = fuse::TransferStatus::Incomplete;
    fuse::TransferStatus recv_status = fuse::TransferStatus::Incomplete;
    std::vector<uint8_t> received;
    fuse::TransferStats send_stats;
    fuse::TransferStats recv_stats;
};

Outcome round_trip(const std::vector<uint8_t> &payload, uint16_t lanes,
                   const std::string &psk = "", uint16_t block = 1200) {
    Outcome o;
    const uint16_t port = next_port();

    fuse::TransferConfig rx;
    rx.bind_address = "127.0.0.1";
    rx.base_port = port;
    rx.lanes = lanes;
    rx.pre_shared_key = psk;
    rx.block_size = block;
    rx.timeout_ms = 20000;

    fuse::TransferConfig tx = rx;
    tx.host = "127.0.0.1";

    std::thread receiver([&] {
        o.recv_status = fuse::receive_buffer(rx, &o.received, &o.recv_stats);
    });
    // Give the receiver time to bind before the sender's opening message.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    o.send_status = fuse::send_buffer(tx, payload.data(), payload.size(), &o.send_stats);
    receiver.join();
    return o;
}

} // namespace

TEST(Transfer, RoundTripsExactBytes) {
    const auto payload = make_payload(1 << 20); // 1 MiB
    Outcome o = round_trip(payload, 2);

    ASSERT_EQ(o.send_status, fuse::TransferStatus::Ok) << fuse::to_string(o.send_status);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok) << fuse::to_string(o.recv_status);
    EXPECT_EQ(o.received, payload) << "received bytes must match the source exactly";
    EXPECT_EQ(o.recv_stats.bytes, payload.size());
}

TEST(Transfer, SingleLane) {
    const auto payload = make_payload(256 * 1024);
    Outcome o = round_trip(payload, 1);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok);
    EXPECT_EQ(o.received, payload);
}

TEST(Transfer, ManyLanesSplitAndReassemble) {
    // With more lanes than a trivially-divisible size, shard boundaries do
    // not fall on clean multiples — the stitching must still be exact.
    const auto payload = make_payload(700003);
    Outcome o = round_trip(payload, 8);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok);
    EXPECT_EQ(o.received, payload);
}

TEST(Transfer, PayloadSmallerThanOneBlock) {
    const auto payload = make_payload(17);
    Outcome o = round_trip(payload, 4);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok);
    EXPECT_EQ(o.received, payload);
}

TEST(Transfer, ReportsUsefulStats) {
    const auto payload = make_payload(1 << 20);
    Outcome o = round_trip(payload, 2);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok);
    EXPECT_EQ(o.send_stats.bytes, payload.size());
    EXPECT_GT(o.send_stats.final_block_size, 0);
    EXPECT_GE(o.recv_stats.throughput_mb_per_s(), 0.0);
}

#if FUSE_PROTO_WITH_DTLS

TEST(Transfer, EncryptedRoundTrip) {
    ASSERT_TRUE(fuse::encryption_available());
    const auto payload = make_payload(1 << 20);
    Outcome o = round_trip(payload, 4, "a-shared-secret-between-peers");

    ASSERT_EQ(o.send_status, fuse::TransferStatus::Ok) << fuse::to_string(o.send_status);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok) << fuse::to_string(o.recv_status);
    EXPECT_EQ(o.received, payload);
    EXPECT_EQ(o.recv_stats.auth_failures, 0u);
}

// A receiver holding the wrong key must not produce plausible-looking data:
// every block fails authentication, so the transfer does not complete.
TEST(Transfer, WrongKeyDoesNotYieldData) {
    const auto payload = make_payload(128 * 1024);
    const uint16_t port = next_port();

    fuse::TransferConfig rx;
    rx.bind_address = "127.0.0.1";
    rx.base_port = port;
    rx.lanes = 2;
    rx.pre_shared_key = "receiver-key-which-is-wrong";
    rx.timeout_ms = 3000;

    fuse::TransferConfig tx = rx;
    tx.host = "127.0.0.1";
    tx.pre_shared_key = "sender-key";

    std::vector<uint8_t> got;
    fuse::TransferStatus rs = fuse::TransferStatus::Ok;
    std::thread receiver([&] { rs = fuse::receive_buffer(rx, &got, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    fuse::send_buffer(tx, payload.data(), payload.size(), nullptr);
    receiver.join();

    // Specifically AuthFailed, not just "not Ok": the idle-timeout exit path
    // used to skip the auth-failure classification entirely (it lived only
    // after the loop's natural-completion exit, which this scenario can
    // never reach — every block fails auth, so rx.on_receive is never
    // called and base_seq_no never advances), so a wrong key always
    // reported a generic Timeout instead of the more specific, actionable
    // AuthFailed.
    EXPECT_EQ(rs, fuse::TransferStatus::AuthFailed) << fuse::to_string(rs);
    EXPECT_NE(got, payload) << "no plaintext may be recovered with the wrong key";
}

#endif // FUSE_PROTO_WITH_DTLS

// A block claiming an offset near UINT64_MAX, combined with a small
// payload, makes `offset + payload_len` wrap past zero under naive unsigned
// addition — small enough to slip past a bounds check of the form
// "offset + len <= size()", after which the receiver would memcpy at the
// real, unwrapped (and wildly out-of-range) offset. Nothing authenticates
// `offset` in plaintext mode, so this is reachable by anyone who can reach
// the port, not just a party holding the PSK.
TEST(Transfer, MaliciousOffsetIsRejectedNotWritten) {
    using namespace fuse::proto;
    const uint16_t port = next_port();
    const auto payload = make_payload(64);

    fuse::TransferConfig rx;
    rx.bind_address = "127.0.0.1";
    rx.base_port = port;
    rx.lanes = 1;
    rx.timeout_ms = 3000;

    std::vector<uint8_t> got;
    fuse::TransferStatus rs = fuse::TransferStatus::Incomplete;
    std::thread receiver([&] { rs = fuse::receive_buffer(rx, &got, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    UdpSocket atk;
    ASSERT_TRUE(atk.open("127.0.0.1", 0));
    PeerAddr dst;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", port, &dst));

    StreamStart ss;
    ss.stream_id = 0;
    ss.total_bytes = payload.size();
    ss.nonce = 0x1234;
    uint8_t ssdg[256];
    const size_t ssn = encode_stream_start(ss, ssdg, sizeof(ssdg));
    ASSERT_GT(ssn, 0u);
    ASSERT_TRUE(atk.send_to(ssdg, ssn, dst));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The malicious block: in-window seq_no (so the receiver's window logic
    // accepts it) but an offset designed to wrap.
    {
        BlockHeader hdr;
        hdr.stream_id = 0;
        hdr.seq_no = 1;
        hdr.flags = 0;
        hdr.payload_len = 8;
        hdr.offset = UINT64_MAX - 4;
        uint8_t body[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
        uint8_t dg[512];
        const size_t dn = encode_data_datagram(hdr, 0, body, dg, sizeof(dg));
        ASSERT_GT(dn, 0u);
        ASSERT_TRUE(atk.send_to(dg, dn, dst));
    }

    // The real, final block — completes the transfer.
    {
        BlockHeader hdr;
        hdr.stream_id = 0;
        hdr.seq_no = 0;
        hdr.flags = kFlagLastBlock;
        hdr.payload_len = static_cast<uint16_t>(payload.size());
        hdr.offset = 0;
        uint8_t dg[512];
        const size_t dn = encode_data_datagram(hdr, 0, payload.data(), dg, sizeof(dg));
        ASSERT_GT(dn, 0u);
        ASSERT_TRUE(atk.send_to(dg, dn, dst));
    }

    receiver.join();
    ASSERT_EQ(rs, fuse::TransferStatus::Ok) << fuse::to_string(rs);
    EXPECT_EQ(got, payload) << "an out-of-range offset must be rejected, not written";
}

// StreamStart's total_bytes is an unauthenticated wire value even under a
// PSK (it is read before any per-block auth check). A hostile claim near
// UINT64_MAX must fail this lane cleanly, not propagate an uncaught
// allocation-failure exception out of the lane's thread and terminate the
// whole process.
TEST(Transfer, HugeStreamStartSizeFailsCleanlyInsteadOfCrashing) {
    using namespace fuse::proto;
    const uint16_t port = next_port();

    fuse::TransferConfig rx;
    rx.bind_address = "127.0.0.1";
    rx.base_port = port;
    rx.lanes = 1;
    rx.timeout_ms = 3000;

    std::vector<uint8_t> got;
    fuse::TransferStatus rs = fuse::TransferStatus::Ok;
    std::thread receiver([&] { rs = fuse::receive_buffer(rx, &got, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    UdpSocket atk;
    ASSERT_TRUE(atk.open("127.0.0.1", 0));
    PeerAddr dst;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", port, &dst));

    StreamStart ss;
    ss.stream_id = 0;
    ss.total_bytes = UINT64_MAX - 8; // exceeds any real allocator's max_size()
    ss.nonce = 0x5;
    uint8_t dg[256];
    const size_t n = encode_stream_start(ss, dg, sizeof(dg));
    ASSERT_GT(n, 0u);
    ASSERT_TRUE(atk.send_to(dg, n, dst));

    receiver.join(); // must return promptly, not crash the whole test binary
    EXPECT_EQ(rs, fuse::TransferStatus::ResourceLimit) << fuse::to_string(rs);
}

// --- Progress reporting and graceful exit ----------------------------------

namespace fakepeer {

using namespace fuse::proto;
using Clock = std::chrono::steady_clock;

void set_rcv_timeout_ms(UdpSocket &s, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Waits up to `ms` for a datagram of `type`, skipping anything else. Returns
// its length, or 0 on timeout.
size_t wait_for(UdpSocket &s, MsgType type, uint8_t *buf, size_t cap, int ms,
                PeerAddr *from = nullptr) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    set_rcv_timeout_ms(s, 20);
    while (Clock::now() < deadline) {
        size_t got = 0;
        PeerAddr src;
        if (!s.recv_from(buf, cap, &got, &src)) continue;
        MsgType t;
        if (peek_msg_type(buf, got, &t) && t == type) {
            if (from) *from = src;
            return got;
        }
    }
    return 0;
}

void send_ack(UdpSocket &s, const PeerAddr &to, uint64_t nonce) {
    Ack ack;
    ack.nonce = nonce;
    uint8_t buf[kMaxAuxDatagramSize];
    const size_t n = encode_ack(ack, buf, sizeof(buf));
    s.send_to(buf, n, to);
}

void send_close(UdpSocket &s, const PeerAddr &to, uint64_t nonce, uint8_t reason) {
    StreamClose sc;
    sc.nonce = nonce;
    sc.reason = reason;
    uint8_t buf[64];
    const size_t n = encode_stream_close(sc, buf, sizeof(buf));
    s.send_to(buf, n, to);
}

// True if an aborting StreamClose for `nonce` arrives within `ms`.
bool expect_abort(UdpSocket &s, uint64_t nonce, int ms) {
    uint8_t buf[kMaxDatagramSize + 64];
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
        const size_t n = wait_for(s, MsgType::StreamClose, buf, sizeof(buf), 50);
        StreamClose sc;
        if (n && decode_stream_close(buf, n, &sc) && sc.nonce == nonce &&
            sc.reason == kStreamCloseAborted) {
            return true;
        }
    }
    return false;
}

double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

} // namespace fakepeer

TEST(Transfer, ProgressIsReportedOnBothSides) {
    const auto payload = make_payload(8 * 1024 * 1024 + 123);
    const uint16_t port = next_port();

    std::vector<fuse::TransferProgress> rx_seen, tx_seen;

    fuse::TransferConfig rx;
    rx.bind_address = "127.0.0.1";
    rx.base_port = port;
    rx.lanes = 2;
    rx.timeout_ms = 20000;
    rx.progress_interval_ms = 10;
    rx.on_progress = [&](const fuse::TransferProgress &p) { rx_seen.push_back(p); };

    fuse::TransferConfig tx = rx;
    tx.host = "127.0.0.1";
    tx.on_progress = [&](const fuse::TransferProgress &p) { tx_seen.push_back(p); };

    std::vector<uint8_t> got;
    fuse::TransferStatus rs = fuse::TransferStatus::Incomplete;
    std::thread receiver([&] { rs = fuse::receive_buffer(rx, &got, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto ss = fuse::send_buffer(tx, payload.data(), payload.size(), nullptr);
    receiver.join();

    ASSERT_EQ(ss, fuse::TransferStatus::Ok) << fuse::to_string(ss);
    ASSERT_EQ(rs, fuse::TransferStatus::Ok) << fuse::to_string(rs);
    ASSERT_EQ(got, payload);

    for (const auto *seen : {&tx_seen, &rx_seen}) {
        ASSERT_FALSE(seen->empty()) << "the final report must always be delivered";
        for (size_t i = 1; i < seen->size(); ++i) {
            EXPECT_GE((*seen)[i].bytes_done, (*seen)[i - 1].bytes_done) << "progress went backwards";
        }
        const auto &last = seen->back();
        EXPECT_EQ(last.bytes_done, payload.size());
        EXPECT_EQ(last.bytes_total, payload.size());
        EXPECT_EQ(last.lanes_done, 2);
        EXPECT_EQ(last.lanes_total, 2);
        EXPECT_DOUBLE_EQ(last.fraction(), 1.0);
    }
}

// Once the sender has every Ack it sends a "finished" close, so the
// receiver returns right away instead of lingering for more Acks to send.
TEST(Transfer, ReceiverExitsPromptlyAfterSenderFinishes) {
    const auto payload = make_payload(1024 * 1024);
    const uint16_t port = next_port();

    fuse::TransferConfig rx;
    rx.bind_address = "127.0.0.1";
    rx.base_port = port;
    rx.lanes = 2;
    rx.timeout_ms = 20000;
    fuse::TransferConfig tx = rx;
    tx.host = "127.0.0.1";

    std::vector<uint8_t> got;
    fuse::TransferStatus rs = fuse::TransferStatus::Incomplete;
    fakepeer::Clock::time_point recv_done;
    std::thread receiver([&] {
        rs = fuse::receive_buffer(rx, &got, nullptr);
        recv_done = fakepeer::Clock::now();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto ss = fuse::send_buffer(tx, payload.data(), payload.size(), nullptr);
    const auto send_done = fakepeer::Clock::now();
    receiver.join();

    ASSERT_EQ(ss, fuse::TransferStatus::Ok);
    ASSERT_EQ(rs, fuse::TransferStatus::Ok);
    EXPECT_EQ(got, payload);
    // Without the close the receiver waits at least two idle ticks (~400 ms).
    const double lag_ms =
        std::chrono::duration<double, std::milli>(recv_done - send_done).count();
    EXPECT_LT(lag_ms, 300.0) << "receiver lingered after the sender had finished";
}

// Cancelling the sender must tell the receiver, and return promptly.
TEST(Transfer, SenderCancelTellsReceiverAndReturnsPromptly) {
    using namespace fuse::proto;
    const uint16_t port = next_port();
    const auto payload = make_payload(4 * 1024 * 1024);

    UdpSocket fake_rx;
    ASSERT_TRUE(fake_rx.open("127.0.0.1", port));

    std::atomic<bool> cancel{false};
    fuse::TransferConfig tx;
    tx.host = "127.0.0.1";
    tx.base_port = port;
    tx.lanes = 1;
    tx.timeout_ms = 20000;
    tx.cancel = &cancel;

    fuse::TransferStatus st = fuse::TransferStatus::Ok;
    std::thread sender([&] { st = fuse::send_buffer(tx, payload.data(), payload.size(), nullptr); });

    // Accept the handshake, then never acknowledge any data: the sender is
    // stuck mid-transfer, exactly where a user would hit Ctrl+C.
    uint8_t buf[kMaxDatagramSize + 64];
    PeerAddr sender_addr;
    const size_t n = fakepeer::wait_for(fake_rx, MsgType::StreamStart, buf, sizeof(buf), 3000,
                                        &sender_addr);
    ASSERT_GT(n, 0u);
    StreamStart ss;
    ASSERT_TRUE(decode_stream_start(buf, n, &ss));
    fakepeer::send_ack(fake_rx, sender_addr, ss.nonce);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto cancelled_at = fakepeer::Clock::now();
    cancel.store(true);
    EXPECT_TRUE(fakepeer::expect_abort(fake_rx, ss.nonce, 2000))
        << "the receiver was never told the sender gave up";
    sender.join();
    EXPECT_EQ(st, fuse::TransferStatus::Cancelled) << fuse::to_string(st);
    EXPECT_LT(fakepeer::ms_since(cancelled_at), 1000.0);
}

// Cancelling the receiver must tell the sender, and return promptly.
TEST(Transfer, ReceiverCancelTellsSenderAndReturnsPromptly) {
    using namespace fuse::proto;
    const uint16_t port = next_port();

    std::atomic<bool> cancel{false};
    fuse::TransferConfig rx;
    rx.bind_address = "127.0.0.1";
    rx.base_port = port;
    rx.lanes = 1;
    rx.timeout_ms = 20000;
    rx.cancel = &cancel;

    std::vector<uint8_t> got;
    fuse::TransferStatus rs = fuse::TransferStatus::Ok;
    std::thread receiver([&] { rs = fuse::receive_buffer(rx, &got, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    UdpSocket fake_tx;
    ASSERT_TRUE(fake_tx.open("127.0.0.1", 0));
    PeerAddr dst;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", port, &dst));
    StreamStart ss;
    ss.total_bytes = 1 << 20;
    ss.nonce = 0x77;
    uint8_t buf[kMaxDatagramSize + 64];
    const size_t n = encode_stream_start(ss, buf, sizeof(buf));
    ASSERT_TRUE(fake_tx.send_to(buf, n, dst));
    ASSERT_GT(fakepeer::wait_for(fake_tx, MsgType::Ack, buf, sizeof(buf), 2000), 0u);

    const auto cancelled_at = fakepeer::Clock::now();
    cancel.store(true);
    EXPECT_TRUE(fakepeer::expect_abort(fake_tx, ss.nonce, 2000))
        << "the sender was never told the receiver gave up";
    receiver.join();
    EXPECT_EQ(rs, fuse::TransferStatus::Cancelled) << fuse::to_string(rs);
    EXPECT_TRUE(got.empty()) << "a cancelled receive must not hand back partial data";
    EXPECT_LT(fakepeer::ms_since(cancelled_at), 1000.0);
}

// A peer's abort ends the receiver at once — but only one carrying this
// session's nonce.
TEST(Transfer, PeerAbortEndsReceiverPromptly) {
    using namespace fuse::proto;
    const uint16_t port = next_port();

    fuse::TransferConfig rx;
    rx.bind_address = "127.0.0.1";
    rx.base_port = port;
    rx.lanes = 1;
    rx.timeout_ms = 20000;

    std::vector<uint8_t> got;
    std::atomic<bool> done{false};
    fuse::TransferStatus rs = fuse::TransferStatus::Ok;
    std::thread receiver([&] {
        rs = fuse::receive_buffer(rx, &got, nullptr);
        done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    UdpSocket fake_tx;
    ASSERT_TRUE(fake_tx.open("127.0.0.1", 0));
    PeerAddr dst;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", port, &dst));
    StreamStart ss;
    ss.total_bytes = 1 << 20;
    ss.nonce = 0x99;
    uint8_t buf[kMaxDatagramSize + 64];
    const size_t n = encode_stream_start(ss, buf, sizeof(buf));
    ASSERT_TRUE(fake_tx.send_to(buf, n, dst));
    ASSERT_GT(fakepeer::wait_for(fake_tx, MsgType::Ack, buf, sizeof(buf), 2000), 0u);

    fakepeer::send_close(fake_tx, dst, 0x98, kStreamCloseAborted); // wrong session
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(done.load()) << "a close with another session's nonce ended this transfer";

    const auto aborted_at = fakepeer::Clock::now();
    fakepeer::send_close(fake_tx, dst, ss.nonce, kStreamCloseAborted);
    receiver.join();
    EXPECT_EQ(rs, fuse::TransferStatus::PeerAborted) << fuse::to_string(rs);
    EXPECT_LT(fakepeer::ms_since(aborted_at), 1000.0);
}

// The StreamStart handshake used to give up after 40 ms in total, so any
// receiver slower than that (a long RTT, a big allocation) failed the send
// outright. Here the receiver answers after 300 ms, then aborts: the sender
// must get through the handshake and report the abort, not a Timeout.
TEST(Transfer, SlowHandshakeSucceedsAndPeerAbortEndsSender) {
    using namespace fuse::proto;
    const uint16_t port = next_port();
    const auto payload = make_payload(1024 * 1024);

    UdpSocket fake_rx;
    ASSERT_TRUE(fake_rx.open("127.0.0.1", port));

    fuse::TransferConfig tx;
    tx.host = "127.0.0.1";
    tx.base_port = port;
    tx.lanes = 1;
    tx.timeout_ms = 20000;

    fuse::TransferStatus st = fuse::TransferStatus::Ok;
    std::thread sender([&] { st = fuse::send_buffer(tx, payload.data(), payload.size(), nullptr); });

    uint8_t buf[kMaxDatagramSize + 64];
    PeerAddr sender_addr;
    const size_t n = fakepeer::wait_for(fake_rx, MsgType::StreamStart, buf, sizeof(buf), 3000,
                                        &sender_addr);
    ASSERT_GT(n, 0u);
    StreamStart ss;
    ASSERT_TRUE(decode_stream_start(buf, n, &ss));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    fakepeer::send_ack(fake_rx, sender_addr, ss.nonce);
    // Wait until data is flowing, i.e. the handshake really completed.
    ASSERT_GT(fakepeer::wait_for(fake_rx, MsgType::Data, buf, sizeof(buf), 3000), 0u);

    const auto aborted_at = fakepeer::Clock::now();
    fakepeer::send_close(fake_rx, sender_addr, ss.nonce, kStreamCloseAborted);
    sender.join();
    EXPECT_EQ(st, fuse::TransferStatus::PeerAborted) << fuse::to_string(st);
    EXPECT_LT(fakepeer::ms_since(aborted_at), 1000.0);
}

// Ctrl+C while the sender is still loading its file: the cancel is seen
// before any StreamStart reaches the receiver, so there is no session nonce
// yet. The receiver must still be released (on every lane), not left
// waiting out its full timeout for a sender that is never coming.
TEST(Transfer, SenderCancelledBeforeHandshakeReleasesReceiver) {
    const uint16_t port = next_port();
    const auto payload = make_payload(1024 * 1024);

    fuse::TransferConfig rx;
    rx.bind_address = "127.0.0.1";
    rx.base_port = port;
    rx.lanes = 4;
    rx.timeout_ms = 20000;

    std::vector<uint8_t> got;
    fuse::TransferStatus rs = fuse::TransferStatus::Ok;
    std::thread receiver([&] { rs = fuse::receive_buffer(rx, &got, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::atomic<bool> cancel{true}; // already set when the send begins
    fuse::TransferConfig tx = rx;
    tx.host = "127.0.0.1";
    tx.cancel = &cancel;

    const auto started = fakepeer::Clock::now();
    EXPECT_EQ(fuse::send_buffer(tx, payload.data(), payload.size(), nullptr),
              fuse::TransferStatus::Cancelled);
    receiver.join();
    EXPECT_EQ(rs, fuse::TransferStatus::PeerAborted) << fuse::to_string(rs);
    EXPECT_LT(fakepeer::ms_since(started), 1000.0) << "receiver waited out its timeout";
}

// A receiver that isn't there (never started, or stopped before the sender
// reached it and so couldn't say so) is reported after connect_timeout_ms,
// not after the much longer no-progress timeout_ms.
TEST(Transfer, MissingReceiverFailsAfterConnectTimeout) {
    const uint16_t port = next_port(); // nothing listens here
    const auto payload = make_payload(4096);

    fuse::TransferConfig tx;
    tx.host = "127.0.0.1";
    tx.base_port = port;
    tx.lanes = 2;
    tx.timeout_ms = 60000;
    tx.connect_timeout_ms = 300;

    const auto started = fakepeer::Clock::now();
    EXPECT_EQ(fuse::send_buffer(tx, payload.data(), payload.size(), nullptr),
              fuse::TransferStatus::Timeout);
    const double ms = fakepeer::ms_since(started);
    EXPECT_GE(ms, 250.0);
    EXPECT_LT(ms, 2000.0) << "waited for timeout_ms instead of connect_timeout_ms";
}

TEST(Transfer, RejectsBadConfiguration) {
    fuse::TransferConfig cfg;
    cfg.lanes = 0;
    const uint8_t byte = 0;
    EXPECT_EQ(fuse::send_buffer(cfg, &byte, 1, nullptr), fuse::TransferStatus::ConfigError);

    std::vector<uint8_t> out;
    EXPECT_EQ(fuse::receive_buffer(cfg, &out, nullptr), fuse::TransferStatus::ConfigError);
}

TEST(Transfer, StatusStringsAreDistinct) {
    // Error reporting is part of the API surface: each status needs a
    // message a developer can act on.
    EXPECT_STRNE(fuse::to_string(fuse::TransferStatus::Ok),
                 fuse::to_string(fuse::TransferStatus::Timeout));
    EXPECT_STRNE(fuse::to_string(fuse::TransferStatus::SocketError),
                 fuse::to_string(fuse::TransferStatus::ConfigError));
}
