#!/bin/bash
# Container entrypoint: runs one side (rx or tx) of a fuse_quickstart transfer.
# ROLE/PEER/SIZE_MB/LANES come in as env vars (see rx-pod.yaml/tx-pod.yaml).
# No VM, no init system - the container *is* the network endpoint, so this
# just execs the binary against whatever real interface podman gave it.
#
# Output is teed to /var/log/fuse-netsim/<role>.log (a host-mounted volume -
# see the `logs` volume in rx-pod.yaml/tx-pod.yaml), line-buffered via
# stdbuf so `tail -f` on that file (or `podman logs -f`) shows progress live
# instead of only on exit - piping to tee otherwise makes fuse's stdout
# fully-buffered since it's no longer a tty.
set -euo pipefail

export PATH="/opt/fuse/bin:$PATH"
export LD_LIBRARY_PATH="/opt/fuse/lib"

PORT=5000
PSK=2142edb68d8ce7923d4843c485d3ed5aef5016d6173f63b09920b526b6c6e76c
LANES="${LANES:-4}"
LOG="/var/log/fuse-netsim/${ROLE:-unknown}.log"
mkdir -p "$(dirname "$LOG")"

case "${ROLE:-}" in
  rx)
    {
      echo "=== fuse-net-sim: listening on 0.0.0.0:$PORT ==="
      stdbuf -oL -eL fuse_quickstart_recv 0.0.0.0 "$PORT" /tmp/out.bin "$LANES" "$PSK"
      echo "=== fuse-net-sim RESULT sha256 $(sha256sum /tmp/out.bin) ==="
    } 2>&1 | tee "$LOG"
    ;;
  tx)
    {
      dd if=/dev/urandom of=/tmp/in.bin bs=1M count="${SIZE_MB:-64}" 2>/dev/null
      echo "=== fuse-net-sim RESULT sha256-source $(sha256sum /tmp/in.bin) ==="
      sleep 3  # give the rx pod's server a moment to bind, in case it's slow to start
      stdbuf -oL -eL fuse_quickstart_send "${PEER:?PEER env var required for tx}" "$PORT" /tmp/in.bin "$LANES" "$PSK"
    } 2>&1 | tee "$LOG"
    ;;
  *)
    echo "ROLE must be rx or tx (got '${ROLE:-<unset>}')" >&2
    exit 1
    ;;
esac
