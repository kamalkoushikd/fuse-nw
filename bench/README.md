# Benchmarks

Two harnesses live here:

- `fuse_bench` — synthetic mixed-workload throughput/latency across
  threading and topology configurations, ± encryption (Stage 8).
- `fuse_filebench` + `quic/` — **reliable file transfer, Fuse vs a
  reference QUIC implementation.** This is the comparison that actually
  tests the project's premise.

## Building

The Fuse side builds with the normal CMake tree:

```sh
cmake --preset default
cmake --build build/default --target fuse_filebench fuse_bench
```

The QUIC baseline is a separate Rust project (quinn), deliberately not
wired into CMake so the C++ build has no Rust dependency:

```sh
cd bench/quic && cargo build --release
```

## Running the head-to-head

```sh
./bench/run_file_benchmark.sh 3 1 8 64   # runs, then sizes in MiB
```

Every run is verified byte-identical against the source with sha256 — a
protocol that transfers fast but corrupts the file is not a result.

## Results

16-core host, loopback, median of 3 runs, single-stream bulk transfer:

| Size | Protocol | recv MB/s | send MB/s | verified |
|---|---|---|---|---|
| 1 MiB | Fuse (unencrypted) | 216.7 | 194.1 | yes |
| 1 MiB | QUIC (TLS 1.3) | **555.4** | 345.1 | yes |
| 8 MiB | Fuse (unencrypted) | 227.5 | 205.1 | yes |
| 8 MiB | QUIC (TLS 1.3) | **665.8** | 410.7 | yes |
| 64 MiB | Fuse (unencrypted) | 237.2 | 216.9 | yes |
| 64 MiB | QUIC (TLS 1.3) | **596.6** | 456.8 | yes |

**QUIC is ~2.5–2.9× faster than Fuse** — while doing strictly more work,
since QUIC always encrypts and Fuse here does not. Enabling Fuse's DTLS
layer costs a further 44–56% (measured separately by `fuse_bench`), which
widens the gap rather than closing it.

### Why: Fuse is packet-rate bound, not byte-rate bound

Varying only the block size on the same 8 MiB transfer:

| Block size | Datagrams | Throughput | Implied packets/sec |
|---|---|---|---|
| 300 B | 27,963 | 61.5 MB/s | 215,000 |
| 600 B | 13,982 | 123.8 MB/s | 216,000 |
| 1200 B | 6,991 | 242.9 MB/s | 212,000 |

Throughput scales almost perfectly linearly with block size, and the
packet rate is flat at **~213k datagrams/sec regardless of payload size**.
The cost is per-datagram, not per-byte: Fuse issues one `sendto()` syscall
per block, so it is sitting on a syscall ceiling. quinn batches packets
into far fewer syscalls via UDP GSO (`UDP_SEGMENT`) / `sendmmsg`.

This is an implementation gap, not a protocol-design gap, and it is the
single highest-value thing to fix:

1. **Batch the send path** — `sendmmsg` and/or UDP GSO. This is where the
   ~2.5× is, and it requires no wire-format change.
2. **Raise the in-flight window ceiling.** The window is capped at 64
   blocks (~77 KB) because the ACK bitmask is a single `uint64`. That is
   fine on loopback but will throttle any high bandwidth-delay-product
   path. Fixing it means a wider or run-length-encoded ACK range format.
3. **Batch receive** with `recvmmsg` for the same reason.

### What these numbers do *not* say

- Loopback has no loss and ~µs RTT, so this measures per-packet
  implementation overhead, not congestion-control quality, loss recovery,
  or behaviour at high BDP. Fuse's per-stream congestion control and
  loss-tolerant stream handling — the actual design differentiators — are
  not exercised by a single lossless bulk transfer.
- A fair test of the premise needs a lossy, delayed path (`tc netem`) and
  the mixed loss-tolerant + loss-intolerant workload, where head-of-line
  blocking and per-stream vs per-connection congestion control matter.
  That is the experiment that could still vindicate the design; this one
  does not.
