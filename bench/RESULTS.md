# Fuse benchmark results — consolidated

All measurements on one host, loopback, unless stated otherwise.

**Test host:** 16 cores, Linux 7.0.14-201.fc44.x86_64, loopback MTU 65536,
AES-NI + PCLMULQDQ available, 14 GiB RAM.
**Reference QUIC:** quinn 0.11.11 / rustls 0.23 (Rust, release + LTO).
**Verification:** every file-transfer run is SHA-256 checked against the
source. A fast transfer that corrupts the file is not counted as a result.

Read the caveats at the end before quoting any of this. Several of these
numbers are loopback artefacts and say nothing about a real network.

---

## 1. File transfer throughput — unencrypted

1 GiB file, median of 3 runs, both protocols sharding across the same
number of lanes.

| lanes | Fuse (unencrypted) | QUIC (TLS 1.3) | Fuse advantage |
|---:|---:|---:|---:|
| 1 | **1327.7 MB/s** | 678.6 MB/s | 1.96x |
| 4 | **3980.8 MB/s** | 1847.4 MB/s | 2.15x |
| 8 | **4225.6 MB/s** | 3238.4 MB/s | 1.30x |

Not like-for-like: QUIC is encrypting and Fuse is not. See section 2.

## 2. File transfer throughput — encrypted, like-for-like

1 GiB file, best of 2, encryption enabled on **both** sides.
Fuse: AES-256-GCM, session-wide key, per-lane nonce separation.

| lanes | Fuse + AES-256-GCM | QUIC + TLS 1.3 | Fuse advantage |
|---:|---:|---:|---:|
| 1 | **1093.3 MB/s** | 710.1 MB/s | 1.54x |
| 4 | **3559.7 MB/s** | 1946.6 MB/s | 1.83x |
| 8 | **4509.2 MB/s** | 3494.5 MB/s | 1.29x |

Cost of encryption to Fuse: **~18%** (1327.7 -> 1093.3 at one lane), versus
44-56% for the DTLS-session path. The difference is architectural: deriving
keys once per session and encrypting per-lane keeps crypto parallel and
preserves GSO batching, where a per-lane DTLS session would serialise on its
record layer and give up batching entirely.

**Security asymmetry — this is not a free win.** Fuse's mode here uses a
pre-shared key plus a per-session salt: keys are fresh per session but there
is **no forward secrecy**. QUIC's TLS 1.3 handshake provides it. QUIC is
buying a stronger security property with part of its throughput.

## 3. Latency under load

The measurement that tests the design premise rather than implementation
speed: what a saturating bulk transfer does to a concurrent small-message
stream. 5 s runs, 4 bulk lanes, 64-byte probes. Latency in microseconds.

| configuration | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|
| Fuse — idle | 14.1 | 23.6 | 39.6 | 101.9 | 146 |
| **Fuse — under 6.3 GB/s bulk** | **15.8** | **24.9** | **38.2** | **131.5** | 1061 |
| QUIC — idle (single connection) | 55.3 | 95.4 | 153.6 | 451.7 | 739 |
| **QUIC — under bulk, same connection** | **326.9** | **1829.7** | **12584.8** | **21455.6** | 22970 |
| QUIC — under bulk, separate connections | 42.5 | 62.2 | 82.4 | 99.1 | 191 |

- Fuse: **no measurable latency inflation under load** (p99 39.6 -> 38.2),
  zero probe loss.
- QUIC multiplexing both workloads on one connection: **p99 inflates 82x**
  (153.6 -> 12584.8). This is shared-congestion-controller and
  shared-send-queue coupling — the specific problem per-stream congestion
  control exists to avoid.
- QUIC with a second connection recovers to p99 82.4 us. **This is the
  honest comparison**: the enormous gap is against QUIC used idiomatically
  (one connection, many streams), not against QUIC as such. Fuse's actual
  claim is narrower and still holds — it provides isolation *within one
  session*, where QUIC needs an extra connection, handshake and congestion
  controller. Fuse is ~2.2x better at p99 than separate-connection QUIC.

## 4. Goodput vs wire bytes

Goodput counts only application payload delivered; wire bytes include block
headers and AEAD tags.

| configuration | payload | wire | efficiency |
|---|---:|---:|---:|
| 1200-byte blocks, unencrypted | 32.90 GB | 33.74 GB | **97.48%** |
| 1200-byte blocks, + AEAD tag (calculated) | — | — | ~96.2% |

97.48% matches the 31-byte wire-v2 header exactly (1200 / 1231). The AEAD
tag adds 16 bytes per block.

## 4a. Behaviour under real network conditions

Loopback is not a network. A transport has to be characterised under loss,
propagation delay, jitter, reordering and duplication. `tc netem` is the
usual tool but needs root; instead these runs go through
`bench/fuse_netem`, a **userspace UDP impairment relay** (built here) that
does the same job unprivileged and identically for both protocols:

    client  ->  fuse_netem (impairments)  ->  server

32 MiB, 4 lanes, every run **sha256-verified byte-identical**. Fuse
unencrypted, QUIC with TLS 1.3.

| condition | Fuse MB/s | QUIC MB/s | integrity |
|---|---:|---:|:--:|
| baseline (clean relay) | 2096 | 502 | both ok |
| loss 0.5% | 912 | 417 | both ok |
| loss 2% | 358 | 201 | both ok |
| loss 5% | 27–226 (variable) | 83 | both ok |
| delay 10 ms (20 ms RTT) | 130 | 142 | both ok |
| delay 25 ms (50 ms RTT) | 53 | 55 | both ok |
| delay 50 ms (100 ms RTT) | 27 | 30 | both ok |
| jitter 10 ms ± 5 ms | 50 | 0.6 | both ok |
| reorder 10 % @ 10 ms | 41 | 1.0 | both ok |
| duplication 3 % @ 5 ms | 249 | 218 | both ok |
| WAN: 25 ms RTT + 1 % loss | 52 | 2.8 | both ok |
| lossy WAN: 50 ms RTT + 2 % loss | 17 | 0.9 | both ok |

**The most important result is the last column: every transfer arrived
byte-identical under every condition, for both protocols.** Correctness
does not depend on the network being kind.

What the throughput numbers do and do not say:

- **Fuse tolerates reordering and jitter far better** (40–50 vs ~1 MB/s).
  This is architectural: Fuse's receiver records arrivals in a bitmask and
  only NACKs a gap after an RTT-scaled tolerance, so out-of-order delivery
  costs nothing. QUIC (quinn) infers loss from packet-number gaps and enters
  recovery under reordering, collapsing its window. The *direction* is a
  genuine design difference; the *magnitude* is inflated by the emulator,
  whose per-packet independent jitter reorders more aggressively than a
  typical real path.
- **Under heavy loss (5 %) Fuse is faster on average but wildly variable
  (27–226), while QUIC is steady (~83).** The variance is a real Fuse
  weakness: with static sharding, one lane that hits an unlucky loss pattern
  becomes a straggler, and the transfer only finishes when the slowest lane
  does. QUIC's loss recovery is more mature and more predictable. This is
  the clearest place Fuse loses.
- **The relay itself penalises QUIC's baseline** (502 vs Fuse's 2096): QUIC's
  congestion control reacts to the queuing the relay adds, where Fuse blasts
  through it. So the fair reading of the impaired rows is each protocol
  *relative to its own baseline*, not the absolute Fuse-vs-QUIC gap — the
  clean like-for-like throughput comparison is Sections 1–2, not this table.
- **High-RTT rows are window/BDP-limited for both** and roughly tied.
  Throughput ≈ in-flight-window-bytes / RTT, and Fuse's 64-block window is
  the ceiling there, not the emulated link. Raising it for high-BDP paths
  needs a wider ACK range format (see the gaps in ROADMAP).

## 4a-fix. Retransmission timeout: a bug this testing found and fixed

The very first high-RTT run exposed a serious bug. Fuse's retransmission
timeout was a **fixed 5 ms** — fine on loopback, catastrophic on any real
path, because it fires long before an ACK a full RTT away can return, so the
sender retransmits every in-flight block many times over.

| 16 MiB @ 50 ms RTT | throughput | retransmits |
|---|---:|---:|
| fixed 5 ms RTO (before) | **4.8 MB/s** | **1877** |
| RTT-adaptive RTO (after) | **39.6 MB/s** | **0** |

The fix makes the RTO ~2×RTT (the congestion controller already estimates
RTT from the ACK echo), and makes the receiver's reorder/re-NACK intervals
RTT-scaled too (estimated on the receiver's own clock from the delay between
a NACK and its retransmission). An 8× throughput gain and spurious
retransmissions eliminated. Shipped in both the library
(`src/proto/transfer.cpp`) and the benchmark. After the fix, high-RTT
throughput degrades smoothly as ~1/RTT — the correct window-limited
behaviour — instead of collapsing.

## 4b. Why throughput does not scale linearly with lanes

Scaling is linear *until the CPU saturates*, then it declines. 512 MiB,
16-core host, both endpoints on the same machine:

| lanes | MB/s | cores used | scaling |
|---:|---:|---:|---:|
| 1 | 1413.9 | 4.2 | 1.00x |
| 2 | 2357.0 | 7.4 | 1.67x |
| 4 | 3985.4 | 13.1 | 2.82x |
| **6** | **4255.2** | **16.0** | **3.01x** (peak) |
| 8 | 3889.3 | 16.3 | 2.75x |
| 12 | 3263.3 | 16.5 | 2.31x |
| 16 | 3033.5 | 16.7 | 2.15x |

The knee is exactly where `cores used` reaches the machine's 16. Three
things combine:

1. **Each lane costs ~2.7 cores, not 2.** A lane is one sender thread plus
   one receiver thread, but the kernel side is not free: UDP processing, GSO
   segmentation, loopback delivery and the two user/kernel copies all burn
   CPU too. Six lanes is therefore ~16 cores.
2. **Loopback doubles the bill.** Both endpoints share the same 16 cores.
   Across two machines each side would have its own, so the knee would be
   roughly twice as far out.
3. **Past saturation it gets actively worse**, not merely flat. Involuntary
   context switches rise from 306 (6 lanes) to 3336 (16 lanes) as 32 threads
   contend for 16 cores, and CPU per byte rises from 3.41 to 5.31 ns.

**Causal check.** If this is CPU saturation, restricting the cores should
move the peak. Limiting both processes to 8 cores with `taskset`:

| lanes | MB/s (8 cores) |
|---:|---:|
| 2 | 2503.2 |
| **4** | **3711.4** (peak) |
| 6 | 3481.6 |
| 8 | 3294.4 |

The peak moves from 6 lanes to 4 — as predicted.

**Memory bandwidth is a secondary factor, not the wall.** Measured ceiling
on this host is ~22.5 GB/s (multi-threaded `memcpy`, read+write). The data
path does several copies per byte (registry store, GSO staging,
`copy_from_user`, `copy_to_user`, shard write), so at ~4.3 GB/s goodput it
is a meaningful fraction of that budget — but a bandwidth ceiling would
produce a flat plateau, whereas the measured curve *declines*. Declining
throughput with rising CPU per byte is the signature of scheduling
contention, not bandwidth exhaustion.

**What would push the knee further out**, in rough order of value:
- Run the endpoints on separate machines (the single biggest factor here).
- Drop a copy: the sender `memcpy`s every block into the retransmit
  registry, but the source buffer is immutable for the duration of a
  transfer, so the registry could hold a reference instead.
- Decouple lanes from threads — service several lanes from one thread via
  `epoll`, so stream parallelism stops implying thread count.
- Let the orchestrator (`fuse/proto/orchestrator.hpp`) choose the worker
  count from measured utilisation rather than a hardcoded lane count. This
  measurement is precisely the case it exists for.

## 5. Packet-rate ceiling — why batching mattered

Same 8 MiB transfer, varying only block size, **before** batched I/O:

| block size | datagrams | throughput | implied packets/sec |
|---:|---:|---:|---:|
| 300 B | 27,963 | 61.5 MB/s | 215,000 |
| 600 B | 13,982 | 123.8 MB/s | 216,000 |
| 1200 B | 6,991 | 242.9 MB/s | 212,000 |

Throughput is linear in block size and the packet rate is flat at ~213k/s
regardless of payload. The bottleneck was **one `sendto()` syscall per
datagram**, not bandwidth. Fixing this (UDP GSO on send, `recvmmsg` on
receive) is what turned a 2.5-2.9x loss against QUIC into a win.

## 6. AES-GCM primitive throughput

wolfSSL's CMake build has **no AES-NI option** (it is autotools-only), so by
default it silently uses portable C AES.

| build | AES-256-GCM |
|---|---:|
| default CMake build (software) | 151 MB/s |
| + `aes_asm.S` + `aes_gcm_asm.S`, `-DWOLFSSL_AESNI` | **4852 MB/s** |

**32x**, purely build configuration. With software AES, encrypted Fuse ran
at 86 MB/s and lost to QUIC by 8x; with AES-NI it wins. `FuseWolfSSL.cmake`
now wires this up and logs which path it selected.

## 7. Loss recovery and adaptive block size

Deliberate send-side loss injection (`FUSE_DROP_PCT`), 64 MiB, 4 lanes.
Loopback never drops, so this is the only way to exercise the recovery path.

| injected loss | retransmits | final block size | file |
|---:|---:|---:|---|
| 0% | 0 | 16384 (grew to ceiling) | intact |
| 3% | 66 | 16384 (stayed large) | intact |
| 10% | 224 | **1200 (backed off to MTU-safe floor)** | intact |

Both directions of the adaptive sizing are exercised: it grows on a clean
link and retreats under sustained loss, and the file arrives byte-identical
either way.

## 8. Threading and topology (synthetic mixed workload)

8 MiB bulk + 20k 64-byte telemetry messages, 4 lanes, consistent over 4 runs.

| configuration | throughput | p50 latency | CPU |
|---|---:|---:|---:|
| (a) single-threaded | ~52 MB/s | 7.1 us | 0.14 s |
| (b) multi-worker, unpinned | **~136 MB/s** | 7.4 us | 0.15 s |
| (c) multi-worker, pinned | ~128 MB/s | 8.4 us | 0.17 s |

**CPU pinning consistently hurt** — (c) is reproducibly ~6% slower than (b)
with higher CPU and worse p50. Pinning is therefore *not* claimed as a win;
it is opt-in and off by default, and the dynamic orchestrator
(`orchestrator.hpp`) exists because the right worker count is not knowable
in advance either (4 lanes beat 8 on some runs).

## 9. Optimisation history — file transfer, single lane

How the throughput number moved, and why:

| stage | throughput | change |
|---|---:|---|
| Original: 1 syscall/datagram, no sharding | 237 MB/s | baseline (lost to QUIC 2.5-2.9x) |
| + `MSG_WAITFORONE`, non-blocking drain | 448 MB/s | 1.9x — fixed a batch-fill deadlock |
| + adaptive block size to 16 KiB | 995 MB/s | 2.2x |
| + 1 GiB working set | 1327 MB/s | — |
| 4 lanes | 3981 MB/s | sharding |

## 10. Correctness

| suite | tests |
|---|---:|
| `fuse_proto_tests` + `fuse_tests`, crypto build | **111 passing** |
| same, no-crypto build | **97 passing** |

Includes RFC 9000/9001 vector checks, a zero-heap-allocation guard on the
data path (global `operator new` interposition), AEAD tamper/nonce/AAD
tests, and DTLS PSK handshake tests with ciphertext verified on the wire.

---

## Caveats — read before quoting any of the above

1. **Most numbers are loopback.** No propagation delay, no real queueing,
   ~microsecond RTT. Sections 1–4, 4b, 5–9 measure per-packet implementation
   overhead. Real-network behaviour is Section 4a (via a userspace emulator);
   an actual two-host, real-NIC test is still not done.
1b. **The emulator is a single userspace relay.** It caps aggregate
   throughput (~2.5 GB/s), penalises QUIC's baseline more than Fuse's, and
   reorders more aggressively than a typical real path. Section 4a's fair
   reading is each protocol relative to its own baseline and the integrity
   column, not the absolute Fuse-vs-QUIC throughput gap.
2. **The 16 KiB block size is a loopback artefact.** Loopback MTU is 65536.
   On a 1500-MTU path the adaptive sizing would stay near 1200 and a large
   part of the throughput advantage would disappear. Section 7's 10% row is
   the honest preview.
3. **Fuse's encryption has no forward secrecy** (pre-shared key). QUIC's
   TLS 1.3 does. Not a like-for-like security posture.
4. **The 82x latency figure is against single-connection QUIC**, which is
   idiomatic QUIC but not the only option. Separate-connection QUIC closes
   most of the gap (section 3).
5. **QUIC's probe stream under-sampled.** tokio's sleep granularity capped
   it near 500-650 Hz against a 2000 Hz target, so its latency distribution
   is coarser than Fuse's. Individual probe latencies remain valid.
6. **Runtime asymmetry.** QUIC runs on tokio's async runtime; Fuse uses
   dedicated threads. Some of the ~40 us idle-latency difference is
   scheduling, not protocol.
7. **Sender and receiver share a host**, so one-way latency is meaningful
   (shared CLOCK_MONOTONIC) but they also compete for the same cores.

## Reproducing

```sh
cmake --preset default && cmake --build --preset default

# throughput, Fuse vs QUIC (needs: cd bench/quic && cargo build --release)
BENCH_WORKDIR=/var/tmp ./bench/run_file_benchmark.sh 3 1024 1 4 8

# encrypted
FUSE_ENCRYPT=1 ./build/default/bench/fuse_filebench recv 5000 4 /tmp/out.bin &
FUSE_ENCRYPT=1 ./build/default/bench/fuse_filebench send 127.0.0.1 5000 4 /tmp/in.bin

# latency under load (phase 0 = idle, 1 = loaded)
./build/default/bench/fuse_latbench recv 5100 4 &
./build/default/bench/fuse_latbench send 127.0.0.1 5100 4 5 2000 1

# loss recovery / block-size back-off
FUSE_DROP_PCT=10 ./build/default/bench/fuse_filebench send 127.0.0.1 5000 4 /tmp/in.bin

# network-conditions matrix (loss/delay/jitter/reorder/dup), Fuse vs QUIC
BENCH_WORKDIR=/var/tmp ./bench/run_network_matrix.sh 32 4

# one impaired transfer by hand: 50 ms RTT + 1% loss
./build/default/bench/fuse_filebench recv 42000 4 /tmp/out.bin &
./build/default/bench/fuse_netem   43000 4 127.0.0.1 42000 --delay-ms 25 --loss-pct 1 &
./build/default/bench/fuse_filebench send 127.0.0.1 43000 4 /tmp/in.bin
```
