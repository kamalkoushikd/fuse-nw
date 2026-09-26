#!/usr/bin/env sh
#
# Receiver side of the QUIC comparison test (see test_receiver.sh for the
# fuse equivalent). Same shape: binds, waits, writes the file, reports
# throughput. Output is always /tmp/testoutfile_quic — a different name
# from fuse's /tmp/testoutfile so a side-by-side run of both doesn't clobber
# either result.
#
#   ./test_receiver_quic.sh <bind-address> <port>
#
# Requires: pip install aioquic (see quic_receiver.py)

set -eu

BIND="${1:?usage: $0 <bind-address> <port>}"
PORT="${2:?usage: $0 <bind-address> <port>}"
OUT="/tmp/testoutfile_quic"
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

"$PY" "$SCRIPT_DIR/quic_receiver.py" "$BIND" "$PORT"

echo "==> sha256:"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$OUT"
else
    shasum -a 256 "$OUT"
fi
