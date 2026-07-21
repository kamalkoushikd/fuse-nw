#ifndef FUSE_PROTO_REASSEMBLY_HPP
#define FUSE_PROTO_REASSEMBLY_HPP

// Stage 4 receiver-side delivery, branching on a stream's ORDERED flag.
//
//   ORDERED=1: out-of-order blocks are buffered and surfaced upward only
//              in seq_no order (an in-memory reorder buffer bounded by the
//              window).
//   ORDERED=0: each block is surfaced the instant it arrives, regardless
//              of order, via a positional write at offset
//              seq_no * block_size — no reorder buffer at all.
//
// Both modes write payloads into the same caller-provided sink at
// offset = seq_no * block_size, so the fully-received result is identical;
// only the *order of delivery* differs. delivery_order() records the seqs
// in the order they were surfaced, which is the trace evidence that an
// unordered stream really does hand data up out of arrival order.
//
// total_blocks (from the start of a bulk stream, 4.2) lets the receiver
// tell "still coming" from "that was the last block, and it is missing":
// is_complete() is true only once every block below total_blocks has been
// delivered.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "fuse/proto/setup.hpp"
#include "fuse/proto/wire.hpp"

namespace fuse::proto {

class ReassemblyStream {
public:
    // `sink`/`sink_cap` receive positional writes; total_blocks may be 0 if
    // the stream length is not known up front.
    ReassemblyStream(const StreamConfig &config, uint8_t *sink, size_t sink_cap,
                     uint64_t total_blocks = 0);

    bool ordered() const { return ordered_; }
    uint64_t delivered_count() const { return delivered_count_; }
    bool is_complete() const { return total_blocks_ != 0 && delivered_count_ == total_blocks_; }

    const std::vector<uint64_t> &delivery_order() const { return delivery_order_; }

    // Feeds one received block. Returns the number of blocks surfaced to the
    // sink as a result (1 immediately for unordered; 0..N for ordered, as a
    // newly-arrived block may unblock a run of buffered successors).
    uint32_t on_block(uint64_t seq_no, const uint8_t *payload, uint16_t len);

private:
    void deliver(uint64_t seq_no, const uint8_t *payload, uint16_t len);

    bool ordered_;
    uint16_t block_size_;
    uint8_t *sink_;
    size_t sink_cap_;
    uint64_t total_blocks_;
    uint64_t delivered_count_ = 0;
    std::vector<uint64_t> delivery_order_;

    // Ordered-mode reorder buffer: a fixed ring keyed by seq_no, bounded by
    // the window, so it needs no per-block allocation.
    uint64_t next_expected_ = 0;
    struct Slot {
        uint64_t seq_no = 0;
        uint16_t len = 0;
        bool valid = false;
        std::array<uint8_t, kMaxPayloadSize> payload{};
    };
    std::array<Slot, kMaxWindow> buffer_{};
};

} // namespace fuse::proto

#endif // FUSE_PROTO_REASSEMBLY_HPP
