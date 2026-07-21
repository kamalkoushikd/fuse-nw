#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "fuse/proto/setup.hpp"
#include "fuse/proto/topology.hpp"
#include "fuse/proto/worker.hpp"

using namespace fuse::proto;

namespace {

SetupPayload topology_payload(uint16_t num_workers, uint16_t num_streams) {
    SetupPayload p;
    p.num_workers = num_workers;
    p.num_streams = num_streams;
    for (uint16_t i = 0; i < num_streams; ++i) {
        p.streams[i].stream_id = static_cast<uint16_t>(200 + i);
        p.streams[i].worker_id = static_cast<uint16_t>(i % num_workers);
        p.streams[i].stream_flags = kStreamFlagLossless;
        p.streams[i].block_size = 128;
        p.streams[i].window_size = 16;
    }
    return p;
}

template <typename Pred>
bool spin_until(Pred pred, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return pred();
}

} // namespace

TEST(Topology, PinCurrentThreadRestrictsAffinityToOneCore) {
    std::vector<int> affinity;
    bool pinned = false;
    std::thread t([&] {
        pinned = pin_current_thread_to_core(0);
        affinity = current_thread_affinity();
    });
    t.join();

    ASSERT_TRUE(pinned) << "pinning to core 0 should succeed on this host";
    ASSERT_EQ(affinity.size(), 1u);
    EXPECT_EQ(affinity[0], 0);
}

// Acceptance: with no topology hints, workers behave exactly as Stage 3 —
// provably a no-op (no pinning, affinity unrestricted).
TEST(Topology, NoHintIsANoOp) {
    SetupPayload p = topology_payload(3, 6);
    WorkerPool pool(p);
    pool.start(/*blocks_per_stream=*/8); // no hints
    ASSERT_TRUE(spin_until([&] {
        return pool.worker(0).producing_done() && pool.worker(1).producing_done() &&
               pool.worker(2).producing_done();
    }));
    pool.stop();

    for (uint16_t w = 0; w < 3; ++w) {
        EXPECT_FALSE(pool.worker(w).pinned()) << "worker " << w << " must not be pinned";
        // Unpinned: allowed on more than one core (the process default set).
        EXPECT_GT(pool.worker(w).applied_affinity().size(), 1u);
    }
}

// Acceptance: with hints supplied, each worker's affinity matches the hint.
TEST(Topology, HintsPinEachWorkerToItsCore) {
    if (std::thread::hardware_concurrency() < 3) {
        GTEST_SKIP() << "needs at least 3 cores";
    }
    SetupPayload p = topology_payload(3, 6);
    WorkerPool pool(p);

    std::vector<TopologyHint> hints(3);
    hints[0].cpu_core = 0;
    hints[1].cpu_core = 1;
    hints[2].cpu_core = 2;

    pool.start(/*blocks_per_stream=*/8, hints);
    ASSERT_TRUE(spin_until([&] {
        return pool.worker(0).producing_done() && pool.worker(1).producing_done() &&
               pool.worker(2).producing_done();
    }));
    pool.stop();

    for (uint16_t w = 0; w < 3; ++w) {
        EXPECT_TRUE(pool.worker(w).pinned()) << "worker " << w << " should be pinned";
        const auto &aff = pool.worker(w).applied_affinity();
        ASSERT_EQ(aff.size(), 1u) << "worker " << w << " affinity must be a single core";
        EXPECT_EQ(aff[0], static_cast<int>(w)) << "worker " << w << " pinned to wrong core";
    }
}

TEST(Topology, NumaHookDegradesGracefullyWhenUnavailable) {
    // Whether or not libnuma is compiled in, these calls must be safe and
    // consistent: an unavailable NUMA layer simply returns false.
    bool supported = numa_supported();
    bool bound = prefer_current_thread_numa_node(0);
    if (!supported) {
        EXPECT_FALSE(bound) << "NUMA binding must be a no-op when unsupported";
    }
    // A negative node is always rejected.
    EXPECT_FALSE(prefer_current_thread_numa_node(-1));
}
