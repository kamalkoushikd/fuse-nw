#ifndef FUSE_PROTO_SEND_QUEUE_HPP
#define FUSE_PROTO_SEND_QUEUE_HPP

// Stage 4 sender-side backpressure policy, branching on a stream's
// COALESCE flag.
//
//   COALESCE=1: under send-queue pressure, the oldest not-yet-sent block is
//               dropped to make room for the newest — appropriate for a
//               loss-tolerant stream where fresher data supersedes stale.
//   COALESCE=0: nothing is ever dropped; a full queue rejects the push so
//               the producer backs up instead (pairs with LOSSLESS=1).
//
// The queue holds seq_nos of blocks waiting to be sent; the payload bytes
// themselves live in the sender registry. It is a bounded ring with no
// allocation after construction, single-threaded (a worker owns it).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fuse::proto {

class SendQueue {
public:
    SendQueue(size_t capacity, bool coalesce)
        : buffer_(capacity), cap_(capacity), coalesce_(coalesce) {}

    bool coalesce() const { return coalesce_; }
    size_t size() const { return size_; }
    size_t capacity() const { return cap_; }
    bool empty() const { return size_ == 0; }
    bool full() const { return size_ == cap_; }
    uint64_t dropped_count() const { return dropped_; }

    // Enqueues a block's seq_no. On a full queue: if coalescing, drops the
    // oldest queued seq_no and enqueues this one (returns true, counts a
    // drop); otherwise refuses and signals backpressure (returns false).
    bool push(uint64_t seq_no) {
        if (full()) {
            if (!coalesce_) {
                return false; // backpressure
            }
            uint64_t discarded;
            pop(discarded); // drop oldest to favor the newer block
            ++dropped_;
        }
        buffer_[tail_] = seq_no;
        tail_ = (tail_ + 1) % cap_;
        ++size_;
        return true;
    }

    bool pop(uint64_t &seq_no) {
        if (empty()) {
            return false;
        }
        seq_no = buffer_[head_];
        head_ = (head_ + 1) % cap_;
        --size_;
        return true;
    }

private:
    std::vector<uint64_t> buffer_;
    size_t cap_;
    bool coalesce_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t size_ = 0;
    uint64_t dropped_ = 0;
};

} // namespace fuse::proto

#endif // FUSE_PROTO_SEND_QUEUE_HPP
