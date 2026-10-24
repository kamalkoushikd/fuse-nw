#include "fuse/proto/receiver.hpp"

namespace fuse::proto {

ReceiverStream::ReceiverStream(uint16_t stream_id, uint16_t window_size, bool lossless)
    : stream_id_(stream_id), lossless_(lossless) {
    if (window_size < 1) {
        window_size = 1;
    } else if (window_size > kMaxWindow) {
        window_size = kMaxWindow;
    }
    window_size_ = window_size;
}

uint64_t ReceiverStream::relative_word(size_t w) const {
    const size_t start = slot(base_seq_no_ + 64 * static_cast<uint64_t>(w));
    const size_t q = start / 64, r = start % 64;
    uint64_t v = ring_[q] >> r;
    if (r != 0) v |= ring_[(q + 1) % kMaskWords] << (64 - r);
    // Ring positions past the window alias sequence numbers below the base
    // (always clear) or beyond the window (never set), so no masking is
    // needed for correctness; trim to the window anyway for a tidy ACK.
    const uint64_t first = 64 * static_cast<uint64_t>(w);
    if (first >= window_size_) return 0;
    if (window_size_ - first < 64) v &= (1ull << (window_size_ - first)) - 1;
    return v;
}

void ReceiverStream::slide_base_over_contiguous_prefix() {
    // Each delivered position is cleared exactly once as the base passes it,
    // which keeps the "below base is clear" invariant the ring relies on.
    while (ring_test(base_seq_no_)) {
        const size_t i = slot(base_seq_no_);
        ring_[i / 64] &= ~(1ull << (i % 64));
        first_missing_ns_[i] = 0;
        last_nack_ns_[i] = 0;
        ++base_seq_no_;
    }
}

ReceiveResult ReceiverStream::on_receive(uint64_t seq_no, uint64_t send_time_ns,
                                         uint64_t now_ns) {
    // base_seq_no_ starts at 0 (a Stage 1 stream's first seq_no); it must
    // NOT be anchored to whichever datagram happens to arrive first, since
    // that first arrival may be reordered ahead of lower seq_nos that are
    // still in flight. Anything below base has already been delivered.
    if (seq_no < base_seq_no_) {
        return ReceiveResult::Duplicate;
    }
    if (seq_no - base_seq_no_ >= window_size_) {
        ++window_overflow_count_;
        return ReceiveResult::OutOfWindow;
    }
    if (ring_test(seq_no)) {
        return ReceiveResult::Duplicate;
    }
    const size_t i = slot(seq_no);
    ring_[i / 64] |= 1ull << (i % 64);

    if (!have_any_ || seq_no > highest_received_) {
        // Every position between the previous highest and this block is now
        // a genuine gap (a higher block arrived without it). None of them
        // can have arrived yet -- they are above the old highest -- and each
        // is stamped exactly once, when it first becomes a gap.
        uint64_t from = have_any_ ? highest_received_ + 1 : base_seq_no_;
        if (from < base_seq_no_) from = base_seq_no_;
        for (uint64_t gap = from; gap < seq_no; ++gap) {
            uint64_t &first = first_missing_ns_[slot(gap)];
            if (first == 0) first = now_ns;
        }
        highest_received_ = seq_no;
        last_send_time_ = send_time_ns;
    }
    have_any_ = true;

    slide_base_over_contiguous_prefix();
    return ReceiveResult::Accepted;
}

Ack ReceiverStream::build_ack() const {
    Ack ack;
    ack.stream_id = stream_id_;
    ack.base_seq_no = base_seq_no_;
    for (size_t w = 0; w < kMaskWords; ++w) {
        ack.received_bitmask[w] = relative_word(w);
    }
    ack.echoed_send_time = last_send_time_;
    return ack;
}

uint16_t ReceiverStream::collect_nacks(uint64_t now_ns, uint64_t reorder_delay_ns,
                                       uint64_t renack_interval_ns, Nack *out) {
    out->stream_id = stream_id_;
    out->count = 0;

    // A loss-tolerant stream (LOSSLESS=0) never retransmits: gaps are an
    // acceptable drop, so no NACK is ever emitted for it.
    if (!lossless_ || !have_any_ || highest_received_ < base_seq_no_) {
        return 0;
    }

    // Only positions up to the highest block seen can be missing. Walk the
    // unset bits of that range a word at a time.
    const uint64_t span = highest_received_ - base_seq_no_; // positions 0..span
    for (size_t w = 0; w * 64 <= span && w < kMaskWords; ++w) {
        uint64_t missing = ~relative_word(w);
        const uint64_t first = 64 * static_cast<uint64_t>(w);
        if (span - first < 63) missing &= (2ull << (span - first)) - 1;
        while (missing != 0) {
            const unsigned b = static_cast<unsigned>(__builtin_ctzll(missing));
            missing &= missing - 1;
            const uint64_t seq = base_seq_no_ + first + b;
            const size_t i = slot(seq);
            const uint64_t first_ns = first_missing_ns_[i];
            if (first_ns == 0 || now_ns - first_ns < reorder_delay_ns) {
                continue; // still within reorder tolerance
            }
            const uint64_t last = last_nack_ns_[i];
            if (last != 0 && now_ns - last < renack_interval_ns) {
                continue; // rate-limit repeated NACKs of the same gap
            }
            if (out->count < kMaxWindow) {
                out->missing[out->count++] = seq;
                last_nack_ns_[i] = now_ns;
            }
        }
    }
    return out->count;
}

} // namespace fuse::proto
