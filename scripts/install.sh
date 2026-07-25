#!/usr/bin/env sh
#
# Fuse SDK installer.
#
#   curl -fsSL https://github.com/OWNER/REPO/releases/latest/download/install.sh | sh
#
# Options (as flags, or the matching environment variable):
#   --prefix DIR     where to install        (FUSE_PREFIX)
#   --version X.Y.Z  release to fetch        (FUSE_VERSION, default: latest)
#   --tarball FILE   install a local tarball instead of downloading
#   --repo OWNER/REPO
#   --no-python      skip installing the Python package
#   --uninstall      remove a previous install
#
# With no --prefix, this installs to /usr/local when run as root and to
# ~/.local otherwise — so it never needs sudo to be useful.

set -eu

REPO="${FUSE_REPO:-kamalkoushikd/fuse-nw}"
VERSION="${FUSE_VERSION:-latest}"
PREFIX="${FUSE_PREFIX:-}"
TARBALL=""
WITH_PYTHON=1
UNINSTALL=0

say()  { printf '%s\n' "$*"; }
warn() { printf '\033[33m!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\033[32m==>\033[0m %s\n' "$*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)    PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
        --version)   VERSION="${2:?--version needs a value}"; shift 2 ;;
        --tarball)   TARBALL="${2:?--tarball needs a path}"; shift 2 ;;
        --repo)      REPO="${2:?--repo needs OWNER/REPO}"; shift 2 ;;
        --no-python) WITH_PYTHON=0; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help)   sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           die "unknown option: $1 (try --help)" ;;
    esac
done

# --- environment checks ----------------------------------------------------

[ "$(uname -s)" = "Linux" ] || die "Fuse requires Linux (uses recvmmsg and UDP GSO); found $(uname -s)"

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|aarch64) ;;
    *) die "no prebuilt release for $ARCH — build from source: https://github.com/$REPO" ;;
esac

if [ -z "$PREFIX" ]; then
    if [ "$(id -u)" = "0" ]; then PREFIX="/usr/local"; else PREFIX="$HOME/.local"; fi
fi

# --- uninstall -------------------------------------------------------------

if [ "$UNINSTALL" = "1" ]; then
    step "removing Fuse from $PREFIX"
    rm -f  "$PREFIX"/lib/libfuse_proto.so* "$PREFIX"/lib/libfuse.so* \
           "$PREFIX"/lib/libwolfssl.so* "$PREFIX"/lib/pkgconfig/fuse.pc \
           "$PREFIX"/bin/fuse_quickstart_send "$PREFIX"/bin/fuse_quickstart_recv
    rm -rf "$PREFIX"/include/fuse "$PREFIX"/lib/cmake/fuse \
           "$PREFIX"/share/fuse "$PREFIX"/share/doc/fuse
    say "done. (A pip-installed 'fuse' package, if any, needs 'pip uninstall fuse'.)"
    exit 0
fi

# --- fetch -----------------------------------------------------------------

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

if [ -n "$TARBALL" ]; then
    [ -f "$TARBALL" ] || die "no such tarball: $TARBALL"
    step "using local tarball $TARBALL"
    cp "$TARBALL" "$TMP/fuse.tar.gz"
else
    if [ "$VERSION" = "latest" ]; then
        BASE="https://github.com/$REPO/releases/latest/download"
    else
        BASE="https://github.com/$REPO/releases/download/v${VERSION#v}"
    fi

    # The asset name embeds the version, which we do not know for "latest".
    # GitHub redirects a versionless name only if one was uploaded, so try the
    # stable alias first and fall back to querying the API.
    ASSET="fuse-sdk-linux-${ARCH}.tar.gz"
    URL="$BASE/$ASSET"

    fetch() {
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL "$1" -o "$2"
        elif command -v wget >/dev/null 2>&1; then
            wget -qO "$2" "$1"
        else
            die "need curl or wget to download"
        fi
    }

    step "downloading $ASSET"
    if ! fetch "$URL" "$TMP/fuse.tar.gz" 2>/dev/null; then
        # Fall back to resolving the exact asset name via the releases API.
        API="https://api.github.com/repos/$REPO/releases/${VERSION:+tags/v${VERSION#v}}"
        [ "$VERSION" = "latest" ] && API="https://api.github.com/repos/$REPO/releases/latest"
        fetch "$API" "$TMP/rel.json" || die "cannot reach GitHub for $REPO"
        URL="$(grep -o '"browser_download_url": *"[^"]*fuse-sdk[^"]*linux-'"$ARCH"'\.tar\.gz"' \
               "$TMP/rel.json" | head -1 | cut -d'"' -f4)"
        [ -n "$URL" ] || die "no linux-$ARCH asset in release '$VERSION' of $REPO"
        fetch "$URL" "$TMP/fuse.tar.gz" || die "download failed: $URL"
    fi

    # Verify the checksum when the release publishes one.
    if fetch "${URL}.sha256" "$TMP/sum" 2>/dev/null; then
        EXPECT="$(cut -d' ' -f1 "$TMP/sum")"
        ACTUAL="$(sha256sum "$TMP/fuse.tar.gz" | cut -d' ' -f1)"
        [ "$EXPECT" = "$ACTUAL" ] || die "checksum mismatch — refusing to install"
        say "    checksum ok"
    else
        warn "release published no .sha256; skipping checksum verification"
    fi
fi

# --- install ---------------------------------------------------------------

step "installing to $PREFIX"
mkdir -p "$PREFIX" || die "cannot create $PREFIX (try --prefix ~/.local, or run with sudo)"
[ -w "$PREFIX" ] || die "$PREFIX is not writable (try --prefix ~/.local, or run with sudo)"

tar -xzf "$TMP/fuse.tar.gz" -C "$PREFIX"

# The tarball is relocatable, but every pkg-config file carries an absolute
# prefix, so rewrite them all — including the bundled wolfssl.pc, which fuse.pc
# pulls in via Requires.private and which would otherwise contribute a bogus
# "-I/include" to every compile. (The CMake package files compute their prefix
# relative to their own location and need no fixing.)
for PC in "$PREFIX"/lib/pkgconfig/*.pc "$PREFIX"/lib64/pkgconfig/*.pc; do
    [ -f "$PC" ] || continue
    sed -i "s|^prefix=.*|prefix=$PREFIX|" "$PC"
done

# Refresh the loader cache for a system prefix; for a user prefix, tell the
# user how to make the libraries findable.
if [ "$(id -u)" = "0" ] && command -v ldconfig >/dev/null 2>&1; then
    ldconfig 2>/dev/null || true
fi

# --- python ----------------------------------------------------------------

PY_NOTE=""
if [ "$WITH_PYTHON" = "1" ] && [ -d "$PREFIX/share/fuse/python/fuse" ]; then
    if command -v python3 >/dev/null 2>&1; then
        SITE="$(python3 -c 'import site,sys; print((site.getusersitepackages() if hasattr(site,"getusersitepackages") else ""))' 2>/dev/null || true)"
        if [ "$(id -u)" = "0" ]; then
            SITE="$(python3 -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])' 2>/dev/null || true)"
        fi
        if [ -n "$SITE" ]; then
            mkdir -p "$SITE"
            rm -rf "$SITE/fuse"
            cp -a "$PREFIX/share/fuse/python/fuse" "$SITE/fuse"
            PY_NOTE="python: import fuse   (installed to $SITE)"
        fi
    fi
    [ -n "$PY_NOTE" ] || PY_NOTE="python: add $PREFIX/share/fuse/python to PYTHONPATH"
fi

# --- report ----------------------------------------------------------------

say ""
say "Fuse SDK installed."
say ""
say "  C / C++ :  #include <fuse/sdk.h>"
say "             cc app.c \$(pkg-config --cflags --libs fuse)"
say "  CMake   :  find_package(fuse CONFIG REQUIRED)"
say "             target_link_libraries(app PRIVATE fuse::proto)"
[ -n "$PY_NOTE" ] && say "  $PY_NOTE"
say ""
say "  smoke test:"
say "    $PREFIX/bin/fuse_quickstart_recv 4433 /tmp/out.bin &"
say "    $PREFIX/bin/fuse_quickstart_send 127.0.0.1 4433 /etc/hostname"
say ""

if [ "$PREFIX" != "/usr" ] && [ "$PREFIX" != "/usr/local" ]; then
    say "  Since you installed to a non-standard prefix, add these to your shell:"
    say "    export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:\$PKG_CONFIG_PATH"
    say "    export PATH=$PREFIX/bin:\$PATH"
    say ""
    say "  (The libraries carry an rpath, so LD_LIBRARY_PATH is not needed.)"
    say ""
fi
