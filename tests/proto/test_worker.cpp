#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <set>
#include <thread>

#include "fuse/proto/aux.hpp"
#include "fuse/proto/routing.hpp"
#include "fuse/proto/setup.hpp"
#include "fuse/proto/spsc_queue.hpp"
#include "fuse/proto/worker.hpp"

using namespace fuse::proto;

namespace {

// 3 workers, 7 streams unevenly assigned: worker 0 owns {100,103,106},
// worker 1 owns {101,104}, worker 2 owns {102,105}.
SetupPayload topology() {
    SetupPayload p;
    p.num_workers = 3;
    p.num_streams = 7;
    const uint16_t worker_of[7] = {0, 1, 2, 0, 1, 2, 0};
    for (uint16_t i = 0; i < 7; ++i) {
        p.streams[i].stream_id = static_cast<uint16_t>(100 + i);
        p.streams[i].worker_id = worker_of[i];
        p.streams[i].stream_flags = kStreamFlagLossless;
        p.streams[i].block_size = 256;
        p.streams[i].window_size = 32;
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

// --- SPSC queue ----------------------------------------------------------

TEST(Spsc, PushPopFifo) {
    SpscQueue<int> q(8);
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(q.push(i));
    for (int i = 0; i < 5; ++i) {
        int v = -1;
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }
    int v = -1;
    EXPECT_FALSE(q.pop(v)); // empty
}

TEST(Spsc, ReportsFull) {
    SpscQueue<int> q(4);
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.push(i));
    EXPECT_FALSE(q.push(99)); // capacity reached
}

TEST(Spsc, ConcurrentProducerConsumer) {
    SpscQueue<uint64_t> q(1024);
    constexpr uint64_t kN = 100000;
    std::atomic<bool> ok{true};

    std::thread consumer([&] {
        uint64_t expected = 0;
        while (expected < kN) {
            uint64_t v = 0;
            if (q.pop(v)) {
                if (v != expected) ok.store(false);
                ++expected;
            }
        }
    });

    for (uint64_t i = 0; i < kN;) {
        if (q.push(i)) ++i;
    }
    consumer.join();
    EXPECT_TRUE(ok.load());
}

// --- Router --------------------------------------------------------------

TEST(StreamRouter, MapsStreamsToOwningWorkers) {
    StreamRouter router(topology());
    EXPECT_EQ(router.num_workers(), 3);
    EXPECT_EQ(router.worker_for_stream(100), 0);
    EXPECT_EQ(router.worker_for_stream(101), 1);
    EXPECT_EQ(router.worker_for_stream(102), 2);
    EXPECT_EQ(router.worker_for_stream(106), 0);
    EXPECT_EQ(router.worker_for_stream(999), -1); // unknown stream
}

// --- Worker pool: thread-per-worker + ownership --------------------------

TEST(WorkerPool, EachWorkerOnDistinctThreadOwningItsSetupStreams) {
    SetupPayload p = topology();
    WorkerPool pool(p);
    ASSERT_EQ(pool.num_workers(), 3);

    pool.start(/*blocks_per_stream=*/16);
    ASSERT_TRUE(spin_until([&] {
        return pool.worker(0).producing_done() && pool.worker(1).producing_done() &&
               pool.worker(2).producing_done();
    }));
    pool.stop();

    // Ownership matches the SETUP table exactly.
    auto owned = [&](uint16_t w) {
        std::set<uint16_t> s(pool.worker(w).owned_streams().begin(),
                             pool.worker(w).owned_streams().end());
        return s;
    };
    EXPECT_EQ(owned(0), (std::set<uint16_t>{100, 103, 106}));
    EXPECT_EQ(owned(1), (std::set<uint16_t>{101, 104}));
    EXPECT_EQ(owned(2), (std::set<uint16_t>{102, 105}));

    // Distinct threads.
    std::set<std::thread::id> ids{pool.worker(0).thread_id(), pool.worker(1).thread_id(),
                                  pool.worker(2).thread_id()};
    EXPECT_EQ(ids.size(), 3u) << "each worker must run on its own thread";

    // Each worker produced blocks_per_stream per owned stream.
    EXPECT_EQ(pool.worker(0).sent_count(), 3u * 16u);
    EXPECT_EQ(pool.worker(1).sent_count(), 2u * 16u);
    EXPECT_EQ(pool.worker(2).sent_count(), 2u * 16u);
}

// --- Aux routing: NACK reaches only the owning worker --------------------

TEST(WorkerPool, NackRoutedOnlyToOwningWorker) {
    SetupPayload p = topology();
    WorkerPool pool(p);

    pool.start(/*blocks_per_stream=*/16);
    ASSERT_TRUE(spin_until([&] {
        return pool.worker(0).producing_done() && pool.worker(1).producing_done() &&
               pool.worker(2).producing_done();
    }));

    // A NACK for stream 102 (owned by worker 2), routed by a dedicated aux
    // thread — the single producer for the workers' queues.
    Nack nack;
    nack.stream_id = 102;
    nack.count = 2;
    nack.missing[0] = 3;
    nack.missing[1] = 7;

    uint16_t routed = 0;
    std::thread aux([&] { routed = pool.route_nack(nack); });
    aux.join();
    EXPECT_EQ(routed, 2);

    // Worker 2 services exactly the two requested blocks.
    ASSERT_TRUE(spin_until([&] { return pool.worker(2).retransmit_count() >= 2; }));
    pool.stop();

    EXPECT_EQ(pool.worker(2).retransmit_count(), 2u);
    EXPECT_EQ(pool.worker(2).unrecoverable_count(), 0u);

    // Workers 0 and 1 were never touched by this NACK.
    EXPECT_EQ(pool.worker(0).retransmit_count(), 0u);
    EXPECT_EQ(pool.worker(1).retransmit_count(), 0u);
    EXPECT_EQ(pool.worker(0).unrecoverable_count(), 0u);
    EXPECT_EQ(pool.worker(1).unrecoverable_count(), 0u);
}

TEST(WorkerPool, NackForUnknownStreamRoutesNowhere) {
    SetupPayload p = topology();
    WorkerPool pool(p);
    pool.start(8);
    ASSERT_TRUE(spin_until([&] {
        return pool.worker(0).producing_done() && pool.worker(1).producing_done() &&
               pool.worker(2).producing_done();
    }));

    Nack nack;
    nack.stream_id = 999; // not in the topology
    nack.count = 1;
    nack.missing[0] = 0;
    EXPECT_EQ(pool.route_nack(nack), 0);

    pool.stop();
    for (uint16_t w = 0; w < 3; ++w) {
        EXPECT_EQ(pool.worker(w).retransmit_count(), 0u);
        EXPECT_EQ(pool.worker(w).unrecoverable_count(), 0u);
    }
}
