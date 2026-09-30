#!/usr/bin/env bash
# Create the bridge + two tap devices the VMs plug into. Idempotent - safe to
# rerun. Needs root for the `ip` calls (asks via sudo); the taps are created
# owned by the invoking user so later qemu-system-x86_64 runs don't need root.
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./config.sh

USER_NAME="$(id -un)"
# No controlling tty under the agent harness, so plain `sudo` can't prompt -
# go through pkexec (polkit's graphical agent) instead, and batch every
# privileged step into one call so it only prompts once.

CMD=""
if [ ! -e /dev/net/tun ]; then
  CMD="$CMD
    modprobe tun"
fi
if ! ip link show "$BRIDGE" &>/dev/null; then
  CMD="$CMD
    ip link add name '$BRIDGE' type bridge
    ip addr add '$IP_HOST/$PREFIX' dev '$BRIDGE'
    ip link set '$BRIDGE' up"
fi
for tap in "$TAP_RX" "$TAP_TX"; do
  if ! ip link show "$tap" &>/dev/null; then
    CMD="$CMD
    ip tuntap add dev '$tap' mode tap user '$USER_NAME'
    ip link set '$tap' master '$BRIDGE'
    ip link set '$tap' up"
  fi
done

if [ -n "$CMD" ]; then
  echo "==> setting up $BRIDGE / $TAP_RX / $TAP_TX (missing pieces only)"
  pkexec bash -c "set -e$CMD"
else
  echo "==> $BRIDGE, $TAP_RX, $TAP_TX already exist, leaving them alone"
fi

echo "==> $BRIDGE is up: $(ip -br addr show "$BRIDGE")"
echo "==> run scripts/net-sim/teardown.sh to remove all of this again"
