# Using the Fuse protocol core in your project

This is the wire protocol (`fuse::proto`): framing, congestion control, the
handshake, and the worker/registry machinery everything else is built on.
Most applications want the ergonomic SDK instead, built on top of this and
published from its own repo: see
[fuse-sdk's USAGE.md](https://github.com/kamalkoushikd/fuse-sdk/blob/main/docs/USAGE.md).

If you're building your own layer directly on the protocol, or doing
research or benchmarking against it, keep reading.

## 1. Install

### From a package

```sh
cmake --preset default
cmake --build --preset default
cd build/default && cpack          # produces .deb / .rpm / .tar.gz
sudo dpkg -i fuse-0.1.0-Linux.deb  # or: sudo rpm -i fuse-0.1.0-1.x86_64.rpm
```

### From source

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j
sudo cmake --install build
sudo ldconfig                      # only needed for a system prefix
```

Set `CMAKE_INSTALL_PREFIX` at **configure** time, not on `cmake --install`:
the pkg-config file's `prefix=` is resolved when the install runs, and the
libraries' rpath is baked at build time.

> Requires CMake 3.16+, a C++17 compiler, and Linux (the transport uses
> `sendmmsg`/`recvmmsg` and UDP GSO). wolfSSL is fetched and built
> automatically unless you already have one installed.

## 2. Use it, CMake

```cmake
find_package(fuse CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE fuse::proto)
```

If you installed to a non-standard prefix, point CMake at it:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/fuse
```

## 3. Use it, pkg-config (no CMake)

```sh
g++ -std=c++17 main.cpp -o my_app $(pkg-config --cflags --libs fuse)
```

That is sufficient even for a non-system prefix: the `.pc` file carries an
rpath, so the resulting binary runs without `LD_LIBRARY_PATH`.

## 4. The pieces

`fuse::proto` is deliberately low-level. The ergonomic send/recv API
(`fuse/sdk.h`) and the one-call buffer/file transfer API (`fuse/transfer.hpp`)
that most applications actually want are built from these same pieces, in
the [fuse-sdk](https://github.com/kamalkoushikd/fuse-sdk) repo.

| header | what it gives you |
|---|---|
| `fuse/proto/udp.hpp` | UDP socket with GSO batching and `recvmmsg` |
| `fuse/proto/block.hpp` | block header encode/decode |
| `fuse/proto/registry.hpp` | sender-side retransmit registry |
| `fuse/proto/receiver.hpp` | receive window, O(1) loss detection |
| `fuse/proto/congestion.hpp` | per-stream AIMD controller |
| `fuse/proto/session_crypto.hpp` | session keys + per-lane AEAD |
| `fuse/proto/orchestrator.hpp` | autoscaling worker pool |
| `fuse/proto/setup.hpp` | SETUP handshake |

## 5. Runnable examples

The Stage 1 demos exercise the protocol directly, with no handshake yet
(that's a later stage) — a good starting point for seeing the framing and
loss-recovery machinery work without the SDK layer on top:

```sh
./build/default/examples/fuse_stage1_receiver 48000 &
./build/default/examples/fuse_stage1_sender 127.0.0.1 48000 --drop 3 --count 8
```

Measured performance and its caveats: [`../bench/RESULTS.md`](../bench/RESULTS.md).
