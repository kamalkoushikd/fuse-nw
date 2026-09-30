#!/usr/bin/env bash
# One-shot: play the pod, wait for both VMs to finish, print their console
# logs, tear the pod down again. See README.md for the one-time setup
# (build-guest.sh + podman build) this assumes is already done.
cd "$(dirname "${BASH_SOURCE[0]}")"
set -euo pipefail

podman pod exists fuse-netsim 2>/dev/null && podman kube down pod.yaml >/dev/null

podman kube play --network=none pod.yaml

echo "==> waiting for rx/tx to finish"
for _ in $(seq 1 120); do
  state="$(podman inspect fuse-netsim-rx fuse-netsim-tx --format '{{.State.Status}}' 2>/dev/null | sort -u)"
  [ "$state" = "exited" ] && break
  sleep 1
done

echo
echo "=== tx (sender) ==="
podman logs fuse-netsim-tx 2>&1 | grep "fuse-net-sim\|^sent \|^resumed"
echo "=== rx (receiver) ==="
podman logs fuse-netsim-rx 2>&1 | grep "fuse-net-sim\|^received \|^resumed"

SRC_SHA=$(podman logs fuse-netsim-tx 2>&1 | awk '/RESULT sha256-source/{print $5}')
DST_SHA=$(podman logs fuse-netsim-rx 2>&1 | awk '/RESULT sha256 /{print $5}')

podman kube down pod.yaml >/dev/null

echo
if [ -n "$SRC_SHA" ] && [ "$SRC_SHA" = "$DST_SHA" ]; then
  echo "==> OK: sha256 matches ($SRC_SHA)"
else
  echo "==> MISMATCH or no result: sender=$SRC_SHA receiver=$DST_SHA"
  exit 1
fi
