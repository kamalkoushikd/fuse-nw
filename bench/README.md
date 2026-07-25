# Benchmarks

> **All measured results in one place: [RESULTS.md](RESULTS.md)** —
> throughput (encrypted and not), latency under load, goodput efficiency,
> loss recovery, the packet-rate and AES-NI findings, and the full caveat
> list. This file covers how to build and run them.


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

## Testing under network conditions

`bench/fuse_netem` is a userspace UDP impairment relay (a `tc netem`
substitute that needs no root); `bench/run_network_matrix.sh` sweeps loss,
delay, jitter, reordering and duplication for both Fuse and QUIC through it,
verifying every transfer byte-for-byte. Results in
[RESULTS.md](RESULTS.md) §4a.

```sh
BENCH_WORKDIR=/var/tmp ./bench/run_network_matrix.sh 32 4   # size-MiB, lanes
```

## Running the head-to-head

```sh
# runs, size in MiB, then lane counts
BENCH_WORKDIR=/var/tmp ./bench/run_file_benchmark.sh 3 1024 1 4 8
```

Every run is verified byte-identical against the source with sha256 — a
protocol that transfers fast but corrupts the file is not a result.

## Results

16-core host, loopback, **1 GiB file**, median of 3 runs, SHA-256 verified
every run. Both protocols shard the file across the same number of lanes.

| lanes | Fuse (unencrypted) | QUIC (TLS 1.3) | Fuse advantage |
|---|---|---|---|
| 1 | **1327.7 MB/s** | 678.6 MB/s | 1.96x |
| 4 | **3980.8 MB/s** | 1847.4 MB/s | 2.16x |
| 8 | **4225.6 MB/s** | 3238.4 MB/s | 1.30x |

Fuse is faster at every lane count. Note QUIC is doing strictly more work
(it always encrypts; Fuse here does not), so this is not a like-for-like
security comparison — see the caveats below.

### Encrypted, like-for-like

The table above has Fuse unencrypted. With encryption enabled on *both*
sides (1 GiB, best of 2):

| lanes | Fuse + AES-256-GCM | QUIC + TLS 1.3 | ratio |
|---|---|---|---|
| 1 | **1093.3 MB/s** | 710.1 MB/s | 1.54x |
| 4 | **3559.7 MB/s** | 1946.6 MB/s | 1.83x |
| 8 | **4509.2 MB/s** | 3494.5 MB/s | 1.29x |

Fuse stays ahead with encryption on, and encryption costs it only ~18%
(1327 -> 1093 at one lane) rather than the 44-56% the DTLS path costs.

**Why one session key beats one DTLS session per lane.** The intuitive
design — give every lane its own DTLS session — is wrong twice over. A DTLS
session owns record-layer state (sequence numbers, epoch, replay window), so
sharing one across lanes needs a mutex, and a mutex serialises exactly the
parallelism sharding exists to create. Worse, routing data through
`wolfSSL_write` hands socket ownership to DTLS one record at a time, which
destroys GSO batching — the single largest source of Fuse's advantage.

So keys are derived **once for the whole session** (`session_crypto.hpp`)
and every lane encrypts independently with that key under a nonce unique to
`(lane, seq)`. Crypto runs in parallel, each encrypted block stays a
fixed-size datagram, and GSO batching survives. This is the shape fast QUIC
stacks use: per-packet AEAD, then batch the ciphertext into one `sendmsg`.

**AES-NI is not optional.** wolfSSL's CMake build has no AES-NI switch (it is
an autotools-only flag), so by default it silently uses portable C AES:
measured at **151 MB/s**, which capped encrypted throughput at 86 MB/s and
made Fuse *lose to QUIC by 8x*. Compiling `aes_asm.S` + `aes_gcm_asm.S` with
`-DWOLFSSL_AESNI` takes the primitive to **4852 MB/s** — a 32x swing that is
purely build configuration. `cmake/FuseWolfSSL.cmake` now wires this up and
reports which path it took.

### What changed to get here

The first version of this benchmark lost to QUIC by 2.5-2.9x. Three
changes, in descending order of impact:

1. **Batched syscalls.** The old send path issued one `sendto()` per
   datagram and sat on a ~213k packets/sec ceiling that had nothing to do
   with bandwidth. The sender now packs many equal-sized datagrams into one
   buffer and hands them to the kernel with UDP GSO (`UDP_SEGMENT`) in a
   single syscall; the receiver drains with `recvmmsg`. Wire packets stay
   MTU-sized — the kernel does the segmentation.
2. **Sharding across lanes.** The file is split into N contiguous shards,
   each an independent reliable stream with its own socket, registry,
   window and congestion controller, reassembled in lane order. This
   multiplies the packet-rate ceiling rather than contending for one
   socket.
3. **Adaptive block size.** On a link that stays clean the sender doubles
   its block size (1200 -> 16384 here); on loss it drops back to the
   MTU-safe floor. Blocks carry an explicit byte offset (wire v2) so the
   size can change mid-stream without the receiver misplacing payloads.

Scaling is *not* monotonic in lane count — 4 lanes beat 8 on some runs,
and Fuse plateaus around 4 GB/s. That is what motivates the dynamic worker
orchestrator (`include/fuse/proto/orchestrator.hpp`): the right concurrency
is discovered at runtime rather than configured.

### Loss recovery

Verified with deliberate send-side loss injection
(`FUSE_DROP_PCT=n`), since loopback itself never drops:

| injected loss | retransmits | final block size | file |
|---|---|---|---|
| 3% | 66 | 16384 (stayed large) | intact |
| 10% | 224 | 1200 (backed off to floor) | intact |

Both directions of the adaptive sizing are exercised: it grows on a clean
link and retreats to the MTU-safe floor under sustained loss, and the file
arrives byte-identical either way.

## Latency and goodput

Throughput says how fast bulk data moves when nothing else is happening.
The more revealing question for a transport is what a saturating bulk
transfer does to a small, latency-sensitive message running alongside it —
which is exactly what per-stream congestion control is supposed to fix.

`bench/fuse_latbench` runs a bulk stream flat-out plus a 64-byte probe
stream at a fixed rate on its own lane, and reports the probe latency
distribution idle and under load. `quic_latbench` does the same over QUIC.
One-way latency is meaningful because both ends share CLOCK_MONOTONIC on
one host.

5 s runs, 4 bulk lanes, probe latency in microseconds:

| configuration | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| Fuse — idle | 14.1 | 23.6 | 39.6 | 101.9 | 146 |
| **Fuse — under 6.3 GB/s bulk** | **15.8** | **24.9** | **38.2** | **131.5** | 1061 |
| QUIC — idle (1 conn) | 55.3 | 95.4 | 153.6 | 451.7 | 739 |
| **QUIC — under bulk, same connection** | **326.9** | **1829.7** | **12584.8** | **21455.6** | 22970 |
| QUIC — under bulk, separate connections | 42.5 | 62.2 | 82.4 | 99.1 | 191 |

**Fuse's probe latency is essentially unaffected by load**: p99 goes 39.6 ->
38.2 us with 6.3 GB/s of bulk traffic in flight — no measurable inflation,
zero probe loss.

**Multiplexing both onto one QUIC connection is catastrophic for latency**:
p99 inflates 82x (153 -> 12585 us). This is the shared-congestion-controller
and shared-send-queue coupling that connection-wide congestion control
implies, and it is the specific problem Fuse's per-stream design targets.

**But QUIC recovers most of it with separate connections** (p99 82 us). That
is the honest framing: the enormous gap is against QUIC used *idiomatically*
— one connection, many streams — not against QUIC as such. Fuse's real claim
is narrower and still holds: it provides that isolation **within a single
session**, where QUIC must spend an extra connection (and an extra handshake,
and separate congestion state) to buy the same property. Fuse is still ~2.2x
better at p99 than even separate-connection QUIC.

### Goodput vs wire bytes

Goodput counts only application payload delivered; the wire figure includes
block headers. Measured at 1200-byte blocks: **97.48% efficiency**
(32.90 GB payload / 33.74 GB wire), which matches the 31-byte v2 header
exactly. Encryption adds a 16-byte AEAD tag per block, taking a 1200-byte
block to ~96.2%.

### Latency caveats

- The QUIC probe stream could not sustain the requested 2000 Hz (tokio's
  sleep granularity capped it near 500-650 Hz), so its sample count is lower.
  Each probe's latency is still measured individually, so the distribution
  is valid, but the sampling is coarser.
- QUIC runs on tokio's async runtime while Fuse uses dedicated threads.
  Some of the ~40 us idle-latency difference is runtime scheduling, not
  protocol.
- Same-host loopback: no propagation delay, no real queueing, no loss.

### What these numbers do *not* say

- **The encrypted comparison uses a pre-shared key, not a handshake.** Keys
  come from a PSK plus a per-session random salt. That gives key freshness,
  but it is NOT forward secrecy: an attacker who records traffic and later
  obtains the PSK can decrypt it. QUIC's TLS 1.3 handshake *does* provide
  forward secrecy, so QUIC is still offering a stronger security property
  for its throughput. Fuse's DTLS path (ECDHE-PSK) provides it too, at the
  cost of the batching described above. This is a real trade, not a free
  win.
- **Loopback has no loss and microsecond RTT.** This measures per-packet
  implementation overhead, not congestion control, loss recovery quality,
  or behaviour at high bandwidth-delay product.
- **The 16 KiB block size is a loopback artefact.** Loopback MTU is 65536;
  on a 1500-MTU path the adaptive sizing would stay near 1200 and a large
  part of this advantage would disappear. The `10%` row above is the
  honest preview of that.
- Fuse's actual design differentiators — per-stream congestion control,
  loss-tolerant streams, head-of-line-blocking avoidance — are still not
  exercised by a single lossless bulk transfer. A `tc netem` path with the
  mixed workload remains the test that would justify the design rather
  than just the implementation.
