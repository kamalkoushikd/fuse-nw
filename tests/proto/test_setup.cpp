#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "fuse/proto/block.hpp"
#include "fuse/proto/setup.hpp"
#include "fuse/proto/udp.hpp"

using namespace fuse::proto;

namespace {

// A representative SETUP: 3 workers, 5 streams unevenly assigned, with a
// mix of flags/block sizes/windows — enough that a corrupted byte changes
// the hash and a field mismatch is visible.
SetupPayload sample_payload() {
    SetupPayload p;
    p.protocol_version = kProtocolVersion;
    p.num_workers = 3;
    p.num_streams = 5;
    for (uint16_t i = 0; i < 5; ++i) {
        p.streams[i].stream_id = static_cast<uint16_t>(100 + i);
        p.streams[i].worker_id = static_cast<uint16_t>(i % 3);
        p.streams[i].stream_flags =
            static_cast<uint8_t>((i % 2) ? kStreamFlagLossless | kStreamFlagOrdered
                                         : kStreamFlagCoalesce);
        p.streams[i].block_size = static_cast<uint16_t>(1000 + i * 20);
        p.streams[i].window_size = static_cast<uint8_t>(8 + i);
    }
    return p;
}

} // namespace

// --- Wire round trips ----------------------------------------------------

TEST(Setup, DataDatagramRoundTrip) {
    SetupPayload p = sample_payload();
    uint8_t buf[kMaxSetupDatagramSize];
    size_t len = encode_setup_data(p, buf, sizeof(buf));
    ASSERT_GT(len, 0u);

    SetupPayload got;
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    ASSERT_TRUE(decode_setup_data(buf, len, &got, &body, &body_len));
    EXPECT_TRUE(got == p);
    EXPECT_NE(body, nullptr);
    EXPECT_GT(body_len, 0u);
}

TEST(Setup, ZeroStreamsIsValid) {
    SetupPayload p;
    p.num_workers = 1;
    p.num_streams = 0;
    uint8_t buf[kMaxSetupDatagramSize];
    size_t len = encode_setup_data(p, buf, sizeof(buf));
    ASSERT_GT(len, 0u);
    SetupPayload got;
    ASSERT_TRUE(decode_setup_data(buf, len, &got, nullptr, nullptr));
    EXPECT_EQ(got.num_streams, 0);
    EXPECT_EQ(got.num_workers, 1);
}

TEST(Setup, DecodeRejectsTruncated) {
    SetupPayload p = sample_payload();
    uint8_t buf[kMaxSetupDatagramSize];
    size_t len = encode_setup_data(p, buf, sizeof(buf));
    SetupPayload got;
    EXPECT_FALSE(decode_setup_data(buf, len - 1, &got, nullptr, nullptr));
}

TEST(Setup, DecodeRejectsTooManyStreams) {
    // Hand-craft a SetupData claiming more streams than kMaxStreams.
    uint8_t buf[16];
    size_t off = 0;
    buf[off++] = kProtocolVersion;
    buf[off++] = static_cast<uint8_t>(MsgType::SetupData);
    buf[off++] = kProtocolVersion;        // payload protocol_version
    buf[off++] = 0; buf[off++] = 1;       // num_workers = 1
    buf[off++] = 0xFF; buf[off++] = 0xFF; // num_streams = 65535
    SetupPayload got;
    EXPECT_FALSE(decode_setup_data(buf, off, &got, nullptr, nullptr));
}

TEST(Setup, HashMessagesRoundTrip) {
    uint8_t buf[kOuterHeaderSize + 8];
    ASSERT_GT(encode_setup_hash(MsgType::SetupHashReply, 0xABCDEF12ULL, buf, sizeof(buf)), 0u);
    uint64_t h = 0;
    ASSERT_TRUE(decode_setup_hash(MsgType::SetupHashReply, buf, sizeof(buf), &h));
    EXPECT_EQ(h, 0xABCDEF12ULL);
    // A HASH-REPLY must not decode as a FINACK.
    EXPECT_FALSE(decode_setup_hash(MsgType::SetupFinAck, buf, sizeof(buf), &h));
}

// --- In-process handshake harness with loss / corruption injection -------

namespace {

enum class Inject { None, Drop, Corrupt };

struct Injector {
    Inject mode = Inject::None;
    MsgType target = MsgType::SetupData;
    int nth = 0;        // 1-based occurrence to hit; 0 = every occurrence
    int seen = 0;

    // Returns true if the datagram should be delivered (possibly mutated).
    bool apply(std::vector<uint8_t> &dg) {
        MsgType t;
        if (!peek_msg_type(dg.data(), dg.size(), &t) || t != target || mode == Inject::None) {
            return true;
        }
        ++seen;
        if (nth != 0 && seen != nth) {
            return true;
        }
        if (mode == Inject::Drop) {
            return false;
        }
        // Corrupt: flip a byte in the body (after the outer header).
        if (dg.size() > kOuterHeaderSize) {
            dg[kOuterHeaderSize] ^= 0xFF;
        }
        return true;
    }
};

struct HandshakeResult {
    bool init_matched = false;
    bool resp_complete = false;
    bool init_failed = false;
    bool resp_failed = false;
    int rounds = 0;
};

HandshakeResult run_handshake(const SetupPayload &payload, Injector inj,
                              uint8_t max_retries = kDefaultSetupMaxRetries) {
    const uint64_t timeout = kDefaultSetupTimeoutNs;
    SetupInitiator init(payload, timeout, max_retries);
    SetupResponder resp(timeout, max_retries);

    std::vector<std::vector<uint8_t>> to_resp, to_init;
    uint8_t out[kMaxSetupDatagramSize];

    auto enqueue = [&](std::vector<std::vector<uint8_t>> &box, const uint8_t *d, size_t n) {
        if (n == 0) return;
        std::vector<uint8_t> dg(d, d + n);
        if (inj.apply(dg)) box.push_back(std::move(dg));
    };

    uint64_t now = 0;
    size_t n = init.start(out, sizeof(out), now);
    enqueue(to_resp, out, n);

    HandshakeResult r;
    for (r.rounds = 0; r.rounds < 500; ++r.rounds) {
        // Deliver everything queued to the responder.
        std::vector<std::vector<uint8_t>> resp_in;
        resp_in.swap(to_resp);
        for (auto &dg : resp_in) {
            size_t m = resp.on_datagram(dg.data(), dg.size(), out, sizeof(out), now);
            enqueue(to_init, out, m);
        }
        // Deliver everything queued to the initiator.
        std::vector<std::vector<uint8_t>> init_in;
        init_in.swap(to_init);
        for (auto &dg : init_in) {
            size_t m = init.on_datagram(dg.data(), dg.size(), out, sizeof(out), now);
            enqueue(to_resp, out, m);
        }

        if (init.is_matched() && resp.is_complete()) break;
        if (init.is_failed() || resp.is_failed()) break;

        if (to_resp.empty() && to_init.empty()) {
            // Both stalled: advance past the timeout and fire retransmits.
            // Note we do NOT break just because the resulting datagram was
            // injected away — under sustained loss the boxes stay empty yet
            // retries keep accruing until a side hits its cap and fails,
            // which the terminal checks at the top of the loop catch.
            now += timeout + 1;
            size_t a = init.on_timeout(out, sizeof(out), now);
            enqueue(to_resp, out, a);
            size_t b = resp.on_timeout(out, sizeof(out), now);
            enqueue(to_init, out, b);
        }
    }

    r.init_matched = init.is_matched();
    r.resp_complete = resp.is_complete();
    r.init_failed = init.is_failed();
    r.resp_failed = resp.is_failed();

    // On success, both sides must hold identical stream-to-worker tables.
    if (r.init_matched && r.resp_complete) {
        EXPECT_TRUE(resp.has_config());
        EXPECT_TRUE(resp.config() == payload)
            << "responder's negotiated config must match the initiator's exactly";
    }
    return r;
}

} // namespace

TEST(Setup, CleanHandshakeCompletesWithIdenticalTables) {
    HandshakeResult r = run_handshake(sample_payload(), {});
    EXPECT_TRUE(r.init_matched);
    EXPECT_TRUE(r.resp_complete);
    EXPECT_FALSE(r.init_failed);
    EXPECT_FALSE(r.resp_failed);
}

TEST(Setup, RecoversFromLostData) {
    Injector inj{Inject::Drop, MsgType::SetupData, /*nth=*/1, 0};
    HandshakeResult r = run_handshake(sample_payload(), inj);
    EXPECT_TRUE(r.init_matched);
    EXPECT_TRUE(r.resp_complete);
    EXPECT_FALSE(r.init_failed);
}

TEST(Setup, RecoversFromLostHashReply) {
    Injector inj{Inject::Drop, MsgType::SetupHashReply, /*nth=*/1, 0};
    HandshakeResult r = run_handshake(sample_payload(), inj);
    EXPECT_TRUE(r.init_matched);
    EXPECT_TRUE(r.resp_complete);
}

TEST(Setup, RecoversFromLostFinAck) {
    Injector inj{Inject::Drop, MsgType::SetupFinAck, /*nth=*/1, 0};
    HandshakeResult r = run_handshake(sample_payload(), inj);
    EXPECT_TRUE(r.init_matched);
    EXPECT_TRUE(r.resp_complete);
}

TEST(Setup, RecoversFromCorruptedDataViaHashMismatch) {
    // A corrupted DATA makes the responder's hash differ; the initiator
    // rejects the mismatched HASH-REPLY and resends clean DATA.
    Injector inj{Inject::Corrupt, MsgType::SetupData, /*nth=*/1, 0};
    HandshakeResult r = run_handshake(sample_payload(), inj);
    EXPECT_TRUE(r.init_matched);
    EXPECT_TRUE(r.resp_complete);
}

TEST(Setup, FailsAfterRetryCapWhenDataAlwaysLost) {
    Injector inj{Inject::Drop, MsgType::SetupData, /*nth=*/0, 0}; // drop every DATA
    HandshakeResult r = run_handshake(sample_payload(), inj, /*max_retries=*/5);
    EXPECT_TRUE(r.init_failed);
    EXPECT_FALSE(r.resp_complete);
}

// --- Real UDP clean handshake -------------------------------------------

namespace {
void set_recv_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
}

TEST(Setup, CompletesOverRealUdpLoopback) {
    UdpSocket init_sock, resp_sock;
    ASSERT_TRUE(init_sock.open("127.0.0.1", 0));
    ASSERT_TRUE(resp_sock.open("127.0.0.1", 0));
    set_recv_timeout(init_sock.fd(), 1000);
    set_recv_timeout(resp_sock.fd(), 1000);

    PeerAddr resp_addr;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", resp_sock.local_port(), &resp_addr));

    SetupPayload payload = sample_payload();
    SetupInitiator init(payload);
    SetupResponder resp;

    uint8_t buf[kMaxSetupDatagramSize];
    size_t n = init.start(buf, sizeof(buf), 0);
    ASSERT_GT(n, 0u);
    ASSERT_TRUE(init_sock.send_to(buf, n, resp_addr));

    // Responder: receive DATA, reply HASH-REPLY.
    size_t got = 0;
    PeerAddr init_addr;
    ASSERT_TRUE(resp_sock.recv_from(buf, sizeof(buf), &got, &init_addr));
    n = resp.on_datagram(buf, got, buf, sizeof(buf), 0);
    ASSERT_GT(n, 0u);
    ASSERT_TRUE(resp_sock.send_to(buf, n, init_addr));

    // Initiator: receive HASH-REPLY, send FINACK.
    ASSERT_TRUE(init_sock.recv_from(buf, sizeof(buf), &got, nullptr));
    n = init.on_datagram(buf, got, buf, sizeof(buf), 0);
    ASSERT_GT(n, 0u);
    EXPECT_TRUE(init.is_matched());
    ASSERT_TRUE(init_sock.send_to(buf, n, resp_addr));

    // Responder: receive FINACK, complete.
    ASSERT_TRUE(resp_sock.recv_from(buf, sizeof(buf), &got, nullptr));
    resp.on_datagram(buf, got, buf, sizeof(buf), 0);
    EXPECT_TRUE(resp.is_complete());

    ASSERT_TRUE(resp.has_config());
    EXPECT_TRUE(resp.config() == payload);
}
