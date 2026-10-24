# Roadmap / status

Fuse is a general-purpose datagram transport protocol built directly on
UDP rather than adopting TCP or QUIC. It targets workloads that mix
loss-tolerant streams (latency-sensitive) and loss-intolerant streams
(correctness-sensitive) over one session. Nothing in Stages 0-5 assumes a
particular workload. Only the optional Stage 6/7 hooks and the calling
application are allowed to be deployment-specific.

Implementation followed the staged plan in `BUILD.md`, which is now
historical. The wire format as it exists today is documented in
[PROTOCOL.md](PROTOCOL.md). Status below reflects what is actually built and
verified by the test suite, not what is planned.

*Last reviewed: 2026-10-24, at protocol version 8, project version 0.2.0. The
protocol-core fixes made since the 2026-10-19 review are summarised under
[Known protocol issues](#known-protocol-issues).*

## Implemented

| Stage | Scope | Status |
|---|---|---|
| 1 | Datagram block layer, sender registry, receiver bitmask, gap detection, ACK/NACK, retransmission | Done. Window widened from 64 to 1024 blocks (v3/v5) |
| 2 | SETUP handshake (DATA → HASH-REPLY → FINACK) with independent retry state machines | Done. Only the mux transfer uses it |
| 3 | Worker threads + stream→worker routing | **Superseded.** Static routing was replaced by `WorkerOrchestrator`, a utilisation-driven autoscaling pool used by the mux transfer. SETUP's `worker_id` is now vestigial |
| 4 | Per-stream flags: LOSSLESS, ORDERED, COALESCE; bulk-stream length preamble | Primitives done (`ReassemblyStream`, `SendQueue`). The shipping transports are LOSSLESS-only, and ORDERED/COALESCE are not wired into any of them |
| 5 | Per-stream AIMD congestion control, RTT from ACK echo | Done, used by all three transports |
| 6 | Topology-aware worker pinning (CPU affinity / NUMA) | **Removed.** Pinning measurably hurt (see below), so the code was deleted. There is no NUMA support |
| 7 | Optional encryption | Two paths. Per-lane AES-256-GCM from a PSK (used by transfer and SDK, no forward secrecy), and DTLS 1.2 ECDHE-PSK (standalone primitive, not used by any transport) |
| 8 | Benchmark harness + head-to-head vs reference QUIC | Done (`bench/`, `scripts/net-sim/`) |

Beyond the original plan:

- **`fuse/transfer.hpp`**: sharded, GSO-batched buffer/file transfer, with
  in-place writes, cancel/progress, and resumable `receive_file` with
  whole-file digest verification (v6/v7).
- **`fuse/sdk.h`**: socket-style C API (`listen/accept/connect/send/recv`),
  message-oriented, one listen port with TFTP-style per-connection ports.
- **`fuse/mux_transfer.hpp`**: N logical streams over one socket, serviced
  by the autoscaling worker pool.
- **`fuse/http.h`**: HTTP/1.1 request/response mapped onto one SDK message
  each way (Alt-Svc token `hfuse-00`).
- **macOS** builds (`udp_darwin.cpp`), without batched syscalls.

The SDK, mux transfer and HTTP layers are built from this repo (targets
`fuse::sdk`, `fuse::http`) but are not installed by it. They are released
from [fuse-sdk](https://github.com/kamalkoushikd/fuse-sdk).

The test suite has 206 tests with the crypto backend (184 without), all
passing, including under `ctest -j`.

### Key design points

- **Gap detection is bitwise.** `expected & ~received` over a multi-word
  bitmask, with set bits enumerated via `__builtin_ctzll`, and no hashing.
  The received bits and gap timers are rings indexed by `seq % 1024`, so
  per-block receive bookkeeping is O(1) amortised.
- **Zero heap allocation in the Stage 1 primitives** (registry + receiver)
  after startup. A test that replaces global `operator new` enforces this
  for that loop. The higher-level transports allocate at setup and on a few
  paths (resume bookkeeping, `ReassemblyStream`'s duplicate set), and they
  are not covered by that test.
- **Congestion control is per-stream, not per-session**, a deliberate
  divergence from QUIC.
- **Encryption is off by default and never negotiated in-band.** It is a
  local config decision, and a peer that requires it fails closed rather
  than falling back to plaintext.

## Measured results

*These numbers are historical. They were measured during development, on
one 16-core host, mostly over loopback, and at earlier protocol versions.
They have not been re-run since the v5–v7 wire changes. Full tables and
caveats: [`bench/RESULTS.md`](../bench/RESULTS.md).*

- **File transfer vs QUIC (quinn), 1 GiB, loopback:** 1.3–2.2× QUIC
  unencrypted, 1.3–1.8× with encryption on both sides.
- **Latency under load:** small-message p99 39.6 → 38.2 µs with 6.3 GB/s
  of bulk traffic on the same session. One QUIC connection carrying both
  inflates p99 82×. A second QUIC connection recovers most of that.
- **Emulated impairments** (`bench/fuse_netem`): every transfer
  byte-identical. Fuse is better than QUIC under reordering/jitter, and
  faster on average but higher-variance at 5% loss.
- **CPU pinning hurt** (~6% slower, higher p50). That is why Stage 6 was
  removed.
- **wolfSSL's CMake build silently used software AES** (151 MB/s). Wiring
  in the AES-NI asm brought it to 4852 MB/s.

The caveats are load-bearing. The PSK path gives no forward secrecy where
QUIC's TLS 1.3 does. Loopback exercises none of Fuse's differentiators.
The adaptive block size reaches 16 KiB, which is only safe on loopback or
jumbo-frame paths (see the IP fragmentation issue below).

## Known protocol issues

Found in a full code review on 2026-10-19 (numbers kept for reference). The
protocol-core pass that followed fixed everything outside the socket-style
SDK (`sdk.cpp`) and the HTTP mapping (`http.cpp`), which were deliberately
left for their own pass. Each fix has its own commit, and tests next to it.

### Fixed in the protocol core (v8)

| # | Issue | Fix |
|---|---|---|
| 1 | AES-GCM nonce reuse on every retransmission (transfer) | The retransmission bit is masked out of the AAD, so a resend is the identical ciphertext and tag |
| 2 | Control plane unauthenticated under PSK (transfer) | Every control datagram carries a `ctr + GMAC` trailer under a separate control key; unsigned ones are ignored |
| 3 | Address migration without path validation (transfer) | `PathChallenge`/`PathResponse`: a new source must echo a random challenge before the peer address moves |
| 5 | No session binding without encryption; replayable SETUP | Retry tokens bind a lane to a validated address; SETUP replies carry a fresh responder nonce the FINACK must echo |
| 6 | No key confirmation (transfer) | The StreamStart's signature confirms the key; a mismatch returns `AuthFailed` at the handshake |
| 7 | Block growth not MTU-aware | Growth capped at the route MTU (kernel-reported); a loss after growth sets a ceiling, re-probed DPLPMTUD-style |
| 8 | Window counted in blocks; no slow start | Window in configured-block bytes, capped by the path's largest block; opt-in slow start (used by transfer and mux) |
| 9 | No flow control | ACKs carry `rwnd`; the transfer sender stays within it, and the receiver sends window updates |
| 10 | SACK bitmask discarded | `apply_ack()` releases SACKed blocks; RTO resends the base plus known holes |
| 11 | Version mismatch is silent | Fixed-layout version-mismatch reply; `TransferStatus::VersionMismatch` |
| 15 | `file_id` from name+size+mtime only | Also hashes ctime, inode and device |
| 16 | O(window) receive path | Ring-indexed `ReceiverStream` |
| 17 | Unbounded `vector<bool>` in `ReassemblyStream` | Window-bounded ring; delivery trace capped |
| 18 | Amplification from the transfer receiver | Retry before anything else; idle ACKs stop after three to a silent peer |
| 19 | PSK brute-forceable if it is a password | Transfer refuses PSKs under 16 bytes and documents that keys must be random (PAKE remains open) |
| 20 | Predictable nonces (`mt19937_64`) | `secure_random_u64()` (crypto RNG or `getrandom`) |
| 21 | No AES-GCM usage limit | Lane keys rotate every 2²² blocks, derived identically on both ends |
| 22 | No pacing | Per-lane pacing over the RTT; smaller GSO bursts on real-RTT paths |
| 23 | Aggressive RTO, no backoff | `max(SRTT + 4·RTTVAR, 2·SRTT)`, exponential backoff, one halving per run of timeouts |
| 24 | No extension mechanism | TLV extension areas in StreamStart and SETUP, with critical-bit semantics |
| 25 | `base_port + lane` wraps past 65535 | `ConfigError` |
| 28 | PSK over 256 bytes fails derivation (core part) | Copied into a buffer sized to the key |
| 30 | SETUP unvalidated; mux stream indexing; dropped NACKs; `SendQueue(0)` | SETUP validated by the responder; mux requires ids 0..n−1; NACKs de-duplicated and resent at once if the backlog is full; capacity clamped |
| 31 | DTLS debug copy in production path; `send_time_ns` leaks uptime | Wire recording opt-in; transfer and mux send times carry a random per-stream offset |
| — | Senders always opened IPv4 sockets (IPv6 hosts unreachable) | Socket opened in the destination's family |

Measured through `bench/fuse_netem` (32 MiB, 4 lanes, every output
byte-identical), against the v7 code: 2% loss 81 → 220 MB/s, 5% loss
37 → ~170 MB/s; 50 ms RTT 54 → ~51 MB/s; 25 ms RTT + 1% loss
6.7 → 5.6 MB/s (the window no longer inflates with block size, #8);
jitter and reordering unchanged. Loopback throughput is unchanged.

### Still open

**Socket-style SDK and HTTP mapping (next pass).** These were left untouched
on purpose, and still have:

- #1 (SDK half): `retransmit_locked` re-seals with the retransmission bit in
  the AAD, so the **SDK still has the GCM nonce-reuse flaw**.
- #2, #3 (SDK half): SDK control messages are unauthenticated, and the SDK
  re-anchors its peer on any Ack, Nack or Close.
- #4: the SDK server never authenticates the client and allocates per HELLO.
- #12: no liveness on idle SDK connections (Heartbeat is never sent).
- #13: the SDK is stop-and-wait per message.
- #26: about 17 MB per SDK connection (its registry slots embed 16 KiB
  payloads while it only sends 1200-byte blocks).
- #27: `fuse_close` deletes after 2 s even if a caller is still inside.
- #28 (SDK half): `fuse_connect` takes the PSK through `strlen`.
- #29: HTTP header injection, timeout scope, reason truncation, detached
  threads, `fuse_http_serve` status, and no HTTP tests.

**Protocol core, needing redesign rather than a fix:**

- #8 (part): every lane still runs its own controller, so N lanes compete
  like N flows on a shared bottleneck. That is the per-stream design premise.
- #14: transfer lanes still need N consecutive open UDP ports. A single-port
  transfer needs a reworked receive pipeline (demultiplexing lanes by
  `stream_id` on one socket).
- #19 (full): password-derived keys need a PAKE (e.g. CPace) or ECDHE-PSK.
- The mux transfer has no encryption option at all, so its control plane
  is unauthenticated too, and it has no address validation.

## Gaps / not yet done

1. **Real-NIC testing.** Impairments are emulated (`bench/fuse_netem`,
   `scripts/net-sim/`). There is still no physical two-host run, and no run
   on a real 1500-MTU path.
2. **Straggler lanes under heavy loss.** Static sharding means the slowest
   lane decides completion. Dynamic re-sharding or work-stealing is the fix.
3. **Stage 4 flags in the transports.** ORDERED=0, COALESCE and LOSSLESS=0
   exist only as primitives. The mixed loss-tolerant + loss-intolerant
   session that motivates the protocol is benchmarked by `fuse_bench` but
   is not available through any public API.
4. **DTLS is not wired into any transport,** so the forward-secret option
   exists only as a primitive.
5. **macOS batching.** `udp_darwin.cpp` does one syscall per datagram, and
   `path_mtu()` is not implemented there (blocks don't grow on macOS).
6. **Build and release hygiene.** Fixed: the missing `scripts/net-sim/src`
   no longer breaks configure; `http.h` is no longer installed; the C
   scaffold's status enum is now `fuse_result`, so it no longer clashes with
   `sdk.h`; the project version is 0.2.0; tests probe free ports (so
   `ctest -j` works) and have a per-test timeout; resume tests no longer
   skip on fast machines. Still open: `scripts/net-sim/` expects
   `fuse_quickstart_send/recv`, which moved to fuse-sdk.
7. **The C scaffold (`fuse::fuse`)** is still unused by the protocol.

## Explicit non-goals (do not implement without revisiting)

- Mid-session block-size or window-size *renegotiation*. (The sender's own
  adaptive block size is local and needs no negotiation.)
- Mandatory encryption. Fuse does not force it the way QUIC does; a
  deployment where the endpoints are not mutually trusted should set a
  pre-shared key.
- Certificate/PKI authentication. Stage 7 is PSK by design.
- Any hardcoded workload assumption (GPU telemetry, files, …) in Stages 0-5.

The original plan also listed "mid-session worker rebalancing or
autoscaling" as a non-goal. `WorkerOrchestrator` now does exactly that for
the mux transfer, after pinning was measured to hurt. Threads scale, but
stream configuration remains fixed at SETUP.
