// Shared by the two quickstart tools: Ctrl+C handling and a progress line.
//
// Ctrl+C (SIGINT) or SIGTERM sets the transfer's cancel flag, so the
// transfer tells the peer it is aborting and returns promptly — the peer
// then stops too, instead of waiting out its timeout. A second Ctrl+C
// exits immediately without that courtesy.

#ifndef FUSE_EXAMPLES_QUICKSTART_COMMON_HPP
#define FUSE_EXAMPLES_QUICKSTART_COMMON_HPP

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>

#include <fuse/transfer.hpp>

namespace quickstart {

inline std::atomic<bool> g_cancel{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "the signal handler below relies on a lock-free atomic");

inline void on_signal(int) {
    if (g_cancel.exchange(true)) {
        _exit(130); // second signal: the user really wants out now
    }
}

inline void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// Wires cancellation and a progress line on stderr into `cfg`. On a
// terminal the line redraws in place; when stderr is redirected (a log
// file, a script) it prints one plain line every couple of seconds.
inline void attach(fuse::TransferConfig &cfg, const char *verb) {
    install_signal_handlers();
    cfg.cancel = &g_cancel;

    const bool tty = isatty(STDERR_FILENO) != 0;
    cfg.progress_interval_ms = tty ? 250 : 2000;
    cfg.on_progress = [tty, verb](const fuse::TransferProgress &p) {
        const double mib = static_cast<double>(p.bytes_done) / (1024.0 * 1024.0);
        const double rate = p.mb_per_s();
        if (p.bytes_total > 0) {
            const double total_mib = static_cast<double>(p.bytes_total) / (1024.0 * 1024.0);
            const double left = total_mib - mib;
            const double eta = rate > 0.0 ? left / rate : 0.0;
            std::fprintf(stderr, "%s%s %5.1f%%  %.1f / %.1f MiB  %.1f MiB/s  ETA %.0fs  lanes %u/%u%s",
                         tty ? "\r" : "", verb, 100.0 * p.fraction(), mib, total_mib, rate, eta,
                         p.lanes_done, p.lanes_total, tty ? "   " : "\n");
        } else {
            std::fprintf(stderr, "%s%s %.1f MiB  %.1f MiB/s  lanes %u/%u%s", tty ? "\r" : "", verb,
                         mib, rate, p.lanes_done, p.lanes_total, tty ? "   " : "\n");
        }
        std::fflush(stderr);
    };
}

// Ends the in-place progress line so later output starts on a fresh line.
inline void finish_progress_line() {
    if (isatty(STDERR_FILENO)) std::fputc('\n', stderr);
}

// Exit code: 130 is the shell convention for "stopped by Ctrl+C".
inline int exit_code_for(fuse::TransferStatus st) {
    if (st == fuse::TransferStatus::Ok) return 0;
    if (st == fuse::TransferStatus::Cancelled) return 130;
    return 1;
}

} // namespace quickstart

#endif // FUSE_EXAMPLES_QUICKSTART_COMMON_HPP
