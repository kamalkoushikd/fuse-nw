# Fuse — Build Work Plan

A general-purpose custom transport protocol/framework — not "UDP," a
protocol of its own, built on top of raw datagram sockets rather than
adopting TCP or QUIC. Suitable for any workload mixing loss-tolerant
streams (telemetry-like, latency-sensitive) and loss-intolerant streams
(bulk transfer, correctness-sensitive) over the same session. GPU
telemetry is one possible application of this, not the design target —
nothing in the core protocol should assume GPUs, NUMA topology, or any
specific workload exist on either end.

This document is the staged spec for an agent to implement against —
each stage has a fixed scope, explicit acceptance criteria, and depends
only on prior stages. Do not skip ahead. Each stage should compile,
run, and pass its acceptance criteria before the next stage begins.

---

## Stage 0 — Frame format (DONE, reference implementation exists)

Fixed 6-byte header (`length: uint32BE`, `version: uint8`, `msg_type: uint8`)
followed by opaque payload. `send_all`/`recv_all` wrappers guarantee full
reads/writes over stream-style sockets.

**Status:** implemented and verified (`frame.h`, `tcp_io.h`, `sender_demo.cpp`,
`receiver_demo.cpp`) as a proof of the framing concept over TCP. The block
header in Stage 1 supersedes this for the actual data plane, which moves
to datagrams — but the version-gating and full-read/write discipline
established here carries forward everywhere.

---

## Stage 1 — Datagram block layer + registry

**Goal:** the data plane moves as independently-addressed datagrams
("blocks"), each individually retransmittable, over raw sockets — not
routed through TCP's connection/ordering semantics.

### 1.1 Block header
```
stream_id   : uint16
seq_no      : uint64   // monotonic per stream, assigned by sender
flags       : uint8    // bit0: retransmission, bit1: last-block-in-message
payload_len : uint16
```
Payload size target: ~1200 bytes (stays under typical path MTU, avoids IP
fragmentation). Do not use 16-byte blocks — header overhead alone (13 bytes)
would dominate a block that small.

### 1.2 Sender-side registry
- Fixed-size circular array per stream: `std::array<RegistrySlot, 64>`
  (window size configurable per stream at SETUP, see Stage 2 — 64 is
  the default, not a hard global cap).
- `RegistrySlot = { seq_no: uint64, payload_len: uint16,
  payload: uint8[MAX_PAYLOAD_SIZE], send_time_ns: uint64, valid: bool }`.
- Index by `seq_no % window_size` — no hash map, no dynamic allocation
  after startup.
- On send: write slot, mark valid, send datagram.
- On ACK confirming a seq_no: mark that slot invalid (evictable).
- No `std::string`/`std::vector`/`std::unordered_map` in this path —
  fixed-size buffers only, memcpy in, memcpy out.

### 1.3 Receiver-side tracking
- Per stream: `received_bitmask: uint64` + `base_seq_no: uint64`.
- Bit `i` set = `base_seq_no + i` received.
- On receiving a block: set the corresponding bit (accounting for
  window slides as `base_seq_no` advances).

### 1.4 Missing-block detection (O(1))
- `missing = expected_mask & ~received_bitmask` — single bitwise op.
- Enumerate set bits via `__builtin_ctzll` in a loop bounded at 64
  iterations — extract lowest set bit, clear it, repeat.
- No hashing, no Hamming codes, no range-checksums for this purpose —
  ruled out earlier: they can't detect data that never arrived, only
  corruption in data already present.

### 1.5 Aux channel (separate flow or distinguished by msg_type)
- **Heartbeat:** periodic, carries highest seq_no sent per stream.
- **ACK:** `{stream_id, base_seq_no, received_bitmask, echoed_send_time}`
  — the timestamp echo is required by Stage 5, include it from the start.
- **NACK:** `{stream_id, missing_seq_no[]}` — sent only after a short
  reorder-tolerance delay (do not NACK on first gap sighting; a
  slightly-late-but-arriving block is not a loss).

### 1.6 Retransmission
- Sender receives NACK -> look up `registry[seq_no % window_size]` ->
  if `slot.seq_no == seq_no` (not yet overwritten by window advance),
  resend `slot.payload`. If overwritten, log as unrecoverable (should
  not happen within a healthy window; a metric worth counting if it does).

**Acceptance criteria:**
- Compiles and runs a sender/receiver pair over datagram sockets on
  loopback.
- Simulated packet drop (skip sending one seq_no deliberately) is
  detected by the receiver's bitmask within one reorder-tolerance
  window, a NACK is sent, and the sender retransmits the exact missing
  block from its registry -- verified by log output showing the
  specific seq_no round-tripping through drop -> NACK -> retransmit ->
  received.
- Memory profiler (e.g. `valgrind --tool=massif` or equivalent) shows
  zero heap allocations after startup, across a sustained run.

---

## Stage 2 — SETUP handshake

**Goal:** negotiate stream/worker topology once, before any Stage 1
machinery is relied upon. This message needs its own lightweight
reliability, since nothing else exists yet to protect it. Nothing in
SETUP should assume a particular kind of workload -- it's a generic
stream-configuration exchange.

### 2.1 SETUP payload
```
protocol_version : uint8
num_workers      : uint16
num_streams      : uint16
stream_config[]  : {
  stream_id    : uint16
  worker_id    : uint16   // explicit, not computed -- avoid cross-side
                          // formula-mismatch assumptions
  stream_flags : uint8    // bit0 LOSSLESS, bit1 ORDERED, bit2 COALESCE
  block_size   : uint16
  window_size  : uint8    // per-stream registry size, application-chosen
}
```

### 2.2 Three-way handshake
1. **Sender -> Receiver: DATA** -- SETUP payload sent; sender computes and
   holds its own hash of the payload in memory.
2. **Receiver -> Sender: HASH-REPLY** -- receiver independently hashes what
   it received and sends back its *own computed hash* (not a boolean
   match/no-match -- a corrupted boolean reply has no way to be caught;
   a corrupted hash simply fails to match anything).
3. **Sender -> Receiver: FINACK** -- sender compares hashes; on match,
   sends FINACK. Receiver starts its workers on FINACK receipt. No
   reply expected after FINACK.

### 2.3 Retry state machine
- **Sender:** `has_data`, `has_reply`. Timeout with `has_reply == false`
  -> resend DATA (covers both DATA-lost and HASH-REPLY-lost cases,
  since both look identical from the sender's side -- silence).
- **Receiver:** `has_reply_sent`, `has_finack`. Timeout with
  `has_finack == false` -> resend HASH-REPLY (receiver already has
  verified data; it just needs to prompt the sender again -- do not
  resend DATA from the receiver side, there's nothing wrong with the
  receiver's copy).
- Cap retries (e.g. 5 attempts) before surfacing a session-start
  failure to the caller.

**Acceptance criteria:**
- SETUP completes successfully on a clean link.
- Simulated loss of DATA, HASH-REPLY, and FINACK independently (three
  separate test runs) each recover via the retry rules above without
  falling through to a full restart in the HASH-REPLY/FINACK-loss cases.
- Both sides end the handshake with an identical stream-to-worker
  table -- assert this explicitly in a test, don't just assume it from
  the handshake completing.

---

## Stage 3 — Worker threads + static routing

**Goal:** each worker owns a disjoint set of streams and its own
registries. No shared mutable state between workers -- single-writer
per registry, no locks in the hot path.

### 3.1 Structure
- `num_workers` and the `stream_id -> worker_id` table come directly
  from the SETUP payload (Stage 2) -- static for the session, decided
  once, no rebalancing, no autoscaling. (Explicitly descoped per
  design discussion -- revisit only as a future stage if session-level
  static assignment proves insufficient in practice.)
- Each worker thread: owns registries for its assigned streams, runs
  its own send loop, has no need to lock against any other worker.

### 3.2 Aux thread
- Single shared thread handling heartbeats/ACK/NACK traffic for *all*
  streams.
- On NACK: look up `worker_id = stream_to_worker[stream_id]`, push a
  small retransmit request onto a lock-free SPSC queue feeding that
  worker -- the aux thread never touches a worker's registry directly.
- Aux thread does not carry payload bytes; it only routes signals.

**Acceptance criteria:**
- N workers, M streams (M > N, uneven distribution) running
  concurrently, each worker on its own thread, confirmed via thread
  IDs in logs mapping correctly to the SETUP-provided stream table.
- A NACK for a stream owned by worker 2 is provably routed only to
  worker 2's queue (log assertion), never touches worker 0 or 1's
  registries.
- No mutex/lock present in any worker's send/registry-access path --
  code review checklist item, not just a runtime test.

---

## Stage 4 — Per-stream flags (LOSSLESS / ORDERED / COALESCE)

**Goal:** let stream behavior branch on flags set at SETUP, rather than
assuming behavior from a fixed application-specific mode. This is what
makes the protocol general-purpose: any application picks its own
tradeoffs per stream, the protocol doesn't hardcode "telemetry" or
"file transfer" as special cases anywhere in the core logic.

### 4.1 Flag semantics
- `LOSSLESS=0`: on missing block, do not NACK, do not retransmit --
  treat as an acceptable drop.
- `LOSSLESS=1`: full NACK/retransmit path from Stage 1 applies until
  the block is confirmed delivered.
- `ORDERED=1`: receiver buffers out-of-order blocks and only surfaces
  data upward in seq_no order.
- `ORDERED=0`: receiver may hand blocks upward as they arrive,
  regardless of order (e.g. positional writes via offset =
  seq_no x block_size for stream consumers that support it, avoiding
  an in-memory reorder buffer entirely).
- `COALESCE=1`: under send-queue pressure, drop older unsent blocks in
  favor of newer ones.
- `COALESCE=0`: never drop; let the send queue back up instead (pairs
  with `LOSSLESS=1`).

### 4.2 Bulk-transfer-style streams (any large, ordered payload --
not assumed to be "a file" specifically)
- Needs an explicit `total_blocks` (or `total_size`) field, sent at the
  start of that specific stream (not in session-wide SETUP), so the
  receiver can distinguish "still coming" from "genuinely the last
  block, and it's missing."

**Acceptance criteria:**
- A `LOSSLESS=0`-flagged stream under simulated loss shows zero NACKs
  sent and zero retransmits -- verify via counters, not just absence of
  errors.
- A `LOSSLESS=1`-flagged stream under identical simulated loss shows
  full recovery -- every block eventually received, verified by a
  checksum of the reconstructed payload matching the source.
- A stream with `LOSSLESS=1, ORDERED=0` demonstrates blocks being
  consumed/written out of arrival order without waiting on a reorder
  buffer (distinct log/trace evidence from the `ORDERED=1` case).

---

## Stage 5 — Per-stream congestion control (LOSSLESS streams)

**Goal:** sustained loss on a `LOSSLESS=1` stream should reduce send
rate, not just keep retransmitting into a struggling link. Explicitly
out of scope for `LOSSLESS=0` streams -- they coalesce instead of
backing off. Congestion state lives per-stream, not per-session -- this
is a deliberate difference from QUIC, where congestion control is
connection-wide even though flow control is per-stream.

### 5.1 Effective window (AIMD, per stream, capped at that stream's
`window_size` from SETUP)
- Start conservative (e.g. window_size / 8).
- Sustained NACKs over a measured RTT window -> halve effective window
  (never below a floor, e.g. 2).
- N consecutive clean windows (no NACKs) -> increase by a fixed step,
  capped at the SETUP-negotiated `window_size`.

### 5.2 RTT measurement
- Uses the timestamp echo already included in the ACK format (Stage
  1.5) -- RTT and "windows since last NACK" are measured in actual
  elapsed time, not just NACK counts.

**Acceptance criteria:**
- Under simulated bandwidth constraint + induced loss, a `LOSSLESS=1`
  stream's effective window measurably shrinks (log the value over
  time) and recovers on a clean stretch.
- A `LOSSLESS=0` stream running concurrently on the same session shows
  no window adjustment at all -- congestion state is per-stream, not
  connection-wide -- verify this holds under the same test conditions.

---

## Stage 6 — Topology-aware worker placement (optional, deployment-specific)

**Goal:** on hardware where it's applicable, worker threads can be
pinned to specific cores/memory nodes matching the physical origin of
their assigned streams. This is an optional deployment optimization,
not part of the core protocol -- the protocol itself has no concept of
GPUs, NUMA nodes, or any particular hardware topology. Any application
using this protocol supplies its own topology mapping (worker <-> core/
node) at startup; the protocol just accepts and uses it.

### 6.1 Pinning (generic mechanism)
- Each worker thread pinnable to a specific CPU core via
  `sched_setaffinity`, using whatever affinity mapping the calling
  application provides.
- Worker's registry memory allocatable via `numa_alloc_onnode` (libnuma)
  on a caller-specified node, when the application supplies one -- this
  is opt-in, not assumed.
- The protocol's job here is exposing a hook (accept an optional
  core/node hint per worker at SETUP or at startup config), not
  deciding what that hint should be -- topology discovery (e.g. via
  vendor-specific tools, `lscpu`, or any other domain-specific method)
  is the calling application's responsibility, not the protocol's.

**Acceptance criteria:**
- With no topology hints supplied, workers behave exactly as in Stage
  3 -- this stage must be provably a no-op by default.
- With topology hints supplied (in a test harness, not necessarily
  specialized hardware), `/proc/<pid>/task/<tid>/status` confirms each
  worker's `Cpus_allowed` matches the supplied hint.
- Memory-locality check (e.g. `numastat`) confirms registry allocations
  land on the hinted node when one is supplied.

---

## Stage 7 — Optional forced encryption (wolfSSL / DTLS)

**Goal:** encryption is off by default (matching the trusted-network
assumption elsewhere in this plan), but an application using Fuse can
force it on for a session. Since Fuse's data plane is datagram-based
(Stage 1), the correct primitive is **DTLS, not TLS** -- TLS assumes a
reliable, ordered byte stream underneath it, which Fuse's block layer
does not provide. wolfSSL supports DTLS 1.2/1.3 and is the intended
library, chosen for its small footprint versus OpenSSL.

### 7.1 Where encryption sits in the stack
- DTLS wraps the raw socket **beneath** the block layer, not beneath
  individual streams -- one DTLS session secures the whole
  daemon-to-peer link, all streams and the aux channel ride inside it.
- Consequence for ordering: the SETUP handshake (Stage 2) must happen
  *after* the DTLS handshake completes when encryption is forced, not
  before -- otherwise SETUP negotiates stream topology in the clear,
  which defeats the purpose. When encryption is off, SETUP behaves
  exactly as specified in Stage 2, unchanged.
- Practically: `encrypted_send(payload)` = `wolfSSL_write(ssl, ...)`,
  `encrypted_recv()` = `wolfSSL_read(ssl, ...)`, substituted in place of
  the raw `sendto`/`recvfrom` calls from Stage 1, only when encryption
  is active for that session. The block header, registry, and
  bitmask/NACK logic above this layer are unchanged either way -- they
  don't need to know whether the bytes underneath are encrypted.

### 7.2 Key material -- PSK, not certificates
- Given the trust model elsewhere in this plan (known daemon, known
  peer, not an anonymous internet client), wolfSSL's **PSK (pre-shared
  key) cipher suites** are the right fit -- avoids the complexity and
  overhead of a certificate/PKI setup for a link where both ends are
  already known to each other. Certificate-based auth is a valid
  future option if Fuse is ever deployed between parties that don't
  already share a secret out-of-band, but it is not the default here.

### 7.3 Negotiation
- Encryption is a **local configuration decision**, not something
  negotiated over the wire after the fact -- both peers must be
  started with the same `encryption_required` setting (or one refuses
  to proceed). This avoids a downgrade-attack shape where one side
  could be tricked into skipping encryption via a manipulated
  in-band negotiation message.
- Add `encryption_required : bool` to the startup configuration (not
  the SETUP wire payload from Stage 2, since SETUP itself may need to
  be inside the DTLS tunnel -- see 7.1). If true, the DTLS handshake
  must succeed before any SETUP or block traffic is attempted; failure
  aborts session start rather than silently falling back to plaintext.

**Acceptance criteria:**
- With `encryption_required = false` on both sides, behavior is
  identical to Stages 0-6 -- this stage must be provably a no-op by
  default, same standard as Stage 6.
- With `encryption_required = true` on both sides, a full session
  (SETUP + at least one LOSSLESS and one non-LOSSLESS stream) completes
  successfully over the DTLS tunnel, verified by packet capture showing
  ciphertext on the wire (not readable block headers/payloads).
- Mismatched settings (one side requires encryption, the other doesn't
  attempt it) fail closed -- session does not start, no silent
  plaintext fallback.
- A basic PSK misuse case (wrong pre-shared key on one side) is
  rejected at the DTLS handshake, not discovered later as a garbled
  SETUP payload.

---

## Stage 8 — Benchmark harness

**Goal:** turn architectural claims into measured results -- this is
what separates "structurally different from QUIC" from "measurably
better than QUIC" for a given workload. Use generic synthetic
workloads, not domain-specific ones, so results generalize.

### 8.1 Required comparisons
- Synthetic workload mix: many small loss-tolerant messages (stream
  flagged `LOSSLESS=0`) + one large ordered bulk transfer (stream
  flagged `LOSSLESS=1`), run three ways: (a) single-threaded baseline,
  (b) multi-worker with no topology pinning, (c) multi-worker with
  topology pinning (Stage 6), on hardware where pinning is applicable.
- Same three configurations repeated with encryption forced on (Stage
  7), to measure the DTLS/wolfSSL overhead separately from the
  threading/pinning comparisons -- do not conflate the two.
- Same workload run against a reference QUIC implementation for
  side-by-side throughput/latency numbers -- this is the comparison
  that actually substantiates or disproves any "better than QUIC"
  claim; do not skip it in favor of only benchmarking against yourself.

### 8.2 Metrics to capture
- Throughput (bulk-transfer stream), delivery latency (loss-tolerant
  stream), CPU utilization per core, memory footprint over time
  (confirming Stage 1's zero-heap-growth claim under load, not just at
  idle), and encryption overhead (throughput/latency delta between
  Stage 7 on vs. off).

**Acceptance criteria:**
- A results table (numbers, not impressions) showing where (b) and (c)
  differ, and whether (c) measurably outperforms (b) -- if it doesn't,
  the topology-pinning benefit needs revisiting before being stated as
  a real advantage anywhere.
- A results table showing where this protocol wins, loses, or ties
  against the QUIC baseline, per stream-flag combination -- report all
  three outcomes, not only favorable ones.
- A results table isolating encryption overhead specifically, so the
  cost of forcing it on is known and quotable, not assumed.

---

## Explicit non-goals (descoped by earlier design discussion -- do not
implement unless revisited deliberately)
- Mid-session worker/stream rebalancing or autoscaling -- static at
  SETUP only, for this version.
- Mid-session block-size or window-size renegotiation.
- Encryption is optional and off by default (Stage 7) -- Fuse does not
  force encryption unconditionally the way QUIC does. Any deployment
  where the two endpoints aren't fully trusted should set
  `encryption_required = true`; this is a deployment decision, not
  something the protocol should assume for every user.
- Certificate/PKI-based authentication -- Stage 7 uses PSK by design;
  certificate support is a future option, not in this plan.
- Any hardcoded assumption about workload type (GPU telemetry, files,
  or otherwise) inside Stages 0-5 -- those stages must remain workload-
  agnostic; only Stage 6 (optional), Stage 7 (optional), and the
  calling application are allowed to be domain- or deployment-specific.