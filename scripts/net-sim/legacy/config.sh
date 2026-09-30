#!/usr/bin/env bash
# Shared config for the KVM network-simulation harness. Two minimal VMs on a
# private Linux bridge, joined by tap devices: a real UDP/IP stack and 1500
# MTU (unlike loopback, which the README notes lets fuse adapt into a 16 KiB
# block a real path never would) but still host-local speed, no physical NIC
# in the loop.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CACHE="$REPO/scripts/net-sim/.cache"
ROOTFS="$CACHE/rootfs"
IMAGE="$CACHE/guest.img"
INITRD="$CACHE/initramfs-guest.img"
KERNEL="/boot/vmlinuz-$(uname -r)"

BRIDGE="fusebr0"
TAP_RX="fusetap0"
TAP_TX="fusetap1"

NET="10.77.7"
IP_HOST="$NET.1"
IP_RX="$NET.12"
IP_TX="$NET.11"
PREFIX=24

IMAGE_SIZE_MB="${IMAGE_SIZE_MB:-768}"
VM_MEM="${VM_MEM:-1024}"
VM_SMP="${VM_SMP:-4}"
