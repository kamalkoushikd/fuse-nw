#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

#include "fuse/proto/orchestrator.hpp"

using namespace fuse::proto;

namespace {

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

// A task whose "busy or idle" behaviour the test controls, so scaling
// decisions can be driven deterministically without a real workload.
struct SyntheticLoad {
    std::atomic<bool> busy{false};

    bool operator()(uint16_t) {
        if (!busy.load(std::memory_order_relaxed)) {
            return false; // nothing to do -> counts as idle
        }
        // Burn a small, bounded slice so utilisation reads as high.
        const uint64_t until = now_ns() + 300000; // 0.3 ms
        while (now_ns() < until) {
        }
        return true;
    }
};

// Drives ticks until `pred` holds or the deadline passes.
template <typename Pred>
bool tick_until(WorkerOrchestrator &orc, Pred pred, int timeout_ms = 4000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        orc.tick(now_ns());
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

OrchestratorConfig fast_config(uint16_t lo, uint16_t hi) {
    OrchestratorConfig c;
    c.min_workers = lo;
    c.max_workers = hi;
    c.target_utilization = 0.60;
    c.scale_in_utilization = 0.20;
    c.stabilization_ns = 20'000'000; // 20 ms, so tests stay quick
    return c;
}

} // namespace

TEST(Orchestrator, StartsAtMinimumAndStopsCleanly) {
    SyntheticLoad load;
    WorkerOrchestrator orc(fast_config(2, 8), [&](uint16_t id) { return load(id); });
    orc.start();
    EXPECT_EQ(orc.worker_count(), 2);
    orc.stop();
    EXPECT_EQ(orc.worker_count(), 0) << "stop() must drain and join every worker";
}

TEST(Orchestrator, ScalesOutUnderLoadUpToMax) {
    SyntheticLoad load;
    WorkerOrchestrator orc(fast_config(1, 4), [&](uint16_t id) { return load(id); });
    orc.start();
    ASSERT_EQ(orc.worker_count(), 1);

    load.busy.store(true);
    EXPECT_TRUE(tick_until(orc, [&] { return orc.worker_count() >= 4; }))
        << "sustained load should scale out to max_workers";
    EXPECT_EQ(orc.worker_count(), 4);

    // And never beyond the cap.
    tick_until(orc, [] { return false; }, 200);
    EXPECT_LE(orc.worker_count(), 4);

    orc.stop();
}

TEST(Orchestrator, ScalesInWhenIdleDownToMin) {
    SyntheticLoad load;
    WorkerOrchestrator orc(fast_config(1, 4), [&](uint16_t id) { return load(id); });
    orc.start();

    load.busy.store(true);
    ASSERT_TRUE(tick_until(orc, [&] { return orc.worker_count() >= 3; }));

    // Work dries up: the pool should retire workers back to the floor.
    load.busy.store(false);
    EXPECT_TRUE(tick_until(orc, [&] { return orc.worker_count() == 1; }))
        << "an idle pool should scale in to min_workers";

    orc.stop();
}

// The stabilisation window is what stops the controller thrashing threads in
// response to noise.
TEST(Orchestrator, StabilizationWindowLimitsScalingRate) {
    SyntheticLoad load;
    OrchestratorConfig cfg = fast_config(1, 8);
    cfg.stabilization_ns = 500'000'000; // 500 ms: at most one action per window
    WorkerOrchestrator orc(cfg, [&](uint16_t id) { return load(id); });
    orc.start();

    load.busy.store(true);
    // Hammer ticks for well under two stabilisation windows.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < deadline) {
        orc.tick(now_ns());
    }

    EXPECT_LE(orc.worker_count(), 2)
        << "only one scale action should fit inside a 500 ms stabilization window";
    orc.stop();
}

TEST(Orchestrator, PlacesWorkersOnDistinctLeastLoadedCores) {
    if (std::thread::hardware_concurrency() < 4) {
        GTEST_SKIP() << "needs at least 4 cores";
    }
    SyntheticLoad load;
    WorkerOrchestrator orc(fast_config(4, 4), [&](uint16_t id) { return load(id); });
    orc.start();

    auto cores = orc.assigned_cores();
    ASSERT_EQ(cores.size(), 4u);
    std::set<int> distinct(cores.begin(), cores.end());
    EXPECT_EQ(distinct.size(), 4u)
        << "least-loaded placement should spread workers across cores";
    orc.stop();
}

TEST(Orchestrator, ReportsUtilizationAndActionCounts) {
    SyntheticLoad load;
    WorkerOrchestrator orc(fast_config(1, 3), [&](uint16_t id) { return load(id); });
    orc.start();

    load.busy.store(true);
    tick_until(orc, [&] { return orc.worker_count() >= 2; });
    OrchestratorStats busy_stats = orc.stats();
    EXPECT_GT(busy_stats.ticks, 0u);
    EXPECT_GE(busy_stats.scale_outs, 1u);
    EXPECT_GT(busy_stats.last_utilization, 0.0);

    load.busy.store(false);
    tick_until(orc, [&] { return orc.worker_count() == 1; });
    EXPECT_GE(orc.stats().scale_ins, 1u);

    orc.stop();
}

TEST(Orchestrator, TaskSeesStableWorkerIds) {
    std::atomic<uint32_t> id_mask{0};
    SyntheticLoad load;
    load.busy.store(true);
    WorkerOrchestrator orc(fast_config(3, 3), [&](uint16_t id) {
        if (id < 32) id_mask.fetch_or(1u << id, std::memory_order_relaxed);
        return load(id);
    });
    orc.start();
    tick_until(orc, [&] { return __builtin_popcount(id_mask.load()) >= 3; }, 2000);
    orc.stop();
    EXPECT_GE(__builtin_popcount(id_mask.load()), 3)
        << "each worker should run the task under its own id";
}
