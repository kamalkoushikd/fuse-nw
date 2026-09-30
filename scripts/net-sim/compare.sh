#!/usr/bin/env bash
# Runs the fuse transfer (run.sh) and the QUIC/HTTP3 comparison
# (quic/run.sh) back to back on the same fusenet bridge, then prints a
# throughput table from both. Two separate pkexec prompts (root needed for
# each network/pod bring-up - see README.md "Why root").
#
# To keep the comparison apples-to-apples, set the same payload size in both
# tx-pod.yaml (fuse) and quic/rx-pod.yaml (QUIC generates its own payload,
# since HTTP serves rather than accepts pushed files) - both default to
# SIZE_MB: "2048".
cd "$(dirname "${BASH_SOURCE[0]}")"
set -euo pipefail

echo "############################################"
echo "# 1/2: fuse (custom UDP protocol)"
echo "############################################"
./run.sh "$@" && FUSE_RC=0 || FUSE_RC=$?

echo
echo "############################################"
echo "# 2/2: QUIC/HTTP3 (caddy + curl)"
echo "############################################"
./quic/run.sh "$@" && QUIC_RC=0 || QUIC_RC=$?

echo
echo "############################################"
echo "# comparison"
echo "############################################"

FUSE_LINE=$(grep -oE '^sent [0-9]+ bytes in [0-9.]+ s \([0-9.]+ MB/s\)' .cache/logs/tx.log || true)
QUIC_LINE=$(grep -oE 'time_total=[0-9.]+s speed_download=[0-9.]+B/s size_download=[0-9]+B' .cache/logs/quic-tx.log || true)

printf '%-10s %s\n' "fuse:" "${FUSE_LINE:-no result (see .cache/logs/tx.log)}"

if [ -n "$QUIC_LINE" ]; then
  Q_TIME=$(grep -oE 'time_total=[0-9.]+' <<<"$QUIC_LINE" | cut -d= -f2)
  Q_BYTES=$(grep -oE 'size_download=[0-9]+' <<<"$QUIC_LINE" | cut -d= -f2)
  Q_MBPS=$(awk -v b="$Q_BYTES" -v t="$Q_TIME" 'BEGIN { if (t > 0) printf "%.1f", b / t / 1000000; else print "n/a" }')
  printf '%-10s sent %s bytes in %s s (%s MB/s)\n' "quic:" "$Q_BYTES" "$Q_TIME" "$Q_MBPS"
else
  printf '%-10s %s\n' "quic:" "no result (see .cache/logs/quic-tx.log)"
fi

[ "$FUSE_RC" = 0 ] && [ "$QUIC_RC" = 0 ]
