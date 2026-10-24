#include <gtest/gtest.h>

#include <algorithm>

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

// ROADMAP #23: the RTO has a variance term and backs off exponentially.
TEST(Congestion, RtoFollowsRfc6298AndBacksOff) {
    CongestionController cc(128);
    EXPECT_EQ(cc.rto_ns(5'000'000, 1'000'000'000, 1'000'000'000), 1'000'000'000u)
        << "initial RTO before any sample";
    cc.on_rtt_sample(10'000'000); // srtt 10 ms, rttvar 5 ms
    EXPECT_EQ(cc.rto_ns(5'000'000, 1'000'000'000, 1'000'000'000), 30'000'000u);

    // Jitter raises the RTO through the variance term, not just via SRTT.
    for (int i = 0; i < 8; ++i) cc.on_rtt_sample(i % 2 ? 2'000'000 : 18'000'000);
    const uint64_t jittery = cc.rto_ns(5'000'000, 1'000'000'000, 1'000'000'000);
    EXPECT_GT(jittery, cc.rtt_ns() * 2);

    const uint32_t before = cc.window();
    cc.poll(1);
    cc.on_timeout();
    const uint64_t once = cc.rto_ns(5'000'000, 1'000'000'000, 1'000'000'000);
    EXPECT_EQ(once, std::min<uint64_t>(jittery * 2, 1'000'000'000));
    cc.on_timeout();
    cc.on_timeout();
    EXPECT_EQ(cc.backoff(), 3u);
    cc.poll(1 + cc.rtt_ns()); // close exactly one epoch
    EXPECT_EQ(cc.window(), std::max<uint32_t>(2, before / 2))
        << "a run of timeouts halves the window once";
    cc.on_progress();
    EXPECT_EQ(cc.rto_ns(5'000'000, 1'000'000'000, 1'000'000'000), jittery);
    EXPECT_EQ(cc.rto_ns(5'000'000, 20'000'000, 1'000'000'000), 20'000'000u) << "capped at max";
}

// ROADMAP #8: opt-in slow start doubles per clean RTT until the first loss,
// then continues as AIMD from half the window.
TEST(Congestion, SlowStartDoublesUntilFirstLoss) {
    CongestionController cc(1024, true, 1, 16);
    cc.enable_slow_start(16);
    EXPECT_EQ(cc.window(), 16u);
    cc.on_rtt_sample(1000);
    uint64_t t = 1;
    cc.poll(t);
    for (uint32_t want : {32u, 64u, 128u}) {
        t += 1000;
        cc.poll(t);
        EXPECT_EQ(cc.window(), want);
    }
    EXPECT_TRUE(cc.in_slow_start());
    cc.on_loss();
    t += 1000;
    cc.poll(t);
    EXPECT_EQ(cc.window(), 64u);
    EXPECT_FALSE(cc.in_slow_start());
    t += 1000;
    cc.poll(t);
    EXPECT_EQ(cc.window(), 80u) << "additive increase after the first loss";

    CongestionController capped(100, true, 1, 16);
    capped.enable_slow_start(64);
    capped.on_rtt_sample(1000);
    capped.poll(1);
    capped.poll(1001);
    EXPECT_EQ(capped.window(), 100u);
    EXPECT_FALSE(capped.in_slow_start());
}

// ROADMAP #22: pacing spreads a window over (4/5 of) one SRTT.
TEST(Congestion, PacingSpreadsTheWindowOverAnRtt) {
    CongestionController cc(1024);
    EXPECT_EQ(cc.pacing_interval_ns(60000, 1200000), 0u) << "no pacing before an RTT sample";
    cc.on_rtt_sample(10'000'000); // 10 ms
    // A 60 KB batch of a 1.2 MB window is 1/20 of the window: 1/20 of 8 ms.
    EXPECT_EQ(cc.pacing_interval_ns(60000, 1200000), 400'000u);
    EXPECT_EQ(cc.pacing_interval_ns(1200000, 1200000), 8'000'000u);
}
