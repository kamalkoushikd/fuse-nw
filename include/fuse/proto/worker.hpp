#ifndef FUSE_PROTO_WORKER_HPP
#define FUSE_PROTO_WORKER_HPP

// Stage 3 worker threads + static routing.
//
// Each worker owns a disjoint set of streams and the registries for them.
// A registry has exactly one writer — its owning worker's thread — so no
// worker ever locks against another, and the send/retransmit path holds no
// mutex at all. A single aux thread handles all ACK/NACK/heartbeat
// signalling: on a NACK it looks up the owning worker and pushes a
// (stream_id, seq_no) retransmit request onto that worker's lock-free SPSC
// queue. The aux thread never touches a worker's registry directly; it only
// routes signals, and the worker services the request from its own memory.

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "fuse/proto/aux.hpp" // Nack
#include "fuse/proto/registry.hpp"
#include "fuse/proto/routing.hpp"
#include "fuse/proto/setup.hpp"
#include "fuse/proto/spsc_queue.hpp"
#include "fuse/proto/topology.hpp"

namespace fuse::proto {

class Worker {
public:
    explicit Worker(uint16_t worker_id);

    uint16_t worker_id() const { return worker_id_; }

    // Setup-time only (before the thread starts): give this worker a stream
    // to own and allocate its registry.
    void add_stream(const StreamConfig &config);

    // Optional topology hint applied when the thread starts (Stage 6). With
    // the default (all -1) hint, starting the worker pins nothing.
    void set_topology_hint(const TopologyHint &hint) { hint_ = hint; }

    // Observability for the applied hint (valid after the thread has run;
    // read after producing_done()/stop()).
    bool pinned() const { return pinned_.load(std::memory_order_acquire); }
    const std::vector<int> &applied_affinity() const { return applied_affinity_; }

    const std::vector<uint16_t> &owned_streams() const { return owned_stream_ids_; }
    bool owns_stream(uint16_t stream_id) const;

    // Producer side (called by the aux thread only): enqueue a retransmit
    // request. Returns false if the worker's queue is momentarily full.
    bool enqueue_retransmit(const RetransmitRequest &req);

    // Starts the worker thread. It first produces `blocks_per_stream` blocks
    // for each owned stream (its "own send loop", storing them in its
    // registries), then services retransmit requests until stopped.
    void start(uint32_t blocks_per_stream);
    void stop();

    // Observability (safe to read after stop()/join, or live via atomics).
    std::thread::id thread_id() const { return thread_id_; }
    uint64_t sent_count() const { return sent_.load(std::memory_order_relaxed); }
    uint64_t retransmit_count() const { return retransmit_.load(std::memory_order_relaxed); }
    uint64_t unrecoverable_count() const { return unrecoverable_.load(std::memory_order_relaxed); }
    uint64_t last_retransmit_seq() const { return last_retransmit_seq_.load(std::memory_order_relaxed); }
    bool producing_done() const { return producing_done_.load(std::memory_order_acquire); }

private:
    struct OwnedStream {
        uint16_t stream_id = 0;
        uint16_t block_size = 0;
        std::unique_ptr<SenderRegistry> registry;
    };

    void run(uint32_t blocks_per_stream);
    void drain_queue();
    SenderRegistry *registry_for(uint16_t stream_id);

    uint16_t worker_id_;
    std::vector<uint16_t> owned_stream_ids_;
    std::vector<OwnedStream> streams_;
    SpscQueue<RetransmitRequest> queue_{1024};

    TopologyHint hint_{};
    std::atomic<bool> pinned_{false};
    std::vector<int> applied_affinity_; // written by the worker thread at start

    std::thread thread_;
    std::thread::id thread_id_{};
    std::atomic<bool> stop_{false};
    std::atomic<bool> producing_done_{false};
    std::atomic<uint64_t> sent_{0};
    std::atomic<uint64_t> retransmit_{0};
    std::atomic<uint64_t> unrecoverable_{0};
    std::atomic<uint64_t> last_retransmit_seq_{~0ull};
};

// Owns the workers for a session and routes NACKs to them. Constructed from
// the SETUP payload, which fixes both the worker count and the
// stream->worker assignment for the session's lifetime.
class WorkerPool {
public:
    explicit WorkerPool(const SetupPayload &payload);

    uint16_t num_workers() const { return static_cast<uint16_t>(workers_.size()); }
    Worker &worker(uint16_t id) { return *workers_[id]; }
    const StreamRouter &router() const { return router_; }

    // Starts all workers. If `hints` is non-empty it must have one entry per
    // worker; each worker is pinned per its hint (Stage 6). An empty `hints`
    // (the default) pins nothing — provably identical to Stage 3.
    void start(uint32_t blocks_per_stream, const std::vector<TopologyHint> &hints = {});
    void stop();

    // Called by the aux thread: route each missing seq_no in a NACK to the
    // worker that owns its stream. Returns the number of requests enqueued.
    // The aux thread is the single producer for every worker's queue.
    uint16_t route_nack(const Nack &nack);

private:
    StreamRouter router_;
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace fuse::proto

#endif // FUSE_PROTO_WORKER_HPP
