#!/usr/bin/env sh
#
# Sender side of the QUIC comparison test (see test_sender.sh for the fuse
# equivalent). Generates the same 1024 MiB random test file, sends it over
# QUIC, prints its SHA-256 to compare against the receiver's. Run
# test_receiver_quic.sh on the other machine FIRST.
#
#   ./test_sender_quic.sh <receiver-address> <port>
#
# Requires: pip install aioquic (see quic_sender.py)

set -eu

HOST="${1:?usage: $0 <receiver-address> <port>}"
PORT="${2:?usage: $0 <receiver-address> <port>}"
SIZE_MB=1024
IN="/tmp/fuse_test_send.bin"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

PY="$(command -v python3 || true)"
if [ -z "$PY" ]; then
    echo "python3 not found on PATH" >&2
    exit 1
fi
if ! "$PY" -c "import aioquic" 2>/dev/null; then
    echo "aioquic not installed — run: pip3 install aioquic" >&2
    exit 1
fi

echo "==> generating ${SIZE_MB} MiB test file at $IN"
dd if=/dev/urandom of="$IN" bs=1M count="$SIZE_MB" status=progress

echo "==> sha256 (before send) — compare this against the receiver's printed hash:"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$IN"
else
    shasum -a 256 "$IN"  # macOS has no sha256sum, only shasum
fi

echo "==> sending to ${HOST}:${PORT} (QUIC)"
"$PY" "$SCRIPT_DIR/quic_sender.py" "$HOST" "$PORT" "$IN"
