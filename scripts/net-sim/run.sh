#!/usr/bin/env bash
# One-shot: stage fuse binaries, build the runner image, bring up the fusenet
# bridge, play rx then tx as two separate pods (two separate network
# namespaces) on it, wait for both to finish, print the result, tear
# everything down again. See README.md.
#
# While it's running, watch either side live in another terminal (no root
# needed - see the `logs` volume in rx-pod.yaml/tx-pod.yaml):
#   tail -f scripts/net-sim/.cache/logs/rx.log
#   tail -f scripts/net-sim/.cache/logs/tx.log
#
# To change transfer size/lanes, edit the env values in rx-pod.yaml /
# tx-pod.yaml directly - net-sim's config lives in YAML, not script flags.
#
# Usage: scripts/net-sim/run.sh [--force]   (--force rebuilds the image)
#
# Needs root for the network/pod bring-up itself: rootless podman's default
# (pasta) networking fails in this environment even for a plain bridge
# network (`Failed to remount /: Permission denied`) - see README.md. Root
# podman sidesteps it (real netavark bridge, no pasta) the same way
# setup-net.sh used to need root for host bridge/tap devices. Batched into
# one pkexec call (one graphical password prompt) since there's no
# controlling tty here for plain `sudo` to prompt on.
cd "$(dirname "${BASH_SOURCE[0]}")"
set -euo pipefail

NETSIM="$(pwd)"
FORCE_BUILD=0
[ "${1:-}" = "--force" ] && FORCE_BUILD=1

command -v podman >/dev/null || { echo "podman not found" >&2; exit 1; }

./stage-bin.sh

mkdir -p .cache/logs
rm -f .cache/logs/rx.log .cache/logs/tx.log

echo "==> bringing up fusenet + rx/tx pods (needs root once)"
echo "    tail -f $NETSIM/.cache/logs/{rx,tx}.log in another terminal to watch it live"
pkexec bash -c "
  set -euo pipefail
  # Always hand the logs back to the invoking user, even if something below
  # fails under set -e (e.g. a teardown step erroring would otherwise skip
  # the chown that was the last line here - happened for real once).
  trap 'chown -R $(id -u):$(id -g) \"$NETSIM/.cache/logs\" 2>/dev/null || true' EXIT
  if [ '$FORCE_BUILD' = 1 ] || ! podman image exists fuse-netsim; then
    podman build -t fuse-netsim '$NETSIM'
  fi
  podman network exists fusenet || podman network create fusenet >/dev/null

  podman pod exists rx && podman kube down '$NETSIM/rx-pod.yaml' >/dev/null
  podman pod exists tx && podman kube down '$NETSIM/tx-pod.yaml' >/dev/null

  podman kube play --network fusenet '$NETSIM/rx-pod.yaml' >/dev/null
  echo '    waiting for rx to start listening...'
  for _ in \$(seq 1 100); do
    grep -q 'listening on' '$NETSIM/.cache/logs/rx.log' 2>/dev/null && break
    sleep 0.3
  done

  RX_IP=\$(podman inspect rx-app --format '{{.NetworkSettings.Networks.fusenet.IPAddress}}')
  [ -n \"\$RX_IP\" ] || { echo 'could not read rx-app IP on fusenet' >&2; exit 1; }
  echo \"    rx is at \$RX_IP\"
  sed \"s/@RX_IP@/\$RX_IP/\" '$NETSIM/tx-pod.yaml' > /tmp/fuse-netsim-tx-pod.yaml
  podman kube play --network fusenet /tmp/fuse-netsim-tx-pod.yaml >/dev/null
  rm -f /tmp/fuse-netsim-tx-pod.yaml
  echo '    waiting for tx/rx to finish...'
  for _ in \$(seq 1 900); do
    state=\$(podman inspect rx-app tx-app --format '{{.State.Status}}' 2>/dev/null | sort -u)
    [ \"\$state\" = exited ] && break
    sleep 1
  done

  podman kube down '$NETSIM/rx-pod.yaml' '$NETSIM/tx-pod.yaml' >/dev/null
  podman network rm fusenet >/dev/null
"

echo
echo "=== tx (sender) ==="
grep -iE "fuse-net-sim|^sent |^resumed|error|denied|failed|traceback|panic" .cache/logs/tx.log || true
echo "=== rx (receiver) ==="
grep -iE "fuse-net-sim|^received |^resumed|error|denied|failed|traceback|panic" .cache/logs/rx.log || true

SRC_SHA=$(awk '/RESULT sha256-source/{print $5}' .cache/logs/tx.log)
DST_SHA=$(awk '/RESULT sha256 /{print $5}' .cache/logs/rx.log)
echo
echo "    full logs: $NETSIM/.cache/logs/{tx,rx}.log (persists after teardown)"
if [ -n "$SRC_SHA" ] && [ "$SRC_SHA" = "$DST_SHA" ]; then
  echo "==> OK: sha256 matches ($SRC_SHA)"
else
  echo "==> MISMATCH: sender=$SRC_SHA receiver=$DST_SHA"
  exit 1
fi
