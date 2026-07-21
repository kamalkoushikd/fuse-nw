#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "fuse/proto/hash.hpp"
#include "fuse/proto/reassembly.hpp"
#include "fuse/proto/receiver.hpp"
#include "fuse/proto/registry.hpp"
#include "fuse/proto/send_queue.hpp"
#include "fuse/proto/setup.hpp"

using namespace fuse::proto;

namespace {

StreamConfig make_config(uint16_t id, uint8_t flags, uint16_t block_size, uint8_t window) {
    StreamConfig c;
    c.stream_id = id;
    c.stream_flags = flags;
    c.block_size = block_size;
    c.window_size = window;
    return c;
}

void fill_block(uint16_t stream_id, uint64_t seq, uint8_t *buf, uint16_t len) {
    for (uint16_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(stream_id * 7 + seq * 31 + i);
    }
}

constexpr uint64_t kReorder = 1000;
constexpr uint64_t kRenack = 5000;

} // namespace

// --- LOSSLESS flag: NACK gating ------------------------------------------

TEST(Flags, LosslessZeroStreamNeverNacks) {
    // A loss-tolerant stream: gaps are acceptable, no NACK ever emitted.
    ReceiverStream rx(/*stream=*/1, /*window=*/16, /*lossless=*/false);
    ASSERT_EQ(rx.on_receive(0, 0, 0), ReceiveResult::Accepted);
    ASSERT_EQ(rx.on_receive(2, 2, 100), ReceiveResult::Accepted); // gap at 1
    ASSERT_EQ(rx.on_receive(5, 5, 200), ReceiveResult::Accepted); // gaps 3,4

    Nack nack;
    uint64_t nacks = 0;
    for (uint64_t t = 0; t < 10; ++t) {
        nacks += rx.collect_nacks(1000 + t * kRenack, kReorder, kRenack, &nack);
    }
    EXPECT_EQ(nacks, 0u) << "a LOSSLESS=0 stream must never NACK";
}

TEST(Flags, LosslessOneStreamDoesNack) {
    // Control: the same gaps on a lossless stream do produce NACKs.
    ReceiverStream rx(/*stream=*/1, /*window=*/16, /*lossless=*/true);
    ASSERT_EQ(rx.on_receive(0, 0, 0), ReceiveResult::Accepted);
    ASSERT_EQ(rx.on_receive(2, 2, 100), ReceiveResult::Accepted);

    Nack nack;
    EXPECT_EQ(rx.collect_nacks(100 + kReorder, kReorder, kRenack, &nack), 1);
    EXPECT_EQ(nack.missing[0], 1u);
}

// --- LOSSLESS=1 full recovery, verified by checksum ----------------------

TEST(Flags, LosslessOneStreamFullyRecoversUnderLoss) {
    constexpr uint16_t kStream = 1;
    constexpr uint16_t kBlock = 100;
    constexpr uint64_t kN = 10;
    constexpr uint8_t kWindow = 32;

    StreamConfig cfg = make_config(kStream, kStreamFlagLossless | kStreamFlagOrdered, kBlock, kWindow);

    // Build the source and store every block in the sender registry.
    std::vector<uint8_t> source(kN * kBlock);
    SenderRegistry reg(kStream, kWindow);
    for (uint64_t seq = 0; seq < kN; ++seq) {
        fill_block(kStream, seq, source.data() + seq * kBlock, kBlock);
        ASSERT_TRUE(reg.store(seq, source.data() + seq * kBlock, kBlock, seq));
    }

    std::vector<uint8_t> sink(kN * kBlock, 0);
    ReassemblyStream reasm(cfg, sink.data(), sink.size(), /*total_blocks=*/kN);

    // Deliver every block except the "lost" set {3, 7}.
    const std::vector<uint64_t> lost = {3, 7};
    auto is_lost = [&](uint64_t s) {
        for (uint64_t l : lost) if (l == s) return true;
        return false;
    };
    for (uint64_t seq = 0; seq < kN; ++seq) {
        if (is_lost(seq)) continue;
        reasm.on_block(seq, source.data() + seq * kBlock, kBlock);
    }
    EXPECT_FALSE(reasm.is_complete()) << "not complete while blocks are still missing";

    // Retransmit the lost blocks straight out of the registry.
    for (uint64_t seq : lost) {
        const RegistrySlot *slot = reg.lookup(seq);
        ASSERT_NE(slot, nullptr);
        reasm.on_block(slot->seq_no, slot->payload, slot->payload_len);
    }

    EXPECT_TRUE(reasm.is_complete());
    EXPECT_EQ(reasm.delivered_count(), kN);
    EXPECT_EQ(hash64(sink.data(), sink.size()), hash64(source.data(), source.size()))
        << "reconstructed payload must checksum-match the source";
}

// --- ORDERED flag: delivery order ----------------------------------------

TEST(Flags, UnorderedStreamSurfacesInArrivalOrder) {
    constexpr uint16_t kBlock = 8;
    StreamConfig cfg = make_config(1, /*flags=*/0, kBlock, 16); // ORDERED off

    std::vector<uint8_t> sink(5 * kBlock, 0);
    ReassemblyStream reasm(cfg, sink.data(), sink.size(), 5);

    const uint64_t arrival[5] = {0, 2, 1, 4, 3};
    uint8_t blk[kBlock];
    for (uint64_t seq : arrival) {
        fill_block(1, seq, blk, kBlock);
        uint32_t n = reasm.on_block(seq, blk, kBlock);
        EXPECT_EQ(n, 1u) << "unordered delivery surfaces each block immediately";
    }

    std::vector<uint64_t> expected(arrival, arrival + 5);
    EXPECT_EQ(reasm.delivery_order(), expected)
        << "unordered stream must surface blocks in arrival order, not seq order";
}

TEST(Flags, OrderedStreamSurfacesInSeqOrder) {
    constexpr uint16_t kBlock = 8;
    StreamConfig cfg = make_config(1, kStreamFlagOrdered, kBlock, 16);

    std::vector<uint8_t> sink(5 * kBlock, 0);
    ReassemblyStream reasm(cfg, sink.data(), sink.size(), 5);

    const uint64_t arrival[5] = {0, 2, 1, 4, 3};
    uint8_t blk[kBlock];
    for (uint64_t seq : arrival) {
        fill_block(1, seq, blk, kBlock);
        reasm.on_block(seq, blk, kBlock);
    }

    std::vector<uint64_t> expected = {0, 1, 2, 3, 4};
    EXPECT_EQ(reasm.delivery_order(), expected)
        << "ordered stream must surface blocks in seq order despite arrival order";
}

TEST(Flags, OrderedAndUnorderedProduceIdenticalFinalBytes) {
    constexpr uint16_t kBlock = 16;
    constexpr uint64_t kN = 6;
    const uint64_t arrival[kN] = {5, 0, 3, 1, 4, 2};

    auto run = [&](uint8_t flags) {
        StreamConfig cfg = make_config(1, flags, kBlock, 16);
        std::vector<uint8_t> sink(kN * kBlock, 0);
        ReassemblyStream reasm(cfg, sink.data(), sink.size(), kN);
        uint8_t blk[kBlock];
        for (uint64_t seq : arrival) {
            fill_block(1, seq, blk, kBlock);
            reasm.on_block(seq, blk, kBlock);
        }
        return sink;
    };

    EXPECT_EQ(run(0), run(kStreamFlagOrdered))
        << "final reconstructed bytes must match regardless of delivery order";
}

// --- COALESCE flag: send-queue policy ------------------------------------

TEST(Flags, CoalesceDropsOldestUnderPressure) {
    SendQueue q(/*capacity=*/4, /*coalesce=*/true);
    for (uint64_t i = 0; i < 6; ++i) {
        EXPECT_TRUE(q.push(i)) << "coalescing push always succeeds";
    }
    EXPECT_EQ(q.dropped_count(), 2u); // 0 and 1 dropped in favor of newer

    std::vector<uint64_t> drained;
    uint64_t v;
    while (q.pop(v)) drained.push_back(v);
    EXPECT_EQ(drained, (std::vector<uint64_t>{2, 3, 4, 5}))
        << "coalescing keeps the newest blocks";
}

TEST(Flags, NonCoalesceBacksUpUnderPressure) {
    SendQueue q(/*capacity=*/4, /*coalesce=*/false);
    for (uint64_t i = 0; i < 4; ++i) EXPECT_TRUE(q.push(i));
    EXPECT_FALSE(q.push(99)) << "a full non-coalescing queue rejects (backpressure)";
    EXPECT_EQ(q.dropped_count(), 0u) << "non-coalescing never drops";

    std::vector<uint64_t> drained;
    uint64_t v;
    while (q.pop(v)) drained.push_back(v);
    EXPECT_EQ(drained, (std::vector<uint64_t>{0, 1, 2, 3}));
}
