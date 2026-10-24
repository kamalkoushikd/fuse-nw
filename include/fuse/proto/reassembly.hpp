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
// Both modes write payloads into the same caller-provided sink at the
// caller-supplied `offset` (wire.hpp v2's explicit block-header field, not
// derived as seq_no * block_size — that derivation breaks the moment block
// size varies within a stream, which adaptive sizing does by design), so
// the fully-received result is identical; only the *order of delivery*
// differs. delivery_order() records the seqs in the order they were
// surfaced, which is the trace evidence that an unordered stream really
// does hand data up out of arrival order.
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

    // The seqs in the order they were surfaced -- trace evidence for tests,
    // capped at kMaxDeliveryTrace entries so a long stream can't grow it
    // without bound (ROADMAP #17).
    static constexpr size_t kMaxDeliveryTrace = 1u << 16;
    const std::vector<uint64_t> &delivery_order() const { return delivery_order_; }

    // Feeds one received block, at its wire-carried byte `offset` within
    // the stream. Returns the number of blocks surfaced to the sink as a
    // result (1 immediately for unordered; 0..N for ordered, as a
    // newly-arrived block may unblock a run of buffered successors).
    uint32_t on_block(uint64_t seq_no, uint64_t offset, const uint8_t *payload, uint16_t len);

private:
    void deliver(uint64_t seq_no, uint64_t offset, const uint8_t *payload, uint16_t len);
    void write_sink(uint64_t offset, const uint8_t *payload, uint16_t len);

    bool ordered_;
    uint8_t *sink_;
    size_t sink_cap_;
    uint64_t total_blocks_;
    uint64_t delivered_count_ = 0;
    std::vector<uint64_t> delivery_order_;
    // Unordered mode's duplicate detection: without it, a retransmitted
    // duplicate would double-count delivered_count_, letting is_complete()
    // go true while a real gap remains. It is a window-bounded ring --
    // unordered_low_ is the lowest seq not yet seen, bit (seq % kMaxWindow)
    // marks seqs seen above it -- not a vector grown to seq_no + 1 from a
    // wire-supplied value (ROADMAP #17). A block more than kMaxWindow
    // beyond the lowest gap is outside any window a sender may use and is
    // refused.
    uint64_t unordered_low_ = 0;
    std::array<uint64_t, kMaskWords> unordered_seen_{};
    void record(uint64_t seq_no);

    // Ordered-mode reorder buffer: a fixed ring keyed by seq_no, bounded by
    // the window, so it needs no per-block allocation. An out-of-order
    // block's payload is written straight to the sink the instant it
    // arrives (its offset is already known), so the slot is only a
    // presence flag for the later in-order drain — it holds no payload.
    uint64_t next_expected_ = 0;
    struct Slot {
        uint64_t seq_no = 0;
        uint16_t len = 0;
        bool valid = false;
    };
    std::array<Slot, kMaxWindow> buffer_{};
};

} // namespace fuse::proto

#endif // FUSE_PROTO_REASSEMBLY_HPP
