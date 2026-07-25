# fuse

Fuse is a general-purpose transport protocol built directly on UDP
datagrams rather than adopting TCP or QUIC. It targets workloads that mix
**loss-tolerant** streams (telemetry-like, latency-sensitive) and
**loss-intolerant** streams (bulk transfer, correctness-sensitive) over a
single session, letting each stream pick its own tradeoffs.

Nothing in the core protocol assumes a particular workload, hardware
topology, or application domain.

**What distinguishes it from QUIC:**
- **Per-stream congestion control**, not connection-wide. Sustained loss
  on one lossless stream doesn't throttle an unrelated telemetry stream.
- **Per-stream loss/ordering/coalescing policy** chosen by the
  application at handshake time, rather than a fixed reliable-ordered
  model.
- **Encryption is optional and off by default** (QUIC mandates it), for
  deployments on already-trusted links — but it fails closed when
  required, and is never negotiated in-band.
- **No allocation on the data path** and no locks in the worker hot path.

**Implemented and tested** (120 tests; see `docs/ROADMAP.md` for detail and
the honest list of gaps): a one-call `fuse/transfer.hpp` send/receive API
over a sharded, GSO-batched data plane with registry-backed retransmission,
O(1) bitmask loss detection, RTT-adaptive timeouts, per-stream AIMD
congestion control, adaptive block sizing, optional AES-256-GCM encryption,
and a userspace network-emulator test harness.

> **Measured against QUIC on a 1 GiB checksum-verified file transfer, Fuse
> is 1.3–2.2× faster** than a reference QUIC implementation (quinn) at
> matched shard counts — and **1.3–1.8× faster with encryption enabled on
> both sides** (1093 vs 710 MB/s single-lane, 3560 vs 1947 at four).
>
> **Under load, Fuse's latency does not move.** With 6.3 GB/s of bulk
> traffic in flight, small-message p99 latency goes 39.6 → 38.2 µs — no
> measurable inflation. Multiplexing the same two workloads onto a single
> QUIC connection inflates p99 by 82× (153 → 12,585 µs). QUIC recovers with
> a *second* connection (p99 82 µs); Fuse gets that isolation inside one
> session, which is precisely the per-stream-congestion-control claim.
>
> **Tested under emulated network conditions** (loss, delay, jitter,
> reordering, duplication via a userspace `tc netem` substitute), every
> transfer arrived byte-identical — for both Fuse and QUIC. Fuse tolerates
> reordering/jitter far better (~40 vs ~1 MB/s); under heavy 5% loss it is
> faster on average but higher-variance than QUIC. This testing also found
> and fixed a real bug: a fixed 5 ms retransmit timeout that collapsed
> throughput on any real-RTT path (4.8 → 39.6 MB/s at 50 ms RTT). Details:
> **[bench/RESULTS.md](bench/RESULTS.md)** §4a.
>
> Read that with its caveats, which are load-bearing. Fuse's encryption uses
> a **pre-shared key, which gives no forward secrecy**, where QUIC's TLS 1.3
> handshake does — QUIC is buying a stronger security property with some of
> its throughput. A lossless microsecond-RTT loopback also exercises none of
> Fuse's actual differentiators, and the 16 KiB block the sender adapts into
> is a loopback artefact a 1500-MTU path would not allow. Full numbers,
> loss-injection results, and what they do *not* say:
> **[bench/RESULTS.md](bench/RESULTS.md)**.

## Install

```sh
curl -fsSL https://github.com/kamalkoushikd/fuse-nw/releases/latest/download/install.sh | sh
```

Installs to `/usr/local` as root, `~/.local` otherwise (`--prefix DIR` for
anywhere else, `--uninstall` to remove). One download gives you the C/C++
libraries and headers, CMake + pkg-config integration, and the Python
package — wolfSSL is bundled, so there is nothing else to fetch.

## Quickstart — write a networked program

Fuse gives you a socket-style API: **listen, accept, connect, send, recv**.
Messages keep their boundaries (one `send` = one `recv`), delivery is
reliable, and `send` returns only once the peer has the data.

**Python**

```python
import fuse

# server
with fuse.listen(port=4433) as server:
    conn = server.accept()
    print(conn.recv())
    conn.send(b"pong")

# client
with fuse.connect("192.0.2.10", 4433) as conn:
    conn.send(b"ping")
    print(conn.recv())
```

**C**

```c
#include <fuse/sdk.h>

fuse_config cfg;
fuse_config_init(&cfg);
cfg.host = "192.0.2.10";
cfg.port = 4433;

fuse_conn *c = fuse_connect(&cfg, NULL);
fuse_send(c, "ping", 4);

char buf[4096]; size_t n;
fuse_recv(c, buf, sizeof buf, &n, 5000);
fuse_close(c);
```

```sh
cc app.c -o app $(pkg-config --cflags --libs fuse)   # or find_package(fuse CONFIG)
```

Add `key="shared-secret"` (Python) or `cfg.pre_shared_key` (C) on both ends
for AES-256-GCM encryption.

**Full SDK guide — API reference, encryption, ports, threading,
troubleshooting: [docs/SDK.md](docs/SDK.md).**
Moving whole files or large buffers instead? That is a separate, faster
API: [docs/USAGE.md](docs/USAGE.md).

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
| `FUSE_BUILD_BENCH` | `ON` (top-level) | Build the benchmark harness |

Enabling `FUSE_WITH_CRYPTO` also compiles the optional DTLS/PSK layer
(`FUSE_PROTO_WITH_DTLS`). Encryption still defaults to **off** at runtime;
a peer that sets `encryption_required` on a build without DTLS fails
closed rather than sending plaintext.

A `no-crypto` preset (`cmake --preset no-crypto`) is available for a
fast build of just the framing/socket code without pulling in wolfSSL.

### Installing system-wide

```sh
cmake --build --preset default
sudo cmake --install build/default
```

This installs `libfuse`, headers, a CMake package config (so
downstream projects can `find_package(fuse CONFIG REQUIRED)` and link
`fuse::fuse`), and a `fuse.pc` pkg-config file. `fuse.pc`'s `prefix=`
is resolved at install time, so both `-DCMAKE_INSTALL_PREFIX=...` at
configure time and `cmake --install build --prefix ...` work correctly.

### Bringing your own wolfSSL

If you'd rather not let the build fetch wolfSSL, install one yourself
built with the flags fuse's crypto module needs:

```sh
cmake -S . -B build -DWOLFSSL_TLS13=yes -DWOLFSSL_QUIC=yes
cmake --build build && sudo cmake --install build
```

then configure fuse with `-DFUSE_FETCH_WOLFSSL=OFF`; `find_package(wolfssl CONFIG)`
will pick it up.

## Installing a package instead of building from source

For developers who just want fuse available to build against, without
setting up the CMake build themselves, CPack produces installable
packages straight from this same build:

```sh
cmake --preset default
cmake --build --preset default
cd build/default
cpack               # builds whichever of DEB/RPM/TGZ your tools support
```

This yields a single combined package (headers, `libfuse`, CMake
package config, pkg-config file — plus the wolfSSL that was fetched to
build it, since that's not otherwise available as a system package with
QUIC support enabled) rather than separate runtime/`-dev` packages;
that's the standard approach for a pre-1.0 library distributed outside
a distro's own repositories.

```sh
# Debian/Ubuntu (needs dpkg-dev, normally already present)
sudo dpkg -i fuse-0.1.0-Linux.deb

# Fedora/RHEL (needs rpm-build)
sudo rpm -i fuse-0.1.0-1.x86_64.rpm
```

Every push to `main` also builds these packages in CI and uploads them
as a `fuse-packages` workflow artifact (see `.github/workflows/ci.yml`),
so you don't need a local toolchain at all to grab a build.

Once installed, a downstream project just needs:

```cmake
find_package(fuse CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE fuse::fuse)
```

or, for a non-CMake build, `pkg-config --cflags --libs fuse`.

### A note on licensing

wolfSSL's default license is the GPLv2 (a commercial license is also
available from wolfSSL Inc. for closed-source distribution). fuse
itself is MIT-licensed, but **a binary that statically or dynamically
links GPLv2 wolfSSL is a combined work subject to the GPL** when you
distribute it. If you need to ship closed-source software built on
fuse's crypto module, either obtain a commercial wolfSSL license or
swap in a permissively-licensed backend.

## Examples

The Stage 1 demos speak the actual protocol. Run the receiver first, then
the sender with `--drop N` to deliberately withhold one block and watch
the loss-recovery path work end to end:

```sh
./build/default/examples/fuse_stage1_receiver 48000 &
./build/default/examples/fuse_stage1_sender 127.0.0.1 48000 --drop 3 --count 8
```

The receiver's `base` stalls at the dropped seq_no, it emits `NACK seq=3`
once the reorder window passes, the sender retransmits from its registry,
and delivery completes:

```
recv seq=2  base=3  received_total=3
recv seq=4  base=3  received_total=4
...
NACK seq=3
recv seq=3 [retransmit]  base=8  received_total=8
```

The older `fuse_echo_server` / `fuse_echo_client` pair exercises only the
C scaffold's socket wrapper, not the protocol.

## Benchmarks

```sh
./build/default/bench/fuse_bench --bulk-mb 8 --telemetry 20000 --lanes 4
```

Runs a mixed workload (large ordered bulk transfer + many small
loss-tolerant messages) across single-threaded, multi-worker, and pinned
configurations, with and without encryption. Current results and their
caveats are in `docs/ROADMAP.md` — including the finding that **CPU
pinning measurably hurt** on the test host and is therefore not claimed
as a win.
