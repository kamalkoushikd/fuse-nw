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

#include <arpa/inet.h>
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

#include "fuse/proto/aux.hpp"
#include "fuse/proto/block.hpp"
#include "fuse/proto/congestion.hpp"
#include "fuse/proto/receiver.hpp"
#include "fuse/proto/registry.hpp"
#include "fuse/proto/session_crypto.hpp"
#include "fuse/proto/udp.hpp"
#include "fuse/version.h"

using namespace fuse::proto;

namespace {

constexpr uint8_t kWindow = kMaxWindow;              // 64 blocks in flight
constexpr uint16_t kBlockSize = kDefaultPayloadSize; // 1200: MTU-safe
constexpr size_t kMaxMessage = 64u << 20;            // 64 MiB per message
constexpr size_t kRxBatch = 32;
constexpr uint32_t kDefaultTimeoutMs = 10000;

// The two directions of a connection must never share an AEAD nonce, and
// both sides start their sequence numbers at zero — so each direction gets
// its own lane id, which is part of the nonce.
constexpr uint16_t kLaneClientToServer = 0;
constexpr uint16_t kLaneServerToClient = 1;

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

// Associated data binds a block to its position so a valid block cannot be
// replayed elsewhere in the stream.
size_t build_aad(uint8_t *out, uint16_t lane, uint64_t seq, uint64_t offset) {
    size_t n = 0;
    n += put_u16(out + n, lane);
    n += put_u64(out + n, seq);
    n += put_u64(out + n, offset);
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

size_t encode_hello_ack(const uint8_t proof[kProofLen], uint8_t *out) {
    size_t off = 0;
    off += put_u8(out + off, kProtocolVersion);
    off += put_u8(out + off, static_cast<uint8_t>(MsgType::HelloAck));
    std::memcpy(out + off, proof, kProofLen);
    return off + kProofLen;
}

bool decode_hello_ack(const uint8_t *in, size_t len, uint8_t proof[kProofLen]) {
    if (len != kOuterHeaderSize + kProofLen) return false;
    if (in[0] != kProtocolVersion || static_cast<MsgType>(in[1]) != MsgType::HelloAck) return false;
    std::memcpy(proof, in + kOuterHeaderSize, kProofLen);
    return true;
}

size_t encode_close(uint8_t *out) {
    out[0] = kProtocolVersion;
    out[1] = static_cast<uint8_t>(MsgType::Close);
    return kOuterHeaderSize;
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
    LaneCipher tx_cipher;
    LaneCipher rx_cipher;

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

    ReceiverStream rx{0, kWindow, true};
    std::vector<uint8_t> asm_buf;      // message under reassembly
    uint64_t asm_end_seq = UINT64_MAX; // sequence carrying kFlagLastBlock
    size_t asm_total = 0;
    std::deque<std::vector<uint8_t>> inbox;

    bool peer_closed = false;
    bool failed = false;
    bool ack_pending = false;
    uint64_t last_ack_ns = 0;
    uint64_t rtt_est_ns = 0; // receiver-side estimate, for NACK pacing
    uint64_t nack_sent_ns = 0;

    fuse_conn_stats stats{};

    std::thread pump;
    std::atomic<bool> stop{false};

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
        uint32_t addr = 0;
        uint16_t port = 0;
        uint8_t challenge[kChallengeLen] = {};
        bool used = false;
    };
    Seen recent[16];
    size_t recent_at = 0;

    bool is_duplicate(const PeerAddr &from, const uint8_t challenge[kChallengeLen]) {
        for (const Seen &s : recent) {
            if (s.used && s.addr == from.addr.sin_addr.s_addr &&
                s.port == from.addr.sin_port &&
                std::memcmp(s.challenge, challenge, kChallengeLen) == 0) {
                return true;
            }
        }
        Seen &slot = recent[recent_at];
        recent_at = (recent_at + 1) % (sizeof(recent) / sizeof(recent[0]));
        slot.used = true;
        slot.addr = from.addr.sin_addr.s_addr;
        slot.port = from.addr.sin_port;
        std::memcpy(slot.challenge, challenge, kChallengeLen);
        return false;
    }
};

// ---------------------------------------------------------------------------
// Pump thread: everything that must keep happening whether or not the
// application is currently inside a send or a recv.

namespace {

void send_ack_locked(fuse_conn *c, uint8_t *scratch, size_t cap) {
    Ack ack = c->rx.build_ack();
    ack.stream_id = c->rx_lane;
    const size_t n = encode_ack(ack, scratch, cap);
    if (n) c->sock.send_to(scratch, n, c->peer);
    c->ack_pending = false;
    c->last_ack_ns = now_ns();
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
        uint8_t aad[18];
        const size_t al = build_aad(aad, c->tx_lane, seq, meta.offset);
        if (!c->tx_cipher.seal(c->tx_lane, seq, aad, al, slot->payload, slot->payload_len,
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

void handle_data_locked(fuse_conn *c, const uint8_t *dg, size_t dlen, uint8_t *plain) {
    BlockHeader hdr;
    uint64_t send_time = 0;
    const uint8_t *payload = nullptr;
    if (!decode_data_datagram(dg, dlen, &hdr, &send_time, &payload)) return;
    if (hdr.stream_id != c->rx_lane) return; // not our direction

    const uint8_t *body = payload;
    uint16_t body_len = hdr.payload_len;

    if (c->encrypted) {
        // Authenticate BEFORE admitting the block to the window: a forged
        // block that advanced the window would slide it past data that was
        // never written.
        uint8_t aad[18];
        const size_t al = build_aad(aad, c->rx_lane, hdr.seq_no, hdr.offset);
        if (hdr.payload_len < kAeadTagLen ||
            !c->rx_cipher.open(c->rx_lane, hdr.seq_no, aad, al, payload, hdr.payload_len,
                               plain)) {
            ++c->stats.auth_failures;
            return;
        }
        body = plain;
        body_len = static_cast<uint16_t>(hdr.payload_len - kAeadTagLen);
    }

    // A retransmission arriving after we NACKed measures one RTT on our own
    // clock, with no cross-host clock comparison.
    if ((hdr.flags & kFlagRetransmission) && c->nack_sent_ns != 0) {
        const uint64_t sample = now_ns() - c->nack_sent_ns;
        c->rtt_est_ns = (c->rtt_est_ns == 0) ? sample : (c->rtt_est_ns * 7 + sample) / 8;
        c->nack_sent_ns = 0;
    }

    if (c->rx.on_receive(hdr.seq_no, send_time, now_ns()) == ReceiveResult::Accepted) {
        const size_t end = static_cast<size_t>(hdr.offset) + body_len;
        if (end <= kMaxMessage) {
            if (c->asm_buf.size() < end) c->asm_buf.resize(end);
            if (body_len > 0) std::memcpy(c->asm_buf.data() + hdr.offset, body, body_len);
        }
        if (hdr.flags & kFlagLastBlock) {
            c->asm_end_seq = hdr.seq_no;
            c->asm_total = end;
        }
    }
    c->ack_pending = true;

    // The message is whole once every block up to and including its final
    // one has arrived contiguously.
    if (c->asm_end_seq != UINT64_MAX && c->rx.base_seq_no() > c->asm_end_seq) {
        c->asm_buf.resize(c->asm_total);
        c->inbox.push_back(std::move(c->asm_buf));
        c->asm_buf.clear();
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
    std::vector<uint8_t> scratch(kMaxDatagramSize + 64);
    std::vector<uint8_t> plain(kMaxPayloadSize + kAeadTagLen);
    const size_t slot = kMaxDatagramSize + 64;

    while (!c->stop.load(std::memory_order_relaxed)) {
        const int got = c->sock.recv_batch(rxbuf.data(), slot, kRxBatch, lens.data(), nullptr);

        std::unique_lock<std::mutex> lk(c->mu);
        for (int i = 0; i < got; ++i) {
            const uint8_t *dg = rxbuf.data() + i * slot;
            MsgType type;
            if (!peek_msg_type(dg, lens[i], &type)) continue;

            switch (type) {
                case MsgType::Data:
                    handle_data_locked(c, dg, lens[i], plain.data());
                    break;
                case MsgType::Ack: {
                    Ack ack;
                    if (!decode_ack(dg, lens[i], &ack)) break;
                    if (ack.base_seq_no > c->tx_base) {
                        for (uint64_t s = c->tx_base; s < ack.base_seq_no; ++s) c->reg.confirm(s);
                        c->tx_base = ack.base_seq_no;
                        c->last_progress_ns = now_ns();
                        c->cv.notify_all();
                    }
                    if (ack.echoed_send_time > 0) {
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
        // from us.
        if (c->ack_pending || (got > 0)) {
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
            const size_t n = encode_nack(nack, scratch.data(), scratch.size());
            if (n) c->sock.send_to(scratch.data(), n, c->peer);
            c->nack_sent_ns = t;
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

    c->reg.store(seq, data, len, now_ns());
    c->tx_meta[seq % kWindow] = TxMeta{offset, hdr.flags};

    const uint8_t *body = data;
    uint8_t sealed[kMaxPayloadSize + kAeadTagLen];
    if (c->encrypted) {
        uint8_t aad[18];
        const size_t al = build_aad(aad, c->tx_lane, seq, offset);
        if (!c->tx_cipher.seal(c->tx_lane, seq, aad, al, data, len, sealed)) return false;
        body = sealed;
        hdr.payload_len = static_cast<uint16_t>(len + kAeadTagLen);
    }

    const size_t n = encode_data_datagram(hdr, now_ns(), body, scratch, cap);
    if (n == 0) return false;
    c->sock.send_to(scratch, n, c->peer); // loss is repaired by ACK/NACK
    return true;
}

fuse_status wait_for(fuse_conn *c, std::unique_lock<std::mutex> &lk,
                     const std::function<bool()> &ready, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    while (!ready()) {
        if (c->failed) return FUSE_ERR_INTERNAL;
        if (timeout_ms < 0) {
            c->cv.wait_for(lk, std::chrono::milliseconds(100));
        } else {
            if (std::chrono::steady_clock::now() >= deadline) return FUSE_ERR_TIMEOUT;
            c->cv.wait_until(lk, deadline);
        }
    }
    return FUSE_OK;
}

void start_conn(fuse_conn *c, uint16_t tx_lane, uint16_t rx_lane, uint32_t timeout_ms) {
    c->tx_lane = tx_lane;
    c->rx_lane = rx_lane;
    c->timeout_ms = timeout_ms ? timeout_ms : kDefaultTimeoutMs;
    c->reg = SenderRegistry(tx_lane, kWindow);
    c->rx = ReceiverStream(rx_lane, kWindow, /*lossless=*/true);
    c->last_progress_ns = now_ns();
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
}

// ---- Server ---------------------------------------------------------------

fuse_listener *fuse_listen(const fuse_config *cfg, fuse_status *err) {
    auto fail = [&](fuse_status s) -> fuse_listener * {
        if (err) *err = s;
        return nullptr;
    };
    if (cfg == nullptr) return fail(FUSE_ERR_CONFIG);
    const bool want_crypto = cfg->pre_shared_key && cfg->pre_shared_key[0] != '\0';
    if (want_crypto && !session_crypto_available()) return fail(FUSE_ERR_UNSUPPORTED);

    auto *l = new (std::nothrow) fuse_listener();
    if (l == nullptr) return fail(FUSE_ERR_INTERNAL);
    if (!l->sock.open(cfg->bind_address, cfg->port)) {
        delete l;
        return fail(FUSE_ERR_SOCKET);
    }
    if (want_crypto) l->psk = cfg->pre_shared_key;
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

    uint8_t buf[512];
    for (;;) {
        if (timeout_ms == 0 || (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline)) {
            if (timeout_ms == 0) {
                // one non-blocking sweep only
            } else {
                return fail(FUSE_ERR_TIMEOUT);
            }
        }

        size_t got = 0;
        PeerAddr from;
        if (!l->sock.recv_from(buf, sizeof(buf), &got, &from)) {
            if (timeout_ms == 0) return fail(FUSE_ERR_TIMEOUT);
            continue;
        }

        uint8_t salt[kSessionSaltLen], challenge[kChallengeLen];
        if (!decode_hello(buf, got, salt, challenge)) continue;
        if (l->is_duplicate(from, challenge)) continue;

        auto *c = new (std::nothrow) fuse_conn();
        if (c == nullptr) return fail(FUSE_ERR_INTERNAL);

        // Each connection gets its own ephemeral socket, so one listen port
        // can serve many peers concurrently.
        if (!c->sock.open("0.0.0.0", 0)) {
            delete c;
            return fail(FUSE_ERR_SOCKET);
        }
        c->peer = from;

        uint8_t proof[kProofLen] = {};
        if (!l->psk.empty()) {
            uint8_t key[kSessionKeyLen];
            if (!derive_session_key(reinterpret_cast<const uint8_t *>(l->psk.data()),
                                    l->psk.size(), salt, key) ||
                !c->tx_cipher.init(key) || !c->rx_cipher.init(key)) {
                delete c;
                return fail(FUSE_ERR_UNSUPPORTED);
            }
            c->encrypted = true;
            // Prove we hold the key by sealing the client's challenge.
            if (!c->tx_cipher.seal(0xFFFF, 0, nullptr, 0, challenge, kChallengeLen, proof)) {
                delete c;
                return fail(FUSE_ERR_INTERNAL);
            }
        }

        uint8_t ack[64];
        const size_t n = encode_hello_ack(proof, ack);
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
    const bool want_crypto = cfg->pre_shared_key && cfg->pre_shared_key[0] != '\0';
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

    uint8_t salt[kSessionSaltLen] = {};
    uint8_t key[kSessionKeyLen] = {};
    if (want_crypto) {
        if (!random_bytes(salt, sizeof(salt)) ||
            !derive_session_key(reinterpret_cast<const uint8_t *>(cfg->pre_shared_key),
                                std::strlen(cfg->pre_shared_key), salt, key) ||
            !c->tx_cipher.init(key) || !c->rx_cipher.init(key)) {
            delete c;
            return fail(FUSE_ERR_UNSUPPORTED);
        }
        c->encrypted = true;
    }

    const uint32_t total_ms = cfg->timeout_ms ? cfg->timeout_ms : kDefaultTimeoutMs;
    set_rcv_timeout_ms(c->sock.fd(), 200);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);
    uint8_t hello[64], reply[128];

    while (std::chrono::steady_clock::now() < deadline) {
        // A fresh challenge per attempt, so a retry is a distinct handshake
        // and cannot be answered by a stale reply.
        uint8_t challenge[kChallengeLen];
        if (!want_crypto) {
            std::memset(challenge, 0, sizeof(challenge));
            const uint64_t t = now_ns();
            std::memcpy(challenge, &t, sizeof(challenge));
        } else if (!random_bytes(challenge, sizeof(challenge))) {
            delete c;
            return fail(FUSE_ERR_INTERNAL);
        }

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

        uint8_t proof[kProofLen];
        if (!decode_hello_ack(reply, got, proof)) continue;

        if (want_crypto) {
            uint8_t opened[kChallengeLen];
            if (!c->rx_cipher.open(0xFFFF, 0, nullptr, 0, proof, kProofLen, opened) ||
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

    std::vector<uint8_t> scratch(kMaxDatagramSize + 64);
    std::unique_lock<std::mutex> lk(c->mu);
    if (c->peer_closed) return FUSE_ERR_CLOSED;

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
    c->inbox.pop_front();
    return FUSE_OK;
}

fuse_status fuse_recv_alloc(fuse_conn *c, void **out, size_t *out_len, int timeout_ms) {
    if (c == nullptr || out == nullptr || out_len == nullptr) return FUSE_ERR_CONFIG;
    std::unique_lock<std::mutex> lk(c->mu);

    const auto have = [&] { return !c->inbox.empty() || c->peer_closed; };
    const fuse_status st = wait_for(c, lk, have, timeout_ms);
    if (st != FUSE_OK) return st;
    if (c->inbox.empty()) return FUSE_ERR_CLOSED;

    std::vector<uint8_t> msg = std::move(c->inbox.front());
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
    if (addr != nullptr && addr_cap > 0) {
        if (inet_ntop(AF_INET, &c->peer.addr.sin_addr, addr, static_cast<socklen_t>(addr_cap)) ==
            nullptr) {
            return FUSE_ERR_INTERNAL;
        }
    }
    if (port != nullptr) *port = ntohs(c->peer.addr.sin_port);
    return FUSE_OK;
}

int fuse_conn_is_closed(const fuse_conn *c) {
    if (c == nullptr) return 1;
    auto *m = const_cast<fuse_conn *>(c);
    std::lock_guard<std::mutex> lk(m->mu);
    return (m->peer_closed && m->inbox.empty()) || m->failed ? 1 : 0;
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
        uint8_t bye[8];
        const size_t n = encode_close(bye);
        // Best effort, and repeated: this is a courtesy so the peer's recv
        // returns promptly instead of waiting out its timeout.
        for (int i = 0; i < 3; ++i) c->sock.send_to(bye, n, c->peer);
    }
    c->stop.store(true, std::memory_order_relaxed);
    if (c->pump.joinable()) c->pump.join();
    delete c;
}

} // extern "C"
