#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "fuse/proto/congestion.hpp"

using namespace fuse::proto;

namespace {
constexpr uint64_t kRtt = 1'000'000; // 1 ms, for a clear virtual clock
}

TEST(Congestion, StartsConservativelyBelowCap) {
    CongestionController cc(/*max_window=*/32, /*enabled=*/true);
    // Starts around max/8, never below the floor of 2, well under the cap.
    EXPECT_GE(cc.window(), 2u);
    EXPECT_LT(cc.window(), 32u);
}

TEST(Congestion, RttEstimateSeedsThenSmooths) {
    CongestionController cc(32, true);
    cc.on_rtt_sample(2'000'000);
    EXPECT_EQ(cc.rtt_ns(), 2'000'000u); // first sample seeds directly
    cc.on_rtt_sample(1'000'000);
    // EWMA pulls it down but not all the way.
    EXPECT_LT(cc.rtt_ns(), 2'000'000u);
    EXPECT_GT(cc.rtt_ns(), 1'000'000u);
}

// Acceptance: under sustained loss a LOSSLESS stream's effective window
// measurably shrinks, then recovers over a clean stretch.
TEST(Congestion, LosslessWindowShrinksUnderLossThenRecovers) {
    CongestionController cc(/*max_window=*/64, /*enabled=*/true,
                            /*clean_windows_to_grow=*/2, /*grow_step=*/4);
    cc.on_rtt_sample(kRtt);

    uint64_t now = 0;
    cc.poll(now); // establish the first window boundary

    // Let it grow on a clean stretch first, so there is room to shrink.
    std::vector<uint32_t> trace;
    for (int i = 0; i < 20; ++i) {
        now += kRtt;
        cc.poll(now);
        trace.push_back(cc.window());
    }
    uint32_t grown = cc.window();
    EXPECT_GT(grown, 2u);

    // Now sustained loss: a NACK every window for several windows.
    for (int i = 0; i < 6; ++i) {
        cc.on_loss();
        now += kRtt;
        cc.poll(now);
        trace.push_back(cc.window());
    }
    uint32_t shrunk = cc.window();
    EXPECT_LT(shrunk, grown) << "window must shrink under sustained loss";
    EXPECT_GE(shrunk, 2u) << "window must not drop below the floor";

    // Clean stretch again: it recovers upward.
    for (int i = 0; i < 20; ++i) {
        now += kRtt;
        cc.poll(now);
        trace.push_back(cc.window());
    }
    EXPECT_GT(cc.window(), shrunk) << "window must recover on a clean stretch";
}

TEST(Congestion, WindowNeverExceedsCap) {
    CongestionController cc(/*max_window=*/8, /*enabled=*/true, 1, 4);
    cc.on_rtt_sample(kRtt);
    uint64_t now = 0;
    cc.poll(now);
    for (int i = 0; i < 100; ++i) {
        now += kRtt;
        cc.poll(now);
    }
    EXPECT_LE(cc.window(), 8u) << "window is capped at the SETUP-negotiated window_size";
}

// Acceptance: a LOSSLESS=0 stream, under identical stimulus, never adjusts
// its window — congestion state is per-stream, not connection-wide.
TEST(Congestion, LosslessZeroStreamNeverAdjusts) {
    CongestionController cc(/*max_window=*/32, /*enabled=*/false);
    const uint32_t initial = cc.window();
    EXPECT_EQ(initial, 32u);

    cc.on_rtt_sample(kRtt);
    uint64_t now = 0;
    for (int i = 0; i < 30; ++i) {
        cc.on_loss(); // ignored for a non-lossless stream
        now += kRtt;
        cc.poll(now);
        EXPECT_EQ(cc.window(), initial) << "a LOSSLESS=0 stream must not adjust its window";
    }
}

// Two streams on the same session evolve independently — per-stream, not
// per-session, congestion state.
TEST(Congestion, PerStreamStateIsIndependent) {
    CongestionController lossy(64, /*enabled=*/true, 2, 4);
    CongestionController tolerant(64, /*enabled=*/false);
    lossy.on_rtt_sample(kRtt);
    tolerant.on_rtt_sample(kRtt);

    uint64_t now = 0;
    lossy.poll(now);
    tolerant.poll(now);
    // Grow the lossy stream, then hit it with loss.
    for (int i = 0; i < 10; ++i) { now += kRtt; lossy.poll(now); tolerant.poll(now); }
    for (int i = 0; i < 6; ++i) {
        lossy.on_loss();
        tolerant.on_loss();
        now += kRtt;
        lossy.poll(now);
        tolerant.poll(now);
    }
    // The lossy stream reacted; the tolerant stream did not move at all.
    EXPECT_EQ(tolerant.window(), 64u);
    EXPECT_LT(lossy.window(), 64u);
}
