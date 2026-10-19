# Fuse wire protocol (as built, version 7)

This is the on-the-wire format that the code in this repo actually speaks
today (`kProtocolVersion = 7`, `include/fuse/proto/wire.hpp`). `BUILD.md`
is the original staged design plan. Where the two disagree, this file and
the code are what's current. Known weaknesses in this format are listed in
[ROADMAP.md](ROADMAP.md#known-protocol-issues) rather than here.

All multi-byte integers are big-endian. Sizes are in bytes.

## 1. Outer header

Every datagram starts with:

| field | size | notes |
|---|---|---|
| `version` | 1 | must equal 7; anything else is dropped silently (no error reply) |
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
| `send_time_ns` | 8 | sender's monotonic clock, echoed in ACKs for RTT |
| payload | `payload_len` | |

Block size starts at 1200 (`kDefaultPayloadSize`). The transfer API doubles
it after 8 clean ACK-advances, up to 16384, and drops back to the
configured size on any NACK. The SDK always uses 1200. The mux transfer
uses whatever `block_size` the caller configured.

### ACK (156 bytes)

| field | size | notes |
|---|---|---|
| outer header | 2 | |
| `stream_id` | 2 | |
| `base_seq_no` | 8 | every seq below this has been received |
| `received_bitmask` | 16 × 8 | bit *i* = `base_seq_no + i` received (1024-block window) |
| `echoed_send_time` | 8 | `send_time_ns` of the highest-seq block accepted |
| `nonce` | 8 | echoes the `StreamStart` nonce (0 in the SDK, which has none) |

Senders currently act only on `base_seq_no` and `echoed_send_time`. The
bitmask is carried but not used for selective acknowledgement.

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

### HEARTBEAT (12 bytes, unused)

`stream_id:2, highest_seq_no:8`. Nothing sends this today.

## 3. Bulk transfer (`fuse/transfer.hpp`)

One UDP port per lane (`base_port + lane`), one independent stream per
lane, no session-level handshake tying lanes together.

```
sender                                   receiver (lane i)
  StreamStart(nonce)          ───►        (first valid one claims the lane)
                              ◄───        ResumeRanges(nonce)[parts]  + Ack(nonce)
  Data ...                    ───►
                              ◄───        Ack / Nack
  [resumed only] FileDigest   ───►
                              ◄───        FileDigest (echo = acknowledgement)
  StreamClose(finished)       ───►
```

StreamStart is retried with backoff (1 ms → 200 ms) until both an `Ack`
and every `ResumeRanges` part carrying the same nonce have arrived, bounded
by `connect_timeout_ms`.

### STREAM_START (70 bytes)

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
| `file_id` | 8 | hash of name+size+mtime, 0 = not resumable (v7) |

Receivers validate `total_bytes ≤ file_total_bytes` and
`stream_base_offset ≤ file_total_bytes − total_bytes` before using either,
and every lane must agree on `file_total_bytes`.

### RESUME_RANGES (19 + 16·count bytes)

`stream_id:2, nonce:8, flags:1 (0x01 = resumed), part:2, parts:2, count:2`,
then `count` × `(offset:8, length:8)` stream-relative ranges, ≤ 64 per
datagram. A fresh transfer answers with one range covering the whole lane.
The receiver coarsens its list to at most 4096 ranges per lane.

### STREAM_CLOSE (13 bytes)

`stream_id:2, nonce:8, reason:1` (`0` finished, `1` aborted). Sent three
times, unacknowledged. Before a lane has a session, a receiver accepts an
`aborted` close with any nonce.

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

Under PSK, only the server proves key possession (via `proof`). The client
proves nothing until its first DATA block authenticates.

## 5. Multiplexed transfer (`fuse/mux_transfer.hpp`)

One socket, `num_streams` streams, preceded by the SETUP handshake. No
encryption.

- **SETUP_DATA**: `protocol_version:1, num_workers:2, num_streams:2`, then
  per stream `stream_id:2, worker_id:2, flags:1 (0x01 LOSSLESS, 0x02
  ORDERED, 0x04 COALESCE), block_size:2, window_size:2` (≤ 64 streams).
  The mux transfer always sends `num_workers = 1`, `worker_id = 0`, and
  LOSSLESS only.
- **SETUP_HASH_REPLY / SETUP_FINACK** (10 bytes): `hash:8`, the FNV-1a-64
  of the SETUP body exactly as the responder received it.

Each stream then runs its own `StreamStart`/Data/Ack/Nack exchange as in
§3, without resume, digest or `StreamClose`.

## 6. Encryption (PSK mode)

- **Key**: HKDF-SHA256. Extract with `salt` = the 16-byte StreamStart salt
  (transfer) or `salt_c ‖ salt_s` (SDK), and IKM = the PSK. Then
  Expand-Label with prefix `"fuse2 "`, label `"session key"`, empty
  context, 32 bytes. Each session gets its own key. There is **no forward
  secrecy**.
- **AEAD**: AES-256-GCM. Nonce = `stream_id:2 ‖ seq_no:8 ‖ 0x0000`.
  AAD = `stream_id:2 ‖ seq_no:8 ‖ offset:8 ‖ flags:1`. The 16-byte tag is
  appended to the ciphertext, inside `payload_len`.
- **What is protected**: only DATA payloads and the headers bound into the
  AAD. Every control message (StreamStart, Ack, Nack, StreamClose,
  ResumeRanges, FileDigest, Hello, Close, SETUP) is plaintext and
  unauthenticated in every mode.
- **DTLS** (`fuse/proto/dtls.hpp`, DTLS 1.2 with ECDHE-PSK, which does give
  forward secrecy) exists as a standalone primitive. It is exercised by the
  tests and by `fuse_bench`, but none of the three transports above uses it.
