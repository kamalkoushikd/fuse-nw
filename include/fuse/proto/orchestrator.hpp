#ifndef FUSE_PROTO_ORCHESTRATOR_HPP
#define FUSE_PROTO_ORCHESTRATOR_HPP

// Dynamic worker orchestration — the replacement for static CPU pinning.
//
// Stage 6 pinned each worker to a fixed core for the session. Measurement
// showed that consistently *hurt*: pinning senders while their peers float
// stops the scheduler co-locating them, and a fixed worker count is wrong
// as soon as load changes. Benchmarking also showed throughput is not
// monotonic in worker count (4 lanes beat 8 on this host), so the right
// concurrency is something to discover at runtime, not configure ahead of
// time. Core placement — pinned or merely hinted — added complexity with
// no measured benefit, so this orchestrator leaves thread placement to the
// OS scheduler entirely.
//
// This orchestrator borrows the shape of a Kubernetes horizontal pod
// autoscaler:
//
//   * a target utilisation band rather than a fixed replica count,
//   * min/max replica bounds,
//   * a stabilisation window so it cannot flap between scale decisions,
//   * graceful drain: a retired worker finishes its current work and exits
//     rather than being killed mid-operation.
//
// The orchestrator is workload-agnostic: it runs a caller-supplied task and
// only needs to know whether each invocation did useful work.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fuse::proto {

struct OrchestratorConfig {
    uint16_t min_workers = 1;
    uint16_t max_workers = 8;

    // Scale out above `target_utilization`, scale in below
    // `scale_in_utilization`. The gap between them is deliberate: a single
    // threshold makes the controller oscillate around it.
    double target_utilization = 0.70;
    double scale_in_utilization = 0.30;

    // Minimum time between scaling actions. Without this the controller
    // reacts to noise and thrashes threads.
    uint64_t stabilization_ns = 200'000'000; // 200 ms
};

struct OrchestratorStats {
    uint64_t scale_outs = 0;
    uint64_t scale_ins = 0;
    uint64_t ticks = 0;
    double last_utilization = 0.0;
};

// The task returns true when the invocation did useful work and false when
// it found nothing to do; that ratio is the utilisation signal.
using WorkerTask = std::function<bool(uint16_t worker_id)>;

class WorkerOrchestrator {
public:
    WorkerOrchestrator(const OrchestratorConfig &config, WorkerTask task);
    ~WorkerOrchestrator();

    WorkerOrchestrator(const WorkerOrchestrator &) = delete;
    WorkerOrchestrator &operator=(const WorkerOrchestrator &) = delete;

    // Brings the pool up to min_workers.
    void start();

    // Drains and joins every worker.
    void stop();

    // Evaluates one scaling decision. Call periodically from a control
    // thread; it never blocks on worker work.
    void tick(uint64_t now_ns);

    uint16_t worker_count() const;
    OrchestratorStats stats() const;

private:
    struct Worker {
        uint16_t id = 0;
        std::thread thread;
        std::atomic<bool> drain{false};
        std::atomic<uint64_t> busy_ns{0};
        std::atomic<uint64_t> total_ns{0};
        std::atomic<bool> exited{false};
    };

    void scale_out(uint64_t now_ns);
    void scale_in(uint64_t now_ns);
    void run_worker(Worker *w);

    OrchestratorConfig config_;
    WorkerTask task_;

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Worker>> workers_;
    uint16_t next_id_ = 0;
    uint64_t last_action_ns_ = 0;
    bool running_ = false;

    std::atomic<uint64_t> scale_outs_{0};
    std::atomic<uint64_t> scale_ins_{0};
    std::atomic<uint64_t> ticks_{0};
    std::atomic<uint64_t> last_util_milli_{0};
};

} // namespace fuse::proto

#endif // FUSE_PROTO_ORCHESTRATOR_HPP
