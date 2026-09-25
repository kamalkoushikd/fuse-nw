#!/usr/bin/env sh
#
# Sender side of a Fuse throughput test: generates a random test file (so
# reordering/corruption bugs actually change the checksum, unlike an
# all-zero file), sends it with fuse_quickstart_send (fuse::send_file, the
# real multi-lane bulk-transfer path), and prints its SHA-256. Run
# test_receiver.sh on the other machine FIRST.
#
#   ./test_sender.sh <host> <port> [size-mb] [lanes] [key]
#
# Defaults to a 1024 MiB (1 GiB) file, 4 lanes, no encryption.

set -eu

HOST="${1:?usage: $0 <host> <port> [size-mb] [lanes] [key]}"
PORT="${2:?usage: $0 <host> <port> [size-mb] [lanes] [key]}"
SIZE_MB="${3:-1024}"
LANES="${4:-4}"
KEY="${5:-}"
IN="/tmp/fuse_test_send.bin"

SEND_BIN="$(command -v fuse_quickstart_send || true)"
if [ -z "$SEND_BIN" ]; then
    echo "fuse_quickstart_send not found on PATH — install fuse first (see docs/USAGE.md)" >&2
    exit 1
fi

echo "==> generating ${SIZE_MB} MiB test file at $IN"
dd if=/dev/urandom of="$IN" bs=1M count="$SIZE_MB" status=progress

echo "==> sha256 (before send) — compare this against the receiver's printed hash:"
sha256sum "$IN"

echo "==> sending to ${HOST}:${PORT} (${LANES} lanes)"
# $KEY left unquoted on purpose: empty means "pass no 5th argument at all"
# (fuse_quickstart_send only reads argv[5] when argc > 5), not an empty key.
"$SEND_BIN" "$HOST" "$PORT" "$IN" "$LANES" $KEY
