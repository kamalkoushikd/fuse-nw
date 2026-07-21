#include <gtest/gtest.h>

#include <cstring>

#include "fuse/proto/registry.hpp"

using namespace fuse::proto;

TEST(Registry, StoreAndLookup) {
    SenderRegistry reg(/*stream_id=*/1, /*window_size=*/8);

    uint8_t payload[16];
    for (int i = 0; i < 16; ++i) payload[i] = static_cast<uint8_t>(i + 1);

    ASSERT_TRUE(reg.store(5, payload, 16, /*send_time_ns=*/999));

    const RegistrySlot *slot = reg.lookup(5);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->seq_no, 5u);
    EXPECT_EQ(slot->payload_len, 16);
    EXPECT_EQ(slot->send_time_ns, 999u);
    EXPECT_TRUE(slot->valid);
    EXPECT_EQ(0, std::memcmp(slot->payload, payload, 16));
}

TEST(Registry, ConfirmInvalidatesSlot) {
    SenderRegistry reg(1, 8);
    uint8_t payload[4] = {1, 2, 3, 4};
    reg.store(3, payload, 4, 0);
    ASSERT_NE(reg.lookup(3), nullptr);

    reg.confirm(3);
    EXPECT_EQ(reg.lookup(3), nullptr);
}

TEST(Registry, OverwriteByWindowAdvanceLosesOldSeq) {
    // window_size 8: seq 2 and seq 10 map to the same index (2 % 8 == 10 % 8).
    SenderRegistry reg(1, 8);
    uint8_t p[2] = {0xAA, 0xBB};

    reg.store(2, p, 2, 0);
    ASSERT_NE(reg.lookup(2), nullptr);

    reg.store(10, p, 2, 0); // overwrites index 2
    EXPECT_EQ(reg.lookup(2), nullptr) << "seq 2 should be gone after seq 10 overwrote its slot";
    EXPECT_NE(reg.lookup(10), nullptr);
}

TEST(Registry, LookupMissingSeqReturnsNull) {
    SenderRegistry reg(1, 8);
    EXPECT_EQ(reg.lookup(99), nullptr);
}

TEST(Registry, WindowSizeClamped) {
    SenderRegistry too_big(1, 200);
    EXPECT_EQ(too_big.window_size(), kMaxWindow);

    SenderRegistry zero(1, 0);
    EXPECT_EQ(zero.window_size(), 1);
}

TEST(Registry, ValidCountTracksLiveSlots) {
    SenderRegistry reg(1, 8);
    uint8_t p[1] = {0};
    reg.store(0, p, 1, 0);
    reg.store(1, p, 1, 0);
    reg.store(2, p, 1, 0);
    EXPECT_EQ(reg.valid_count(), 3);
    reg.confirm(1);
    EXPECT_EQ(reg.valid_count(), 2);
}
