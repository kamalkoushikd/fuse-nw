# Fuse wire protocol (as built, version 8)

This is the on-the-wire format that the code in this repo actually speaks
today (`kProtocolVersion = 8`, `include/fuse/proto/wire.hpp`). `BUILD.md`
is the original staged design plan. Where the two disagree, this file and
the code are what's current. Known weaknesses in this format are listed in
[ROADMAP.md](ROADMAP.md#known-protocol-issues) rather than here.

All multi-byte integers are big-endian. Sizes are in bytes.

## 1. Outer header

Every datagram starts with:

| field | size | notes |
|---|---|---|
| `version` | 1 | must equal 8 |
| `msg_type` | 1 | see table below |

| value | type | sent by | used by |
|---|---|---|---|
| 0 | `Data` | data sender | all transports |
| 1 | `Heartbeat` | — | defined and encodable, but **no transport sends it** |
| 2 | `Ack` | data receiver | all transports |
| 3 | `Nack` | data receiver | all transports |
| 4 | `SetupData` | initiator | mux transfer only |
| 5 | `SetupHashReply` | responder | mux transfer only |
| 6 | `SetupFinAck` | initiator | mux transfer only |
| 7 | `StreamStart` | data sender | transfer, mux transfer |
| 8 | `Hello` | SDK client | SDK (`fuse/sdk.h`) |
| 9 | `HelloAck` | SDK server | SDK |
| 10 | `Close` | either SDK peer | SDK |
| 11 | `StreamClose` | either transfer peer | transfer |
| 12 | `ResumeRanges` | transfer receiver | transfer |
| 13 | `FileDigest` | transfer sender (echoed by receiver) | transfer (resumed files only) |
| 14 | `Retry` | transfer receiver | transfer (v8, address validation) |
| 15 | `PathChallenge` | either transfer peer | transfer (v8, path validation) |
| 16 | `PathResponse` | either transfer peer | transfer (v8, path validation) |

### Version mismatch (v8)

A datagram whose `version` is not ours is not interpreted further. Instead
the transfer and mux receivers answer it (at most once per 100 ms) with a
3-byte **version-mismatch reply**:

| field | size | notes |
|---|---|---|
| `version` | 1 | the replier's own version |
| marker | 1 | `0xFF` |
| `version` | 1 | the replier's version again |

This layout is fixed independently of `kProtocolVersion`, so any version
from 8 on recognises it whatever version it speaks. It is never answered
itself. A sender that receives one returns `TransferStatus::VersionMismatch`
instead of timing out. (Peers older than v8 still just drop our datagrams.)

### Extensions (v8)

`StreamStart` and `SETUP_DATA` end in an extension area:

| field | size |
|---|---|
| `ext_len` | 2 (≤ 256) |
| entries | `ext_len`: repeated `type:2, len:2, value:len` |

A receiver skips entry types it does not know, but refuses the whole message
if the area is malformed or holds an unknown **critical** type (top bit set,
`type ≥ 0x8000`). No types are defined yet; this is how optional fields get
added in future without another version bump.

## 2. Data plane

### DATA (31-byte prefix + payload)

| field | size | notes |
|---|---|---|
| outer header | 2 | |
| `stream_id` | 2 | lane / stream / SDK direction |
| `seq_no` | 8 | per-stream, starts at 0, +1 per block |
| `flags` | 1 | `0x01` retransmission, `0x02` last block of the stream/message |
| `payload_len` | 2 | ≤ 16384 (`kMaxPayloadSize`); includes the AEAD tag when encrypted |
| `offset` | 8 | byte position of this payload within the stream/message (v2) |
| `send_time_ns` | 8 | sender's clock, echoed in ACKs for RTT (see below) |
| payload | `payload_len` | |

`send_time_ns` is opaque to the receiver, which only echoes it. Since v8 the
transfer and mux senders put `monotonic clock + a random per-stream offset`
there, so the wire does not reveal the host's uptime. RTT is the wrap-safe
difference in that offset domain. (The SDK still sends its raw clock.)

Block size starts at the configured `block_size` (1200 by default). The
transfer API doubles it after 8 clean ACK-advances, never past what one
datagram of the route MTU holds (as the kernel reports it), and drops back
on any NACK. A loss right after a growth step makes the previous size a
ceiling, re-probed after a clean stretch that doubles with each failed
probe. The SDK always uses 1200; the mux transfer uses its configured size.

### ACK (160 bytes)

| field | size | notes |
|---|---|---|
| outer header | 2 | |
| `stream_id` | 2 | |
| `base_seq_no` | 8 | every seq below this has been received |
| `received_bitmask` | 16 × 8 | bit *i* = `base_seq_no + i` received (1024-block window) |
| `echoed_send_time` | 8 | `send_time_ns` of the highest-seq block accepted |
| `nonce` | 8 | echoes the `StreamStart` nonce (0 in the SDK, which has none) |
| `rwnd` | 4 | v8: blocks the receiver can hold now; `0xFFFFFFFF` = no limit |

The transfer and mux senders act on the bitmask (selective acknowledgement):
SACKed blocks are released from the retransmit registry and never resent. On
a retransmission timeout they resend the base block and up to seven further
holes below the highest block the receiver holds. The transfer sender keeps
its bytes in flight within `rwnd` × block size; its receiver advertises the
free space in its writer rings plus the blocks being written, and sends a
window-update ACK as soon as a low window reopens. The SDK ignores `rwnd`.

### NACK (6 + 8·count bytes, max 8198)

| field | size |
|---|---|
| outer header | 2 |
| `stream_id` | 2 |
| `count` | 2 (≤ 1024) |
| `missing_seq_no` | 8 × count |

A receiver NACKs a gap only after it has persisted for a reorder delay
(≈ RTT/2, 3 ms before any RTT sample) and re-NACKs no sooner than ≈ 1.5 RTT
(40 ms before any sample). Streams flagged non-LOSSLESS never NACK.

### Retransmission timeout

Transfer and mux: `max(SRTT + 4·RTTVAR, 2·SRTT)` with at least 1 ms of
variance allowance, clamped to [5 ms, 1 s], 1 s before any sample, and doubled
for each consecutive timeout (up to 64×). Only the first timeout of a run
halves the congestion window.

### HEARTBEAT (12 bytes, unused)

`stream_id:2, highest_seq_no:8`. Nothing sends this today.

## 3. Bulk transfer (`fuse/transfer.hpp`)

One UDP port per lane (`base_port + lane`, which must not exceed 65535), one
independent stream per lane, no session-level handshake tying lanes together.

```
sender                                   receiver (lane i)
  StreamStart(nonce, token=0)  ───►
                               ◄───      Retry(nonce, token)        (nothing else yet)
  StreamStart(nonce, token)    ───►      (first valid one claims the lane)
                               ◄───      ResumeRanges(nonce)[parts]  + Ack(nonce)
  Data ...                     ───►
                               ◄───      Ack / Nack
  [resumed only] FileDigest    ───►
                               ◄───      FileDigest (echo = acknowledgement)
  StreamClose(finished)        ───►
```

**Address validation (v8).** A StreamStart whose `token` doesn't match gets
only a 20-byte `Retry` carrying
`token = SipHash-2-4(per-lane secret, source address ‖ nonce)`. Nothing is
allocated or claimed until a StreamStart echoes it, so a spoofed source
address can neither take the lane nor use the receiver as an amplifier.
After a session exists, timer-driven ACKs stop after three to a peer that has
gone silent.

**Path validation (v8).** When session traffic arrives from a new source
address (a NAT rebinding, say), the receiving side sends a `PathChallenge`
with a random 64-bit value there, and switches its peer address only when a
matching `PathResponse` comes back from it. Data from the new source is
accepted meanwhile.

StreamStart is retried with backoff (1 ms → 200 ms) until both an `Ack`
and every `ResumeRanges` part carrying the same nonce have arrived, bounded
by `connect_timeout_ms`. Lane nonces are random 64-bit values from a secure
source.

### STREAM_START (80 + ext bytes)

| field | size | notes |
|---|---|---|
| outer header | 2 | |
| `stream_id` | 2 | |
| `total_blocks` | 8 | always 0 from the transfer API (block size adapts) |
| `block_size` | 2 | initial block size |
| `total_bytes` | 8 | this lane's shard length |
| `session_salt` | 16 | all-zero = unencrypted |
| `nonce` | 8 | random per lane per attempt (v3) |
| `stream_base_offset` | 8 | where the shard sits in the whole file (v6) |
| `file_total_bytes` | 8 | whole transfer size (v6) |
| `file_id` | 8 | hash of name, size, mtime, ctime, inode, device; 0 = not resumable (v7, widened in v8) |
| `token` | 8 | address-validation token from the receiver's `Retry`; 0 on the first attempt (v8) |
| `ext_len` + extensions | 2 + n | see §1 (v8) |

Receivers validate `total_bytes ≤ file_total_bytes` and
`stream_base_offset ≤ file_total_bytes − total_bytes` before using either,
and every lane must agree on `file_total_bytes`.

### RETRY (20 bytes, v8)

`stream_id:2, nonce:8, token:8`. Unauthenticated; a forged one can only make
the token wrong, and the receiver then retries.

### PATH_CHALLENGE / PATH_RESPONSE (20 bytes each, v8)

`stream_id:2, nonce:8 (the lane nonce), challenge:8`. The response echoes the
challenge value.

### RESUME_RANGES (19 + 16·count bytes)

`stream_id:2, nonce:8, flags:1 (0x01 = resumed), part:2, parts:2, count:2`,
then `count` × `(offset:8, length:8)` stream-relative ranges, ≤ 64 per
datagram. A fresh transfer answers with one range covering the whole lane.
The receiver coarsens its list to at most 4096 ranges per lane.

### STREAM_CLOSE (13 bytes)

`stream_id:2, nonce:8, reason:1`:

| reason | meaning |
|---|---|
| 0 | finished (sender: every byte acknowledged) |
| 1 | aborted (either side gave up) |
| 2 | auth failed (v8, receiver: the StreamStart did not authenticate; sent unauthenticated, rate-limited) |

Sent three times, unacknowledged. Before a lane has a session, a plaintext
receiver accepts an `aborted` close with any nonce; under a PSK it can't be
verified yet, so it is ignored.

### FILE_DIGEST (44 bytes)

`stream_id:2, nonce:8, digest:32`, lane 0 only, resumed transfers only. The
digest is SHA-256 over the list of per-64 MiB-segment SHA-256s plus the
length (`segmented_digest`). Builds without crypto use a non-cryptographic
FNV fallback.

### Resume state on disk

`receive_file` writes into `<path>.part`. Behind the data it keeps a trailer
of `[bitmap: 1 bit per chunk][footer: file_id, total, chunk_size, nchunks,
checksum (u64 each), "FUSERSM1"]`. Chunks are 64 KiB, doubled until there
are at most 8 M of them. The trailer is cut off before the final rename.

## 4. Socket-style SDK (`fuse/sdk.h`)

Unchanged by v8 except for the shared formats above (the ACK is 160 bytes;
its `rwnd` is not used). The SDK does not send `Retry`, path probes or the
control-plane trailer; see the roadmap for its pending fixes.

```
client (ephemeral port)        server listen port      server per-conn port
  Hello(salt_c, challenge) ───►  (allocates a conn + socket + thread)
                           ◄─────────────────────────── HelloAck(salt_s, proof)
  Data / Ack / Nack        ◄──────────────────────────► (client adopts this source)
  Close                    ◄──────────────────────────►
```

- **HELLO** (26 bytes): `salt_c:16, challenge:8`. The challenge is random
  under PSK and a timestamp otherwise.
- **HELLO_ACK** (42 bytes): `salt_s:16, proof:24`. Under PSK, `proof` is the
  challenge sealed with the session key (lane `0xFFFF`, seq 0, no AAD).
  Unencrypted, both fields are zero.
- **CLOSE** (2 bytes): outer header only.

Directions are separate streams: `stream_id` 0 is client→server and 1 is
server→client. Each `fuse_send()` is one message: a run of DATA blocks
whose `offset` is relative to the message, with `0x02` on the last. Only one
message is in flight per direction, because `fuse_send` returns after the
whole message is acknowledged. The maximum message size is 64 MiB.

## 5. Multiplexed transfer (`fuse/mux_transfer.hpp`)

One socket, `num_streams` streams, preceded by the SETUP handshake. No
encryption.

- **SETUP_DATA**: `protocol_version:1, num_workers:2, num_streams:2`, then
  per stream `stream_id:2, worker_id:2, flags:1 (0x01 LOSSLESS, 0x02
  ORDERED, 0x04 COALESCE), block_size:2, window_size:2` (≤ 64 streams), then
  `ext_len:2` + extensions (v8). The mux transfer always sends
  `num_workers = 1`, `worker_id = 0`, LOSSLESS only, and stream ids 0..n−1.
- **SETUP_HASH_REPLY / SETUP_FINACK** (18 bytes): `hash:8` (the FNV-1a-64 of
  the SETUP body exactly as the responder received it) and `responder_nonce:8`
  (v8). The responder draws the nonce fresh for every configuration it is
  offered; only a FINACK echoing it completes the handshake, so a recorded
  handshake cannot be replayed.
- The responder never confirms a SETUP whose `protocol_version` isn't ours,
  whose stream ids repeat, or whose block size or window is out of range;
  the mux receiver also requires ids 0..n−1.

Each stream then runs its own `StreamStart`/Data/Ack/Nack exchange as in
§3, without address validation, resume, digest or `StreamClose`.

## 6. Encryption (PSK mode, transfer API)

- **Key**: HKDF-SHA256. Extract with `salt` = the 16-byte StreamStart salt
  (transfer) or `salt_c ‖ salt_s` (SDK), and IKM = the PSK. Then
  Expand-Label with prefix `"fuse2 "`, label `"session key"`, empty
  context, 32 bytes. Each session gets its own key. There is **no forward
  secrecy**. The transfer API refuses PSKs shorter than 16 bytes; the key
  must be random bytes, not a password.
- **Data keys (v8)**: block `seq` of lane `l` is encrypted under
  `Expand-Label(session_key, "lane key", l:2 ‖ (seq >> 22):8)`, so each key
  protects at most 2²² blocks per lane. Both ends derive it independently.
- **AEAD**: AES-256-GCM. Nonce = `stream_id:2 ‖ seq_no:8 ‖ 0x0000`.
  AAD = `stream_id:2 ‖ seq_no:8 ‖ offset:8 ‖ flags:1`, where since v8 the
  transfer API clears the retransmission bit (`0x01`) in the AAD's copy of
  `flags`, so a retransmission is byte-for-byte the original ciphertext and
  tag. The 16-byte tag is appended to the ciphertext, inside `payload_len`.
- **Control plane (v8)**: every transfer control datagram (StreamStart, Ack,
  Nack, ResumeRanges, StreamClose, FileDigest, PathChallenge, PathResponse)
  carries a trailer `ctr:8 ‖ tag:16`: a GMAC over the whole datagram under
  `Expand-Label(session_key, "control key", "")`, nonce
  `stream_id:2 ‖ ctr:8 ‖ dir:1 ‖ 0x01` (`dir` 0 = data sender, 1 = receiver).
  Unsigned control messages are ignored. The StreamStart's tag confirms the
  key at the handshake. Exceptions, unauthenticated by necessity: `Retry`,
  the version-mismatch reply, and `StreamClose(reason = 2)`.
- **SDK**: unchanged by v8. Only DATA payloads are protected; its control
  messages remain unauthenticated.
- **DTLS** (`fuse/proto/dtls.hpp`, DTLS 1.2 with ECDHE-PSK, which does give
  forward secrecy) exists as a standalone primitive. It is exercised by the
  tests and by `fuse_bench`, but none of the three transports above uses it.
