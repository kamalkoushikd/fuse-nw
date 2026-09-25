#!/usr/bin/env sh
#
# Receiver side of a Fuse throughput test: runs fuse_quickstart_recv (the
# real multi-lane bulk-transfer path, fuse::receive_file) and prints a
# SHA-256 of what arrived. Start this BEFORE test_sender.sh on the other
# machine — it must be bound before the sender's opening message arrives.
#
#   ./test_receiver.sh <port> [out-file] [lanes] [key]
#
# There's no shared filesystem between the two machines, so integrity is
# checked by eye: compare this script's printed hash against
# test_sender.sh's "sha256 (before send)" line on the sender.

set -eu

PORT="${1:?usage: $0 <port> [out-file] [lanes] [key]}"
OUT="${2:-/tmp/fuse_test_recv.bin}"
LANES="${3:-4}"
KEY="${4:-}"

RECV_BIN="$(command -v fuse_quickstart_recv || true)"
if [ -z "$RECV_BIN" ]; then
    echo "fuse_quickstart_recv not found on PATH — install fuse first (see docs/USAGE.md)" >&2
    exit 1
fi

echo "==> listening on UDP ${PORT}-$((PORT + LANES - 1)), writing to $OUT"
# $KEY left unquoted on purpose: empty means "pass no 4th argument at all"
# (fuse_quickstart_recv only reads argv[4] when argc > 4), not an empty key.
"$RECV_BIN" "$PORT" "$OUT" "$LANES" $KEY

echo "==> sha256:"
sha256sum "$OUT"
