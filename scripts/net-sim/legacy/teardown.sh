#!/usr/bin/env bash
# Removes the bridge + tap devices setup-net.sh created. Safe to rerun.
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./config.sh

CMD=""
for dev in "$TAP_RX" "$TAP_TX" "$BRIDGE"; do
  ip link show "$dev" &>/dev/null && CMD="$CMD
    ip link del '$dev'"
done

if [ -n "$CMD" ]; then
  echo "==> removing $BRIDGE / $TAP_RX / $TAP_TX"
  pkexec bash -c "set -e$CMD"
else
  echo "==> nothing to remove"
fi
