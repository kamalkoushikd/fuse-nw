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

*Last reviewed: 2026-10-19, at protocol version 7, project version 0.1.0.*

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

The test suite has 174 tests with the crypto backend, all passing when run
serially (`ctest` without `-j`, see the testing gaps below).

### Key design points

- **Gap detection is bitwise.** `expected & ~received` over a multi-word
  bitmask, with set bits enumerated via `__builtin_ctzll`, and no hashing.
  (Per-block receive bookkeeping is O(window), not O(1). See the
  performance issues below.)
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

Found in a full code review on 2026-10-19. Ordered by severity.

### Security

1. **AES-GCM nonce reuse on every retransmission (critical).** The
   retransmit paths (`transfer.cpp`, `sdk.cpp`) re-seal the same
   `(stream_id, seq_no)` nonce with `kFlagRetransmission` OR'd into the
   flags. Those flags are part of the AAD. Two tags under one key/nonce
   with known-different AAD and identical ciphertext let a passive observer
   solve for the GHASH key *H*. With *H*, any observed block can be forged.
   This happens on every ordinary loss. Fix: leave the retransmission bit
   out of the AAD (or out of the flags byte entirely), or retransmit the
   stored ciphertext instead of re-sealing.
2. **The control plane is unauthenticated even under PSK.** Ack, Nack,
   StreamStart, StreamClose, ResumeRanges, FileDigest, Hello and Close are
   all plaintext with no MAC. An on-path attacker can do all of these:
   - Forge `Ack`s. The sender then believes undelivered data arrived. In
     the SDK, `fuse_send()` returns `FUSE_OK` for a message the peer never
     got, which breaks its documented guarantee.
   - Abort transfers with `StreamClose`.
   - Make a resumed transfer fail verification with a bad `FileDigest`.
   - Tell the sender "nothing needed" via `ResumeRanges`.

   Data integrity still holds, because the receiver ends `Incomplete`
   rather than accepting forged bytes. Delivery guarantees and availability
   do not.
3. **Address migration without path validation.** The SDK re-anchors
   `c->peer` to the source of *any* Ack, Nack or Close arriving on the
   connection socket, without checking a nonce. An off-path attacker who
   guesses the ephemeral port can redirect a connection, or kill it with a
   2-byte `Close`. The transfer sender re-anchors on any Ack carrying the
   lane nonce, but that nonce is sent in clear in StreamStart. Both can
   turn the sender into a traffic reflector aimed at a third party. QUIC
   requires PATH_CHALLENGE for exactly this reason.
4. **The SDK server never authenticates the client.** Under PSK only the
   server proves key possession. Each HELLO, even one with a spoofed
   source, allocates a socket, a connection and a pump thread, and
   `fuse_accept` hands it to the application. There is no stateless retry
   or cookie. Together with `fuse_http_serve`'s `fuse_recv(-1)`, which
   waits forever, an unauthenticated client can exhaust server threads.
5. **Unencrypted mode has no session binding.** The first StreamStart to
   reach a receiver port claims the lane. The mux SETUP handshake has no
   nonce, so a stale or replayed SETUP can complete. FNV-1a only detects
   corruption, and that is documented.
6. **No key confirmation in the transfer handshake.** A wrong PSK is only
   diagnosed once ≥ 20 data blocks have failed authentication
   (`AuthFailed`). It should fail at StreamStart.

### Transport behaviour

7. **Block-size growth is not MTU-aware.** The transfer sender doubles the
   block up to 16 KiB after 8 clean ACK-advances. It never sets
   `IP_MTU_DISCOVER` and never probes, so on a 1500-MTU path every block
   above ~1450 bytes is IP-fragmented. Losing one fragment loses the whole
   block, and the next NACK resets the size: a sawtooth into fragmentation.
   This needs DPLPMTUD-style probing (RFC 8899).
8. **Congestion control is unfair on a shared bottleneck, and slow to
   recover.**
   - The window is counted in blocks while the block size changes up to
     13×.
   - There is no slow start: the window starts at 128 blocks and grows +16
     per clean RTT (transfer), or +8 (SDK).
   - Any NACK in an RTT halves it.
   - Every lane/stream runs its own controller, so *N* lanes compete like
     *N* TCP flows against other traffic.

   Per-stream CC also cannot isolate latency inside a *network* queue. It
   only prevents coupling between Fuse's own streams. The latency-under-load
   result is a loopback measurement for that reason.
9. **No flow control.** A receiver whose writer rings or SDK inbox
   (256 MiB) are full silently drops blocks. The sender reads that as loss
   and halves its window, so a slow disk or slow application looks like
   congestion.
10. **SACK information is discarded.** The ACK carries a 128-byte bitmask
    every 8 blocks, but senders only use `base_seq_no`. Recovery depends on
    NACKs. The RTO resends only the base block, one per RTO.
11. **Version mismatch is silent.** A peer on another `kProtocolVersion`
    drops every datagram, so the user sees `Timeout`, not "incompatible
    version". There is no version negotiation or error reply.
12. **No liveness on idle SDK connections.** `Heartbeat` is defined but
    never sent, so a peer that vanishes is only noticed when an operation
    times out. `fuse_recv(-1)` can wait forever.
13. **The SDK is stop-and-wait per message.** `fuse_send` returns only
    after the whole message is acknowledged, so small-message throughput
    per connection is bounded by one message per RTT.
14. **Transfer lanes need N consecutive open UDP ports.** That is hostile
    to firewalls and NAT. The SDK's single-port model does not extend to
    the transfer API.
15. **`file_id` is a hash of name+size+mtime.** A file rewritten in place
    with the same size and mtime would resume against stale bytes. The
    final digest still catches this (with crypto), at the cost of a wasted
    transfer. Without crypto the digest is FNV and is not tamper-proof.

### Performance (implementation, not wire format)

16. `ReceiverStream::on_receive` scans the whole window (up to 1024
    positions) on every block, and `slide_base_over_contiguous_prefix`
    shifts two 1024-entry arrays per advanced sequence number. That is
    O(window) per packet, not the O(1) the design describes.
17. `ReassemblyStream` (ORDERED=0) grows a `vector<bool>` to `seq_no + 1`
    using a wire-supplied value, and `delivery_order_` grows without bound.
    No transport uses it today, but it is public API.

### Further issues (second pass)

18. **Traffic amplification from the transfer receiver.**
    - The first StreamStart to reach a waiting receiver lane is accepted
      from whatever source address it carries, with no address validation.
    - After that, the receiver sends a 156-byte Ack to that address on
      every idle loop (about every 20 ms) until its idle timeout
      (`timeout_ms` + 1 s, 121 s by default).
    - So one spoofed 70-byte StreamStart produces about 50 Acks/s for two
      minutes, roughly 0.9 MB aimed at a victim per packet, per lane.
    - A ResumeRanges answer for a fragmented partial file can be up to
      64 × ~1 KB, and it is re-sent for every retried StreamStart.
    - Acks should only flow after data arrives, and the handshake needs a
      return-routability check.
19. **The PSK can be cracked offline if it is a password.** The PSK is used
    directly as HKDF input, with no password hashing and no PAKE. One
    captured SDK handshake is enough: the client challenge is in clear in
    HELLO, and `proof` is that challenge sealed under a key derived from
    the PSK and the public salts. One captured encrypted transfer block
    gives the same check. Either way a human-chosen key can be
    brute-forced offline. At minimum, document that the key must be
    high-entropy random bytes. Better, use a PAKE (e.g. CPace/OPAQUE) or
    ECDHE-PSK.
20. **Session nonces are predictable.** StreamStart nonces come from
    `std::mt19937_64`, which is not a cryptographic generator: its state can
    be recovered from enough observed outputs, and the nonces are sent in
    clear. Since the nonce is the only session check on Acks and
    StreamClose (#2, #3), predicting future nonces enables off-path
    forgery. Use `random_bytes()`.
21. **No AES-GCM key update or usage limit.** One key covers an entire
    transfer or connection. At 1200-byte blocks, a transfer of roughly
    10 GB already exceeds the ~2²³-record confidentiality limit commonly
    applied to AES-GCM (RFC 9001 §6.6).
22. **No pacing.** Each send call carries up to ~60 KB (one GSO batch),
    and the sender issues these back-to-back until the congestion window
    (up to 1024 blocks) is full, all at line rate. On shallow-buffered paths
    those bursts self-induce loss, which the CC then halves for.
23. **The RTO is aggressive and has no backoff.** RTO = 2 × SRTT with a
    5 ms floor (transfer, mux) or 3 × SRTT with a 20 ms floor (SDK). None of
    them has an RTT-variance term or exponential backoff on repeated
    timeouts. Jitter above about SRTT causes spurious
    retransmits, and each one also halves the window.
24. **No extension mechanism.** Every field change bumps
    `kProtocolVersion` (seven times so far), and any version mismatch
    silently breaks interop (#11). There are no TLV/extension fields or
    reserved bits, so optional features can't be added compatibly.
25. **`base_port + lane` silently wraps past 65535.** The sum is cast to
    `uint16_t` and `lanes` isn't checked against `base_port`. Lanes past
    the top bind ports near 0, or fail to bind ports below 1024, with a
    misleading `SocketError`.

### Implementation bugs found in the same pass

26. **Each SDK connection uses about 17 MB.** `RegistrySlot` embeds a
    full `kMaxPayloadSize` (16 KiB) payload, and the SDK allocates 1024
    slots per connection even though it only ever sends 1200-byte blocks.
    `start_conn` builds a second registry while the first still exists,
    so memory briefly peaks around 34 MB. Combined with #4, a flood of
    unauthenticated HELLOs runs the server out of memory.
27. `fuse_close` deletes the connection after 2 s even if another thread
    is still inside it (`in_use > 0`): a use-after-free, though it only
    happens if the caller breaks the API contract.
28. `fuse_connect` takes the PSK through `strlen`, so it can't contain NUL
    bytes. The transfer API takes a `std::string` and can, so the same
    binary key works in one API and not the other. A PSK over 256 bytes
    fails key derivation and is reported as `Unsupported`.
29. HTTP mapping:
    - `fuse_http_fetch`'s `timeout_ms` only bounds the receive, not the
      connect or send, despite what `http.h` says.
    - Method, path, headers and reason phrase are not checked for CR/LF,
      so header injection is possible.
    - A reason phrase longer than the 64-byte status-line buffer is
      silently truncated.
    - Detached handler threads can outlive `user_data` once
      `fuse_http_serve` returns.
30. Mux transfer and SETUP:
    - `SetupPayload` values aren't validated: duplicate `stream_id`s,
      `block_size` > 16384, and `protocol_version` ≠ 7 are all accepted.
    - The mux receiver indexes streams by `stream_id`, assuming they run
      0..n-1.
    - A NACK arriving while the retransmit backlog is full is silently
      dropped.
    - `SendQueue` with capacity 0 divides by zero.
31. `DtlsSession` copies every outgoing record into a 2 KB `last_wire_`
    buffer, a test aid left in the production path. Every DATA block also
    exposes the sender's raw monotonic clock (`send_time_ns`), which
    leaks host uptime.

## Gaps / not yet done

*(Resolved and no longer gaps: batched send/receive (GSO + `recvmmsg`),
RTT-adaptive RTO/NACK timers, the 64-block window cap (now 1024), a public
transfer API, and in-place resumable file receive.)*

1. **Fix the security issues above,** #1 before anything else. Authenticating
   control messages (an AEAD over Ack/Nack/Close/StreamClose/ResumeRanges
   with a separate nonce space) closes #2 and #3 and most of #5.
2. **Real-NIC testing.** Impairments are emulated (`bench/fuse_netem`,
   `scripts/net-sim/`). There is still no physical two-host run, and no run
   on a 1500-MTU path with block growth enabled (#7).
3. **Straggler lanes under heavy loss.** Static sharding means the slowest
   lane decides completion: at 5% loss throughput swings 27–226 MB/s where
   QUIC holds ~83. Dynamic re-sharding or work-stealing is the fix.
4. **Stage 4 flags in the transports.** ORDERED=0, COALESCE and LOSSLESS=0
   exist only as primitives. The mixed loss-tolerant + loss-intolerant
   session that motivates the protocol is benchmarked by `fuse_bench` but
   is not available through any public API.
5. **DTLS is not wired into any transport,** so the forward-secret option
   exists only as a primitive. Either integrate it (accepting the loss of
   GSO batching) or replace the PSK key schedule with an ECDHE-PSK exchange
   in the transfer and SDK handshakes.
6. **macOS batching.** `udp_darwin.cpp` does one syscall per datagram.
   `sendmsg_x`/`recvmsg_x` have not been verified against a real SDK.
7. **Build, test and release hygiene:**
   - `CMakeLists.txt` does `add_subdirectory(scripts/net-sim/src)`, but
     that directory is not in the repo. Any configure with
     `FUSE_BUILD_BENCH=ON` fails, which is the default top-level build,
     `cmake --preset default`, and both CI jobs. `scripts/net-sim/` also
     still expects `fuse_quickstart_send/recv`, which moved to fuse-sdk.
   - `install(DIRECTORY include/fuse ...)` excludes `sdk.h`,
     `transfer.hpp` and `mux_transfer.hpp` but not `http.h`. The
     protocol-only package therefore ships a header whose library it
     doesn't install.
   - `fuse/fuse.h` (C scaffold) and `fuse/sdk.h` both define
     `enum fuse_status`, with conflicting values (`FUSE_ERR_SOCKET` is -5
     in one and -2 in the other), so they can't be included in one
     translation unit.
   - The project version is 0.1.0, but tag `v0.1.1` exists, so
     `fuse_version()` and `fuse.pc` report 0.1.0.
   - Transfer tests use fixed UDP ports. `ctest -j` makes them collide
     (spurious failures, and one 10+ minute hang because no per-test
     `TIMEOUT` is set).
   - Resume tests `GTEST_SKIP` whenever the transfer finishes before it can
     be interrupted, so on a fast machine resume is not actually tested.
   - `fuse/http.h` has no tests in this repo.
   - `fuse_http_serve` returns `FUSE_OK` even when `fuse_accept` fails with
     a real error.
8. **The C scaffold (`fuse::fuse`: varint, packet, connection IDs,
   QUIC-style initial secrets)** is unused by the protocol and is a dead
   end. Decide whether to delete it or build on it.

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
