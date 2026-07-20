# fuse

Fuse is an experimental, from-scratch transport protocol project: the
long-term goal is a UDP-based transport that improves on QUIC in some
specific dimension (e.g. handshake latency, multipath, or congestion
control), not a QUIC-compatible implementation. This repository is the
project's build-system and architecture scaffold, not a finished
protocol.

**What's actually implemented right now:**
- A QUIC-style variable-length integer codec (`fuse/varint.h`)
- A long-header packet framing format (`fuse/packet.h`)
- A Linux UDP socket wrapper (`fuse/socket.h`)
- HKDF-Extract / HKDF-Expand-Label primitives and an Initial-secret
  derivation, backed by wolfSSL (`fuse/crypto.h`) — verified against
  the published RFC 9001 test vectors in `tests/test_crypto.cpp`
- A minimal connection handle with a placeholder state machine
  (`fuse/connection.h`)

**Not yet implemented:** the actual handshake exchange, packet
protection (AEAD encrypt/decrypt), stream multiplexing, loss detection,
and congestion control. See `docs/ROADMAP.md` for the intended order of
work — start there if you're picking this up.

Both a C API (`fuse/fuse.h`) and a C++ RAII wrapper (`fuse/fuse.hpp`)
are installed, so the library is usable from either language.

## Building

Requires CMake 3.16+, a C11/C++17 compiler, and (for the crypto module)
either a system-installed wolfSSL or a network connection so it can be
fetched and built automatically.

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Useful options (pass as `-D<option>=<value>` or via a preset):

| Option | Default | Meaning |
|---|---|---|
| `BUILD_SHARED_LIBS` | `ON` | Build `libfuse` as a shared (`.so`) vs. static library |
| `FUSE_WITH_CRYPTO` | `ON` | Enable the wolfSSL-backed crypto module |
| `FUSE_FETCH_WOLFSSL` | `ON` | Fetch+build wolfSSL from source if not found on the system |
| `FUSE_BUILD_TESTS` | `ON` (top-level) | Build the GoogleTest suite |
| `FUSE_BUILD_EXAMPLES` | `ON` (top-level) | Build the example programs |

A `no-crypto` preset (`cmake --preset no-crypto`) is available for a
fast build of just the framing/socket code without pulling in wolfSSL.

### Installing system-wide

```sh
cmake --build --preset default
sudo cmake --install build/default
```

This installs `libfuse`, headers, a CMake package config (so
downstream projects can `find_package(fuse CONFIG REQUIRED)` and link
`fuse::fuse`), and a `fuse.pc` pkg-config file.

To install to a non-default location, set `-DCMAKE_INSTALL_PREFIX=...`
when you run the first `cmake` (configure) command, not as `--prefix`
to `cmake --install`: the pkg-config file's `prefix=` line is baked in
at configure time, so overriding only the install step leaves it
pointing at the old prefix.

### Bringing your own wolfSSL

If you'd rather not let the build fetch wolfSSL, install one yourself
built with the flags fuse's crypto module needs:

```sh
cmake -S . -B build -DWOLFSSL_TLS13=yes -DWOLFSSL_QUIC=yes
cmake --build build && sudo cmake --install build
```

then configure fuse with `-DFUSE_FETCH_WOLFSSL=OFF`; `find_package(wolfssl CONFIG)`
will pick it up.

### A note on licensing

wolfSSL's default license is the GPLv2 (a commercial license is also
available from wolfSSL Inc. for closed-source distribution). fuse
itself is MIT-licensed, but **a binary that statically or dynamically
links GPLv2 wolfSSL is a combined work subject to the GPL** when you
distribute it. If you need to ship closed-source software built on
fuse's crypto module, either obtain a commercial wolfSSL license or
swap in a permissively-licensed backend.

## Examples

```sh
./build/default/examples/fuse_echo_server &
./build/default/examples/fuse_echo_client
```

These exercise the socket wrapper only (`fuse_socket_*` /
`fuse::socket`), not fuse's own wire protocol — there isn't one to
speak yet.
