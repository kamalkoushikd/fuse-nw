#!/usr/bin/env bash
# Boots the rx VM and tx VM on the fuse-br0 bridge and runs one
# fuse_quickstart transfer between them over a real (virtio) UDP/IP path -
# 1500 MTU, real kernel network stack - at host-local speed. See README.md
# in this directory. Run setup-net.sh and build-guest.sh first.
#
# Usage: scripts/net-sim/run-test.sh [size-MiB] [lanes]
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./config.sh

SIZE_MB="${1:-64}"
LANES="${2:-4}"

command -v qemu-system-x86_64 >/dev/null || { echo "qemu-system-x86_64 not found (dnf install qemu-system-x86)" >&2; exit 1; }
ip link show "$TAP_RX" &>/dev/null || { echo "$TAP_RX missing - run scripts/net-sim/setup-net.sh first" >&2; exit 1; }
[ -f "$IMAGE" ] || { echo "$IMAGE missing - run scripts/net-sim/build-guest.sh first" >&2; exit 1; }

WORK="$(mktemp -d /tmp/fuse-netsim.XXXXXX)"
trap 'kill "${RXPID:-0}" "${TXPID:-0}" 2>/dev/null || true' EXIT

run_vm() {  # run_vm <tap> <mac-suffix> <log> <extra cmdline...>
  local tap="$1" mac="$2" log="$3"; shift 3
  qemu-system-x86_64 -enable-kvm -cpu host -m "$VM_MEM" -smp "$VM_SMP" \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -append "console=ttyS0 root=/dev/vda rw init=/init net.ifnames=0 biosdevname=0 quiet $*" \
    -netdev tap,id=n0,ifname="$tap",script=no,downscript=no \
    -device virtio-net-pci,netdev=n0,mac="52:54:00:00:00:$mac" \
    -drive file="$IMAGE",if=virtio,format=raw,snapshot=on \
    -display none -monitor none -serial file:"$log" -no-reboot
}

echo "==> booting rx VM ($IP_RX)"
run_vm "$TAP_RX" 12 "$WORK/rx.log" "fuserole=rx fuseip=$IP_RX fuselanes=$LANES" &
RXPID=$!

for _ in $(seq 1 100); do
  grep -q "listening on ports" "$WORK/rx.log" 2>/dev/null && break
  sleep 0.3
done
grep -q "listening on ports" "$WORK/rx.log" 2>/dev/null || echo "    (rx not confirmed ready yet, starting tx anyway)"

echo "==> booting tx VM ($IP_TX -> $IP_RX), ${SIZE_MB} MiB, $LANES lanes"
run_vm "$TAP_TX" 11 "$WORK/tx.log" "fuserole=tx fuseip=$IP_TX fusepeer=$IP_RX fusesize=$SIZE_MB fuselanes=$LANES" &
TXPID=$!

wait "$TXPID"; TXRC=$?
wait "$RXPID"; RXRC=$?

echo
echo "=== tx (sender) ==="
grep "fuse-net-sim\|^sent \|^resumed" "$WORK/tx.log"
echo "=== rx (receiver) ==="
grep "fuse-net-sim\|^received \|^resumed" "$WORK/rx.log"

SRC_SHA=$(awk '/RESULT sha256-source/{print $5}' "$WORK/tx.log")
DST_SHA=$(awk '/RESULT sha256 /{print $5}' "$WORK/rx.log")
echo
echo "    full logs: $WORK/{tx,rx}.log"
if [ -n "$SRC_SHA" ] && [ "$SRC_SHA" = "$DST_SHA" ]; then
  echo "==> OK: sha256 matches ($SRC_SHA)"
else
  echo "==> MISMATCH: sender=$SRC_SHA receiver=$DST_SHA"
  exit 1
fi
