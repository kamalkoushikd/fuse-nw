#include "fuse/proto/worker.hpp"

#include <ctime>

namespace fuse::proto {

namespace {
uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}
}

Worker::Worker(uint16_t worker_id) : worker_id_(worker_id) {}

void Worker::add_stream(const StreamConfig &config) {
    OwnedStream s;
    s.stream_id = config.stream_id;
    s.block_size = config.block_size > 0 ? config.block_size : 64;
    if (s.block_size > kMaxPayloadSize) {
        s.block_size = kMaxPayloadSize;
    }
    s.registry = std::make_unique<SenderRegistry>(config.stream_id, config.window_size);
    streams_.push_back(std::move(s));
    owned_stream_ids_.push_back(config.stream_id);
}

bool Worker::owns_stream(uint16_t stream_id) const {
    for (uint16_t id : owned_stream_ids_) {
        if (id == stream_id) return true;
    }
    return false;
}

SenderRegistry *Worker::registry_for(uint16_t stream_id) {
    for (auto &s : streams_) {
        if (s.stream_id == stream_id) return s.registry.get();
    }
    return nullptr;
}

bool Worker::enqueue_retransmit(const RetransmitRequest &req) {
    return queue_.push(req);
}

void Worker::start(uint32_t blocks_per_stream) {
    thread_ = std::thread([this, blocks_per_stream] { run(blocks_per_stream); });
}

void Worker::stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Worker::run(uint32_t blocks_per_stream) {
    thread_id_ = std::this_thread::get_id();

    // Apply the optional topology hint from this thread (Stage 6). With the
    // default hint (cpu_core/numa_node both -1) this does nothing at all.
    if (hint_.cpu_core >= 0) {
        pinned_.store(pin_current_thread_to_core(hint_.cpu_core), std::memory_order_release);
    }
    if (hint_.numa_node >= 0) {
        prefer_current_thread_numa_node(hint_.numa_node);
    }
    applied_affinity_ = current_thread_affinity();

    // The worker's own send loop: produce blocks for each owned stream,
    // storing them in that stream's registry so they can be retransmitted
    // later. This worker's thread is the sole writer of these registries.
    uint8_t payload[kMaxPayloadSize];
    for (auto &s : streams_) {
        for (uint32_t i = 0; i < blocks_per_stream; ++i) {
            for (uint16_t b = 0; b < s.block_size; ++b) {
                payload[b] = static_cast<uint8_t>(s.stream_id ^ i ^ b);
            }
            s.registry->store(i, payload, s.block_size, now_ns());
            sent_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    producing_done_.store(true, std::memory_order_release);

    // Service retransmit requests until asked to stop, then drain once more
    // so a request that arrived during shutdown is not lost.
    while (!stop_.load(std::memory_order_acquire)) {
        drain_queue();
        std::this_thread::yield();
    }
    drain_queue();
}

void Worker::drain_queue() {
    RetransmitRequest req;
    while (queue_.pop(req)) {
        SenderRegistry *reg = registry_for(req.stream_id);
        if (reg == nullptr) {
            // A request for a stream this worker does not own should never
            // be routed here; count it as unrecoverable rather than crash.
            unrecoverable_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const RegistrySlot *slot = reg->lookup(req.seq_no);
        if (slot != nullptr) {
            // In a full data plane this is where the block would be resent;
            // Stage 3 records that the correct worker serviced it.
            retransmit_.fetch_add(1, std::memory_order_relaxed);
            last_retransmit_seq_.store(req.seq_no, std::memory_order_relaxed);
        } else {
            unrecoverable_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// --- WorkerPool ----------------------------------------------------------

WorkerPool::WorkerPool(const SetupPayload &payload) {
    router_.build(payload);
    for (uint16_t i = 0; i < payload.num_workers; ++i) {
        workers_.push_back(std::make_unique<Worker>(i));
    }
    for (uint16_t i = 0; i < payload.num_streams; ++i) {
        const StreamConfig &cfg = payload.streams[i];
        if (cfg.worker_id < workers_.size()) {
            workers_[cfg.worker_id]->add_stream(cfg);
        }
    }
}

void WorkerPool::start(uint32_t blocks_per_stream, const std::vector<TopologyHint> &hints) {
    const bool have_hints = hints.size() == workers_.size();
    for (size_t i = 0; i < workers_.size(); ++i) {
        if (have_hints) {
            workers_[i]->set_topology_hint(hints[i]);
        }
        workers_[i]->start(blocks_per_stream);
    }
}

void WorkerPool::stop() {
    for (auto &w : workers_) {
        w->stop();
    }
}

uint16_t WorkerPool::route_nack(const Nack &nack) {
    int worker_id = router_.worker_for_stream(nack.stream_id);
    if (worker_id < 0 || static_cast<size_t>(worker_id) >= workers_.size()) {
        return 0;
    }
    uint16_t routed = 0;
    for (uint16_t i = 0; i < nack.count; ++i) {
        RetransmitRequest req{nack.stream_id, nack.missing[i]};
        if (workers_[worker_id]->enqueue_retransmit(req)) {
            ++routed;
        }
    }
    return routed;
}

} // namespace fuse::proto
