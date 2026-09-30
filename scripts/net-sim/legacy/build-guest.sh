#!/usr/bin/env bash
# Build the (cached, reusable) guest artifacts: a generic initramfs with the
# virtio_net module dracut's hostonly mode would otherwise strip on a
# bare-metal host, a minimal Fedora rootfs (busybox + glibc + libstdc++),
# fuse's own binaries copied in, and an ext4 disk image built straight from
# that rootfs directory - no loop-mount, no root needed for this part.
#
# Usage: scripts/net-sim/build-guest.sh [--force]
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./config.sh

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

mkdir -p "$CACHE"

if [ ! -f "$INITRD" ] || [ "$FORCE" = 1 ]; then
  echo "==> building generic initramfs (adds virtio_net back in) -> $INITRD"
  dracut --no-hostonly \
    -o "iscsi nfs nfs4 fcoe fcoe-uefi lvmmerge multipath cifs biosdevname" \
    --force "$INITRD" "$(uname -r)"
else
  echo "==> initramfs cached at $INITRD (--force to rebuild)"
fi

if [ ! -d "$ROOTFS" ] || [ "$FORCE" = 1 ]; then
  echo "==> building rootfs via dnf --installroot -> $ROOTFS"
  # RPM scriptlets (ldconfig, chown to root:root, etc.) need real root even
  # when installing into an alternate --installroot. No controlling tty under
  # the agent harness, so go through pkexec (polkit's graphical agent) and
  # batch the whole privileged part into one call (one prompt).
  pkexec bash -c "
    set -e
    rm -rf '$ROOTFS'
    mkdir -p '$ROOTFS'
    dnf --installroot='$ROOTFS' --releasever='$(rpm -E %fedora)' --use-host-config \
      --setopt=install_weak_deps=False -y install glibc libstdc++ libgcc busybox
    chown -R '$(id -u)':'$(id -g)' '$ROOTFS'
  "
  chmod u+w "$ROOTFS"  # the filesystem package ships / as 555; we own it, just need +w back
  rm -f "$ROOTFS/etc/shadow" "$ROOTFS/etc/gshadow"  # mode 000, unreadable even by their new owner; no logins happen in this image anyway
else
  echo "==> rootfs cached at $ROOTFS (--force to rebuild)"
fi

echo "==> installing fuse binaries + libs into rootfs"
BIN_DIR="$(dirname "$(find "$REPO" -maxdepth 4 -type f -name fuse_quickstart_send -newer "$REPO/CMakeLists.txt" 2>/dev/null | head -1)")"
[ -n "$BIN_DIR" ] || BIN_DIR="$(dirname "$(find "$REPO" -maxdepth 4 -type f -name fuse_quickstart_send 2>/dev/null | head -1)")"
[ -d "$BIN_DIR" ] || { echo "fuse_quickstart_send not found under $REPO - build it first (cmake --build --preset default)" >&2; exit 1; }
echo "    using binaries from $BIN_DIR"

mkdir -p "$ROOTFS/opt/fuse/bin" "$ROOTFS/opt/fuse/lib"
cp "$BIN_DIR/fuse_quickstart_send" "$BIN_DIR/fuse_quickstart_recv" "$ROOTFS/opt/fuse/bin/"
for lib in $(ldd "$BIN_DIR/fuse_quickstart_send" | awk '/=>/{print $3} /^[ \t]*\/.*\.so/{print $1}' | grep -v '^$'); do
  case "$lib" in
    /lib64/*|/usr/lib64/*) continue ;;  # already provided by the glibc/libstdc++ packages in $ROOTFS
  esac
  cp -Lv "$lib" "$ROOTFS/opt/fuse/lib/"
done

BB="$ROOTFS/usr/sbin/busybox"
[ -x "$BB" ] || BB="$ROOTFS/sbin/busybox"
[ -x "$BB" ] || BB="$ROOTFS/bin/busybox"
[ -x "$BB" ] || { echo "busybox binary not found in rootfs after install" >&2; exit 1; }
BB_PATH="${BB#"$ROOTFS"}"
echo "    busybox at $BB_PATH"

install -m 0755 guest-init.sh "$ROOTFS/init"
sed -i "s#@BUSYBOX@#$BB_PATH#" "$ROOTFS/init"

echo "==> writing ext4 image ($IMAGE_SIZE_MB MiB) -> $IMAGE"
rm -f "$IMAGE"
mkfs.ext4 -q -F -d "$ROOTFS" "$IMAGE" "${IMAGE_SIZE_MB}M"

echo "==> done. kernel=$KERNEL initrd=$INITRD image=$IMAGE"
