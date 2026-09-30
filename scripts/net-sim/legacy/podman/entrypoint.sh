#!/usr/bin/env bash
# Runs inside one container of the fuse-netsim pod: joins this container's
# tap to the bridge the net-setup initContainer already created (pod
# containers share one network namespace, so it's still there), then boots
# one VM. Role/IP/peer/size/lanes come in as env vars from pod.yaml.
set -euo pipefail

BRIDGE=fusebr0
TAP="fusetap-${FUSE_ROLE}"

ip link show "$BRIDGE" &>/dev/null || { echo "bridge $BRIDGE missing - did the net-setup initContainer run?" >&2; exit 1; }
ip tuntap add dev "$TAP" mode tap
ip link set "$TAP" master "$BRIDGE"
ip link set "$TAP" up

case "$FUSE_ROLE" in
  rx) MAC=52:54:00:00:00:12 ;;
  tx) MAC=52:54:00:00:00:11 ;;
  *) echo "unknown FUSE_ROLE=$FUSE_ROLE" >&2; exit 1 ;;
esac

# uname -r reports the *host's* kernel inside a container (no UTS faking
# here), so this matches whatever ../build-guest.sh generated the initramfs
# for on the host, with no separate version handshake needed.
KERNEL="/boot/vmlinuz-$(uname -r)"
LOG=/tmp/console.log

qemu-system-x86_64 -enable-kvm -cpu host -m "${VM_MEM:-1024}" -smp "${VM_SMP:-4}" \
  -kernel "$KERNEL" -initrd /cache/initramfs-guest.img \
  -append "console=ttyS0 root=/dev/vda rw init=/init net.ifnames=0 biosdevname=0 quiet fuserole=$FUSE_ROLE fuseip=$FUSE_IP fusepeer=${FUSE_PEER:-} fusesize=${FUSE_SIZE_MB:-64} fuselanes=${FUSE_LANES:-4}" \
  -netdev tap,id=n0,ifname="$TAP",script=no,downscript=no \
  -device virtio-net-pci,netdev=n0,mac="$MAC" \
  -drive file=/cache/guest.img,if=virtio,format=raw,snapshot=on \
  -display none -monitor none -serial file:"$LOG" -no-reboot

cat "$LOG"
