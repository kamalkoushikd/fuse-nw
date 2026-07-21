#include "fuse/proto/receiver.hpp"

namespace fuse::proto {

ReceiverStream::ReceiverStream(uint16_t stream_id, uint8_t window_size, bool lossless)
    : stream_id_(stream_id), lossless_(lossless) {
    if (window_size < 1) {
        window_size = 1;
    } else if (window_size > kMaxWindow) {
        window_size = kMaxWindow;
    }
    window_size_ = window_size;
}

uint64_t ReceiverStream::expected_mask() const {
    if (!have_any_ || highest_received_ < base_seq_no_) {
        return 0;
    }
    uint64_t rel_high = highest_received_ - base_seq_no_;
    if (rel_high >= 63) {
        return ~0ull;
    }
    return (1ull << (rel_high + 1)) - 1;
}

void ReceiverStream::slide_base_over_contiguous_prefix() {
    // Advance base across every contiguously-received low bit. Each step
    // shifts the received bitmask and the two gap-timing arrays down by
    // one so index i keeps meaning "seq base_seq_no_ + i".
    while (received_mask_ & 1ull) {
        received_mask_ >>= 1;
        base_seq_no_ += 1;

        for (uint8_t i = 0; i + 1 < window_size_; ++i) {
            first_missing_ns_[i] = first_missing_ns_[i + 1];
            last_nack_ns_[i] = last_nack_ns_[i + 1];
        }
        first_missing_ns_[window_size_ - 1] = 0;
        last_nack_ns_[window_size_ - 1] = 0;
    }
}

ReceiveResult ReceiverStream::on_receive(uint64_t seq_no, uint64_t send_time_ns,
                                         uint64_t now_ns) {
    // base_seq_no_ starts at 0 (a Stage 1 stream's first seq_no); it must
    // NOT be anchored to whichever datagram happens to arrive first, since
    // that first arrival may be reordered ahead of lower seq_nos that are
    // still in flight. Anything below base has already been delivered.
    // (A future stage can make the initial base configurable via SETUP.)
    if (seq_no < base_seq_no_) {
        return ReceiveResult::Duplicate;
    }

    uint64_t rel = seq_no - base_seq_no_;
    if (rel >= window_size_) {
        ++window_overflow_count_;
        return ReceiveResult::OutOfWindow;
    }

    uint64_t bit = 1ull << rel;
    if (received_mask_ & bit) {
        return ReceiveResult::Duplicate;
    }
    received_mask_ |= bit;

    if (!have_any_ || seq_no > highest_received_) {
        highest_received_ = seq_no;
        last_send_time_ = send_time_ns;
    }
    have_any_ = true;

    // Any lower position still unset is now a genuine gap (a higher block
    // arrived without it). Timestamp each newly-exposed gap once.
    uint64_t missing = expected_mask() & ~received_mask_;
    while (missing) {
        uint64_t m = missing & (~missing + 1); // lowest set bit
        int gap_rel = __builtin_ctzll(missing);
        if (first_missing_ns_[gap_rel] == 0) {
            first_missing_ns_[gap_rel] = now_ns;
        }
        missing ^= m;
    }

    slide_base_over_contiguous_prefix();
    return ReceiveResult::Accepted;
}

Ack ReceiverStream::build_ack() const {
    Ack ack;
    ack.stream_id = stream_id_;
    ack.base_seq_no = base_seq_no_;
    ack.received_bitmask = received_mask_;
    ack.echoed_send_time = last_send_time_;
    return ack;
}

uint16_t ReceiverStream::collect_nacks(uint64_t now_ns, uint64_t reorder_delay_ns,
                                       uint64_t renack_interval_ns, Nack *out) {
    out->stream_id = stream_id_;
    out->count = 0;

    // A loss-tolerant stream (LOSSLESS=0) never retransmits: gaps are an
    // acceptable drop, so no NACK is ever emitted for it.
    if (!lossless_) {
        return 0;
    }

    uint64_t missing = expected_mask() & ~received_mask_;
    while (missing) {
        uint64_t m = missing & (~missing + 1);
        int rel = __builtin_ctzll(missing);
        missing ^= m;

        uint64_t first = first_missing_ns_[rel];
        if (first == 0 || now_ns - first < reorder_delay_ns) {
            continue; // still within reorder tolerance
        }
        uint64_t last = last_nack_ns_[rel];
        if (last != 0 && now_ns - last < renack_interval_ns) {
            continue; // rate-limit repeated NACKs of the same gap
        }

        if (out->count < kMaxWindow) {
            out->missing[out->count++] = base_seq_no_ + static_cast<uint64_t>(rel);
            last_nack_ns_[rel] = now_ns;
        }
    }
    return out->count;
}

} // namespace fuse::proto
