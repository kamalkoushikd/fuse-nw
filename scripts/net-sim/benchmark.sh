#!/usr/bin/env bash
# Runs the fuse and QUIC/HTTP3 transfers N times each (default 100) back to
# back on the same fusenet bridge - one pod-create/destroy cycle per
# iteration, same topology as run.sh/quic/run.sh - and records per-run
# throughput + correctness to .cache/logs/benchmark.csv, then prints a
# mean/stdev/min/max/median/p95 summary (bench-summary.py) to
# .cache/logs/benchmark-summary.md.
#
# All N*2 iterations run inside ONE pkexec call (bench-loop.sh) - one
# password prompt for the whole sweep, not N of them. Root reasons are the
# same as run.sh - see README.md "Why root".
#
# This takes a while: each iteration regenerates its own random payload and
# does a couple of podman kube play/down round trips on top of the transfer
# itself. At SIZE_MB=2048 expect roughly 10-30 minutes for N=100 depending
# on the machine - it's unattended after the one password prompt, so start
# it and come back.
#
# Usage: scripts/net-sim/benchmark.sh [N]   (default: 100)
cd "$(dirname "${BASH_SOURCE[0]}")"
set -euo pipefail

N="${1:-100}"
NETSIM="$(pwd)"

command -v podman >/dev/null || { echo "podman not found" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 not found (needed for the summary)" >&2; exit 1; }

./stage-bin.sh

mkdir -p .cache/logs
CSV=".cache/logs/benchmark.csv"
echo "run,protocol,bytes,seconds,mbps,retransmits,sha_match" > "$CSV"

echo "==> running $N iterations each of fuse + QUIC on fusenet (needs root once)"
echo "    unattended after this - go do something else, could take a while"
echo "    tail -f $NETSIM/.cache/logs/benchmark.csv in another terminal to watch rows land"
pkexec bash "$NETSIM/bench-loop.sh" "$NETSIM" "$N" "$(id -u)" "$(id -g)"

echo
echo "==> computing summary..."
python3 "$NETSIM/bench-summary.py" "$CSV" | tee .cache/logs/benchmark-summary.md
