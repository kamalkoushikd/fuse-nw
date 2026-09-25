#include "fuse/proto/reassembly.hpp"

#include <cstring>

namespace fuse::proto {

ReassemblyStream::ReassemblyStream(const StreamConfig &config, uint8_t *sink,
                                   size_t sink_cap, uint64_t total_blocks)
    : ordered_((config.stream_flags & kStreamFlagOrdered) != 0),
      sink_(sink),
      sink_cap_(sink_cap),
      total_blocks_(total_blocks) {}

void ReassemblyStream::deliver(uint64_t seq_no, uint64_t offset, const uint8_t *payload,
                               uint16_t len) {
    write_sink(offset, payload, len);
    delivery_order_.push_back(seq_no);
    ++delivered_count_;
}

void ReassemblyStream::write_sink(uint64_t offset, const uint8_t *payload, uint16_t len) {
    if (sink_ != nullptr && payload != nullptr && len > 0 && offset <= sink_cap_ &&
        len <= sink_cap_ - offset) {
        std::memcpy(sink_ + offset, payload, len);
    }
}

uint32_t ReassemblyStream::on_block(uint64_t seq_no, uint64_t offset, const uint8_t *payload,
                                    uint16_t len) {
    if (!ordered_) {
        // Duplicate detection has no sliding base to bound it the way
        // ordered mode's next_expected_ does, so it needs its own record —
        // without this, a retransmitted duplicate double-counts
        // delivered_count_ and can make is_complete() go true early.
        if (seq_no < unordered_seen_.size() && unordered_seen_[seq_no]) {
            return 0;
        }
        if (seq_no >= unordered_seen_.size()) {
            unordered_seen_.resize(seq_no + 1, false);
        }
        unordered_seen_[seq_no] = true;
        // Positional write, delivered immediately in arrival order.
        deliver(seq_no, offset, payload, len);
        return 1;
    }

    // Ordered: ignore anything already delivered.
    if (seq_no < next_expected_) {
        return 0;
    }

    // Buffer if this block is ahead of what we can deliver now. Its offset
    // is already known (the wire-carried value, not derived), so write it
    // into the sink right away — no need to hold the payload bytes just to
    // copy them again later; the slot only needs to remember that this
    // seq_no landed.
    if (seq_no > next_expected_) {
        uint64_t rel = seq_no - next_expected_;
        if (rel < kMaxWindow) {
            Slot &slot = buffer_[seq_no % kMaxWindow];
            if (!slot.valid || slot.seq_no != seq_no) {
                write_sink(offset, payload, len);
                slot.seq_no = seq_no;
                slot.len = len;
                slot.valid = true;
            }
        }
        return 0; // nothing surfaces until next_expected_ arrives
    }

    // seq_no == next_expected_: deliver it, then drain any contiguous run of
    // buffered successors that this arrival unblocked. Drained slots were
    // already written to the sink on arrival, so draining is bookkeeping
    // only (delivery order + count), not another memcpy.
    uint32_t surfaced = 0;
    deliver(seq_no, offset, payload, len);
    ++next_expected_;
    ++surfaced;

    for (;;) {
        Slot &slot = buffer_[next_expected_ % kMaxWindow];
        if (!slot.valid || slot.seq_no != next_expected_) {
            break;
        }
        delivery_order_.push_back(slot.seq_no);
        ++delivered_count_;
        slot.valid = false;
        ++next_expected_;
        ++surfaced;
    }
    return surfaced;
}

} // namespace fuse::proto
