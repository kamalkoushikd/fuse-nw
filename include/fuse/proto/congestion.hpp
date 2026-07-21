#ifndef FUSE_PROTO_CONGESTION_HPP
#define FUSE_PROTO_CONGESTION_HPP

// Stage 5 per-stream congestion control.
//
// Each LOSSLESS stream carries its own AIMD (additive-increase /
// multiplicative-decrease) controller over an effective window, capped at
// the window_size negotiated at SETUP. Congestion state is per-stream, not
// per-session — a deliberate difference from QUIC, where congestion control
// is connection-wide. A LOSSLESS=0 stream has no controller of its own; it
// coalesces under pressure (Stage 4) instead of backing off, so its
// window never adjusts.
//
// Decisions are evaluated once per RTT "window": if any loss (a NACK) was
// seen during a window, the effective window is halved (down to a floor);
// after a run of clean windows it grows by a fixed step (up to the cap).
// RTT is estimated from the ACK timestamp echo (Stage 1.5) as a smoothed
// average, so both the window length and "windows since last loss" are
// measured in real elapsed time, not raw NACK counts.

#include <cstdint>

namespace fuse::proto {

class CongestionController {
public:
    // `max_window` is the stream's SETUP-negotiated window (the cap).
    // `enabled` mirrors the LOSSLESS flag: a non-lossless stream passes
    // false and its window stays pinned at max_window forever.
    explicit CongestionController(uint8_t max_window, bool enabled = true,
                                  uint32_t clean_windows_to_grow = 3,
                                  uint32_t grow_step = 2)
        : enabled_(enabled),
          clean_windows_to_grow_(clean_windows_to_grow),
          grow_step_(grow_step) {
        max_window_ = max_window < 1 ? 1 : max_window;
        floor_ = max_window_ < 2 ? max_window_ : 2;
        if (!enabled_) {
            window_ = max_window_;
        } else {
            uint32_t start = max_window_ / 8;
            window_ = start < floor_ ? floor_ : start;
        }
    }

    bool enabled() const { return enabled_; }
    uint32_t window() const { return window_; }
    uint64_t rtt_ns() const { return srtt_ns_; }

    // Feeds an RTT sample, typically now - ack.echoed_send_time. The first
    // sample seeds the estimate; later samples smooth it (EWMA, 1/8).
    void on_rtt_sample(uint64_t sample_ns) {
        if (!enabled_ || sample_ns == 0) {
            return;
        }
        if (srtt_ns_ == 0) {
            srtt_ns_ = sample_ns;
        } else {
            srtt_ns_ = (srtt_ns_ * 7 + sample_ns) / 8;
        }
    }

    // Records that a loss (NACK) was observed in the current window.
    void on_loss() {
        if (enabled_) {
            loss_in_window_ = true;
        }
    }

    // Advances congestion-window epochs up to `now_ns`, applying an AIMD
    // decision for each elapsed RTT window. A no-op until an RTT estimate
    // exists and for a disabled (LOSSLESS=0) stream.
    void poll(uint64_t now_ns) {
        if (!enabled_ || srtt_ns_ == 0) {
            return;
        }
        if (window_start_ns_ == 0) {
            window_start_ns_ = now_ns;
            return;
        }
        // Bound the loop so a large time jump can't spin unbounded.
        for (int guard = 0; guard < 4096; ++guard) {
            if (now_ns - window_start_ns_ < srtt_ns_) {
                break;
            }
            window_start_ns_ += srtt_ns_;
            if (loss_in_window_) {
                uint32_t halved = window_ / 2;
                window_ = halved < floor_ ? floor_ : halved;
                clean_streak_ = 0;
            } else if (++clean_streak_ >= clean_windows_to_grow_) {
                uint32_t grown = window_ + grow_step_;
                window_ = grown > max_window_ ? max_window_ : grown;
                clean_streak_ = 0;
            }
            loss_in_window_ = false;
        }
    }

private:
    bool enabled_;
    uint32_t clean_windows_to_grow_;
    uint32_t grow_step_;
    uint32_t max_window_ = 1;
    uint32_t floor_ = 1;
    uint32_t window_ = 1;
    uint32_t clean_streak_ = 0;
    uint64_t srtt_ns_ = 0;
    uint64_t window_start_ns_ = 0;
    bool loss_in_window_ = false;
};

} // namespace fuse::proto

#endif // FUSE_PROTO_CONGESTION_HPP
