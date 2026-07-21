#ifndef FUSE_PROTO_RECEIVER_HPP
#define FUSE_PROTO_RECEIVER_HPP

// Stage 1.3 / 1.4 receiver-side tracking for one stream.
//
// State is a single uint64 received-bitmask plus a base sequence number:
// bit i set means (base_seq_no + i) has arrived. Missing-block detection
// is one bitwise op — expected & ~received — with set bits enumerated via
// __builtin_ctzll, so it is O(popcount) with no hashing and no allocation.
//
// A gap is not NACKed the instant it is noticed: a block that is merely
// reordered (slightly late but still arriving) must not be mistaken for a
// loss, so a gap must persist for a reorder-tolerance delay before it is
// reported, and re-NACKs of the same gap are rate-limited.

#include <array>
#include <cstddef>
#include <cstdint>

#include "fuse/proto/aux.hpp"
#include "fuse/proto/wire.hpp"

namespace fuse::proto {

enum class ReceiveResult {
    Accepted,     // newly recorded
    Duplicate,    // already had it, or below base (already delivered)
    OutOfWindow,  // too far ahead to track; base is stuck on an earlier gap
};

class ReceiverStream {
public:
    // `lossless` reflects the stream's LOSSLESS flag (Stage 4). When false,
    // gaps are still tracked for statistics but collect_nacks never reports
    // them — a loss-tolerant stream accepts drops rather than retransmitting.
    explicit ReceiverStream(uint16_t stream_id, uint8_t window_size, bool lossless = true);

    uint16_t stream_id() const { return stream_id_; }
    uint8_t  window_size() const { return window_size_; }
    uint64_t base_seq_no() const { return base_seq_no_; }
    uint64_t received_bitmask() const { return received_mask_; }
    uint64_t window_overflow_count() const { return window_overflow_count_; }

    // Records receipt of `seq_no` (with the send_time echoed back later in
    // ACKs), using `now_ns` to timestamp any gaps this receipt exposes.
    ReceiveResult on_receive(uint64_t seq_no, uint64_t send_time_ns, uint64_t now_ns);

    // Builds the ACK reflecting current state (base, bitmask, and the
    // send_time of the most recently accepted block).
    Ack build_ack() const;

    // Fills *out with the gaps that have persisted past `reorder_delay_ns`
    // and were not NACKed within the last `renack_interval_ns`. Returns
    // the number of missing seq_nos reported (also in out->count).
    uint16_t collect_nacks(uint64_t now_ns, uint64_t reorder_delay_ns,
                           uint64_t renack_interval_ns, Nack *out);

private:
    // Bitmask of positions [0 .. highest_rel] that should have arrived by
    // now (everything at or below the highest block we've seen).
    uint64_t expected_mask() const;

    void slide_base_over_contiguous_prefix();

    uint16_t stream_id_;
    uint8_t  window_size_;
    bool     lossless_;

    uint64_t base_seq_no_ = 0;
    uint64_t received_mask_ = 0;
    uint64_t highest_received_ = 0;
    bool     have_any_ = false;

    uint64_t last_send_time_ = 0;        // send_time of the highest block seen
    uint64_t window_overflow_count_ = 0; // metric: blocks dropped as untrackable

    // Per-relative-position gap timing (index i tracks seq base_seq_no_+i).
    std::array<uint64_t, kMaxWindow> first_missing_ns_{};
    std::array<uint64_t, kMaxWindow> last_nack_ns_{};
};

} // namespace fuse::proto

#endif // FUSE_PROTO_RECEIVER_HPP
