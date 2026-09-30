#!/usr/bin/env bash
# Stages the host-built fuse binaries + their non-system shared libs into
# .cache/payload/{bin,lib}, bind-mounted read-only into both pods at
# /opt/fuse (see rx-pod.yaml/tx-pod.yaml). Rerun after rebuilding fuse
# (cmake --build --preset default) to refresh it. No root needed.
cd "$(dirname "${BASH_SOURCE[0]}")"
set -euo pipefail

REPO="$(cd ../.. && pwd)"
PAYLOAD="$(pwd)/.cache/payload"

BIN_DIR="$(dirname "$(find "$REPO" -maxdepth 4 -type f -name fuse_quickstart_send -newer "$REPO/CMakeLists.txt" 2>/dev/null | head -1)")"
[ -n "$BIN_DIR" ] && [ -d "$BIN_DIR" ] || BIN_DIR="$(dirname "$(find "$REPO" -maxdepth 4 -type f -name fuse_quickstart_send 2>/dev/null | head -1)")"
[ -d "$BIN_DIR" ] || { echo "fuse_quickstart_send not found under $REPO - build it first (cmake --build --preset default)" >&2; exit 1; }
echo "==> staging binaries from $BIN_DIR"

rm -rf "$PAYLOAD"
mkdir -p "$PAYLOAD/bin" "$PAYLOAD/lib"
cp "$BIN_DIR/fuse_quickstart_send" "$BIN_DIR/fuse_quickstart_recv" "$PAYLOAD/bin/"
for lib in $(ldd "$BIN_DIR/fuse_quickstart_send" | awk '/=>/{print $3} /^[ \t]*\/.*\.so/{print $1}' | grep -v '^$'); do
  case "$lib" in
    /lib64/*|/usr/lib64/*) continue ;;  # already provided by the image's glibc/libstdc++ packages
  esac
  cp -Lv "$lib" "$PAYLOAD/lib/"
done

# Fedora's enforcing SELinux blocks container_t from *executing* content off
# a host bind-mount even after kube play's auto "z" relabel (that only
# covers read/write, not exec) - confirmed via `stdbuf: failed to run
# command ...: Permission denied` inside the pod. container_file_t is
# podman's documented label for exactly this, narrower than the old VM
# harness's spc_t (which disables SELinux confinement for the whole
# container - overkill for just running a bind-mounted binary). No root
# needed to label your own files; no-op on non-SELinux systems.
if command -v chcon >/dev/null && command -v selinuxenabled >/dev/null && selinuxenabled; then
  chcon -t container_file_t -R "$PAYLOAD"
fi

echo "==> done -> $PAYLOAD"
