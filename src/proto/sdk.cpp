// Implementation of the socket-style C API declared in <fuse/sdk.h>.
//
// Shape of a connection:
//
//   * one UDP socket, one peer, a monotonic block sequence number;
//   * a background "pump" thread per connection that does all the wire
//     work — receiving, reassembling, acknowledging, retransmitting — so
//     that a blocked fuse_recv() still ACKs, and a blocked fuse_send()
//     still repairs loss;
//   * fuse_send()/fuse_recv() are thin: they hand a message to, or take a
//     message from, that thread through a mutex + condition variable.
//
// Message framing rides on the existing block header: a message is a run
// of blocks whose `offset` is the byte position within that message, with
// kFlagLastBlock on the final one. The receiver knows the message is whole
// when the receive window's contiguous base has advanced past that final
// block — no length prefix needed, and a truncated message can never be
// handed to the caller.

#include "fuse/sdk.h"

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "fuse/proto/block.hpp"
#include "fuse/proto/congestion.hpp"
#include "fuse/proto/control.hpp"
#include "fuse/proto/path_validator.hpp"
#include "fuse/proto/receiver.hpp"
#include "fuse/proto/registry.hpp"
#include "fuse/proto/session_crypto.hpp"
#include "fuse/proto/udp.hpp"
#include "fuse/version.h"

using namespace fuse::proto;

namespace {

constexpr uint16_t kWindow = kMaxWindow;             // blocks in flight
constexpr uint16_t kBlockSize = kDefaultPayloadSize; // 1200: MTU-safe
constexpr size_t kMaxMessage = 64u << 20;            // 64 MiB per message
// A fast sender and a slow-reading application otherwise have no backpressure
// between them: inbox is an unbounded deque, and Acks go out regardless of
// how much sits in it undrained. This caps total queued-but-undelivered
// bytes; once hit, new blocks are simply not admitted (see
// handle_data_locked), so the sender's own NACK/RTO retry naturally stalls
// until fuse_recv/fuse_recv_alloc drains room.
constexpr size_t kMaxInboxBytes = 256u << 20; // 256 MiB undrained
constexpr size_t kRxBatch = 32;
constexpr uint32_t kDefaultTimeoutMs = 10000;

// The two directions of a connection must never share an AEAD nonce, and
// both sides start their sequence numbers at zero — so each direction gets
// its own lane id, which is part of the nonce.
constexpr uint16_t kLaneClientToServer = 0;
constexpr uint16_t kLaneServerToClient = 1;

// Control datagrams (Ack, Nack, Close, Heartbeat, path probes) are signed
// under a PSK with ControlAuth (session_crypto.hpp), on this lane id; `dir`
// in its nonce separates the two ends (client 0, server 1).
constexpr uint16_t kControlLane = 0xFFFE;
constexpr uint8_t kDirClient = 0;
constexpr uint8_t kDirServer = 1;

// Liveness (ROADMAP #12): an otherwise idle connection sends a Heartbeat
// every kHeartbeatNs, and a peer heard from for no longer than
// max(timeout_ms, kMinDeadPeerNs) is declared lost, so a blocked
// fuse_recv(-1) or fuse_send returns FUSE_ERR_TIMEOUT instead of waiting
// forever on a peer that vanished without a Close.
constexpr uint64_t kHeartbeatNs = 1'000'000'000ull;
constexpr uint64_t kMinDeadPeerNs = 3'000'000'000ull;

constexpr size_t kChallengeLen = 8;
constexpr size_t kProofLen = kChallengeLen + kAeadTagLen; // sealed challenge

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

void set_rcv_timeout_ms(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void set_bufs(int fd, int bytes) {
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

// Associated data binds a block to its position AND its flags so neither
// can be tampered with — without `flags` here, LastBlock is forgeable on an
// otherwise-authentic block even with encryption on, since flags rides
// outside the AEAD envelope on the wire.
//
// The retransmission bit is deliberately left out (ROADMAP #1, SDK half). A
// retransmit re-seals the same plaintext under the same (key, nonce); if the
// AAD differed between the original and the resend, the two tags would let
// an observer solve for GCM's authentication key and forge blocks. With the
// bit masked, a resend is byte-for-byte the original ciphertext and tag.
size_t build_aad(uint8_t *out, uint16_t lane, uint64_t seq, uint64_t offset, uint8_t flags) {
    size_t n = 0;
    n += put_u16(out + n, lane);
    n += put_u64(out + n, seq);
    n += put_u64(out + n, offset);
    n += put_u8(out + n, static_cast<uint8_t>(flags & ~kFlagRetransmission));
    return n;
}

size_t encode_hello(const uint8_t salt[kSessionSaltLen],
                    const uint8_t challenge[kChallengeLen], uint8_t *out) {
    size_t off = 0;
    off += put_u8(out + off, kProtocolVersion);
    off += put_u8(out + off, static_cast<uint8_t>(MsgType::Hello));
    std::memcpy(out + off, salt, kSessionSaltLen);
    off += kSessionSaltLen;
    std::memcpy(out + off, challenge, kChallengeLen);
    off += kChallengeLen;
    return off;
}

bool decode_hello(const uint8_t *in, size_t len, uint8_t salt[kSessionSaltLen],
                  uint8_t challenge[kChallengeLen]) {
    if (len != kOuterHeaderSize + kSessionSaltLen + kChallengeLen) return false;
    if (in[0] != kProtocolVersion || static_cast<MsgType>(in[1]) != MsgType::Hello) return false;
    std::memcpy(salt, in + kOuterHeaderSize, kSessionSaltLen);
    std::memcpy(challenge, in + kOuterHeaderSize + kSessionSaltLen, kChallengeLen);
    return true;
}

// server_salt (v4) lets the server contribute randomness the client cannot
// predict or control into the session key (see the derive below), all-zero
// when the connection is unencrypted — the same convention StreamStart uses
// for session_salt.
size_t encode_hello_ack(const uint8_t server_salt[kSessionSaltLen], const uint8_t proof[kProofLen],
                        uint8_t *out) {
    size_t off = 0;
    off += put_u8(out + off, kProtocolVersion);
    off += put_u8(out + off, static_cast<uint8_t>(MsgType::HelloAck));
    std::memcpy(out + off, server_salt, kSessionSaltLen);
    off += kSessionSaltLen;
    std::memcpy(out + off, proof, kProofLen);
    return off + kProofLen;
}

bool decode_hello_ack(const uint8_t *in, size_t len, uint8_t server_salt[kSessionSaltLen],
                      uint8_t proof[kProofLen]) {
    if (len != kOuterHeaderSize + kSessionSaltLen + kProofLen) return false;
    if (in[0] != kProtocolVersion || static_cast<MsgType>(in[1]) != MsgType::HelloAck) return false;
    std::memcpy(server_salt, in + kOuterHeaderSize, kSessionSaltLen);
    std::memcpy(proof, in + kOuterHeaderSize + kSessionSaltLen, kProofLen);
    return true;
}

size_t encode_close(uint8_t *out) {
    out[0] = kProtocolVersion;
    out[1] = static_cast<uint8_t>(MsgType::Close);
    return kOuterHeaderSize;
}

// The PSK's length: pre_shared_key_len when set (binary keys may contain NUL
// bytes, ROADMAP #28), otherwise strlen. 0 means plaintext.
size_t config_psk_len(const fuse_config *cfg) {
    if (cfg->pre_shared_key == nullptr) return 0;
    if (cfg->pre_shared_key_len != 0) return cfg->pre_shared_key_len;
    return std::strlen(cfg->pre_shared_key);
}

// Metadata the retransmit path needs and the registry does not keep.
struct TxMeta {
    uint64_t offset = 0;
    uint8_t flags = 0;
};

} // namespace

// ---------------------------------------------------------------------------

struct fuse_conn {
    UdpSocket sock;
    PeerAddr peer{};

    uint16_t tx_lane = kLaneClientToServer;
    uint16_t rx_lane = kLaneServerToClient;

    bool encrypted = false;
    // Data keys rotate every 2^22 blocks per direction (ROADMAP #21, SDK
    // half), derived from the session key exactly as the transfer API does,
    // so a long-lived connection never pushes one AES-GCM key past its
    // usage limit. hs_cipher (the session key itself) only seals/opens the
    // handshake proof.
    LaneKeySchedule tx_keys;
    LaneKeySchedule rx_keys;
    LaneCipher hs_cipher;
    // Signs/verifies control datagrams when encrypted (ROADMAP #2, SDK
    // half); nullptr in plaintext.
    std::unique_ptr<ControlAuth> ctl;

    uint32_t timeout_ms = kDefaultTimeoutMs;

    // --- state below is guarded by `mu` ---
    std::mutex mu;
    std::condition_variable cv;

    SenderRegistry reg{0, kWindow};
    CongestionController cc{kWindow, true, 1, 8};
    uint64_t tx_next_seq = 0;
    uint64_t tx_base = 0; // lowest unacknowledged sequence number
    TxMeta tx_meta[kMaxWindow] = {};
    uint64_t last_progress_ns = 0;
    uint64_t last_rtt_echo_ns = 0; // dedupes repeated Ack echoes, see N13

    ReceiverStream rx{0, kWindow, true};
    // A NAT rebinding shows up as traffic from a new source; the peer
    // address only moves there once that source answers a challenge.
    PathValidator path;
    uint64_t last_rx_ns = 0; // last datagram that proved the peer is alive
    uint64_t last_tx_ns = 0; // last datagram we sent, for heartbeats
    bool peer_lost = false;  // nothing heard for the dead-peer interval
    std::vector<uint8_t> asm_buf;      // message under reassembly
    // First sequence number of the message under reassembly: the sender
    // cuts every message into kBlockSize blocks, so block `seq` must sit at
    // offset (seq - asm_start_seq) * kBlockSize. Checking that keeps a
    // forged block (plaintext mode) from growing asm_buf toward 64 MiB with
    // one datagram.
    uint64_t asm_start_seq = 0;
    uint64_t asm_end_seq = UINT64_MAX; // sequence carrying kFlagLastBlock
    size_t asm_total = 0;
    std::deque<std::vector<uint8_t>> inbox;
    size_t inbox_bytes = 0; // sum of inbox message sizes, for the cap below

    bool peer_closed = false;
    bool failed = false;
    bool closing = false; // set by fuse_close, so a blocked wait_for() bails
    bool send_busy = false; // serializes whole-message fuse_send calls
    bool ack_pending = false;
    uint64_t last_ack_ns = 0;
    uint64_t rtt_est_ns = 0; // receiver-side estimate, for NACK pacing
    uint64_t nack_sent_ns = 0;

    fuse_conn_stats stats{};

    std::thread pump;
    std::atomic<bool> stop{false};

    // References to this connection: 1 for the handle itself, plus one per
    // application thread currently inside fuse_send/fuse_recv/
    // fuse_recv_alloc (see InUseGuard). Whoever drops the last reference
    // deletes it, so fuse_close never frees a connection another thread is
    // still inside (ROADMAP #27): if one is still there after fuse_close's
    // grace period, that thread frees it on its way out.
    std::atomic<int> refs{1};

    fuse_conn() {
        // ReceiverStream/SenderRegistry lane ids are set properly in setup().
    }
};

struct fuse_listener {
    UdpSocket sock;
    std::string psk;
    uint32_t timeout_ms = kDefaultTimeoutMs;

    // Small ring of recently seen handshakes, so a client's HELLO retry does
    // not manufacture a second connection.
    struct Seen {
        PeerAddr addr{};
        uint8_t challenge[kChallengeLen] = {};
        bool used = false;
    };
    Seen recent[16];
    size_t recent_at = 0;

    bool is_duplicate(const PeerAddr &from, const uint8_t challenge[kChallengeLen]) {
        for (const Seen &s : recent) {
            if (s.used && s.addr == from &&
                std::memcmp(s.challenge, challenge, kChallengeLen) == 0) {
                return true;
            }
        }
        Seen &slot = recent[recent_at];
        recent_at = (recent_at + 1) % (sizeof(recent) / sizeof(recent[0]));
        slot.used = true;
        slot.addr = from;
        std::memcpy(slot.challenge, challenge, kChallengeLen);
        return false;
    }
};

// ---------------------------------------------------------------------------
// Pump thread: everything that must keep happening whether or not the
// application is currently inside a send or a recv.

namespace {

// Sends an encoded control datagram (`n` bytes in `buf`, room for `cap`),
// signed when the connection is encrypted. Caller holds the lock.
void send_ctl_locked(fuse_conn *c, uint8_t *buf, size_t n, size_t cap, const PeerAddr &to) {
    if (n == 0) return;
    if (c->ctl) n = c->ctl->seal(buf, n, cap);
    if (n == 0) return;
    c->sock.send_to(buf, n, to);
    c->last_tx_ns = now_ns();
}

void send_ack_locked(fuse_conn *c, uint8_t *scratch, size_t cap) {
    Ack ack = c->rx.build_ack();
    ack.stream_id = c->rx_lane;
    send_ctl_locked(c, scratch, encode_ack(ack, scratch, cap), cap, c->peer);
    c->ack_pending = false;
    c->last_ack_ns = now_ns();
}

// Authentic traffic for this connection arrived from `src`, which is not
// the current peer: challenge it rather than switching to it (ROADMAP #3,
// SDK half). Caller holds the lock.
void probe_path_locked(fuse_conn *c, const PeerAddr &src, uint8_t *scratch, size_t cap) {
    uint64_t challenge = 0;
    if (!c->path.want_probe(src, now_ns(), &challenge)) return;
    PathProbe pp;
    pp.stream_id = c->tx_lane;
    pp.challenge = challenge;
    send_ctl_locked(c, scratch, encode_path_probe(MsgType::PathChallenge, pp, scratch, cap), cap,
                    src);
}

// Re-emits one stored block. Caller holds the lock.
void retransmit_locked(fuse_conn *c, uint64_t seq, uint8_t *scratch, size_t cap) {
    const RegistrySlot *slot = c->reg.lookup(seq);
    if (slot == nullptr) return;
    const TxMeta &meta = c->tx_meta[seq % kWindow];

    BlockHeader hdr;
    hdr.stream_id = c->tx_lane;
    hdr.seq_no = seq;
    hdr.flags = static_cast<uint8_t>(meta.flags | kFlagRetransmission);
    hdr.payload_len = slot->payload_len;
    hdr.offset = meta.offset;

    const uint8_t *body = slot->payload;
    uint8_t sealed[kMaxPayloadSize + kAeadTagLen];
    if (c->encrypted) {
        uint8_t aad[19];
        const size_t al = build_aad(aad, c->tx_lane, seq, meta.offset, hdr.flags);
        const LaneCipher *cipher = c->tx_keys.cipher_for(seq);
        if (cipher == nullptr ||
            !cipher->seal(c->tx_lane, seq, aad, al, slot->payload, slot->payload_len,
                               sealed)) {
            return;
        }
        body = sealed;
        hdr.payload_len = static_cast<uint16_t>(slot->payload_len + kAeadTagLen);
    }

    const size_t n = encode_data_datagram(hdr, now_ns(), body, scratch, cap);
    // A refused send (buffer full) must not be counted as progress, or this
    // block would never be tried again.
    if (n && c->sock.send_to(scratch, n, c->peer)) {
        ++c->stats.retransmits;
    }
}

void handle_data_locked(fuse_conn *c, const uint8_t *dg, size_t dlen, uint8_t *plain,
                        const PeerAddr &src, uint8_t *scratch, size_t scratch_cap) {
    BlockHeader hdr;
    uint64_t send_time = 0;
    const uint8_t *payload = nullptr;
    if (!decode_data_datagram(dg, dlen, &hdr, &send_time, &payload)) return;
    if (hdr.stream_id != c->rx_lane) return; // not our direction

    const uint8_t *body = payload;
    uint16_t body_len = hdr.payload_len;

    // Outside the receive window there is nothing to admit. Checked before
    // decrypting, so a forged block can't make us derive an epoch key for an
    // arbitrary far-away seq on every packet. (A block below the base is a
    // resend whose Ack was lost: answer with a fresh Ack.)
    const uint64_t rx_base = c->rx.base_seq_no();
    if (hdr.seq_no < rx_base || hdr.seq_no - rx_base >= kWindow) {
        if (hdr.seq_no < rx_base) c->ack_pending = true;
        return;
    }

    if (c->encrypted) {
        // Authenticate BEFORE admitting the block to the window: a forged
        // block that advanced the window would slide it past data that was
        // never written.
        uint8_t aad[19];
        const size_t al = build_aad(aad, c->rx_lane, hdr.seq_no, hdr.offset, hdr.flags);
        const LaneCipher *cipher = c->rx_keys.cipher_for(hdr.seq_no);
        if (hdr.payload_len < kAeadTagLen || cipher == nullptr ||
            !cipher->open(c->rx_lane, hdr.seq_no, aad, al, payload, hdr.payload_len, plain)) {
            ++c->stats.auth_failures;
            return;
        }
        body = plain;
        body_len = static_cast<uint16_t>(hdr.payload_len - kAeadTagLen);
    }
    // An authentic block (any block from the peer, in plaintext) shows the
    // peer is alive, even a duplicate.
    if (c->encrypted || src == c->peer) c->last_rx_ns = now_ns();

    // Backpressure: too many completed messages are already sitting
    // undrained. Skip admitting this block (rather than reassembling it
    // and growing inbox further) so the sender's own retry loop stalls
    // until the application catches up, instead of buffering an unbounded
    // amount of memory for a slow reader.
    if (c->inbox_bytes >= kMaxInboxBytes) return;

    // Re-anchoring and RTT sampling both wait for on_receive's verdict:
    // `Accepted` means this block is both authentic (if encrypted) AND new,
    // not a replay of something already delivered. That second part
    // matters even for an authenticated block — AEAD doesn't add freshness
    // against an exact-replay, so without this an attacker who captures one
    // genuine datagram and resends it from their own address could
    // redirect where this connection's Acks (and, in plaintext mode, all
    // further data) go, no key needed.
    const ReceiveResult rr = c->rx.on_receive(hdr.seq_no, send_time, now_ns());
    if (rr == ReceiveResult::Accepted) {
        // A new, authentic block from a source other than c->peer is how
        // the peer's NAT mapping changing mid-session (an ISP-forced
        // reconnect, a mobile handover) shows up. Replies move there only
        // once that source answers a path challenge: a block that is
        // authentic and new can still arrive with a rewritten source
        // address, and switching on it alone would let an on-path attacker
        // redirect (or reflect) the connection's traffic.
        if (!(src == c->peer)) probe_path_locked(c, src, scratch, scratch_cap);

        // A retransmission arriving after we NACKed measures one RTT on
        // our own clock, with no cross-host clock comparison.
        if ((hdr.flags & kFlagRetransmission) && c->nack_sent_ns != 0) {
            const uint64_t sample = now_ns() - c->nack_sent_ns;
            c->rtt_est_ns = (c->rtt_est_ns == 0) ? sample : (c->rtt_est_ns * 7 + sample) / 8;
            c->nack_sent_ns = 0;
        }

        // Checked this way round so `hdr.offset + body_len` (wire fields,
        // untrusted before this point in plaintext mode) never gets
        // computed: with the addition done first, an offset near
        // UINT64_MAX wraps the sum small enough to pass a naive
        // "<= kMaxMessage" check, and the memcpy below then writes out of
        // bounds at the unwrapped offset.
        //
        // Every block of a message also sits at exactly
        // (seq - asm_start_seq) * kBlockSize, so a block's offset can't
        // reach past the blocks the window could actually hold: one forged
        // block can't make asm_buf allocate ~64 MiB.
        const bool placed = hdr.seq_no >= c->asm_start_seq && body_len <= kBlockSize &&
                            hdr.seq_no - c->asm_start_seq <= kMaxMessage / kBlockSize &&
                            hdr.offset == (hdr.seq_no - c->asm_start_seq) * kBlockSize;
        if (placed && hdr.offset <= kMaxMessage && body_len <= kMaxMessage - hdr.offset) {
            const size_t end = static_cast<size_t>(hdr.offset) + body_len;
            if (c->asm_buf.size() < end) c->asm_buf.resize(end);
            if (body_len > 0) std::memcpy(c->asm_buf.data() + hdr.offset, body, body_len);
            if (hdr.flags & kFlagLastBlock) {
                c->asm_end_seq = hdr.seq_no;
                c->asm_total = end;
            }
        }
    }
    c->ack_pending = true;

    // The message is whole once every block up to and including its final
    // one has arrived contiguously.
    if (c->asm_end_seq != UINT64_MAX && c->rx.base_seq_no() > c->asm_end_seq) {
        c->asm_buf.resize(c->asm_total);
        c->inbox_bytes += c->asm_buf.size();
        c->inbox.push_back(std::move(c->asm_buf));
        c->asm_buf.clear();
        c->asm_start_seq = c->asm_end_seq + 1;
        c->asm_end_seq = UINT64_MAX;
        c->asm_total = 0;
        ++c->stats.messages_received;
        c->stats.bytes_received += c->inbox.back().size();
        c->cv.notify_all();
    }
}

void pump_main(fuse_conn *c) {
    std::vector<uint8_t> rxbuf(kRxBatch * (kMaxDatagramSize + 64));
    std::vector<size_t> lens(kRxBatch);
    std::vector<PeerAddr> srcs(kRxBatch);
    std::vector<uint8_t> scratch(kMaxDatagramSize + 64);
    std::vector<uint8_t> plain(kMaxPayloadSize + kAeadTagLen);
    const size_t slot = kMaxDatagramSize + 64;

    while (!c->stop.load(std::memory_order_relaxed)) {
        const int got = c->sock.recv_batch(rxbuf.data(), slot, kRxBatch, lens.data(), srcs.data());

        std::unique_lock<std::mutex> lk(c->mu);
        bool got_data = false;
        for (int i = 0; i < got; ++i) {
            const uint8_t *dg = rxbuf.data() + i * slot;
            MsgType type;
            if (!peek_msg_type(dg, lens[i], &type)) continue;

            // Control datagrams: under a PSK only signed ones count, each at
            // most once (ControlAuth's replay window), so nobody without the
            // key can forge an Ack (claiming delivery of data the peer never
            // got), a Nack (halving the window) or a Close. In plaintext,
            // only the peer's own address is listened to, apart from path
            // probes. Either way, control traffic from a new address moves
            // nothing until that address passes a path challenge.
            const bool from_peer = srcs[i] == c->peer;
            const bool probe = type == MsgType::PathChallenge || type == MsgType::PathResponse;
            if (type != MsgType::Data) {
                if (c->ctl && !c->ctl->open(dg, lens[i], nullptr)) continue;
                if (c->ctl || from_peer) c->last_rx_ns = now_ns();
                if (!from_peer && !probe) {
                    // A rebound peer: challenge the new address. Signed
                    // messages are still acted on; plaintext ones from
                    // there are not, until it answers. (The challenge is
                    // smaller than any control message that triggers it.)
                    probe_path_locked(c, srcs[i], scratch.data(), scratch.size());
                    if (!c->ctl) continue;
                }
            }

            switch (type) {
                case MsgType::Data:
                    handle_data_locked(c, dg, lens[i], plain.data(), srcs[i], scratch.data(),
                                       scratch.size());
                    got_data = true;
                    break;
                case MsgType::Ack: {
                    Ack ack;
                    if (!decode_ack(dg, lens[i], &ack)) break;
                    if (ack.base_seq_no > c->tx_base) {
                        // Plaintext Acks are unauthenticated, so base_seq_no
                        // is untrusted input: clamp to what has actually
                        // been sent, or a forged huge value spins this loop
                        // ~2^64 times while holding c->mu.
                        const uint64_t new_base = std::min(ack.base_seq_no, c->tx_next_seq);
                        for (uint64_t s = c->tx_base; s < new_base; ++s) c->reg.confirm(s);
                        c->tx_base = new_base;
                        c->last_progress_ns = now_ns();
                        c->cv.notify_all();
                    }
                    // The receiver only updates its echoed send-time when a
                    // new highest-seq block arrives (receiver.cpp) — every
                    // periodic/final Ack in between repeats the same value.
                    // Treating each repeat as a fresh sample manufactures
                    // an ever-growing fake RTT while the sender is stalled
                    // on a gap, stretching the RTO right when it needs to
                    // stay responsive.
                    if (ack.echoed_send_time > 0 && ack.echoed_send_time != c->last_rtt_echo_ns) {
                        c->last_rtt_echo_ns = ack.echoed_send_time;
                        const uint64_t t = now_ns();
                        if (t > ack.echoed_send_time) c->cc.on_rtt_sample(t - ack.echoed_send_time);
                    }
                    break;
                }
                case MsgType::Nack: {
                    Nack nack;
                    if (!decode_nack(dg, lens[i], &nack)) break;
                    c->cc.on_loss();
                    for (uint16_t k = 0; k < nack.count; ++k) {
                        retransmit_locked(c, nack.missing[k], scratch.data(), scratch.size());
                    }
                    break;
                }
                case MsgType::Close:
                    c->peer_closed = true;
                    c->cv.notify_all();
                    break;
                case MsgType::Heartbeat:
                    break; // liveness only, recorded above
                case MsgType::PathChallenge: {
                    PathProbe pp;
                    if (!decode_path_probe(type, dg, lens[i], &pp)) break;
                    pp.stream_id = c->tx_lane;
                    send_ctl_locked(c, scratch.data(),
                                    encode_path_probe(MsgType::PathResponse, pp, scratch.data(),
                                                      scratch.size()),
                                    scratch.size(), srcs[i]);
                    break;
                }
                case MsgType::PathResponse: {
                    PathProbe pp;
                    if (decode_path_probe(type, dg, lens[i], &pp) &&
                        c->path.validated(srcs[i], pp.challenge)) {
                        c->peer = srcs[i];
                        c->last_rx_ns = now_ns();
                    }
                    break;
                }
                case MsgType::Hello: {
                    // A HELLO retry that raced our HELLO-ACK: answer again so
                    // the client stops retrying.
                    break;
                }
                default:
                    break;
            }
        }

        const uint64_t t = now_ns();
        c->cc.poll(t);

        // Acknowledge promptly: the peer's send() is blocked until it hears
        // from us. Gated on actual Data having been processed (or an Ack
        // already owed for other reasons) — not merely "a batch arrived",
        // which included a batch that was itself only an Ack. Two idle
        // peers each acking the other's Ack is an unbounded ping-pong that
        // never involves any data at all.
        if (c->ack_pending || got_data) {
            send_ack_locked(c, scratch.data(), scratch.size());
        }

        // Report gaps, with timers scaled to the measured RTT so a long path
        // is not spammed with duplicate NACKs.
        const uint64_t reorder_ns =
            (c->rtt_est_ns == 0) ? 3000000ull
                                 : std::clamp<uint64_t>(c->rtt_est_ns / 2, 1000000ull, 100000000ull);
        const uint64_t renack_ns =
            (c->rtt_est_ns == 0)
                ? 40000000ull
                : std::clamp<uint64_t>(c->rtt_est_ns * 3 / 2, 20000000ull, 500000000ull);
        Nack nack;
        if (c->rx.collect_nacks(t, reorder_ns, renack_ns, &nack) > 0) {
            nack.stream_id = c->rx_lane;
            send_ctl_locked(c, scratch.data(), encode_nack(nack, scratch.data(), scratch.size()),
                            scratch.size(), c->peer);
            c->nack_sent_ns = t;
        }

        // Liveness (ROADMAP #12): keep an idle connection audible, and notice
        // a peer that has gone silent without a Close.
        if (!c->peer_closed && t - c->last_tx_ns > kHeartbeatNs) {
            Heartbeat hb;
            hb.stream_id = c->tx_lane;
            hb.highest_seq_no = c->tx_next_seq;
            send_ctl_locked(c, scratch.data(), encode_heartbeat(hb, scratch.data(), scratch.size()),
                            scratch.size(), c->peer);
        }
        const uint64_t dead_ns =
            std::max<uint64_t>(static_cast<uint64_t>(c->timeout_ms) * 1000000ull, kMinDeadPeerNs);
        if (!c->peer_lost && !c->peer_closed && t - c->last_rx_ns > dead_ns) {
            c->peer_lost = true;
            c->cv.notify_all();
        }

        // Retransmission timeout, adaptive to RTT (a fixed timeout is right
        // only for loopback; on a real path it fires before an ACK can
        // possibly return and re-sends everything in flight many times).
        if (c->tx_base < c->tx_next_seq) {
            const uint64_t srtt = c->cc.rtt_ns();
            const uint64_t rto = srtt ? std::clamp<uint64_t>(srtt * 3, 20000000ull, 1000000000ull)
                                      : 200000000ull;
            if (c->last_progress_ns != 0 && t - c->last_progress_ns > rto) {
                c->cc.on_loss();
                retransmit_locked(c, c->tx_base, scratch.data(), scratch.size());
                c->last_progress_ns = t;
            }
        }
    }
}

// Sends one already-framed block. Caller holds the lock.
bool send_block_locked(fuse_conn *c, uint64_t seq, uint64_t offset, const uint8_t *data,
                       uint16_t len, bool last, uint8_t *scratch, size_t cap) {
    BlockHeader hdr;
    hdr.stream_id = c->tx_lane;
    hdr.seq_no = seq;
    hdr.flags = last ? kFlagLastBlock : 0;
    hdr.payload_len = len;
    hdr.offset = offset;

    c->reg.store(seq, data, len, now_ns(), offset);
    c->tx_meta[seq % kWindow] = TxMeta{offset, hdr.flags};
    // last_progress_ns is otherwise only touched by the Ack handler and the
    // RTO branch, so after an idle stretch (nothing in flight, so neither
    // of those ran) it sits stale. Without this, the very next send after
    // an idle period sees `t - last_progress_ns` equal to the whole idle
    // duration, immediately exceeds the RTO, and retransmits a block that
    // was just sent moments ago — spuriously halving the congestion window.
    if (seq == c->tx_base) c->last_progress_ns = now_ns();

    const uint8_t *body = data;
    uint8_t sealed[kMaxPayloadSize + kAeadTagLen];
    if (c->encrypted) {
        uint8_t aad[19];
        const size_t al = build_aad(aad, c->tx_lane, seq, offset, hdr.flags);
        const LaneCipher *cipher = c->tx_keys.cipher_for(seq);
        if (cipher == nullptr || !cipher->seal(c->tx_lane, seq, aad, al, data, len, sealed)) {
            return false;
        }
        body = sealed;
        hdr.payload_len = static_cast<uint16_t>(len + kAeadTagLen);
    }

    const size_t n = encode_data_datagram(hdr, now_ns(), body, scratch, cap);
    if (n == 0) return false;
    c->sock.send_to(scratch, n, c->peer); // loss is repaired by ACK/NACK
    c->last_tx_ns = now_ns();
    return true;
}

fuse_status wait_for(fuse_conn *c, std::unique_lock<std::mutex> &lk,
                     const std::function<bool()> &ready, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    while (!ready()) {
        if (c->failed) return FUSE_ERR_INTERNAL;
        // fuse_close notifies under the lock right after setting this, so a
        // thread already parked below wakes up and leaves instead of still
        // being inside this wait (and about to touch `c`) once fuse_close
        // deletes it.
        if (c->closing) return FUSE_ERR_CLOSED;
        if (c->peer_lost) return FUSE_ERR_TIMEOUT; // the peer went silent
        if (timeout_ms < 0) {
            c->cv.wait_for(lk, std::chrono::milliseconds(100));
        } else {
            if (std::chrono::steady_clock::now() >= deadline) return FUSE_ERR_TIMEOUT;
            c->cv.wait_until(lk, deadline);
        }
    }
    return FUSE_OK;
}

// Holds a reference to the connection for one application thread's
// presence inside fuse_send/fuse_recv/fuse_recv_alloc. Construct it before
// acquiring c->mu, and declare it before any lock or other object that
// touches `c` (it must be destroyed last): if fuse_close has already given
// up waiting and dropped the handle's reference, this guard's destructor is
// what frees the connection.
struct InUseGuard {
    fuse_conn *c;
    explicit InUseGuard(fuse_conn *c_) : c(c_) { c->refs.fetch_add(1, std::memory_order_acq_rel); }
    ~InUseGuard() {
        if (c->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete c;
    }
};

void start_conn(fuse_conn *c, uint16_t tx_lane, uint16_t rx_lane, uint32_t timeout_ms) {
    c->tx_lane = tx_lane;
    c->rx_lane = rx_lane;
    c->timeout_ms = timeout_ms ? timeout_ms : kDefaultTimeoutMs;
    c->reg = SenderRegistry(tx_lane, kWindow);
    c->rx = ReceiverStream(rx_lane, kWindow, /*lossless=*/true);
    c->last_progress_ns = now_ns();
    c->last_rx_ns = c->last_tx_ns = now_ns();
    set_bufs(c->sock.fd(), 8 << 20);
    // A short socket timeout keeps the pump responsive to its stop flag.
    set_rcv_timeout_ms(c->sock.fd(), 20);
    c->pump = std::thread(pump_main, c);
}

} // namespace

// ---------------------------------------------------------------------------
// Public API

extern "C" {

const char *fuse_strerror(fuse_status s) {
    switch (s) {
        case FUSE_OK: return "ok";
        case FUSE_ERR_CONFIG: return "invalid configuration";
        case FUSE_ERR_SOCKET: return "socket error (port in use or blocked?)";
        case FUSE_ERR_TIMEOUT: return "timed out";
        case FUSE_ERR_CLOSED: return "connection closed by peer";
        case FUSE_ERR_AUTH: return "authentication failed (pre-shared key mismatch?)";
        case FUSE_ERR_TOO_LARGE: return "message too large";
        case FUSE_ERR_BUFFER: return "buffer too small";
        case FUSE_ERR_UNSUPPORTED: return "unsupported (built without crypto)";
        case FUSE_ERR_INTERNAL: return "internal error";
    }
    return "unknown error";
}

const char *fuse_version(void) { return FUSE_VERSION_STRING; }

int fuse_encryption_available(void) { return session_crypto_available() ? 1 : 0; }

size_t fuse_max_message(void) { return kMaxMessage; }

void fuse_config_init(fuse_config *cfg) {
    if (cfg == nullptr) return;
    cfg->bind_address = "0.0.0.0";
    cfg->host = "127.0.0.1";
    cfg->port = 0;
    cfg->pre_shared_key = nullptr;
    cfg->timeout_ms = kDefaultTimeoutMs;
    cfg->pre_shared_key_len = 0;
}

// ---- Server ---------------------------------------------------------------

fuse_listener *fuse_listen(const fuse_config *cfg, fuse_status *err) {
    auto fail = [&](fuse_status s) -> fuse_listener * {
        if (err) *err = s;
        return nullptr;
    };
    if (cfg == nullptr) return fail(FUSE_ERR_CONFIG);
    const size_t psk_len = config_psk_len(cfg);
    const bool want_crypto = psk_len != 0;
    if (want_crypto && !session_crypto_available()) return fail(FUSE_ERR_UNSUPPORTED);

    auto *l = new (std::nothrow) fuse_listener();
    if (l == nullptr) return fail(FUSE_ERR_INTERNAL);
    if (!l->sock.open(cfg->bind_address, cfg->port)) {
        delete l;
        return fail(FUSE_ERR_SOCKET);
    }
    if (want_crypto) l->psk.assign(cfg->pre_shared_key, psk_len);
    l->timeout_ms = cfg->timeout_ms ? cfg->timeout_ms : kDefaultTimeoutMs;
    if (err) *err = FUSE_OK;
    return l;
}

uint16_t fuse_listener_port(const fuse_listener *l) {
    return l ? const_cast<UdpSocket &>(l->sock).local_port() : 0;
}

void fuse_listener_close(fuse_listener *l) { delete l; }

fuse_conn *fuse_accept(fuse_listener *l, int timeout_ms, fuse_status *err) {
    auto fail = [&](fuse_status s) -> fuse_conn * {
        if (err) *err = s;
        return nullptr;
    };
    if (l == nullptr) return fail(FUSE_ERR_CONFIG);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    // Poll in slices so a negative (infinite) timeout is still interruptible
    // by closing the listener.
    set_rcv_timeout_ms(l->sock.fd(), 100);
    // timeout_ms == 0 means "one non-blocking sweep, then give up" — the
    // socket itself must be non-blocking for that, not merely given a short
    // timeout, or a single recv_from could still block up to 100ms. Reset
    // explicitly on every call (not just when one_shot) so a prior
    // timeout_ms==0 call's non-blocking flag can't leak into this one.
    const bool one_shot = (timeout_ms == 0);
    l->sock.set_nonblocking(one_shot);

    uint8_t buf[512];
    for (;;) {
        if (!one_shot && timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
            return fail(FUSE_ERR_TIMEOUT);
        }

        size_t got = 0;
        PeerAddr from;
        if (!l->sock.recv_from(buf, sizeof(buf), &got, &from)) {
            if (one_shot) return fail(FUSE_ERR_TIMEOUT);
            continue;
        }

        uint8_t salt[kSessionSaltLen], challenge[kChallengeLen];
        // A non-HELLO datagram or a duplicate must not silently re-block
        // for another slice when the caller asked for exactly one sweep —
        // that turned timeout_ms==0 into "block up to 100ms per stray
        // datagram" instead of the immediate return a poll implies.
        if (!decode_hello(buf, got, salt, challenge) || l->is_duplicate(from, challenge)) {
            if (one_shot) return fail(FUSE_ERR_TIMEOUT);
            continue;
        }

        auto *c = new (std::nothrow) fuse_conn();
        if (c == nullptr) return fail(FUSE_ERR_INTERNAL);

        // Each connection gets its own ephemeral socket, so one listen port
        // can serve many peers concurrently.
        if (!c->sock.open("0.0.0.0", 0)) {
            delete c;
            return fail(FUSE_ERR_SOCKET);
        }
        c->peer = from;

        uint8_t server_salt[kSessionSaltLen] = {};
        uint8_t proof[kProofLen] = {};
        if (!l->psk.empty()) {
            // Freshly random for every HELLO answered, so two connections
            // never derive the same key even if a client's salt is somehow
            // repeated (a retry, or a captured-and-replayed HELLO): the key
            // is never determined by the client's contribution alone.
            if (!random_bytes(server_salt, sizeof(server_salt))) {
                delete c;
                return fail(FUSE_ERR_INTERNAL);
            }
            uint8_t combined[2 * kSessionSaltLen];
            std::memcpy(combined, salt, kSessionSaltLen);
            std::memcpy(combined + kSessionSaltLen, server_salt, kSessionSaltLen);

            uint8_t key[kSessionKeyLen];
            c->ctl = std::make_unique<ControlAuth>();
            const bool keyed = derive_session_key(reinterpret_cast<const uint8_t *>(l->psk.data()),
                                                  l->psk.size(), combined, sizeof(combined), key) &&
                               c->hs_cipher.init(key) &&
                               c->tx_keys.init(key, kLaneServerToClient) &&
                               c->rx_keys.init(key, kLaneClientToServer) &&
                               c->ctl->init(key, kControlLane, kDirServer);
            secure_zero(key, sizeof(key));
            if (!keyed) {
                delete c;
                return fail(FUSE_ERR_UNSUPPORTED);
            }
            c->encrypted = true;
            // Prove we hold the key by sealing the client's challenge.
            if (!c->hs_cipher.seal(0xFFFF, 0, nullptr, 0, challenge, kChallengeLen, proof)) {
                delete c;
                return fail(FUSE_ERR_INTERNAL);
            }
        }

        uint8_t ack[64];
        const size_t n = encode_hello_ack(server_salt, proof, ack);
        // Sent from the NEW socket: its source port is what the client adopts.
        c->sock.send_to(ack, n, c->peer);

        start_conn(c, kLaneServerToClient, kLaneClientToServer, l->timeout_ms);
        if (err) *err = FUSE_OK;
        return c;
    }
}

// ---- Client ---------------------------------------------------------------

fuse_conn *fuse_connect(const fuse_config *cfg, fuse_status *err) {
    auto fail = [&](fuse_status s) -> fuse_conn * {
        if (err) *err = s;
        return nullptr;
    };
    if (cfg == nullptr || cfg->host == nullptr || cfg->port == 0) return fail(FUSE_ERR_CONFIG);
    const size_t psk_len = config_psk_len(cfg);
    const bool want_crypto = psk_len != 0;
    if (want_crypto && !session_crypto_available()) return fail(FUSE_ERR_UNSUPPORTED);

    auto *c = new (std::nothrow) fuse_conn();
    if (c == nullptr) return fail(FUSE_ERR_INTERNAL);
    if (!c->sock.open("0.0.0.0", 0)) {
        delete c;
        return fail(FUSE_ERR_SOCKET);
    }

    PeerAddr server;
    if (!UdpSocket::resolve(cfg->host, cfg->port, &server)) {
        delete c;
        return fail(FUSE_ERR_CONFIG);
    }

    // Key derivation is deferred until the server's HelloAck contributes its
    // own salt (below) — mixing in salt the server alone controls is what
    // stops a captured/retried HELLO from ever reproducing a previously
    // issued key (see wire.hpp's v4 note).
    uint8_t salt[kSessionSaltLen] = {};
    if (want_crypto && !random_bytes(salt, sizeof(salt))) {
        delete c;
        return fail(FUSE_ERR_UNSUPPORTED);
    }

    const uint32_t total_ms = cfg->timeout_ms ? cfg->timeout_ms : kDefaultTimeoutMs;
    set_rcv_timeout_ms(c->sock.fd(), 200);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);
    uint8_t hello[64], reply[128];

    // One challenge for the whole call, not one per retry attempt: a
    // HelloAck answering an earlier attempt that arrives late (the reply
    // was just slow, not a distinct/stale session) used to fail this
    // memcmp against whichever attempt's challenge was current by then,
    // hard-aborting the connect with FUSE_ERR_AUTH on ordinary latency
    // rather than continuing to wait or retry. Freshness against a truly
    // stale/replayed reply from an unrelated earlier connect() call
    // doesn't depend on this: that reply would carry a different `salt`
    // and so derive a different key entirely (wire.hpp's v4 note).
    uint8_t challenge[kChallengeLen];
    if (!want_crypto) {
        std::memset(challenge, 0, sizeof(challenge));
        const uint64_t t = now_ns();
        std::memcpy(challenge, &t, sizeof(challenge));
    } else if (!random_bytes(challenge, sizeof(challenge))) {
        delete c;
        return fail(FUSE_ERR_INTERNAL);
    }

    while (std::chrono::steady_clock::now() < deadline) {
        const size_t hn = encode_hello(salt, challenge, hello);
        if (!c->sock.send_to(hello, hn, server)) {
            delete c;
            return fail(FUSE_ERR_SOCKET);
        }

        size_t got = 0;
        PeerAddr from;
        if (!c->sock.recv_from(reply, sizeof(reply), &got, &from)) {
            continue; // timed out; retry
        }

        uint8_t server_salt[kSessionSaltLen];
        uint8_t proof[kProofLen];
        if (!decode_hello_ack(reply, got, server_salt, proof)) continue;

        if (want_crypto) {
            uint8_t combined[2 * kSessionSaltLen];
            std::memcpy(combined, salt, kSessionSaltLen);
            std::memcpy(combined + kSessionSaltLen, server_salt, kSessionSaltLen);

            uint8_t key[kSessionKeyLen];
            c->ctl = std::make_unique<ControlAuth>();
            const bool keyed = derive_session_key(reinterpret_cast<const uint8_t *>(cfg->pre_shared_key),
                                                  psk_len, combined, sizeof(combined), key) &&
                               c->hs_cipher.init(key) &&
                               c->tx_keys.init(key, kLaneClientToServer) &&
                               c->rx_keys.init(key, kLaneServerToClient) &&
                               c->ctl->init(key, kControlLane, kDirClient);
            secure_zero(key, sizeof(key));
            if (!keyed) {
                delete c;
                return fail(FUSE_ERR_UNSUPPORTED);
            }
            c->encrypted = true;

            uint8_t opened[kChallengeLen];
            if (!c->hs_cipher.open(0xFFFF, 0, nullptr, 0, proof, kProofLen, opened) ||
                std::memcmp(opened, challenge, kChallengeLen) != 0) {
                // The peer could not prove it holds the same key.
                delete c;
                return fail(FUSE_ERR_AUTH);
            }
        }

        // Adopt the reply's source address: the server moved us to a
        // per-connection port.
        c->peer = from;
        start_conn(c, kLaneClientToServer, kLaneServerToClient, total_ms);
        if (err) *err = FUSE_OK;
        return c;
    }

    delete c;
    return fail(FUSE_ERR_TIMEOUT);
}

// ---- Data ------------------------------------------------------------------

fuse_status fuse_send(fuse_conn *c, const void *data, size_t len) {
    if (c == nullptr) return FUSE_ERR_CONFIG;
    if (len > kMaxMessage) return FUSE_ERR_TOO_LARGE;
    const auto *bytes = static_cast<const uint8_t *>(data);
    if (bytes == nullptr && len > 0) return FUSE_ERR_CONFIG;

    InUseGuard in_use_guard(c);
    std::vector<uint8_t> scratch(kMaxDatagramSize + 64);
    std::unique_lock<std::mutex> lk(c->mu);
    if (c->peer_closed || c->closing) return FUSE_ERR_CLOSED;

    // Serializes whole-message sends against each other: the per-block
    // room-wait below releases c->mu between blocks, so two concurrent
    // fuse_send calls would otherwise interleave their blocks onto the
    // same tx_next_seq while each computes its own message-relative offset
    // from 0 — the receiver's reassembly has no message id to disambiguate,
    // so interleaved blocks from two sends silently overwrite each other.
    // sdk.h documents one sender thread and one receiver thread per
    // connection, not two concurrent senders.
    const auto not_busy = [&] { return !c->send_busy || c->peer_closed || c->closing; };
    fuse_status busy_st = wait_for(c, lk, not_busy, static_cast<int>(c->timeout_ms));
    if (busy_st != FUSE_OK) return busy_st;
    if (c->peer_closed || c->closing) return FUSE_ERR_CLOSED;
    c->send_busy = true;
    struct BusyGuard {
        fuse_conn *conn;
        ~BusyGuard() {
            conn->send_busy = false;
            conn->cv.notify_all();
        }
    } busy_guard{c};

    const size_t nblocks = (len == 0) ? 1 : (len + kBlockSize - 1) / kBlockSize;
    const uint64_t first_seq = c->tx_next_seq;
    const uint64_t last_seq = first_seq + nblocks - 1;

    for (size_t i = 0; i < nblocks; ++i) {
        // Respect the congestion window: never more than `window` blocks
        // unacknowledged at once.
        const auto room = [&] {
            const uint64_t w = std::min<uint64_t>(kWindow, std::max<uint32_t>(4, c->cc.window()));
            return (c->tx_next_seq - c->tx_base) < w || c->peer_closed;
        };
        const fuse_status st = wait_for(c, lk, room, static_cast<int>(c->timeout_ms));
        if (st != FUSE_OK) return st;
        if (c->peer_closed) return FUSE_ERR_CLOSED;

        const size_t off = i * kBlockSize;
        const uint16_t blen =
            static_cast<uint16_t>(std::min<size_t>(kBlockSize, len - std::min(off, len)));
        const bool last = (i + 1 == nblocks);
        if (!send_block_locked(c, c->tx_next_seq, off, bytes + off, blen, last, scratch.data(),
                               scratch.size())) {
            return FUSE_ERR_INTERNAL;
        }
        ++c->tx_next_seq;
    }

    // Return only once the peer has acknowledged the whole message.
    const auto delivered = [&] { return c->tx_base > last_seq || c->peer_closed; };
    const fuse_status st = wait_for(c, lk, delivered, static_cast<int>(c->timeout_ms));
    if (st != FUSE_OK) return st;
    if (c->tx_base <= last_seq) return FUSE_ERR_CLOSED;

    ++c->stats.messages_sent;
    c->stats.bytes_sent += len;
    return FUSE_OK;
}

fuse_status fuse_recv(fuse_conn *c, void *buf, size_t cap, size_t *out_len, int timeout_ms) {
    if (c == nullptr || out_len == nullptr) return FUSE_ERR_CONFIG;
    InUseGuard in_use_guard(c);
    std::unique_lock<std::mutex> lk(c->mu);

    const auto have = [&] { return !c->inbox.empty() || c->peer_closed; };
    const fuse_status st = wait_for(c, lk, have, timeout_ms);
    if (st != FUSE_OK) return st;
    if (c->inbox.empty()) return FUSE_ERR_CLOSED;

    const std::vector<uint8_t> &front = c->inbox.front();
    *out_len = front.size();
    if (front.size() > cap) {
        // Leave the message queued so the caller can size a buffer and retry.
        return FUSE_ERR_BUFFER;
    }
    if (!front.empty() && buf != nullptr) std::memcpy(buf, front.data(), front.size());
    c->inbox_bytes -= front.size();
    c->inbox.pop_front();
    return FUSE_OK;
}

fuse_status fuse_recv_alloc(fuse_conn *c, void **out, size_t *out_len, int timeout_ms) {
    if (c == nullptr || out == nullptr || out_len == nullptr) return FUSE_ERR_CONFIG;
    InUseGuard in_use_guard(c);
    std::unique_lock<std::mutex> lk(c->mu);

    const auto have = [&] { return !c->inbox.empty() || c->peer_closed; };
    const fuse_status st = wait_for(c, lk, have, timeout_ms);
    if (st != FUSE_OK) return st;
    if (c->inbox.empty()) return FUSE_ERR_CLOSED;

    std::vector<uint8_t> msg = std::move(c->inbox.front());
    c->inbox_bytes -= msg.size();
    c->inbox.pop_front();
    lk.unlock();

    void *p = std::malloc(msg.empty() ? 1 : msg.size());
    if (p == nullptr) return FUSE_ERR_INTERNAL;
    if (!msg.empty()) std::memcpy(p, msg.data(), msg.size());
    *out = p;
    *out_len = msg.size();
    return FUSE_OK;
}

void fuse_free(void *p) { std::free(p); }

// ---- Connection state ------------------------------------------------------

fuse_status fuse_conn_peer(const fuse_conn *c, char *addr, size_t addr_cap, uint16_t *port) {
    if (c == nullptr) return FUSE_ERR_CONFIG;
    // c->peer is written under c->mu by the pump thread (Data/Ack/Nack
    // handling, NAT-migration re-anchoring) — reading it without the lock,
    // as this used to, is a data race under the C++ memory model, same
    // class of bug the sibling functions below already guard against.
    auto *m = const_cast<fuse_conn *>(c);
    std::lock_guard<std::mutex> lk(m->mu);
    if (addr != nullptr && addr_cap > 0) {
        if (!peer_to_string(c->peer, addr, addr_cap)) {
            return FUSE_ERR_INTERNAL;
        }
    }
    if (port != nullptr) *port = peer_port(c->peer);
    return FUSE_OK;
}

int fuse_conn_is_closed(const fuse_conn *c) {
    if (c == nullptr) return 1;
    auto *m = const_cast<fuse_conn *>(c);
    std::lock_guard<std::mutex> lk(m->mu);
    return (m->peer_closed && m->inbox.empty()) || m->failed || m->peer_lost ? 1 : 0;
}

void fuse_conn_get_stats(const fuse_conn *c, fuse_conn_stats *out) {
    if (c == nullptr || out == nullptr) return;
    auto *m = const_cast<fuse_conn *>(c);
    std::lock_guard<std::mutex> lk(m->mu);
    *out = m->stats;
    out->rtt_us = m->cc.rtt_ns() / 1000;
}

void fuse_close(fuse_conn *c) {
    if (c == nullptr) return;
    {
        std::lock_guard<std::mutex> lk(c->mu);
        c->closing = true;
        // Best effort, and repeated: this is a courtesy so the peer's recv
        // returns promptly instead of waiting out its timeout. Each copy is
        // signed separately under a PSK (the peer accepts each signature
        // once, and any one copy is enough).
        for (int i = 0; i < 3; ++i) {
            uint8_t bye[64];
            send_ctl_locked(c, bye, encode_close(bye), sizeof(bye), c->peer);
        }
        // Wakes any thread already parked in wait_for() (fuse_send/
        // fuse_recv/fuse_recv_alloc), so it observes `closing` and returns
        // FUSE_ERR_CLOSED instead of still being inside this connection's
        // state once it's deleted below.
        c->cv.notify_all();
    }
    c->stop.store(true, std::memory_order_relaxed);
    if (c->pump.joinable()) c->pump.join();

    // A thread that was already past wait_for's check and mid-flight
    // through the rest of fuse_send/fuse_recv/fuse_recv_alloc (running,
    // not blocked) needs to finish touching `c` before this deletes it —
    // in_use (see InUseGuard) tracks that. Every such call either isn't
    // blocked at all (finishes in microseconds) or is blocked in
    // wait_for, which the notify above plus the `closing` check already
    // unblocks within one poll interval, so this drains almost
    // immediately in the normal case; the cap is only a backstop against
    // a caller that violated the "not from another thread concurrently
    // with close, without your own synchronization" contract this shares
    // with most C handle APIs (pthread, BSD sockets, ...).
    for (int waited_ms = 0; c->refs.load(std::memory_order_acquire) > 1 && waited_ms < 2000;
        ++waited_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Drop the handle's reference. If a thread is somehow still inside a
    // call (the contract-violating case above), the connection is freed by
    // that thread's InUseGuard when it leaves, never while it is inside.
    if (c->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete c;
}

} // extern "C"
