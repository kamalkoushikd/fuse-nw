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

// ROADMAP #31: DATA's send_time_ns used to be the sender's raw monotonic
// clock, which tells anyone on the path the host's uptime. Senders now put
// now + a random per-stream offset on the wire; only the sender ever
// interprets the value (the receiver just echoes it), so RTT is the
// wrap-safe difference in that same offset domain. Returns 0 (no sample)
// for an echo that is not plausibly one of ours: in the future, or older
// than a minute.
inline uint64_t rtt_sample_from_echo(uint64_t wire_now, uint64_t echoed) {
    const uint64_t d = wire_now - echoed; // modulo 2^64, like the clock itself
    return d < 60'000'000'000ull ? d : 0;
}

class CongestionController {
public:
    // `max_window` is the stream's SETUP-negotiated window (the cap).
    // `enabled` mirrors the LOSSLESS flag: a non-lossless stream passes
    // false and its window stays pinned at max_window forever.
    explicit CongestionController(uint16_t max_window, bool enabled = true,
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

    // Slow start (ROADMAP #8), opt-in so existing users keep their exact
    // behaviour. The window starts at `initial` and doubles every clean RTT
    // epoch until the first loss (or the cap); the first loss sets the
    // threshold to half the window and the controller continues in plain
    // AIMD. Without it a window had to climb from max/8 by a few blocks per
    // RTT, which takes many round trips on a long path.
    void enable_slow_start(uint32_t initial) {
        if (!enabled_) return;
        slow_start_ = true;
        window_ = initial < floor_ ? floor_ : (initial > max_window_ ? max_window_ : initial);
    }
    bool in_slow_start() const { return slow_start_; }

    bool enabled() const { return enabled_; }
    uint32_t window() const { return window_; }
    uint64_t rtt_ns() const { return srtt_ns_; }
    uint64_t rttvar_ns() const { return rttvar_ns_; }

    // Feeds an RTT sample, typically now - ack.echoed_send_time. The first
    // sample seeds the estimate; later samples smooth it (EWMA, 1/8). The
    // RTT variance is tracked alongside (RFC 6298: 1/4 gain).
    void on_rtt_sample(uint64_t sample_ns) {
        if (!enabled_ || sample_ns == 0) {
            return;
        }
        if (srtt_ns_ == 0) {
            srtt_ns_ = sample_ns;
            rttvar_ns_ = sample_ns / 2;
        } else {
            const uint64_t dev = srtt_ns_ > sample_ns ? srtt_ns_ - sample_ns : sample_ns - srtt_ns_;
            rttvar_ns_ = (rttvar_ns_ * 3 + dev) / 4;
            srtt_ns_ = (srtt_ns_ * 7 + sample_ns) / 8;
        }
    }

    // Retransmission timeout (ROADMAP #23), RFC 6298 style: SRTT + 4·RTTVAR
    // (at least 1 ms of variance allowance, and never under 2·SRTT), clamped
    // to [min_ns, max_ns],
    // `initial_ns` before any sample, and doubled for each consecutive
    // timeout without progress. A variance term keeps jitter from firing
    // spurious retransmits; the backoff keeps a dead or overloaded path
    // from being hammered at a fixed interval.
    uint64_t rto_ns(uint64_t min_ns, uint64_t max_ns, uint64_t initial_ns) const {
        uint64_t rto = initial_ns;
        if (srtt_ns_ != 0) {
            const uint64_t var = rttvar_ns_ * 4 > 1'000'000ull ? rttvar_ns_ * 4 : 1'000'000ull;
            rto = srtt_ns_ + var;
            // Never below 2 x SRTT: NACK-driven repair takes about half an
            // RTT of reorder tolerance plus a round trip, and an RTO that
            // fires first only duplicates that repair.
            if (rto < 2 * srtt_ns_) rto = 2 * srtt_ns_;
        }
        rto = rto < min_ns ? min_ns : (rto > max_ns ? max_ns : rto);
        for (unsigned i = 0; i < backoff_ && rto < max_ns; ++i) rto *= 2;
        return rto > max_ns ? max_ns : rto;
    }

    // The RTO fired. Only the first timeout of a run counts as a loss (one
    // window halving), however long the run lasts; later ones only back off.
    void on_timeout() {
        if (backoff_ == 0) on_loss();
        if (backoff_ < kMaxBackoff) ++backoff_;
    }

    // Pacing (ROADMAP #22): how long to wait after sending `bytes` so that,
    // on average, a window of `window_bytes` is spread over one SRTT rather
    // than leaving in one line-rate burst. Paced at 1.25x the window rate
    // (as is usual) so pacing itself never becomes the bottleneck. 0 before
    // any RTT sample.
    uint64_t pacing_interval_ns(uint64_t bytes, uint64_t window_bytes) const {
        if (srtt_ns_ == 0 || window_bytes == 0) return 0;
        const long double ns = static_cast<long double>(bytes) * srtt_ns_ * 4 / 5 / window_bytes;
        return static_cast<uint64_t>(ns);
    }

    // The peer acknowledged something new: the path works, reset backoff.
    void on_progress() { backoff_ = 0; }
    unsigned backoff() const { return backoff_; }

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
                slow_start_ = false;
            } else if (slow_start_) {
                const uint32_t doubled = window_ * 2;
                window_ = doubled > max_window_ ? max_window_ : doubled;
                if (window_ == max_window_) slow_start_ = false;
            } else if (++clean_streak_ >= clean_windows_to_grow_) {
                uint32_t grown = window_ + grow_step_;
                window_ = grown > max_window_ ? max_window_ : grown;
                clean_streak_ = 0;
            }
            loss_in_window_ = false;
        }
    }

private:
    static constexpr unsigned kMaxBackoff = 6;

    bool enabled_;
    uint32_t clean_windows_to_grow_;
    uint32_t grow_step_;
    uint32_t max_window_ = 1;
    uint32_t floor_ = 1;
    uint32_t window_ = 1;
    uint32_t clean_streak_ = 0;
    uint64_t srtt_ns_ = 0;
    uint64_t rttvar_ns_ = 0;
    unsigned backoff_ = 0;
    uint64_t window_start_ns_ = 0;
    bool loss_in_window_ = false;
    bool slow_start_ = false;
};

} // namespace fuse::proto

#endif // FUSE_PROTO_CONGESTION_HPP
