#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "fuse/proto/spsc_ring.hpp"

using fuse::proto::SpscRing;

TEST(SpscRing, CapacityRoundsUpToPowerOfTwo) {
    EXPECT_EQ(SpscRing<int>(1).capacity(), 2u);
    EXPECT_EQ(SpscRing<int>(5).capacity(), 8u);
    EXPECT_EQ(SpscRing<int>(64).capacity(), 64u);
}

TEST(SpscRing, FullAndEmptyAreReportedNotBlocked) {
    SpscRing<int> r(4);
    EXPECT_EQ(r.readable(), 0u);
    ASSERT_EQ(r.writable(), 4u);
    for (int i = 0; i < 4; ++i) {
        r.slot_for_write(0) = i;
        r.commit();
    }
    EXPECT_EQ(r.writable(), 0u) << "a full ring must say so, not overwrite";
    ASSERT_EQ(r.readable(), 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(r.slot_for_read(0), i);
        r.consume();
    }
    EXPECT_EQ(r.readable(), 0u);
    EXPECT_EQ(r.writable(), 4u);
}

TEST(SpscRing, BatchedWriteAndRead) {
    SpscRing<int> r(8);
    ASSERT_GE(r.writable(), 5u);
    for (int i = 0; i < 5; ++i) r.slot_for_write(i) = 100 + i;
    EXPECT_EQ(r.readable(), 0u) << "nothing is visible before commit";
    r.commit(5);
    ASSERT_EQ(r.readable(), 5u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(r.slot_for_read(i), 100 + i);
    r.consume(5);
    EXPECT_EQ(r.readable(), 0u);
}

// The real thing: one producer thread, one consumer thread, a small ring so
// it wraps and fills constantly. Every item must arrive exactly once, in
// order, with its payload intact — i.e. the release on commit really does
// publish the slot's contents before the consumer's acquire reads them.
// (Run under -fsanitize=thread to have the race detector check it too.)
TEST(SpscRing, ProducerConsumerThreadsTransferEveryItemInOrder) {
    struct Item {
        uint64_t seq;
        uint64_t check; // derived from seq, so a torn/stale slot is detectable
    };
    constexpr uint64_t kItems = 2'000'000;
    SpscRing<Item> r(64);

    std::thread producer([&] {
        uint64_t next = 0;
        while (next < kItems) {
            const size_t room = r.writable();
            if (room == 0) {
                std::this_thread::yield();
                continue;
            }
            size_t n = 0;
            for (; n < room && next < kItems; ++n, ++next) {
                r.slot_for_write(n) = Item{next, next * 0x9E3779B97F4A7C15ULL};
            }
            r.commit(n);
        }
    });

    uint64_t expected = 0, bad = 0;
    while (expected < kItems) {
        const size_t n = r.readable();
        if (n == 0) {
            std::this_thread::yield();
            continue;
        }
        for (size_t i = 0; i < n; ++i) {
            const Item &it = r.slot_for_read(i);
            if (it.seq != expected || it.check != expected * 0x9E3779B97F4A7C15ULL) ++bad;
            ++expected;
        }
        r.consume(n);
    }
    producer.join();
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(expected, kItems);
}
