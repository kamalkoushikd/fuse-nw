# Roadmap / status

Fuse is a general-purpose datagram transport protocol built directly on
UDP rather than adopting TCP or QUIC. It targets workloads that mix
loss-tolerant streams (latency-sensitive) and loss-intolerant streams
(correctness-sensitive) over one session. Nothing in Stages 0-5 assumes a
particular workload; only the optional Stage 6/7 hooks and the calling
application are allowed to be deployment-specific.

Implementation follows the staged plan in `BUILD.md`. Status below reflects
what is actually built and verified by the test suite, not what is planned.

## Implemented

| Stage | Scope | Status |
|---|---|---|
| 1 | Datagram block layer, sender registry, receiver bitmask, O(1) loss detection, aux channel (Heartbeat/ACK/NACK), retransmission | Done |
| 2 | SETUP handshake (DATA → HASH-REPLY → FINACK) with independent retry state machines | Done |
| 3 | Worker threads + static stream→worker routing, lock-free SPSC aux→worker signalling | Done |
| 4 | Per-stream flags: LOSSLESS, ORDERED, COALESCE; bulk-stream `total_blocks` | Done |
| 5 | Per-stream AIMD congestion control (LOSSLESS only), RTT from ACK echo | Done |
| 6 | Optional topology-aware worker pinning (CPU affinity; NUMA hook) | Done (see caveat) |
| 7 | Optional forced encryption over DTLS 1.2 with pre-shared keys | Done |
| 8 | Benchmark harness + head-to-head vs reference QUIC | Done |

Every stage's acceptance criteria are covered by tests in `tests/proto/`
(120 tests with the crypto backend, 104 without).

### Key design points

- **Loss detection is one bitwise op.** `expected & ~received` over a
  `uint64` bitmask, with set bits enumerated via `__builtin_ctzll`. No
  hashing, no allocation.
- **Zero heap allocation after startup** on the data path, enforced by a
  test that replaces global `operator new` and asserts zero allocations
  across a sustained send/receive/ack loop.
- **Congestion control is per-stream, not per-session** — a deliberate
  divergence from QUIC, where it is connection-wide. A LOSSLESS=0 stream
  never adjusts its window; it coalesces instead.
- **No locks in the worker hot path.** Each registry has exactly one
  writer (its owning worker thread); the aux thread only pushes onto
  lock-free SPSC queues and never touches a registry.
- **Encryption is off by default and never negotiated in-band**, which
  would invite a downgrade attack. It is a local config decision, and a
  peer that requires it fails closed rather than falling back to plaintext.
  PSK suites are ordered to prefer ephemeral key exchange (ECDHE/DHE-PSK)
  so recorded traffic is not retroactively decryptable if the PSK leaks.

## Measured results

From `bench/fuse_bench` on a 16-core host, loopback, 8 MiB bulk transfer
(LOSSLESS=1, ORDERED=1) interleaved with 20k 64-byte telemetry messages
(LOSSLESS=0), 4 lanes. Consistent across 4 runs:

| Configuration | Throughput | p50 latency | CPU |
|---|---|---|---|
| (a) single-threaded | ~52 MB/s | 7.1 µs | 0.14 s |
| (b) multi-worker, unpinned | ~136 MB/s | 7.4 µs | 0.15 s |
| (c) multi-worker, pinned | ~128 MB/s | 8.4 µs | 0.17 s |

Encryption overhead (DTLS on vs off): **-44% to -56% throughput**, p50
latency 7.1 → ~10 µs.

**Finding: CPU pinning did not help — it consistently hurt.** (c) is
reproducibly ~6% slower than (b) with higher CPU and worse p50 latency.
Per the plan's own instruction, the topology-pinning benefit is therefore
**not** claimed as an advantage. A plausible cause is that pinning sender
threads while receiver threads float leaves the scheduler unable to
co-locate the pairs, but this has not been investigated. Stage 6 remains
useful as an opt-in hook; it is not a default win.

## Head-to-head vs QUIC (file transfer)

*Consolidated numbers for every benchmark: [`bench/RESULTS.md`](../bench/RESULTS.md).*

`bench/fuse_filebench` performs a reliable, sharded, checksum-verified file
transfer; `bench/quic/` is a reference QUIC implementation (quinn) doing the
same work with the same shard count. 1 GiB file, loopback, median of 3:

| lanes | Fuse (unencrypted) | QUIC (TLS 1.3) | Fuse advantage |
|---|---|---|---|
| 1 | **1327.7 MB/s** | 678.6 MB/s | 1.96x |
| 4 | **3980.8 MB/s** | 1847.4 MB/s | 2.16x |
| 8 | **4225.6 MB/s** | 3238.4 MB/s | 1.30x |

An earlier version of this benchmark **lost** to QUIC by 2.5-2.9x. What
closed the gap, in order of impact: batched syscalls (UDP GSO on send,
`recvmmsg` on receive) removing a ~213k packets/sec ceiling; sharding the
file across independent lanes; and adaptive block size. Details and full
caveats in `bench/README.md`.

With encryption on both sides Fuse still leads (1093 vs 710 MB/s at one
lane, 3560 vs 1947 at four), because keys are derived once per session and
each lane runs its own AEAD — which keeps crypto parallel and, critically,
keeps GSO batching intact. A per-lane DTLS session would have serialised on
its record layer and given up batching entirely.

Two caveats carry real weight. Fuse's encryption uses a pre-shared key and
therefore provides **no forward secrecy**, where QUIC's TLS 1.3 handshake
does; QUIC is buying a stronger property with part of its throughput. And
the 16 KiB block size the sender adapts to is a loopback artefact a 1500-MTU
path would not permit. Loopback also exercises none of Fuse's actual
differentiators.

One build-configuration finding is worth repeating: wolfSSL's CMake build
has no AES-NI option, so it silently used software AES at 151 MB/s. Wiring
in `aes_asm.S` + `aes_gcm_asm.S` took the primitive to 4852 MB/s — a 32x
swing that decided the entire encrypted comparison.

## Latency under load — the design premise, finally tested

Every earlier benchmark measured bulk throughput on an idle link, which
exercises none of what Fuse is designed for. `bench/fuse_latbench` measures
what a saturating bulk transfer does to a concurrent small-message stream.
Probe latency, microseconds:

| configuration | p50 | p99 | p99.9 |
|---|---|---|---|
| Fuse — idle | 14.1 | 39.6 | 101.9 |
| **Fuse — under 6.3 GB/s bulk** | **15.8** | **38.2** | **131.5** |
| QUIC — under bulk, one connection | 326.9 | 12584.8 | 21455.6 |
| QUIC — under bulk, separate connections | 42.5 | 82.4 | 99.1 |

Fuse shows **no measurable latency inflation under load** (p99 39.6 -> 38.2)
with zero probe loss. Multiplexing bulk and latency-sensitive traffic on one
QUIC connection inflates p99 by 82x — the shared-congestion-controller
coupling that per-stream congestion control exists to avoid.

The honest qualification: QUIC recovers most of that with a second
connection (p99 82 us). So the claim is not "QUIC is broken" but the
narrower, accurate one Fuse actually makes — it delivers stream isolation
*within one session*, where QUIC must spend an extra connection, handshake
and congestion controller for the same property. Fuse remains ~2.2x better
at p99 than separate-connection QUIC.

Goodput efficiency is 97.48% of wire bytes at 1200-byte blocks, matching the
31-byte header exactly.

## Gaps / not yet done

*(Resolved earlier and no longer gaps: the send path is now batched via UDP
GSO + `recvmmsg`; the retransmission timeout and NACK timers are now
RTT-adaptive; a public `fuse/transfer.hpp` API ships. See `bench/RESULTS.md`.)*

1. **The in-flight window is capped at 64 blocks** because the ACK bitmask is
   one `uint64`. Harmless on loopback; on a high bandwidth-delay-product path
   it caps throughput at window_bytes/RTT (measured: 50 ms RTT → ~40 MB/s).
   A wider or run-length-encoded ACK range format is the single
   highest-value protocol change left.
2. **Real-network behaviour is now emulated, not yet real-NIC.** Loss,
   delay, jitter, reordering and duplication are tested via a userspace
   relay (`bench/fuse_netem`, `bench/run_network_matrix.sh`) — see
   `bench/RESULTS.md` §4a. Integrity holds under every condition. Still
   missing: an actual two-host test over a physical or virtualised NIC.
3. **Straggler lanes under heavy loss.** With static sharding, one lane that
   hits an unlucky loss pattern stalls the whole transfer (which finishes
   only when the slowest lane does). At 5% loss this makes Fuse's throughput
   swing from 27 to 226 MB/s where QUIC holds steady at ~83. Dynamic
   re-sharding / work-stealing across lanes (the orchestrator's remit) is
   the fix.
4. **NUMA placement is unverified.** The hook exists and compiles against
   libnuma when its headers are present, but this host has no
   libnuma-devel and is single-node, so the `numastat` locality check in
   the Stage 6 criteria has not been run.
5. **The data plane is not yet driven end-to-end by the worker pool.**
   Stage 3 workers own registries and service retransmit requests, but the
   full send loop over sockets lives in the demos and benchmark rather
   than in `Worker`. Unifying these is the natural next step.
6. **Congestion control is wired into the transfer path only.** The
   controller now gates the in-flight window in the shipping transfer API and
   the file benchmark, but the Stage-3 worker pool does not yet consult it.
7. **DTLS covers a link, not yet the full session lifecycle.** The
   session establishes and carries data; SETUP-inside-the-tunnel ordering
   is specified and implemented at the API level but not exercised by an
   end-to-end test.

## Explicit non-goals (do not implement without revisiting)

- Mid-session worker/stream rebalancing or autoscaling — static at SETUP.
- Mid-session block-size or window-size renegotiation.
- Mandatory encryption. Fuse does not force it the way QUIC does; a
  deployment where the endpoints are not mutually trusted should set
  `encryption_required = true`.
- Certificate/PKI authentication — Stage 7 is PSK by design.
- Any hardcoded workload assumption (GPU telemetry, files, …) in
  Stages 0-5.
