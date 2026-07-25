#!/usr/bin/env bash
#
# Builds a self-contained, relocatable Fuse SDK tarball for a GitHub release.
#
#   scripts/make_release.sh [version]
#
# Produces  dist/fuse-sdk-<version>-linux-<arch>.tar.gz  plus a .sha256, and
# copies scripts/install.sh next to them so the release page carries both.
#
# "Self-contained" means the tarball ships libwolfssl alongside libfuse_proto
# and the libraries carry an $ORIGIN rpath, so they find each other wherever
# the user unpacks them — no system wolfSSL required.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

VERSION="${1:-$(sed -n 's/^ *VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt | head -1)}"
ARCH="$(uname -m)"
NAME="fuse-sdk-${VERSION}-linux-${ARCH}"
BUILD="build/release-pkg"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo ">> building Fuse ${VERSION} for ${ARCH}"

# Staging prefix is "/" because the tarball is relocatable: install.sh
# unpacks it under whatever prefix the user chose and fixes up fuse.pc.
cmake -S . -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DFUSE_BUILD_TESTS=OFF \
    -DFUSE_BUILD_EXAMPLES=ON \
    -DFUSE_BUILD_BENCH=OFF >/dev/null

cmake --build "$BUILD" -j"$(nproc)" >/dev/null
DESTDIR="$STAGE" cmake --install "$BUILD" >/dev/null

# NOTE: libdir is pinned to lib/ at configure time above rather than renamed
# here. Both fuse.pc and the generated CMake target files bake the library
# directory in, so moving it after the fact silently breaks -lfuse_proto and
# find_package for everyone who installs the tarball.

# Ship the Python package inside the tarball so one download covers both
# languages.
mkdir -p "$STAGE/share/fuse/python"
cp -a python/fuse "$STAGE/share/fuse/python/"
find "$STAGE/share/fuse/python" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true

# A couple of runnable binaries are genuinely useful for smoke-testing an
# install; drop the rest of the build's examples.
mkdir -p "$STAGE/bin"
for tool in fuse_quickstart_send fuse_quickstart_recv; do
    [ -f "$BUILD/examples/$tool" ] && install -m755 "$BUILD/examples/$tool" "$STAGE/bin/"
done

# Docs worth carrying with the binaries.
mkdir -p "$STAGE/share/doc/fuse"
for f in README.md LICENSE docs/USAGE.md docs/SDK.md bench/RESULTS.md; do
    [ -f "$f" ] && cp "$f" "$STAGE/share/doc/fuse/$(basename "$f")"
done

cat > "$STAGE/share/doc/fuse/MANIFEST.txt" <<EOF
Fuse SDK ${VERSION} (linux-${ARCH})
built $(date -u +%Y-%m-%dT%H:%M:%SZ)

lib/                 shared libraries (libfuse_proto, libfuse, libwolfssl)
lib/pkgconfig/       fuse.pc for non-CMake builds
lib/cmake/fuse/      find_package(fuse CONFIG) support
include/fuse/        C and C++ headers (sdk.h is the socket-style API)
share/fuse/python/   the 'fuse' Python package (pure ctypes, no build step)
bin/                 quickstart send/receive tools
EOF

mkdir -p dist
tar -C "$STAGE" -czf "dist/${NAME}.tar.gz" .
( cd dist && sha256sum "${NAME}.tar.gz" > "${NAME}.tar.gz.sha256" )
cp scripts/install.sh dist/install.sh

echo
echo ">> dist/${NAME}.tar.gz"
echo "   $(du -h "dist/${NAME}.tar.gz" | cut -f1)  $(cat "dist/${NAME}.tar.gz.sha256" | cut -c1-16)..."
echo ">> dist/install.sh"
echo
echo "Attach both (plus the .sha256) to the GitHub release."
