#include "fuse/proto/orchestrator.hpp"

#include <ctime>

namespace fuse::proto {

namespace {
uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}
}

WorkerOrchestrator::WorkerOrchestrator(const OrchestratorConfig &config, WorkerTask task)
    : config_(config), task_(std::move(task)) {
    if (config_.max_workers < config_.min_workers) {
        config_.max_workers = config_.min_workers;
    }
}

WorkerOrchestrator::~WorkerOrchestrator() {
    stop();
}

void WorkerOrchestrator::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }
    running_ = true;
    last_action_ns_ = now_ns();
    while (workers_.size() < config_.min_workers) {
        // scale_out expects the lock held; inline the same steps.
        auto w = std::make_unique<Worker>();
        w->id = next_id_++;
        Worker *raw = w.get();
        raw->thread = std::thread([this, raw] { run_worker(raw); });
        workers_.push_back(std::move(w));
    }
}

void WorkerOrchestrator::stop() {
    std::vector<std::unique_ptr<Worker>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && workers_.empty()) {
            return;
        }
        running_ = false;
        for (auto &w : workers_) {
            w->drain.store(true, std::memory_order_release);
        }
        doomed.swap(workers_);
    }
    // Join outside the lock so a worker finishing its task cannot deadlock
    // against a control-thread tick.
    for (auto &w : doomed) {
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }
}

void WorkerOrchestrator::run_worker(Worker *w) {
    while (!w->drain.load(std::memory_order_acquire)) {
        const uint64_t t0 = now_ns();
        bool did_work = false;
        if (task_) {
            did_work = task_(w->id);
        }
        const uint64_t dt = now_ns() - t0;

        w->total_ns.fetch_add(dt, std::memory_order_relaxed);
        if (did_work) {
            w->busy_ns.fetch_add(dt, std::memory_order_relaxed);
        } else {
            // Idle: yield rather than spin, and charge the sleep to total
            // time so an idle worker reads as genuinely underutilised.
            timespec nap{0, 200000}; // 200 us
            nanosleep(&nap, nullptr);
            w->total_ns.fetch_add(200000, std::memory_order_relaxed);
        }
    }
    w->exited.store(true, std::memory_order_release);
}

void WorkerOrchestrator::tick(uint64_t now) {
    std::unique_ptr<Worker> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || workers_.empty()) {
            return;
        }
        ticks_.fetch_add(1, std::memory_order_relaxed);

        // Mean utilisation over the window since the last tick, then reset
        // the counters so each decision reflects recent behaviour rather
        // than the whole run.
        double sum = 0.0;
        for (auto &w : workers_) {
            const uint64_t busy = w->busy_ns.exchange(0, std::memory_order_relaxed);
            const uint64_t total = w->total_ns.exchange(0, std::memory_order_relaxed);
            sum += (total > 0) ? (static_cast<double>(busy) / static_cast<double>(total)) : 0.0;
        }
        const double util = sum / static_cast<double>(workers_.size());
        last_util_milli_.store(static_cast<uint64_t>(util * 1000.0), std::memory_order_relaxed);

        // Stabilisation window: refuse to act again too soon, so transient
        // spikes cannot start a thread-thrash loop.
        if (now - last_action_ns_ < config_.stabilization_ns) {
            return;
        }

        if (util > config_.target_utilization && workers_.size() < config_.max_workers) {
            auto w = std::make_unique<Worker>();
            w->id = next_id_++;
            Worker *raw = w.get();
            raw->thread = std::thread([this, raw] { run_worker(raw); });
            workers_.push_back(std::move(w));
            last_action_ns_ = now;
            scale_outs_.fetch_add(1, std::memory_order_relaxed);
        } else if (util < config_.scale_in_utilization && workers_.size() > config_.min_workers) {
            retired = std::move(workers_.back());
            workers_.pop_back();
            // Graceful drain: ask it to finish, join below without the lock.
            retired->drain.store(true, std::memory_order_release);
            last_action_ns_ = now;
            scale_ins_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (retired && retired->thread.joinable()) {
        retired->thread.join();
    }
}

uint16_t WorkerOrchestrator::worker_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint16_t>(workers_.size());
}

OrchestratorStats WorkerOrchestrator::stats() const {
    OrchestratorStats s;
    s.scale_outs = scale_outs_.load(std::memory_order_relaxed);
    s.scale_ins = scale_ins_.load(std::memory_order_relaxed);
    s.ticks = ticks_.load(std::memory_order_relaxed);
    s.last_utilization = last_util_milli_.load(std::memory_order_relaxed) / 1000.0;
    return s;
}

} // namespace fuse::proto
