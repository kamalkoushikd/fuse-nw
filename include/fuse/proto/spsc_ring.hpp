#ifndef FUSE_PROTO_SPSC_RING_HPP
#define FUSE_PROTO_SPSC_RING_HPP

// A bounded, lock-free, single-producer / single-consumer ring.
//
// Exactly one thread may call the producer methods and exactly one other
// thread the consumer methods. With that restriction, correctness needs only
// two monotonically increasing counters and acquire/release ordering:
//
//   producer: fill slot(s) ... tail_.store(release)   ──┐ publishes the slots
//   consumer: tail_.load(acquire) ... read slot(s)     <─┘
//
//   consumer: read slot(s) ... head_.store(release)    ──┐ frees the slots
//   producer: head_.load(acquire) ... reuse slot(s)    <─┘
//
// No mutex and no compare-and-swap, so neither side ever blocks or retries
// because of the other: a full ring is reported to the producer, an empty
// one to the consumer, and each decides what to do (Fuse's receive path
// drops a packet on a full ring and lets NACK recover it rather than stall
// the socket reader behind a slow disk).
//
// Each side keeps a private cached copy of the *other* side's counter and
// only re-reads the shared atomic when the cache says the ring looks
// full/empty, so in steady state a transfer touches the other thread's
// cache line once per batch, not once per item. head_ and tail_ live on
// separate cache lines (alignas(64)) so the two threads don't false-share.
//
// Slots are allocated once, at construction, and never initialised by the
// ring itself (a slot is only read after its producer wrote it), so a large
// ring of large slots costs address space up front but physical memory only
// as slots are actually used.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace fuse::proto {

template <typename T>
class SpscRing {
public:
    // Capacity is rounded up to a power of two (minimum 2) so an index is a
    // mask, not a division.
    explicit SpscRing(size_t min_capacity)
        : cap_(round_up_pow2(min_capacity < 2 ? 2 : min_capacity)),
          mask_(cap_ - 1),
          slots_(new T[cap_]) {}

    SpscRing(const SpscRing &) = delete;
    SpscRing &operator=(const SpscRing &) = delete;

    size_t capacity() const { return cap_; }

    // --- producer side ----------------------------------------------------

    // Slots the producer may fill right now (0 means full).
    size_t writable() {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ == cap_) {
            head_cache_ = head_.load(std::memory_order_acquire);
        }
        return cap_ - (tail - head_cache_);
    }

    // The i-th free slot (i < writable()). Not visible to the consumer
    // until commit().
    T &slot_for_write(size_t i) {
        return slots_[(tail_.load(std::memory_order_relaxed) + i) & mask_];
    }

    // Publishes the next n filled slots to the consumer.
    void commit(size_t n = 1) {
        tail_.store(tail_.load(std::memory_order_relaxed) + n, std::memory_order_release);
    }

    // --- consumer side ----------------------------------------------------

    // Slots the consumer may read right now (0 means empty).
    size_t readable() {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
        }
        return tail_cache_ - head;
    }

    // The i-th published slot (i < readable()).
    T &slot_for_read(size_t i) {
        return slots_[(head_.load(std::memory_order_relaxed) + i) & mask_];
    }

    // Returns the next n read slots to the producer.
    void consume(size_t n = 1) {
        head_.store(head_.load(std::memory_order_relaxed) + n, std::memory_order_release);
    }

private:
    static size_t round_up_pow2(size_t v) {
        size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    // Consumer-owned line: its counter plus its cache of the producer's.
    alignas(64) std::atomic<size_t> head_{0};
    size_t tail_cache_ = 0;

    // Producer-owned line: its counter plus its cache of the consumer's.
    alignas(64) std::atomic<size_t> tail_{0};
    size_t head_cache_ = 0;

    alignas(64) const size_t cap_;
    const size_t mask_;
    std::unique_ptr<T[]> slots_;
};

} // namespace fuse::proto

#endif // FUSE_PROTO_SPSC_RING_HPP
