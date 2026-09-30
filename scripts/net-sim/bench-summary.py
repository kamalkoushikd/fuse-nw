#!/usr/bin/env python3
"""Summarize scripts/net-sim/.cache/logs/benchmark.csv into a markdown table.

Usage: bench-summary.py <path-to-benchmark.csv>
"""
import csv
import statistics as st
import sys


def summarize(rows):
    mbps = [float(r["mbps"]) for r in rows if r["mbps"]]
    ok = sum(1 for r in rows if r["sha_match"] == "1")
    out = {"n": len(rows), "ok": ok, "fail": len(rows) - ok}
    if mbps:
        out["mean"] = st.mean(mbps)
        out["stdev"] = st.pstdev(mbps) if len(mbps) > 1 else 0.0
        out["min"] = min(mbps)
        out["max"] = max(mbps)
        out["median"] = st.median(mbps)
        s = sorted(mbps)
        out["p95"] = s[max(0, int(len(s) * 0.95) - 1)]
    return out


def main():
    path = sys.argv[1]
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))

    by_proto = {}
    for r in rows:
        by_proto.setdefault(r["protocol"], []).append(r)

    print("# net-sim benchmark summary\n")
    print(f"{len(rows)} total runs ({path})\n")
    print("| protocol | runs | ok | fail | mean MB/s | stdev | min | max | median | p95 |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for proto, rs in by_proto.items():
        s = summarize(rs)
        if "mean" in s:
            print(
                f"| {proto} | {s['n']} | {s['ok']} | {s['fail']} | "
                f"{s['mean']:.1f} | {s['stdev']:.1f} | {s['min']:.1f} | {s['max']:.1f} | "
                f"{s['median']:.1f} | {s['p95']:.1f} |"
            )
        else:
            print(f"| {proto} | {s['n']} | {s['ok']} | {s['fail']} | - | - | - | - | - | - |")

    fuse_retx = [int(r["retransmits"]) for r in rows if r["protocol"] == "fuse" and r["retransmits"]]
    if fuse_retx:
        print(
            f"\nfuse retransmits per run: mean {st.mean(fuse_retx):.0f}, "
            f"min {min(fuse_retx)}, max {max(fuse_retx)}"
        )

    if by_proto.get("fuse") and by_proto.get("quic"):
        f_mean = summarize(by_proto["fuse"]).get("mean")
        q_mean = summarize(by_proto["quic"]).get("mean")
        if f_mean and q_mean:
            print(f"\nfuse was {f_mean / q_mean:.2f}x QUIC's mean throughput on this run.")


if __name__ == "__main__":
    main()
