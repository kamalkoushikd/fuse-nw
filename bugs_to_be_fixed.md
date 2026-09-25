# Fuse — Exhaustive Pre-Publication Checklist

Everything that must be fixed, completed, or hardened before this protocol
is published. Items are grouped by severity/area; within each group the
order is priority-first.

---

## 🔴 CRITICAL — Protocol Correctness Bugs

### 1. Ordered Reassembly: Out-of-Order Blocks Buffered By Value, Not By Position (Double-Copy Bug)
**File:** [`include/fuse/proto/reassembly.hpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/include/fuse/proto/reassembly.hpp) — `Slot::payload[kMaxPayloadSize]`  
**File:** [`src/proto/reassembly.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/reassembly.cpp) — `on_block()`

When an out-of-order block arrives on an ORDERED stream, the current code copies it into a `Slot` ring buffer and copies it again when it is finally delivered. But because `seq_no` and `block_size` are already known on arrival, the final `sink` offset (`seq_no * block_size`) is also known immediately — the block can be written directly in place the first time. The fix: strip `payload[]` from `Slot` (saving 1 MB per stream), write to sink on arrival, and use the `Slot` only as a presence flag.

**Impact:** Unnecessary ~1 MB per-stream memory overhead, 2× memcpy cost for every out-of-order block.

---

### 2. 64-Block Window Cap: Throughput Collapses on Any Real-RTT Path
**File:** [`include/fuse/proto/wire.hpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/include/fuse/proto/wire.hpp) — `kMaxWindow = 64`  
**File:** [`include/fuse/proto/receiver.hpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/include/fuse/proto/receiver.hpp) — single `uint64` bitmask

The entire receive window is one `uint64`. At 50 ms RTT the hard ceiling is `64 blocks × block_size / RTT ≈ 40 MB/s` — about 3% of Fuse's loopback peak. Any real WAN deployment will hit this immediately. The fix requires a wider ACK format: either a run-length-encoded (RLE) range or a multi-word bitmask. This requires a **wire-format change** and is the single highest-value protocol change remaining.

**Impact:** Renders the protocol functionally useless over any link with > ~5 ms RTT.

---

### 3. Stale `meta` Vector Indexed by `seq_no` Causes Silent Data Corruption
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/transfer.cpp) — lines 311–344 and 411, 418, 462, 468

The sender stores `{offset, len}` in a `std::vector<pair<uint64_t,uint16_t>> meta`, growing it dynamically with `meta.resize(next_seq + 1)` on every new block. When a retransmit NACK arrives for a `seq_no >= meta.size()` — possible if the vector has not yet grown to cover a just-sent block race — the `seq_no >= meta.size()` guard skips the retransmit silently. But the `reg.lookup(seq)` already verified the slot exists, so the guard is inconsistent: it discards valid retransmit requests. Additionally, on high lane counts the `meta` vector can grow to hundreds of thousands of entries, one `pair` per block, burning heap on the data path.

**Fix:** Derive offset from the registry slot's `seq_no × block_size` (the offset is already stored in `RegistrySlot` via `send_time_ns` — but actually the offset is not stored in `RegistrySlot` at all). Either add `offset` to `RegistrySlot` or compute it from `seq_no × block_size`. Remove the `meta` vector entirely.

---

### 4. Race Condition: `StreamStart` Handshake Has No Synchronization
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/transfer.cpp) — `send_lane()` lines 282–294

The sender retries `StreamStart` up to 200 times in a busy loop with a 200 µs socket timeout, then treats "received any ACK" as confirmation that the receiver is ready. But the receiver might be sending a stale ACK from a previous session or a different lane. There is no session ID or nonce in `StreamStart`, so a late ACK from a prior transfer can bypass this handshake. The fix: add a session ID (or nonce) to `StreamStart` and to the confirming ACK.

---

### 5. Retransmit on RTO Resets `last_progress` Even on Failed Send
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/transfer.cpp) — lines 483–492

When the RTO fires and `sock.send_to()` returns `false` (buffer full), the code sleeps 200 µs then sets `last_progress = t` on the else branch. But this resets the RTO clock, so if the send keeps failing (e.g., the kernel buffer stays full) the timeout can never re-fire. The transfer will spin without making progress and without timing out — a potential infinite hang under sustained congestion.

---

### 6. Nonce Exhaustion Not Detected in `LaneCipher`
**File:** [`src/proto/session_crypto.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/session_crypto.cpp) — `build_nonce()`

The nonce is `(lane:2 | seq_no:8 | 0:2)`. AES-GCM fails catastrophically if the same `(key, nonce)` pair is ever reused. The protocol relies on `seq_no` being monotonically unique per lane per session. But there is no enforcement of this — a caller could reuse a `seq_no` (e.g., after a session reset without changing the salt) and silently break the AEAD guarantee. The code has no nonce counter check or sequence exhaustion guard.

---

### 7. `auth_failures` Never Triggers `TransferStatus::AuthFailed`
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/transfer.cpp) — `recv_lane()` lines 170–172, `worst()` lines 513–519

Authentication failures (`auth_failures`) are counted but never acted on. The `TransferStatus::AuthFailed` status exists in the enum and in `to_string()` but is never set anywhere. A receiver under an active MITM attack that corrupts every block will count failures, complete with `status = Incomplete`, and return the wrong status. The fix: if `auth_failures` exceeds some threshold (e.g., > 50% of received blocks), set `status = AuthFailed`.

---

## 🟠 HIGH — Architecture / Design Gaps

### 8. Worker Pool Does Not Drive the Data-Plane Send Loop
**File:** [`src/proto/worker.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/worker.cpp) — `Worker::run()`

The `Worker` thread produces synthetic test blocks into a registry but does not own or call the actual socket send path. The real send loop (`send_lane()`) lives entirely in `transfer.cpp` and is completely disconnected from the `WorkerPool`. Stage 3's design promise — that workers own the data path, freeing the orchestrator to scale them — is not realized. The `Worker::run()` is essentially a stub.

---

### 9. `WorkerOrchestrator` Not Integrated with `WorkerPool` or Transfer Path
**File:** [`include/fuse/proto/orchestrator.hpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/include/fuse/proto/orchestrator.hpp)  
**File:** [`include/fuse/proto/worker.hpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/include/fuse/proto/worker.hpp)

`WorkerOrchestrator` runs a caller-supplied `WorkerTask` lambda. But nowhere in the production code is this wired up to the actual send/receive work in `transfer.cpp`. It is only exercised by its own unit tests. The orchestrator's scaling loop, load tracking, and `least_loaded_core` logic are real and correct, but they manage phantom tasks.

---

### 10. Congestion Controller Not Consulted by Worker Pool
**File:** [`include/fuse/proto/congestion.hpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/include/fuse/proto/congestion.hpp)

The `CongestionController` is correctly wired into `send_lane()` in `transfer.cpp` and the file benchmark. But the `WorkerPool` and `Worker` classes have zero knowledge of it. When the worker pool is eventually unified with the send path (item 8), the congestion window must gate the worker's send queue depth — otherwise the AIMD control is bypassed entirely for that path.

---

### 11. DTLS End-to-End Lifecycle Not Tested
**File:** [`include/fuse/proto/dtls.hpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/include/fuse/proto/dtls.hpp)

The DTLS path (which provides forward secrecy via ECDHE-PSK) exists and its unit tests pass, but there is no end-to-end test that runs SETUP-inside-a-DTLS-tunnel and then transfers data. The `DTLS covers a link, not yet the full session lifecycle` gap from the ROADMAP is unresolved.

---

### 12. `PeerAddr` Is IPv4-Only; No IPv6 Support
**File:** [`include/fuse/proto/udp.hpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/include/fuse/proto/udp.hpp) — `struct PeerAddr { sockaddr_in addr; }`  
**File:** [`src/proto/udp.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/udp.cpp) — `socket(AF_INET, ...)`

The entire socket layer is hard-coded to `AF_INET` / `sockaddr_in`. IPv6 is completely unsupported. This is a pre-publication blocker for any modern deployment.

---

### 13. `SenderRegistry` Stores Full Payload Copy Per Slot (1 MB+ per Lane)
**File:** [`include/fuse/proto/registry.hpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/include/fuse/proto/registry.hpp) — `uint8_t payload[kMaxPayloadSize]`

Each `RegistrySlot` embeds a `uint8_t payload[16384]` — a full copy of the block. With `kMaxWindow = 64`, one `SenderRegistry` consumes `64 × 16384 = 1 MB`. Since the source buffer passed to `send_buffer()` is immutable for the transfer's duration, the registry could hold a `(const uint8_t*, len)` pointer instead, eliminating an entire copy on the send path. The ROADMAP lists this as a known improvement.

---

### 14. Static Sharding with No Work-Stealing Causes Straggler Lanes
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/transfer.cpp) — `send_buffer()`, `receive_buffer()`

All lanes are statically sized at the start (`shard = (len + lanes - 1) / lanes`). Under 5% loss, one unlucky lane can stall the entire transfer (measured range: 27–226 MB/s). There is no mechanism for healthy lanes to pick up work from a straggling lane. The orchestrator was designed for exactly this but is not wired in.

---

### 15. No Session Identifier — Multiple Concurrent Transfers Collide
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/transfer.cpp)

Two simultaneous `send_buffer()` calls targeting the same host/port/lane combination will have their traffic mixed by the receiver, which has no way to distinguish them. There is no session or connection ID in any data-plane header. This makes concurrent multi-transfer scenarios on the same port range fundamentally broken.

---

## 🟡 MEDIUM — Reliability / Safety Issues

### 16. `send_file` Reads the Entire File Into RAM
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/transfer.cpp) — `send_file()` lines 627–634

`send_file()` calls `send_buffer()` on a `std::vector` holding the complete file. For a 100 GB file this requires 100 GB of RAM. The API should use `mmap` or stream the file in chunks aligned to shard boundaries.

---

### 17. `recv_batch` Only Captures the First Sender Address
**File:** [`src/proto/udp.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/udp.cpp) — `recv_batch()` line 209–212

`recv_batch` fills `first_src` from `addrs[0]` only. All subsequent datagrams in the batch lose their source address. In the current single-sender design this is fine, but it prevents any future multi-sender or NAT-traversal work and is a silent invariant violation.

---

### 18. CPU Pinning Hurts Performance But Is Still Opt-In Without Warning
**File:** [`include/fuse/proto/worker.hpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/include/fuse/proto/worker.hpp) — `set_topology_hint()`  
**File:** [`include/fuse/proto/orchestrator.hpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/include/fuse/proto/orchestrator.hpp) — `OrchestratorConfig::pin_to_core`

Measurement shows CPU pinning consistently hurts throughput (~6% slower, higher latency). The option exists and is documented, but there is no runtime warning or deprecation notice when it is enabled. A user that enables it on the basis of intuition will silently regress performance.

---

### 19. `receive_buffer` Shard Ordering Assumes Network Delivery Order
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/transfer.cpp) — `receive_buffer()` lines 615–619

Receiver shards are assembled by iterating `shards` in order (lane 0, 1, 2, …) and concatenating into `*out`. This is correct, but there is no check that each lane's shard completed successfully before concatenation. A timeout on one lane produces a zero-length shard, silently corrupting the output while `worst()` only catches the error code after the fact.

---

### 20. No Graceful Close on the Transfer API
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/transfer.cpp)

The sender sends 0 final ACKs and simply closes its socket after the loop. The receiver sends 8 stray ACKs and then closes. There is no `Close` message handshake on the transfer path (only the SDK layer has `MsgType::Close`). If the receiver's final 8 ACKs are dropped, the sender has no way to know the receiver got everything.

---

### 21. `decode_heartbeat` and `decode_ack` Use Strict Length Checks That Break on Extensions
**File:** [`src/proto/aux.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/aux.cpp) — lines 47, 99

Both decoders use `in_len != expected_size` (strict equality). Any future extension that appends optional fields to these messages will break existing decoders before versioning can be updated. The check should be `in_len < minimum_size` to allow forward compatibility.

---

### 22. `worst()` Picks the Last Non-Ok Status, Not the Most Severe
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5/Computer%20Networks/project/fuse-nw/src/proto/transfer.cpp) — `worst()` lines 513–519

`worst()` iterates lanes and overwrites `st` with any non-`Ok` status. If lane 0 returns `Timeout` and lane 1 returns `Incomplete`, the caller gets `Incomplete` — which is less actionable than `Timeout`. The function should return the most severe status according to a defined severity ordering.

---

### 23. NUMA Placement Is Unverified and May Be Dead Code
**File:** [`src/proto/topology.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5\Computer Networks\project\fuse-nw\src\proto\topology.cpp)

The `prefer_current_thread_numa_node()` path compiles when `libnuma` headers are absent and silently becomes a no-op. The `numastat` locality test in the Stage 6 acceptance criteria has never been run. The code ships as tested when it has not been.

---

## 🔵 LOW — Code Quality / Documentation Gaps

### 24. `send_lane` Has Cohesion Score 0.05 (Graphify Flag)
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5\Computer Networks\project\fuse-nw\src\proto\transfer.cpp) — `send_lane()`

`send_lane()` is a 270-line monolithic function performing handshake, GSO batching, retransmit on NACK, retransmit on RTO, block-size adaptation, encryption, and loop termination. It should be decomposed into focused helper functions to make the control flow auditable.

### 25. 195 Isolated Nodes in Graphify Knowledge Graph
**Graphify Report:** `graphify-out/GRAPH_REPORT.md` — Knowledge Gaps section

195 nodes (`bulk_bytes`, `telemetry_msgs`, etc.) have ≤1 connection in the knowledge graph, indicating either undocumented relationships or dead/abandoned code paths. These should be reviewed and either connected or removed.

### 26. `recv_batch` Stack Allocates `mmsghdr[64]` and `iovec[64]` on Every Call
**File:** [`src/proto/udp.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5\Computer Networks\project\fuse-nw\src\proto\udp.cpp) — `recv_batch()` lines 184–186

`mmsghdr msgs[64]` + `iovec iovs[64]` + `sockaddr_in addrs[64]` stack-allocates ~10 KB on every call in the hot receive loop. These should be pre-allocated (member or caller-owned).

### 27. README Links to `bench/RESULTS.md` Inconsistently With ROADMAP
**File:** [`README.md`](file:///C:/Users/dines/OneDrive/Documents/sem5\Computer Networks\project\fuse-nw\README.md) — line 279

README line 279 says `"Current results and their caveats are in docs/ROADMAP.md"` for the benchmark output, but the actual results are in `bench/RESULTS.md`. The link is wrong.

### 28. No Windows / macOS Build Support
**File:** [`CMakeLists.txt`](file:///C:/Users/dines/OneDrive/Documents/sem5\Computer Networks\project\fuse-nw\CMakeLists.txt) — line 43

The build system emits a warning for non-Linux but proceeds. `sendmmsg`, `recvmmsg`, and `UDP_SEGMENT` are Linux-only. The build will silently produce a non-functional binary on macOS/Windows. Either gate these behind `#ifdef __linux__` with a portable fallback, or make the CMake configuration fail fast on non-Linux.

### 29. Python Package Name Mismatch (`fuse-transport` vs `fuse`)
**File:** [`README.md`](file:///C:/Users/dines/OneDrive/Documents/sem5\Computer Networks\project\fuse-nw\README.md), [`docs/SDK.md`](file:///C:/Users/dines/OneDrive/Documents/sem5\Computer Networks\project\fuse-nw\docs\SDK.md)

The PyPI package is called `fuse-transport` but the module is imported as `import fuse`. This is a non-obvious split that confuses users. It is explained in SDK.md but not in the README quickstart.

### 30. `kMaxAuxDatagramSize` Is Used but Not Defined in Any Public Header
**File:** [`src/proto/transfer.cpp`](file:///C:/Users/dines/OneDrive/Documents/sem5\Computer Networks\project\fuse-nw\src\proto\transfer.cpp) — line 96, 269

`kMaxAuxDatagramSize` appears in `transfer.cpp` but is not visible in `wire.hpp` or `aux.hpp`. It is presumably defined somewhere internal, but the public API docs refer to it without defining it clearly for downstream consumers.

---

## Summary Table

| # | Severity | Area | One-line description |
|---|---|---|---|
| 1 | 🔴 Critical | Reassembly | Out-of-order blocks double-copied instead of written direct to sink |
| 2 | 🔴 Critical | Protocol | 64-block window kills throughput on any real WAN path |
| 3 | 🔴 Critical | Transfer | `meta` vector inconsistency can silently drop retransmits |
| 4 | 🔴 Critical | Handshake | `StreamStart` has no session ID; stale ACKs can bypass it |
| 5 | 🔴 Critical | Transfer | RTO failure path can hang transfer indefinitely |
| 6 | 🔴 Critical | Crypto | No nonce exhaustion / sequence reuse guard in `LaneCipher` |
| 7 | 🔴 Critical | Crypto | `AuthFailed` status is never set; MITM goes unreported |
| 8 | 🟠 High | Architecture | Worker send loop is a stub; real path lives in transfer.cpp |
| 9 | 🟠 High | Architecture | Orchestrator not wired to any production work |
| 10 | 🟠 High | Architecture | Congestion controller not consulted by WorkerPool |
| 11 | 🟠 High | Testing | DTLS full session lifecycle has no end-to-end test |
| 12 | 🟠 High | Portability | IPv4-only socket layer; no IPv6 |
| 13 | 🟠 High | Memory | Registry stores full payload copy; should be a pointer |
| 14 | 🟠 High | Performance | Static sharding with no work-stealing causes straggler lanes |
| 15 | 🟠 High | Protocol | No session ID; concurrent transfers on same port collide |
| 16 | 🟡 Medium | Resource | `send_file` reads entire file into RAM |
| 17 | 🟡 Medium | Correctness | `recv_batch` loses all but first sender address |
| 18 | 🟡 Medium | UX | CPU pinning known to hurt, no runtime warning |
| 19 | 🟡 Medium | Correctness | Failed shard silently corrupts reassembled output |
| 20 | 🟡 Medium | Protocol | No graceful close on the transfer API |
| 21 | 🟡 Medium | Extensibility | Strict-equality length checks break on wire extensions |
| 22 | 🟡 Medium | UX | `worst()` returns last error, not most severe |
| 23 | 🟡 Medium | Testing | NUMA placement untested and possibly dead code |
| 24 | 🔵 Low | Code quality | `send_lane()` monolith (270 lines, cohesion 0.05) |
| 25 | 🔵 Low | Docs | 195 isolated nodes in knowledge graph |
| 26 | 🔵 Low | Performance | `recv_batch` stack-allocates 10 KB on every call |
| 27 | 🔵 Low | Docs | README points to ROADMAP for results; should point to RESULTS.md |
| 28 | 🔵 Low | Portability | Non-Linux builds silently produce broken binaries |
| 29 | 🔵 Low | UX | PyPI name `fuse-transport` vs `import fuse` not explained in README |
| 30 | 🔵 Low | Docs | `kMaxAuxDatagramSize` used but not defined in any public header |
