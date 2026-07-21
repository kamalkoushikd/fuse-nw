#include <gtest/gtest.h>

#include "fuse/proto/receiver.hpp"

using namespace fuse::proto;

namespace {
constexpr uint64_t kReorderDelay = 1000;   // ns, for tests
constexpr uint64_t kRenackInterval = 5000; // ns, for tests
}

TEST(Receiver, InOrderReceiptSlidesBase) {
    ReceiverStream rx(/*stream_id=*/1, /*window_size=*/16);
    for (uint64_t seq = 0; seq < 5; ++seq) {
        EXPECT_EQ(rx.on_receive(seq, /*send_time=*/seq, /*now=*/seq * 10),
                  ReceiveResult::Accepted);
    }
    // All five contiguous -> base advanced past them, nothing outstanding.
    EXPECT_EQ(rx.base_seq_no(), 5u);
    EXPECT_EQ(rx.received_bitmask(), 0u);
}

TEST(Receiver, DuplicateIsRejected) {
    ReceiverStream rx(1, 16);
    EXPECT_EQ(rx.on_receive(0, 0, 0), ReceiveResult::Accepted);
    EXPECT_EQ(rx.on_receive(0, 0, 1), ReceiveResult::Duplicate);
}

TEST(Receiver, GapDetectedButNotNackedBeforeReorderDelay) {
    ReceiverStream rx(1, 16);
    ASSERT_EQ(rx.on_receive(0, 0, 0), ReceiveResult::Accepted); // base -> 1
    ASSERT_EQ(rx.on_receive(2, 2, 100), ReceiveResult::Accepted); // gap at seq 1

    // Immediately after noticing the gap, still within reorder tolerance.
    Nack nack;
    EXPECT_EQ(rx.collect_nacks(/*now=*/100, kReorderDelay, kRenackInterval, &nack), 0);
}

TEST(Receiver, GapNackedAfterReorderDelay) {
    ReceiverStream rx(1, 16);
    ASSERT_EQ(rx.on_receive(0, 0, 0), ReceiveResult::Accepted);
    ASSERT_EQ(rx.on_receive(2, 2, 100), ReceiveResult::Accepted); // gap noticed at t=100

    Nack nack;
    // t=100+kReorderDelay: gap has persisted long enough.
    ASSERT_EQ(rx.collect_nacks(100 + kReorderDelay, kReorderDelay, kRenackInterval, &nack), 1);
    EXPECT_EQ(nack.stream_id, 1);
    EXPECT_EQ(nack.missing[0], 1u);
}

TEST(Receiver, RepeatedNackIsRateLimited) {
    ReceiverStream rx(1, 16);
    ASSERT_EQ(rx.on_receive(0, 0, 0), ReceiveResult::Accepted);
    ASSERT_EQ(rx.on_receive(2, 2, 100), ReceiveResult::Accepted);

    Nack nack;
    uint64_t t = 100 + kReorderDelay;
    ASSERT_EQ(rx.collect_nacks(t, kReorderDelay, kRenackInterval, &nack), 1);
    // Immediately again: rate-limited, no re-NACK.
    EXPECT_EQ(rx.collect_nacks(t + 1, kReorderDelay, kRenackInterval, &nack), 0);
    // After the renack interval: NACK again.
    EXPECT_EQ(rx.collect_nacks(t + kRenackInterval, kReorderDelay, kRenackInterval, &nack), 1);
}

TEST(Receiver, FillingGapSlidesBaseAndStopsNacks) {
    ReceiverStream rx(1, 16);
    ASSERT_EQ(rx.on_receive(0, 0, 0), ReceiveResult::Accepted);   // base -> 1
    ASSERT_EQ(rx.on_receive(2, 2, 100), ReceiveResult::Accepted); // gap at 1

    // Retransmit of seq 1 arrives.
    ASSERT_EQ(rx.on_receive(1, 1, 200), ReceiveResult::Accepted);
    EXPECT_EQ(rx.base_seq_no(), 3u); // 1 and 2 now both delivered
    EXPECT_EQ(rx.received_bitmask(), 0u);

    Nack nack;
    EXPECT_EQ(rx.collect_nacks(100 + 10 * kReorderDelay, kReorderDelay, kRenackInterval, &nack), 0);
}

TEST(Receiver, AckReflectsBaseBitmaskAndEchoedSendTime) {
    ReceiverStream rx(1, 16);
    ASSERT_EQ(rx.on_receive(0, /*send_time=*/500, 0), ReceiveResult::Accepted); // base -> 1
    ASSERT_EQ(rx.on_receive(2, /*send_time=*/700, 100), ReceiveResult::Accepted);

    Ack ack = rx.build_ack();
    EXPECT_EQ(ack.stream_id, 1);
    EXPECT_EQ(ack.base_seq_no, 1u);
    EXPECT_EQ(ack.received_bitmask, 0b10u); // seq 2 = rel 1 set, seq 1 = rel 0 missing
    EXPECT_EQ(ack.echoed_send_time, 700u);  // most recent (highest) block
}

TEST(Receiver, EchoedSendTimeTracksHighestNotLatestArrival) {
    ReceiverStream rx(1, 16);
    ASSERT_EQ(rx.on_receive(5, /*send_time=*/500, 0), ReceiveResult::Accepted);
    // A reordered older block arrives afterward; echo must stay on seq 5.
    ASSERT_EQ(rx.on_receive(4, /*send_time=*/400, 10), ReceiveResult::Accepted);
    EXPECT_EQ(rx.build_ack().echoed_send_time, 500u);
}

TEST(Receiver, BlockBeyondWindowIsCountedAsOverflow) {
    ReceiverStream rx(1, /*window_size=*/4);
    ASSERT_EQ(rx.on_receive(0, 0, 0), ReceiveResult::Accepted); // base -> 1
    // seq 10 is far beyond base+window; untrackable.
    EXPECT_EQ(rx.on_receive(10, 0, 100), ReceiveResult::OutOfWindow);
    EXPECT_EQ(rx.window_overflow_count(), 1u);
}

TEST(Receiver, MultipleGapsAllNacked) {
    ReceiverStream rx(1, 16);
    ASSERT_EQ(rx.on_receive(0, 0, 0), ReceiveResult::Accepted); // base -> 1
    ASSERT_EQ(rx.on_receive(1, 1, 10), ReceiveResult::Accepted); // base -> 2
    // Now receive 5, leaving 2,3,4 missing.
    ASSERT_EQ(rx.on_receive(5, 5, 100), ReceiveResult::Accepted);

    Nack nack;
    uint16_t n = rx.collect_nacks(100 + kReorderDelay, kReorderDelay, kRenackInterval, &nack);
    ASSERT_EQ(n, 3);
    EXPECT_EQ(nack.missing[0], 2u);
    EXPECT_EQ(nack.missing[1], 3u);
    EXPECT_EQ(nack.missing[2], 4u);
}
