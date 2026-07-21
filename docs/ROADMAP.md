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
(93 tests with the crypto backend, 89 without).

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

`bench/fuse_filebench` performs a genuinely reliable file transfer
(checksum-verified) and `bench/quic/` is a reference QUIC implementation
(quinn). Median of 3 runs, loopback, single-stream bulk transfer:

| Size | Fuse (unencrypted) | QUIC (TLS 1.3) |
|---|---|---|
| 1 MiB | 216.7 MB/s | **555.4 MB/s** |
| 8 MiB | 227.5 MB/s | **665.8 MB/s** |
| 64 MiB | 237.2 MB/s | **596.6 MB/s** |

**QUIC is 2.5–2.9× faster, while doing more work** (it always encrypts;
Fuse here does not). Turning on Fuse's DTLS costs a further 44–56%, so the
gap widens rather than closes. On this benchmark the project's premise
does not hold.

**Diagnosis: Fuse is packet-rate bound.** Holding the transfer fixed and
varying only block size gives 61.5 / 123.8 / 242.9 MB/s for 300 / 600 /
1200 B blocks — dead-linear, with a flat ~213k datagrams/sec ceiling
independent of payload size. Fuse issues one `sendto()` per block and is
sitting on a syscall ceiling; quinn batches packets with UDP GSO /
`sendmmsg`. This is an implementation gap, not a design flaw, and it is
fixable with no wire-format change. Details in `bench/README.md`.

Importantly, this benchmark does **not** exercise what Fuse is actually
designed for: loopback has no loss and microsecond RTT, so per-stream
congestion control, loss-tolerant streams, and head-of-line-blocking
avoidance never come into play. A lossy/high-BDP path (`tc netem`) with
the mixed workload is the test that could still vindicate the design.

## Gaps / not yet done

1. **Send path is unbatched.** One syscall per datagram, ~213k pkt/s
   ceiling. Implementing `sendmmsg`/GSO is the highest-value fix and is
   where the QUIC gap lives.
2. **The in-flight window is capped at 64 blocks (~77 KB)** because the
   ACK bitmask is one `uint64`. Harmless on loopback, but it will throttle
   any high bandwidth-delay-product path. Needs a wider or run-length
   ACK range format.
3. **No benchmark under loss or delay.** The differentiating features are
   untested where they would matter.
4. **NUMA placement is unverified.** The hook exists and compiles against
   libnuma when its headers are present, but this host has no
   libnuma-devel and is single-node, so the `numastat` locality check in
   the Stage 6 criteria has not been run.
5. **The data plane is not yet driven end-to-end by the worker pool.**
   Stage 3 workers own registries and service retransmit requests, but the
   full send loop over sockets lives in the demos and benchmark rather
   than in `Worker`. Unifying these is the natural next step.
6. **Congestion control is wired into `fuse_filebench` only.** The
   controller now gates the in-flight window there, but the worker pool
   and `fuse_bench` still do not consult it.
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
