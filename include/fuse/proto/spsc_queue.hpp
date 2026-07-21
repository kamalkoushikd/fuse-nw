#ifndef FUSE_PROTO_SPSC_QUEUE_HPP
#define FUSE_PROTO_SPSC_QUEUE_HPP

// A bounded, lock-free single-producer / single-consumer ring buffer.
//
// This is the one channel between the aux thread and a worker thread
// (Stage 3): the aux thread is the sole producer of retransmit requests
// for a given worker, and that worker is the sole consumer. Because there
// is exactly one producer and one consumer, correctness needs only two
// atomics (head/tail) with acquire/release ordering — no mutex, so a
// worker's hot path never blocks on a lock. Capacity is fixed at
// construction; there is no allocation after startup.

#include <atomic>
#include <cstddef>
#include <vector>

namespace fuse::proto {

template <typename T>
class SpscQueue {
public:
    // Usable capacity is `capacity` items (one slot is reserved to
    // distinguish full from empty). Storage is allocated once, here.
    explicit SpscQueue(size_t capacity)
        : buffer_(capacity + 1), cap_(capacity + 1) {}

    // Producer side only.
    bool push(const T &value) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next = (tail + 1) % cap_;
        if (next == head_.load(std::memory_order_acquire)) {
            return false; // full
        }
        buffer_[tail] = value;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side only.
    bool pop(T &out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = buffer_[head];
        head_.store((head + 1) % cap_, std::memory_order_release);
        return true;
    }

private:
    std::vector<T> buffer_;
    size_t cap_;
    std::atomic<size_t> head_{0}; // next index to read (consumer)
    std::atomic<size_t> tail_{0}; // next index to write (producer)
};

} // namespace fuse::proto

#endif // FUSE_PROTO_SPSC_QUEUE_HPP
