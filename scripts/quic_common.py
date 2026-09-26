"""Shared by quic_sender.py/quic_receiver.py: a progress line matching the
fuse quickstart tools' format (examples/quickstart_common.hpp) — same
percentage/MiB/rate/ETA layout, same tty-aware redraw-in-place vs.
one-line-per-interval behavior when stderr is redirected. The one honest
difference: fuse reports per-lane progress ("lanes 2/4"); this protocol is a
single QUIC stream, so there's no lanes field to show.
"""

import sys
import time


def make_progress_printer(verb: str):
    tty = sys.stderr.isatty()
    interval = 0.25 if tty else 2.0
    last = [0.0]

    def report(bytes_done: int, bytes_total: int, elapsed: float, force: bool = False) -> None:
        now = time.monotonic()
        if not force and now - last[0] < interval:
            return
        last[0] = now

        mib = bytes_done / (1024 * 1024)
        rate = mib / elapsed if elapsed > 0 else 0.0
        if bytes_total > 0:
            total_mib = bytes_total / (1024 * 1024)
            pct = 100.0 * bytes_done / bytes_total
            eta = (total_mib - mib) / rate if rate > 0 else 0.0
            line = f"{verb} {pct:5.1f}%  {mib:.1f} / {total_mib:.1f} MiB  {rate:.1f} MiB/s  ETA {eta:.0f}s"
        else:
            line = f"{verb} {mib:.1f} MiB  {rate:.1f} MiB/s"

        prefix = "\r" if tty else ""
        suffix = "   " if tty else "\n"
        sys.stderr.write(prefix + line + suffix)
        sys.stderr.flush()

    return report


def finish_progress_line() -> None:
    if sys.stderr.isatty():
        sys.stderr.write("\n")
