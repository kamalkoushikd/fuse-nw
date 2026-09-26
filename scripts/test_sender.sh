#!/usr/bin/env sh
#
# Sender side of a Fuse throughput test: generates a random 1 GiB test file
# (so reordering/corruption bugs actually change the checksum, unlike an
# all-zero file), sends it with fuse_quickstart_send (fuse::send_file, the
# real multi-lane bulk-transfer path), and prints its SHA-256. Run
# test_receiver.sh on the other machine FIRST.
#
#   ./test_sender.sh <receiver-address> <port>
#
# Everything else is fixed: 1024 MiB, 4 lanes, and TEST_PSK below (must be
# identical to test_receiver.sh's — same test-only key, not a real secret;
# the two machines have no shared state to generate a matching one from).

set -eu

HOST="${1:?usage: $0 <receiver-address> <port>}"
PORT="${2:?usage: $0 <receiver-address> <port>}"
SIZE_MB=1024
LANES=4
TEST_PSK="2142edb68d8ce7923d4843c485d3ed5aef5016d6173f63b09920b526b6c6e76c"
IN="/tmp/fuse_test_send.bin"

SEND_BIN="$(command -v fuse_quickstart_send || true)"
if [ -z "$SEND_BIN" ]; then
    echo "fuse_quickstart_send not found on PATH — install fuse first (see docs/USAGE.md)" >&2
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

echo "==> sending to ${HOST}:${PORT} (${LANES} lanes)"
"$SEND_BIN" "$HOST" "$PORT" "$IN" "$LANES" "$TEST_PSK"
