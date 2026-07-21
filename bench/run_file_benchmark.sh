#!/usr/bin/env bash
# Head-to-head file-transfer benchmark: Fuse vs a reference QUIC (quinn).
#
# Both split the file into the same number of shards ("lanes"), transfer them
# in parallel over loopback, and stitch them back together. Every run is
# verified byte-identical against the source — a fast protocol that corrupts
# or truncates the file is not a result, so the checksum is part of the
# benchmark rather than an afterthought.
#
# Reports the median of N runs per (protocol, size, lanes).
#
# Usage: bench/run_file_benchmark.sh [runs] [size-MiB] [lane-counts...]
#   e.g. bench/run_file_benchmark.sh 3 64 1 2 4 8

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FUSE_BIN="${FUSE_BIN:-$REPO/build/default/bench/fuse_filebench}"
QUIC_BIN="${QUIC_BIN:-$REPO/bench/quic/target/release/quic_filebench}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

RUNS="${1:-3}"
SIZE_MB="${2:-64}"
shift 2 2>/dev/null || shift $#
LANES=("$@")
[ ${#LANES[@]} -eq 0 ] && LANES=(1 4 8)

for bin in "$FUSE_BIN" "$QUIC_BIN"; do
    if [ ! -x "$bin" ]; then
        echo "missing binary: $bin" >&2
        echo "  cmake --build build/default --target fuse_filebench" >&2
        echo "  (cd bench/quic && cargo build --release)" >&2
        exit 1
    fi
done

port=50200
next_port() { port=$((port + 64)); echo "$port"; }
parse_tp() { sed -n 's/.*throughput=\([0-9.]*\) MB\/s.*/\1/p' "$1" | head -1; }
median() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END {print (NR%2==1)? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}'; }

IN="$WORK/in.bin"
head -c "$((SIZE_MB * 1024 * 1024))" /dev/urandom > "$IN"
SRC_SUM=$(sha256sum "$IN" | cut -d' ' -f1)

run_fuse() { # $1=lanes $2=out
    local p; p=$(next_port)
    "$FUSE_BIN" recv "$p" "$1" "$2" >"$WORK/fr.log" 2>&1 &
    local rx=$!
    sleep 0.4
    "$FUSE_BIN" send 127.0.0.1 "$p" "$1" "$IN" >"$WORK/fs.log" 2>&1
    wait $rx 2>/dev/null
}

run_quic() { # $1=lanes $2=out
    local p; p=$(next_port)
    rm -f "$WORK/cert.der"
    "$QUIC_BIN" recv "$p" "$1" "$2" "$WORK/cert.der" >"$WORK/qr.log" 2>&1 &
    local rx=$!
    for _ in $(seq 1 60); do [ -s "$WORK/cert.der" ] && break; sleep 0.05; done
    "$QUIC_BIN" send 127.0.0.1 "$p" "$1" "$IN" "$WORK/cert.der" >"$WORK/qs.log" 2>&1
    wait $rx 2>/dev/null
}

echo "Fuse vs QUIC — ${SIZE_MB} MiB file, median of $RUNS runs, loopback"
echo "host: $(nproc) cores, $(uname -sr)"
echo
printf '%-7s %-22s %12s %10s\n' "lanes" "protocol" "MB/s" "verified"
printf -- '------------------------------------------------------------\n'

for lanes in "${LANES[@]}"; do
    for proto in fuse quic; do
        tps=(); ok="yes"
        for _ in $(seq 1 "$RUNS"); do
            OUT="$WORK/out.bin"; rm -f "$OUT"
            if [ "$proto" = fuse ]; then run_fuse "$lanes" "$OUT"; else run_quic "$lanes" "$OUT"; fi
            if [ "$(sha256sum "$OUT" 2>/dev/null | cut -d' ' -f1)" != "$SRC_SUM" ]; then
                ok="NO"; continue
            fi
            if [ "$proto" = fuse ]; then tps+=("$(parse_tp "$WORK/fr.log")")
            else tps+=("$(parse_tp "$WORK/qr.log")"); fi
        done
        label=$([ "$proto" = fuse ] && echo "Fuse (unencrypted)" || echo "QUIC (TLS 1.3)")
        if [ ${#tps[@]} -gt 0 ]; then
            printf '%-7s %-22s %12s %10s\n' "$lanes" "$label" "$(median "${tps[@]}")" "$ok"
        else
            printf '%-7s %-22s %12s %10s\n' "$lanes" "$label" "FAIL" "$ok"
        fi
    done
done

cat <<'EOF'

Reading these numbers:
  * Throughput is receiver-side delivery (first datagram / stream open
    through last byte), the most directly comparable figure.
  * Fuse runs UNENCRYPTED here; QUIC always encrypts (TLS 1.3), so the
    comparison is tilted in Fuse's favour. Fuse's own DTLS layer costs a
    further 44-56% when enabled.
  * Loopback has no loss and microsecond RTT. It measures per-packet
    implementation overhead, NOT congestion control, loss recovery, or
    behaviour at high bandwidth-delay product — where the outcome could
    differ entirely. Fuse's adaptive block size in particular grows to
    16 KiB here because loopback MTU is 65536; on a 1500-MTU path it would
    stay near 1200 and this advantage would shrink.
EOF
