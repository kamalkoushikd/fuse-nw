#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

#include "fuse/proto/aux.hpp"
#include "fuse/proto/block.hpp"
#include "fuse/proto/receiver.hpp"
#include "fuse/proto/registry.hpp"
#include "fuse/proto/udp.hpp"

using namespace fuse::proto;

// --- Heap-allocation counter (for the zero-alloc-after-startup guard) ----
//
// Replacing global operator new/delete lets a test assert that a stretch
// of code performed no heap allocation. The counter only accrues while
// g_alloc_guard_active is set, so ordinary allocation elsewhere in the
// test binary is ignored.
namespace {
std::atomic<bool>     g_alloc_guard_active{false};
std::atomic<uint64_t> g_alloc_count{0};
}

void *operator new(std::size_t n) {
    if (g_alloc_guard_active.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void *p = std::malloc(n ? n : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {

void set_recv_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Fills payload[i] = (seq * 31 + i) so each block's bytes are a function
// of its seq_no — lets the receiver verify it got the right block back.
void make_payload(uint64_t seq, uint8_t *payload, uint16_t len) {
    for (uint16_t i = 0; i < len; ++i) {
        payload[i] = static_cast<uint8_t>(seq * 31 + i);
    }
}

} // namespace

// Stage 1 acceptance: a deliberately-dropped block is detected via the
// receiver bitmask, NACKed after the reorder window, retransmitted from
// the sender registry, and received — verified by tracing that specific
// seq_no through drop -> NACK -> retransmit -> received.
TEST(Stage1Loopback, DropNackRetransmitRoundTrip) {
    constexpr uint16_t kStream = 1;
    constexpr uint8_t  kWindow = 16;
    constexpr uint64_t kTotal  = 8;
    constexpr uint64_t kDrop   = 3;   // this seq_no is withheld on first pass
    constexpr uint16_t kLen    = 200;
    constexpr uint64_t kReorderDelay = 1000;
    constexpr uint64_t kRenack       = 5000;

    UdpSocket rx_sock, tx_sock;
    ASSERT_TRUE(rx_sock.open("127.0.0.1", 0));
    ASSERT_TRUE(tx_sock.open("127.0.0.1", 0));
    set_recv_timeout(rx_sock.fd(), 1000);
    set_recv_timeout(tx_sock.fd(), 1000);

    PeerAddr rx_addr;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", rx_sock.local_port(), &rx_addr));

    SenderRegistry registry(kStream, kWindow);
    ReceiverStream receiver(kStream, kWindow);

    uint8_t datagram[kMaxDatagramSize];
    uint8_t payload[kLen];

    // Phase 1: send every block except kDrop. All are stored in the
    // registry regardless (as if they were sent).
    for (uint64_t seq = 0; seq < kTotal; ++seq) {
        make_payload(seq, payload, kLen);
        ASSERT_TRUE(registry.store(seq, payload, kLen, /*send_time_ns=*/seq));

        if (seq == kDrop) {
            continue; // simulate this datagram being lost in flight
        }
        BlockHeader hdr;
        hdr.stream_id = kStream;
        hdr.seq_no = seq;
        hdr.flags = (seq + 1 == kTotal) ? kFlagLastBlock : 0;
        hdr.payload_len = kLen;
        size_t n = encode_data_datagram(hdr, seq, payload, datagram, sizeof(datagram));
        ASSERT_GT(n, 0u);
        ASSERT_TRUE(tx_sock.send_to(datagram, n, rx_addr));
    }

    // Phase 2: receiver drains the kTotal-1 datagrams that were sent.
    PeerAddr learned_tx_addr;
    bool learned = false;
    uint64_t clock = 0;
    for (uint64_t i = 0; i < kTotal - 1; ++i) {
        size_t got = 0;
        PeerAddr src;
        ASSERT_TRUE(rx_sock.recv_from(datagram, sizeof(datagram), &got, &src))
            << "receiver did not get datagram " << i;
        if (!learned) {
            learned_tx_addr = src;
            learned = true;
        }
        BlockHeader hdr;
        uint64_t send_time = 0;
        const uint8_t *pl = nullptr;
        ASSERT_TRUE(decode_data_datagram(datagram, got, &hdr, &send_time, &pl));
        receiver.on_receive(hdr.seq_no, send_time, ++clock);
    }
    ASSERT_TRUE(learned);

    // The dropped block is the one holding base back; everything above it
    // arrived, so base is stuck at kDrop with higher bits set.
    EXPECT_EQ(receiver.base_seq_no(), kDrop)
        << "base should be stuck at the dropped seq_no";

    // Phase 3: after the reorder delay, the gap is NACKed.
    Nack nack;
    clock += kReorderDelay;
    uint16_t nacked = receiver.collect_nacks(clock, kReorderDelay, kRenack, &nack);
    ASSERT_EQ(nacked, 1) << "exactly the dropped block should be NACKed";
    EXPECT_EQ(nack.missing[0], kDrop);

    size_t nack_len = encode_nack(nack, datagram, sizeof(datagram));
    ASSERT_GT(nack_len, 0u);
    ASSERT_TRUE(rx_sock.send_to(datagram, nack_len, learned_tx_addr));

    // Phase 4: sender receives the NACK and retransmits the exact block
    // from its registry, flagged as a retransmission.
    {
        size_t got = 0;
        ASSERT_TRUE(tx_sock.recv_from(datagram, sizeof(datagram), &got, nullptr));
        Nack rx_nack;
        ASSERT_TRUE(decode_nack(datagram, got, &rx_nack));
        ASSERT_EQ(rx_nack.count, 1);
        uint64_t missing_seq = rx_nack.missing[0];
        EXPECT_EQ(missing_seq, kDrop);

        const RegistrySlot *slot = registry.lookup(missing_seq);
        ASSERT_NE(slot, nullptr) << "dropped block must still be resident in the registry";

        BlockHeader hdr;
        hdr.stream_id = kStream;
        hdr.seq_no = missing_seq;
        hdr.flags = kFlagRetransmission;
        hdr.payload_len = slot->payload_len;
        size_t n = encode_data_datagram(hdr, slot->send_time_ns, slot->payload,
                                        datagram, sizeof(datagram));
        ASSERT_GT(n, 0u);
        ASSERT_TRUE(tx_sock.send_to(datagram, n, rx_addr));
    }

    // Phase 5: receiver gets the retransmit, which fills the gap and slides
    // base past every block. Verify the retransmission flag and payload.
    {
        size_t got = 0;
        ASSERT_TRUE(rx_sock.recv_from(datagram, sizeof(datagram), &got, nullptr));
        BlockHeader hdr;
        uint64_t send_time = 0;
        const uint8_t *pl = nullptr;
        ASSERT_TRUE(decode_data_datagram(datagram, got, &hdr, &send_time, &pl));
        EXPECT_EQ(hdr.seq_no, kDrop);
        EXPECT_TRUE(hdr.flags & kFlagRetransmission)
            << "the refilled block must be marked as a retransmission";

        uint8_t expected[kLen];
        make_payload(kDrop, expected, kLen);
        EXPECT_EQ(0, std::memcmp(pl, expected, kLen))
            << "retransmitted payload must match the original block";

        EXPECT_EQ(receiver.on_receive(hdr.seq_no, send_time, ++clock),
                  ReceiveResult::Accepted);
    }

    // Everything delivered: base advanced past the whole message.
    EXPECT_EQ(receiver.base_seq_no(), kTotal);
    EXPECT_EQ(receiver.received_bitmask(), 0u);
    EXPECT_EQ(receiver.window_overflow_count(), 0u);
}

// Stage 1 acceptance: the steady-state send/receive/ack data path performs
// no heap allocation once buffers and state are set up. This is the
// runnable, CI-friendly form of the "zero allocations after startup"
// requirement (a massif run is the heavier external check).
TEST(Stage1Loopback, NoHeapAllocationInSteadyState) {
    constexpr uint16_t kStream = 1;
    constexpr uint8_t  kWindow = 32;
    constexpr uint16_t kLen    = 1200;
    constexpr uint64_t kIterations = 500;

    UdpSocket rx_sock, tx_sock;
    ASSERT_TRUE(rx_sock.open("127.0.0.1", 0));
    ASSERT_TRUE(tx_sock.open("127.0.0.1", 0));
    set_recv_timeout(rx_sock.fd(), 1000);
    set_recv_timeout(tx_sock.fd(), 1000);

    PeerAddr rx_addr, tx_addr;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", rx_sock.local_port(), &rx_addr));
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", tx_sock.local_port(), &tx_addr));

    // Heap-allocate the large registry so its construction (which may
    // allocate internally in a debug STL) happens before the guard.
    auto registry = new SenderRegistry(kStream, kWindow);
    ReceiverStream receiver(kStream, kWindow);

    uint8_t datagram[kMaxDatagramSize];
    uint8_t payload[kLen];
    for (uint16_t i = 0; i < kLen; ++i) payload[i] = static_cast<uint8_t>(i);

    bool ok = true;

    g_alloc_count.store(0, std::memory_order_relaxed);
    g_alloc_guard_active.store(true, std::memory_order_relaxed);

    // Lockstep send -> receive -> ack -> confirm, all on stack buffers.
    for (uint64_t seq = 0; seq < kIterations; ++seq) {
        BlockHeader hdr;
        hdr.stream_id = kStream;
        hdr.seq_no = seq;
        hdr.flags = 0;
        hdr.payload_len = kLen;
        registry->store(seq, payload, kLen, seq);

        size_t n = encode_data_datagram(hdr, seq, payload, datagram, sizeof(datagram));
        ok = ok && (n > 0) && tx_sock.send_to(datagram, n, rx_addr);

        size_t got = 0;
        ok = ok && rx_sock.recv_from(datagram, sizeof(datagram), &got, nullptr);
        BlockHeader rhdr;
        uint64_t send_time = 0;
        const uint8_t *pl = nullptr;
        ok = ok && decode_data_datagram(datagram, got, &rhdr, &send_time, &pl);
        receiver.on_receive(rhdr.seq_no, send_time, seq + 1);

        Ack ack = receiver.build_ack();
        size_t an = encode_ack(ack, datagram, sizeof(datagram));
        ok = ok && (an > 0) && rx_sock.send_to(datagram, an, tx_addr);

        ok = ok && tx_sock.recv_from(datagram, sizeof(datagram), &got, nullptr);
        Ack rack;
        ok = ok && decode_ack(datagram, got, &rack);
        registry->confirm(rack.base_seq_no);
    }

    g_alloc_guard_active.store(false, std::memory_order_relaxed);
    uint64_t allocs = g_alloc_count.load(std::memory_order_relaxed);

    delete registry;

    EXPECT_TRUE(ok) << "steady-state loop hit a socket/encode error";
    EXPECT_EQ(allocs, 0u)
        << "data path allocated " << allocs << " times after startup";
}
