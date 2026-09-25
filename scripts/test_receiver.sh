#!/usr/bin/env sh
#
# Receiver side of a Fuse throughput test: runs fuse_quickstart_recv (the
# real multi-lane bulk-transfer path, fuse::receive_file) and prints a
# SHA-256 of what arrived. Start this BEFORE test_sender.sh on the other
# machine — it must be bound before the sender's opening message arrives.
#
#   ./test_receiver.sh <bind-address> <port>
#
# Everything else is fixed so the only inputs needed are the two above:
# 4 lanes, output written to /tmp/testoutfile, and TEST_PSK below (must be
# identical to test_sender.sh's — same test-only key, not a real secret;
# the two machines have no shared state to generate a matching one from).
#
# There's no shared filesystem between the two machines, so integrity is
# checked by eye: compare this script's printed hash against
# test_sender.sh's "sha256 (before send)" line on the sender.

set -eu

BIND="${1:?usage: $0 <bind-address> <port>}"
PORT="${2:?usage: $0 <bind-address> <port>}"
LANES=4
OUT="/tmp/testoutfile"
TEST_PSK="2142edb68d8ce7923d4843c485d3ed5aef5016d6173f63b09920b526b6c6e76c"

RECV_BIN="$(command -v fuse_quickstart_recv || true)"
if [ -z "$RECV_BIN" ]; then
    echo "fuse_quickstart_recv not found on PATH — install fuse first (see docs/USAGE.md)" >&2
    exit 1
fi

echo "==> listening on ${BIND}, UDP ${PORT}-$((PORT + LANES - 1)), writing to $OUT"
"$RECV_BIN" "$BIND" "$PORT" "$OUT" "$LANES" "$TEST_PSK"

echo "==> sha256:"
sha256sum "$OUT"
