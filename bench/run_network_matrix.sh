#!/usr/bin/env bash
# Network-conditions test matrix: Fuse vs QUIC through a userspace UDP
# impairment emulator (bench/fuse_netem — a tc-netem substitute that needs
# no root). This is the standard way a transport is characterised: not just
# throughput on a clean link, but behaviour under loss, propagation delay,
# jitter, reordering and duplication.
#
# Every run is verified byte-identical against the source (sha256). A fast
# transfer that corrupts or truncates the file counts as a failure, not a
# result.
#
# Topology per run:  client -> fuse_netem(impairments) -> server
# Both protocols traverse the SAME emulator, so the emulator's own overhead
# is a common factor and the comparison stays fair.
#
# Usage: bench/run_network_matrix.sh [size-MiB] [lanes]

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FB="${FB:-$REPO/build/default/bench/fuse_filebench}"
QB="${QB:-$REPO/bench/quic/target/release/quic_filebench}"
NE="${NE:-$REPO/build/default/bench/fuse_netem}"
WORK="$(mktemp -d "${BENCH_WORKDIR:-/tmp}/netmatrix.XXXXXX")"
trap 'rm -rf "$WORK"; kill $(jobs -p) 2>/dev/null' EXIT

SIZE_MB="${1:-32}"
LANES="${2:-4}"

for b in "$FB" "$NE"; do
  [ -x "$b" ] || { echo "missing: $b (build fuse_filebench + fuse_netem)"; exit 1; }
done
HAVE_QUIC=0; [ -x "$QB" ] && HAVE_QUIC=1

IN="$WORK/in.bin"
head -c "$((SIZE_MB*1024*1024))" /dev/urandom > "$IN"
SUM=$(sha256sum "$IN" | cut -d' ' -f1)

sport=42000   # server (receiver) base port
rport=43000   # relay listen base port
step_ports() { sport=$((sport+64)); rport=$((rport+64)); }

parse_tp() { sed -n 's/.*throughput=\([0-9.]*\) MB\/s.*/\1/p' "$1" | head -1; }

# run_fuse <netem-args...> -> echoes "MB/s verified"
run_fuse() {
  step_ports
  "$FB" recv "$sport" "$LANES" "$WORK/o.bin" >"$WORK/fr.log" 2>&1 & local rx=$!
  "$NE" "$rport" "$LANES" 127.0.0.1 "$sport" "$@" >"$WORK/ne.log" 2>&1 & local ne=$!
  sleep 0.6
  timeout 180 "$FB" send 127.0.0.1 "$rport" "$LANES" "$IN" >"$WORK/fs.log" 2>&1
  kill "$ne" 2>/dev/null; wait "$rx" 2>/dev/null
  local tp; tp=$(parse_tp "$WORK/fr.log"); tp=${tp:-0}
  local v="ok"; [ "$(sha256sum "$WORK/o.bin" 2>/dev/null|cut -d' ' -f1)" = "$SUM" ] || v="FAIL"
  echo "$tp $v"
}

# run_quic <netem-args...> -> echoes "MB/s verified"  (relay uses ONE port
# per lane just like Fuse; quic_filebench shards across lanes the same way)
run_quic() {
  step_ports
  rm -f "$WORK/cert.der"
  "$QB" recv "$sport" "$LANES" "$WORK/q.bin" "$WORK/cert.der" >"$WORK/qr.log" 2>&1 & local rx=$!
  for _ in $(seq 1 60); do [ -s "$WORK/cert.der" ] && break; sleep 0.05; done
  "$NE" "$rport" "$LANES" 127.0.0.1 "$sport" "$@" >"$WORK/ne.log" 2>&1 & local ne=$!
  sleep 0.4
  timeout 180 "$QB" send 127.0.0.1 "$rport" "$LANES" "$IN" "$WORK/cert.der" >"$WORK/qs.log" 2>&1
  kill "$ne" 2>/dev/null; wait "$rx" 2>/dev/null
  local tp; tp=$(parse_tp "$WORK/qr.log"); tp=${tp:-0}
  local v="ok"; [ "$(sha256sum "$WORK/q.bin" 2>/dev/null|cut -d' ' -f1)" = "$SUM" ] || v="FAIL"
  echo "$tp $v"
}

echo "Network-conditions matrix — ${SIZE_MB} MiB, ${LANES} lanes, via userspace emulator"
echo "host: $(nproc) cores, $(uname -sr)"
echo
printf '%-34s %14s %8s' "condition" "Fuse MB/s" "ok"
[ "$HAVE_QUIC" = 1 ] && printf ' %14s %8s' "QUIC MB/s" "ok"
printf '\n'
printf -- '%.0s-' {1..90}; printf '\n'

row() {
  local label="$1"; shift
  read -r ftp fok <<<"$(run_fuse "$@")"
  printf '%-34s %14s %8s' "$label" "$ftp" "$fok"
  if [ "$HAVE_QUIC" = 1 ]; then
    read -r qtp qok <<<"$(run_quic "$@")"
    printf ' %14s %8s' "$qtp" "$qok"
  fi
  printf '\n'
}

# --- the matrix -----------------------------------------------------------
row "baseline (clean relay)"
row "loss 0.5%"                 --loss-pct 0.5
row "loss 2%"                   --loss-pct 2
row "loss 5%"                   --loss-pct 5
row "delay 10ms (20ms RTT)"     --delay-ms 10
row "delay 25ms (50ms RTT)"     --delay-ms 25
row "delay 50ms (100ms RTT)"    --delay-ms 50
row "jitter 10ms+-5ms"          --delay-ms 10 --jitter-ms 5
row "reorder 10% @10ms"         --delay-ms 10 --reorder-pct 10
row "duplication 3% @5ms"       --delay-ms 5 --dup-pct 3
row "WAN: 25ms RTT + 1% loss"   --delay-ms 12 --loss-pct 1
row "lossy WAN: 50ms + 2% loss" --delay-ms 25 --loss-pct 2

cat <<'EOF'

Notes:
  * Throughput is capped by the emulator itself (a single userspace relay
    doing recvfrom+sendto per datagram, ~2.5 GB/s on this host). The cap is
    identical for both protocols, so relative numbers and the *shape* of
    degradation under each condition are what matter, not absolute peak.
  * Delay is one-way each direction, so RTT is twice the stated delay.
  * High-RTT rows are window/BDP-limited (throughput ~ window_bytes / RTT):
    Fuse's 64-block in-flight window is the ceiling there, not the link.
  * Every run is sha256-verified; "ok" means the file arrived byte-identical.
EOF
