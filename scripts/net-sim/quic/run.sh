#!/usr/bin/env bash
# QUIC/HTTP3 counterpart to ../run.sh, same fusenet bridge, same two-pod-two-
# netns topology, same root/pkexec reasons (see ../README.md "Why root").
# Differs where the protocols differ: rx is a caddy HTTP/3 server that never
# exits on its own (a server has no "done"), so this waits only for tx (the
# client) to exit, then tears both pods down explicitly.
#
# While it's running, watch either side live in another terminal:
#   tail -f scripts/net-sim/.cache/logs/quic-rx.log
#   tail -f scripts/net-sim/.cache/logs/quic-tx.log
#
# To change payload size, edit SIZE_MB in rx-pod.yaml - config lives in
# YAML, not script flags (same convention as ../run.sh).
#
# Usage: scripts/net-sim/quic/run.sh [--force]   (--force rebuilds the image)
cd "$(dirname "${BASH_SOURCE[0]}")"
set -euo pipefail

QUIC="$(pwd)"
NETSIM="$(cd .. && pwd)"
FORCE_BUILD=0
[ "${1:-}" = "--force" ] && FORCE_BUILD=1

command -v podman >/dev/null || { echo "podman not found" >&2; exit 1; }

mkdir -p "$NETSIM/.cache/logs"
rm -f "$NETSIM/.cache/logs/quic-rx.log" "$NETSIM/.cache/logs/quic-tx.log"

echo "==> bringing up fusenet + quic-rx/quic-tx pods (needs root once)"
echo "    tail -f $NETSIM/.cache/logs/quic-{rx,tx}.log in another terminal to watch it live"
pkexec bash -c "
  set -euo pipefail
  if [ '$FORCE_BUILD' = 1 ] || ! podman image exists fuse-netsim-quic; then
    podman build -t fuse-netsim-quic '$QUIC'
  fi
  podman network exists fusenet || podman network create fusenet >/dev/null

  podman pod exists quic-rx && podman kube down '$QUIC/rx-pod.yaml' >/dev/null
  podman pod exists quic-tx && podman kube down '$QUIC/tx-pod.yaml' >/dev/null

  podman kube play --network fusenet '$QUIC/rx-pod.yaml' >/dev/null

  RX_IP=\$(podman inspect quic-rx-app --format '{{.NetworkSettings.Networks.fusenet.IPAddress}}')
  [ -n \"\$RX_IP\" ] || { echo 'could not read quic-rx-app IP on fusenet' >&2; exit 1; }
  echo \"    quic-rx is at \$RX_IP\"
  sed \"s/@RX_IP@/\$RX_IP/\" '$QUIC/tx-pod.yaml' > /tmp/fuse-netsim-quic-tx-pod.yaml
  podman kube play --network fusenet /tmp/fuse-netsim-quic-tx-pod.yaml >/dev/null
  rm -f /tmp/fuse-netsim-quic-tx-pod.yaml

  echo '    waiting for quic-tx to finish...'
  for _ in \$(seq 1 900); do
    state=\$(podman inspect quic-tx-app --format '{{.State.Status}}' 2>/dev/null)
    [ \"\$state\" = exited ] && break
    sleep 1
  done

  podman kube down '$QUIC/rx-pod.yaml' '$QUIC/tx-pod.yaml' >/dev/null
  podman network rm fusenet >/dev/null
  chown -R $(id -u):$(id -g) '$NETSIM/.cache/logs'
"

echo
echo "=== quic-tx (client) ==="
grep -iE "quic-net-sim|error|denied|failed|refused|traceback|panic" "$NETSIM/.cache/logs/quic-tx.log" || true
echo "=== quic-rx (server) ==="
grep -iE "quic-net-sim|error|denied|failed|traceback|panic" "$NETSIM/.cache/logs/quic-rx.log" || true

SRC_SHA=$(awk '/RESULT sha256-source/{print $5}' "$NETSIM/.cache/logs/quic-rx.log")
DST_SHA=$(awk '/RESULT sha256 /{print $5}' "$NETSIM/.cache/logs/quic-tx.log")
echo
echo "    full logs: $NETSIM/.cache/logs/quic-{rx,tx}.log (persists after teardown)"
if [ -n "$SRC_SHA" ] && [ "$SRC_SHA" = "$DST_SHA" ]; then
  echo "==> OK: sha256 matches ($SRC_SHA)"
else
  echo "==> MISMATCH: source=$SRC_SHA downloaded=$DST_SHA"
  exit 1
fi
