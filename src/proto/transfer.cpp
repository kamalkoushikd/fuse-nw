#include "fuse/transfer.hpp"

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <new>
#include <random>
#include <stdexcept>
#include <thread>

#include "fuse/proto/block.hpp"
#include "fuse/proto/congestion.hpp"
#include "fuse/proto/control.hpp"
#include "fuse/proto/receiver.hpp"
#include "fuse/proto/registry.hpp"
#include "fuse/proto/session_crypto.hpp"
#include "fuse/proto/udp.hpp"

namespace fuse {

using namespace fuse::proto;

namespace {

constexpr uint16_t kWindow = kMaxWindow;
constexpr size_t kRxBatch = 64;
constexpr size_t kGsoBudget = 60000;
constexpr uint32_t kCleanBatchesToGrow = 8;
// A small block_size can otherwise pack far more segments into one
// send_segmented() call than the kernel's per-call GSO segment limit (64 or
// 128 depending on version) — that EINVAL isn't distinguishable from "this
// route doesn't support GSO at all", so it permanently latches gso_failed_
// on the socket (udp.cpp) even though a smaller batch would have worked
// fine. Capping here avoids ever hitting that limit.
constexpr size_t kMaxGsoSegments = 64;

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

void set_bufs(int fd, int bytes) {
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

void set_timeout_us(int fd, long us) {
    timeval tv{};
    tv.tv_sec = us / 1000000;
    tv.tv_usec = us % 1000000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Associated data binds a block to its identity so a valid block cannot be
// replayed at another position.
// Handshake anti-replay nonce for StreamStart (kept independent of the
// crypto backend: this guards session identity, not confidentiality, so it
// must work in unencrypted transfers too).
uint64_t random_nonce() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return rng();
}

size_t build_aad(uint8_t *out, uint16_t lane, uint64_t seq, uint64_t offset, uint8_t flags) {
    size_t n = 0;
    n += put_u16(out + n, lane);
    n += put_u64(out + n, seq);
    n += put_u64(out + n, offset);
    n += put_u8(out + n, flags);
    return n;
}

bool cancel_requested(const TransferConfig &cfg) {
    return cfg.cancel != nullptr && cfg.cancel->load(std::memory_order_acquire);
}

// Tells the peer this lane is over, so it stops now instead of waiting out
// its own timeout. Best effort and unacknowledged: sent a few times because
// one lost datagram would otherwise cost the peer the full timeout, and the
// peer's timeout still covers the case where all copies are lost.
void send_stream_close(UdpSocket &sock, const PeerAddr &to, uint16_t lane, uint64_t nonce,
                       uint8_t reason) {
    StreamClose sc;
    sc.stream_id = lane;
    sc.nonce = nonce;
    sc.reason = reason;
    uint8_t buf[32];
    const size_t n = encode_stream_close(sc, buf, sizeof(buf));
    for (int i = 0; n != 0 && i < 3; ++i) sock.send_to(buf, n, to);
}

// Written by one lane thread, read by the calling thread for progress
// reports (relaxed is enough for the counters: a progress snapshot only
// needs each value to be one that was really stored, not a consistent cut
// across all of them). `bytes` is live: acknowledged bytes on the sender,
// written bytes on the receiver.
struct LaneResult {
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> retransmits{0};
    std::atomic<uint64_t> auth_failures{0};
    std::atomic<uint32_t> final_block{0};
    std::atomic<uint64_t> start_ns{0};
    std::atomic<uint64_t> end_ns{0};
    std::atomic<int> status{static_cast<int>(TransferStatus::Incomplete)};
    // Receiver only: this lane's size, published (release) once its
    // StreamStart has been accepted.
    std::atomic<uint64_t> expected{0};
    std::atomic<bool> started{false};
    // Set (release) after the lane function returns, whatever the outcome.
    std::atomic<bool> finished{false};
};

struct Keys {
    bool enabled = false;
    uint8_t key[kSessionKeyLen] = {};
    uint8_t salt[kSessionSaltLen] = {};
};

// --- Receiver lane -------------------------------------------------------

// `stop_all` is shared by every lane of one call: a lane that is cancelled or
// told the peer aborted sets it, and its siblings then stop too — so one
// lane whose abort datagrams were all lost doesn't sit out its full timeout.
void recv_lane(const TransferConfig &cfg, uint16_t lane, const std::string &psk,
               std::vector<uint8_t> *shard, LaneResult *res, std::atomic<bool> *stop_all) {
    UdpSocket sock;
    if (!sock.open(cfg.bind_address.c_str(), static_cast<uint16_t>(cfg.base_port + lane))) {
        res->status.store(static_cast<int>(TransferStatus::SocketError));
        return;
    }
    set_bufs(sock.fd(), 32 << 20);
    set_timeout_us(sock.fd(), 200000);

    ReceiverStream rx(lane, kWindow, /*lossless=*/true);
    LaneCipher cipher; // initialised once the sender's salt arrives
    const bool want_crypto = !psk.empty();

    std::vector<uint8_t> opened(kMaxPayloadSize + kAeadTagLen);
    const size_t slot = kMaxDatagramSize + 64;
    std::vector<uint8_t> rx_buf(kRxBatch * slot);
    std::vector<size_t> lens(kRxBatch);
    std::vector<PeerAddr> srcs(kRxBatch);
    std::vector<uint8_t> tx(kMaxAuxDatagramSize + 64);

    uint64_t shard_bytes = 0, final_seq = UINT64_MAX, written = 0;
    uint64_t delivered = 0, last_ack_blocks = 0, last_ack_ns = 0, auth_failures = 0;
    // The reorder-tolerance and re-NACK intervals must scale with the path
    // RTT, or a WAN path storms duplicate NACKs (a re-NACK every few ms while
    // the retransmit is still 50 ms away). The receiver estimates RTT purely
    // from its own clock: the delay between sending a NACK and the
    // retransmission arriving. Until it has a sample, conservative WAN-safe
    // defaults are used rather than the loopback-tight ones.
    uint64_t rtt_est_ns = 0, nack_sent_ns = 0;
    // Set once every byte has arrived; the loop keeps running (still
    // answering with fresh Acks on its normal cadence below) instead of
    // exiting immediately. Firing a single burst of Acks and abandoning the
    // socket right away — the previous behavior — means that if that whole
    // burst is lost together (a real risk: they went out back-to-back with
    // no spacing, so they're correlated, not independent draws), the sender
    // can NEVER get confirmation once this socket is gone, and sits
    // retransmitting into the void until its own timeout_ms (2 minutes, by
    // default) gives up. Exit as soon as the peer goes quiet for a couple of
    // idle ticks after completion (it got an Ack and left — the common case,
    // adding well under a second) rather than always waiting out the full
    // grace window; kCloseGraceNs is only the upper bound for the lossy case
    // where every Ack sent so far may have been missed.
    uint64_t completed_at_ns = 0;
    int idle_at_completion = -1;
    constexpr uint64_t kCloseGraceNs = 1'500'000'000ull;
    constexpr int kQuietIdleTicks = 2; // ~400ms at the 200ms recv timeout below
    bool have_start = false, have_peer = false;
    // Set from the sender's StreamClose: "finished" lets a completed lane
    // exit at once instead of lingering; "aborted" ends the lane now.
    bool peer_finished = false, peer_aborted = false;
    uint64_t peer_nonce = 0; // echoed in every Ack once StreamStart arrives
    PeerAddr peer{};
    int idle = 0;
    const int max_idle = static_cast<int>(cfg.timeout_ms / 200) + 5;

    // An active MITM/corruption attack fails most blocks' auth check rather
    // than merely dropping some — that pattern is distinguishable from
    // ordinary loss (which NACK/retransmit already resolves) and deserves a
    // status the caller can act on differently from "just incomplete" or
    // "just timed out". Shared between every exit path below (the natural
    // completion path used to be the only one that checked this, which made
    // the classification dead code for the common wrong-key case: that
    // scenario never reaches natural completion — every block fails auth,
    // so `rx.on_receive` is never called, `base_seq_no` never advances, and
    // the lane can only ever leave via the idle-timeout branch instead).
    // Require a minimum sample size so a couple of early failures during
    // key/salt setup don't misreport a healthy transfer.
    const auto classify = [&](TransferStatus fallback) {
        const uint64_t auth_sample = delivered + auth_failures;
        if (auth_sample >= 20 && auth_failures * 2 > auth_sample) {
            return TransferStatus::AuthFailed;
        }
        return fallback;
    };

    // Every early exit below tells the sender (once we know where it is and
    // which session it is), so it stops at once rather than retransmitting
    // into a closed port until its own timeout.
    const auto give_up = [&](TransferStatus st) {
        if (have_peer && have_start) {
            send_stream_close(sock, peer, lane, peer_nonce, kStreamCloseAborted);
        }
        res->bytes.store(written);
        res->auth_failures.store(auth_failures);
        res->status.store(static_cast<int>(st));
    };

    for (;;) {
        // Polled once per loop; the 200 ms receive timeout bounds how long a
        // cancel can go unnoticed.
        if (cancel_requested(cfg)) {
            give_up(TransferStatus::Cancelled);
            stop_all->store(true, std::memory_order_release);
            return;
        }
        if (stop_all->load(std::memory_order_acquire)) {
            // A sibling lane heard the sender abort; this lane's copy of
            // that message may simply have been lost.
            give_up(classify(TransferStatus::PeerAborted));
            return;
        }

        const int got = sock.recv_batch(rx_buf.data(), slot, kRxBatch, lens.data(), srcs.data());
        if (got > 0) {
            idle = 0;
            if (res->start_ns.load() == 0) res->start_ns.store(now_ns());
        } else if (++idle > max_idle) {
            give_up(classify(TransferStatus::Timeout));
            return;
        }

        for (int i = 0; i < got; ++i) {
            const uint8_t *dg = rx_buf.data() + i * slot;
            const size_t dlen = lens[i];
            MsgType type;
            if (!peek_msg_type(dg, dlen, &type)) continue;

            if (type == MsgType::StreamStart) {
                StreamStart ss;
                if (!decode_stream_start(dg, dlen, &ss)) continue;
                if (!have_start) {
                    // ss.total_bytes is an unauthenticated wire value at
                    // this point (the PSK proof, if any, is checked per
                    // block, not here) — a hostile or corrupt claim near
                    // UINT64_MAX must not be able to take the whole process
                    // down via an uncaught allocation failure escaping this
                    // thread. Try the allocation before committing any
                    // state, so a rejected StreamStart leaves this lane
                    // exactly as if it had never arrived.
                    try {
                        shard->assign(ss.total_bytes, 0);
                    } catch (const std::bad_alloc &) {
                        res->status.store(static_cast<int>(TransferStatus::ResourceLimit));
                        return;
                    } catch (const std::length_error &) {
                        res->status.store(static_cast<int>(TransferStatus::ResourceLimit));
                        return;
                    }
                    have_start = true;
                    peer_nonce = ss.nonce;
                    peer = srcs[i];
                    have_peer = true;
                    shard_bytes = ss.total_bytes;
                    res->expected.store(shard_bytes, std::memory_order_relaxed);
                    res->started.store(true, std::memory_order_release);
                    if (want_crypto) {
                        uint8_t key[kSessionKeyLen];
                        if (!derive_session_key(reinterpret_cast<const uint8_t *>(psk.data()),
                                                psk.size(), ss.session_salt, kSessionSaltLen,
                                                key) ||
                            !cipher.init(key)) {
                            res->status.store(static_cast<int>(TransferStatus::Unsupported));
                            return;
                        }
                    }
                } else if (ss.nonce == peer_nonce) {
                    // A retried StreamStart (our first Ack was lost) carrying
                    // the same nonce we already accepted — safe to treat as
                    // this session even if it now arrives from a different
                    // address (the sender's NAT mapping may have rotated).
                    peer = srcs[i];
                    have_peer = true;
                }
                continue;
            }
            if (type == MsgType::StreamClose) {
                StreamClose sc;
                if (!decode_stream_close(dg, dlen, &sc)) continue;
                if (have_start) {
                    // Only this session's nonce counts once a session
                    // exists: a close left over from an earlier transfer on
                    // this port must not end this one.
                    if (sc.nonce != peer_nonce) continue;
                    if (sc.reason == kStreamCloseAborted) {
                        peer_aborted = true;
                    } else {
                        peer_finished = true;
                    }
                } else if (sc.reason == kStreamCloseAborted) {
                    // No session yet, so no nonce to check: a sender that is
                    // cancelled before its StreamStart reached us (e.g.
                    // Ctrl+C while it was still loading the file) can only
                    // say "I'm not coming". Accepting that is no weaker than
                    // today's pre-session state, where any first StreamStart
                    // already claims the lane.
                    peer_aborted = true;
                }
                continue;
            }
            if (type != MsgType::Data || !have_start) continue;

            BlockHeader hdr;
            uint64_t send_time = 0;
            const uint8_t *payload = nullptr;
            if (!decode_data_datagram(dg, dlen, &hdr, &send_time, &payload)) continue;

            const uint8_t *body = payload;
            uint16_t body_len = hdr.payload_len;

            if (want_crypto) {
                // Authenticate before admitting the block to the window: a
                // forged block that advanced `base` would slide the window
                // past data never written. `flags` rides in the AAD (not
                // just lane/seq/offset) so LastBlock can't be flipped on an
                // otherwise-authentic block to end the lane early.
                uint8_t aad[19];
                const size_t al = build_aad(aad, lane, hdr.seq_no, hdr.offset, hdr.flags);
                if (payload == nullptr || hdr.payload_len < kAeadTagLen ||
                    !cipher.open(lane, hdr.seq_no, aad, al, payload, hdr.payload_len,
                                 opened.data())) {
                    ++auth_failures;
                    continue;
                }
                body = opened.data();
                body_len = static_cast<uint16_t>(hdr.payload_len - kAeadTagLen);
            }

            // flags is only trustworthy once the block has cleared the auth
            // check above (or in plaintext mode, where nothing claims
            // otherwise) — setting this any earlier would let a forged
            // packet end the lane early even under encryption.
            if (hdr.flags & kFlagLastBlock) final_seq = hdr.seq_no;

            // Re-anchoring and RTT sampling both wait for on_receive's
            // verdict now: `Accepted` means this block is both authentic
            // (if encrypted) AND new, not a replay of something already
            // delivered. That second part matters even for an
            // authenticated block — AEAD doesn't add freshness against an
            // exact-replay, so without this an attacker who captures one
            // genuine datagram and resends it from their own address could
            // redirect where this lane's Acks go, no key needed.
            const ReceiveResult rr = rx.on_receive(hdr.seq_no, send_time, now_ns());
            if (rr == ReceiveResult::Accepted) {
                // This block just proved itself and is new — safe to trust
                // its source as where to send ACKs, even if it differs from
                // the address we've been using. This is what makes an
                // in-progress transfer survive the sender's NAT mapping
                // changing mid-flight (an ISP-forced reconnect, a mobile
                // handover, a CGNAT re-lease): the next new block the
                // sender emits re-anchors the receiver's reply address,
                // with no session drop and no explicit handshake.
                peer = srcs[i];
                have_peer = true;

                // A retransmission arriving after we NACK'd measures one
                // RTT on the receiver's own clock (no cross-host clock
                // comparison).
                if ((hdr.flags & kFlagRetransmission) && nack_sent_ns != 0) {
                    const uint64_t sample = now_ns() - nack_sent_ns;
                    rtt_est_ns = (rtt_est_ns == 0) ? sample : (rtt_est_ns * 7 + sample) / 8;
                    nack_sent_ns = 0;
                }

                if (body != nullptr) {
                    // Checked this way round so `hdr.offset + body_len`
                    // (wire fields, attacker-controlled) never gets
                    // computed: with the addition done first, an offset
                    // near UINT64_MAX wraps the sum small enough to pass a
                    // naive "<= size()" check, and the memcpy below then
                    // writes out of bounds at the unwrapped offset.
                    if (hdr.offset <= shard->size() && body_len <= shard->size() - hdr.offset) {
                        std::memcpy(shard->data() + hdr.offset, body, body_len);
                        written += body_len;
                        res->bytes.store(written, std::memory_order_relaxed);
                    }
                    ++delivered;
                }
            }
        }

        if (peer_aborted) {
            // The sender already knows it's over; no need to tell it back.
            // classify() still applies: a sender with the wrong key gives up
            // (and says so) precisely because every block failed auth here,
            // and AuthFailed is the diagnosis the caller needs, not "aborted".
            res->bytes.store(written);
            res->auth_failures.store(auth_failures);
            res->status.store(static_cast<int>(classify(TransferStatus::PeerAborted)));
            stop_all->store(true, std::memory_order_release);
            return;
        }

        if (!have_peer || !have_start) continue;

        const uint64_t t = now_ns();
        if (delivered - last_ack_blocks >= 8 || t - last_ack_ns > 200000) {
            last_ack_blocks = delivered;
            last_ack_ns = t;
            Ack ack = rx.build_ack();
            ack.nonce = peer_nonce;
            const size_t n = encode_ack(ack, tx.data(), tx.size());
            if (n) sock.send_to(tx.data(), n, peer);
        }

        // reorder tolerance ~ half an RTT, re-NACK interval > one RTT so a
        // gap is not re-reported before its retransmit can arrive.
        const uint64_t reorder_ns = (rtt_est_ns == 0) ? 3'000'000
                                                      : std::clamp<uint64_t>(rtt_est_ns / 2,
                                                                             1'000'000, 100'000'000);
        const uint64_t renack_ns = (rtt_est_ns == 0) ? 40'000'000
                                                     : std::clamp<uint64_t>(rtt_est_ns * 3 / 2,
                                                                            20'000'000, 500'000'000);
        Nack nack;
        if (rx.collect_nacks(t, reorder_ns, renack_ns, &nack) > 0) {
            const size_t n = encode_nack(nack, tx.data(), tx.size());
            if (n) sock.send_to(tx.data(), n, peer);
            nack_sent_ns = t;
        }

        if (final_seq != UINT64_MAX && rx.base_seq_no() > final_seq) {
            if (completed_at_ns == 0) {
                completed_at_ns = t;
                idle_at_completion = idle;
                // The transfer's own duration ends here, not after the
                // linger below, so the reported throughput isn't diluted by it.
                res->end_ns.store(t);
            }
            // The sender's "finished" close means it has every Ack it needs:
            // leave now. Otherwise linger, as before, in case our Acks were
            // lost and the sender is still waiting for one.
            if (peer_finished) break;
            if (t - completed_at_ns > kCloseGraceNs ||
                idle - idle_at_completion >= kQuietIdleTicks) {
                break;
            }
        }
    }

    for (int i = 0; !peer_finished && i < 8; ++i) {
        Ack ack = rx.build_ack();
        ack.nonce = peer_nonce;
        const size_t n = encode_ack(ack, tx.data(), tx.size());
        if (n) sock.send_to(tx.data(), n, peer);
    }

    res->bytes.store(written);
    res->auth_failures.store(auth_failures);
    res->status.store(static_cast<int>(
        classify(written == shard_bytes ? TransferStatus::Ok : TransferStatus::Incomplete)));
}

// --- Sender lane ---------------------------------------------------------

// `stop_all`: see recv_lane.
void send_lane(const TransferConfig &cfg, uint16_t lane, const Keys &keys, const uint8_t *data,
               uint64_t shard_bytes, LaneResult *res, std::atomic<bool> *stop_all) {
    UdpSocket sock;
    if (!sock.open("0.0.0.0", 0)) {
        res->status.store(static_cast<int>(TransferStatus::SocketError));
        return;
    }
    set_bufs(sock.fd(), 32 << 20);
    set_timeout_us(sock.fd(), 200);

    PeerAddr dst;
    if (!UdpSocket::resolve(cfg.host.c_str(), static_cast<uint16_t>(cfg.base_port + lane),
                            &dst)) {
        res->status.store(static_cast<int>(TransferStatus::ConfigError));
        return;
    }

    SenderRegistry reg(lane, kWindow);
    CongestionController cc(kWindow, true, 1, 16);

    LaneCipher cipher;
    std::vector<uint8_t> sealed(kMaxPayloadSize + kAeadTagLen);
    if (keys.enabled && !cipher.init(keys.key)) {
        res->status.store(static_cast<int>(TransferStatus::Unsupported));
        return;
    }

    std::vector<uint8_t> staging(kGsoBudget + kMaxDatagramSize);
    std::vector<uint8_t> ctl(kMaxAuxDatagramSize + 64);
    // A retransmit re-encodes a full data block, which can be far larger
    // than any control message.
    std::vector<uint8_t> rtx(kMaxDatagramSize + 64);

    StreamStart ss;
    ss.stream_id = lane;
    ss.total_blocks = 0; // unknown up front: block size adapts
    ss.block_size = cfg.block_size;
    ss.total_bytes = shard_bytes;
    if (keys.enabled) std::memcpy(ss.session_salt, keys.salt, kSessionSaltLen);
    // Fixed for the whole handshake (every retry sends the same nonce) so
    // any one matching Ack confirms it — but unique to this attempt at this
    // lane, so a stale Ack left over from an earlier session on this port
    // can't be mistaken for confirmation of this one.
    ss.nonce = random_nonce();

    // Bounded by time (connect_timeout_ms, capped at timeout_ms), not by a
    // retry count. This used to be 200 tries x 200 us = 40 ms in total, so
    // any path with an RTT over 40 ms — or a receiver that took longer than
    // that to allocate a large shard — failed the handshake with Timeout
    // before the first Ack could possibly arrive. Retries back off from 1 ms
    // to 200 ms so a slow path isn't flooded with StreamStarts.
    bool started = false;
    const uint64_t hs_begin = now_ns();
    const uint64_t hs_limit_ns =
        static_cast<uint64_t>(std::min(cfg.connect_timeout_ms, cfg.timeout_ms)) * 1000000ull;
    uint64_t resend_every_ns = 1'000'000, next_send_ns = 0;
    while (!started) {
        const uint64_t t = now_ns();
        const bool cancelled = cancel_requested(cfg);
        const bool sibling_stopped = stop_all->load(std::memory_order_acquire);
        if (cancelled || sibling_stopped || t - hs_begin > hs_limit_ns) {
            // The receiver may already have accepted an earlier StreamStart
            // whose Ack we just haven't seen yet — or never seen one at all
            // (it accepts a pre-session abort too): either way, tell it
            // we're leaving so it doesn't wait out its own timeout.
            send_stream_close(sock, dst, lane, ss.nonce, kStreamCloseAborted);
            res->status.store(static_cast<int>(cancelled         ? TransferStatus::Cancelled
                                               : sibling_stopped ? TransferStatus::PeerAborted
                                                                 : TransferStatus::Timeout));
            if (cancelled) stop_all->store(true, std::memory_order_release);
            return;
        }
        if (t >= next_send_ns) {
            const size_t n = encode_stream_start(ss, ctl.data(), ctl.size());
            sock.send_to(ctl.data(), n, dst);
            next_send_ns = t + resend_every_ns;
            resend_every_ns = std::min<uint64_t>(resend_every_ns * 2, 200'000'000ull);
        }
        size_t got = 0;
        if (sock.recv_from(ctl.data(), ctl.size(), &got, nullptr)) {
            MsgType mt;
            if (peek_msg_type(ctl.data(), got, &mt) && mt == MsgType::Ack) {
                Ack ack;
                if (decode_ack(ctl.data(), got, &ack) && ack.nonce == ss.nonce) {
                    started = true;
                }
            }
        }
    }

    sock.set_nonblocking(true);

    // The AEAD tag rides inside payload_len, so the plaintext ceiling must
    // leave room for it.
    const uint16_t block_ceiling =
        keys.enabled ? static_cast<uint16_t>(kMaxPayloadSize - kAeadTagLen) : kMaxPayloadSize;
    uint16_t block = std::min<uint16_t>(cfg.block_size, block_ceiling);

    uint32_t clean_batches = 0;
    uint64_t base = 0, next_seq = 0, next_offset = 0, retransmits = 0;
    uint64_t last_base = 0, last_progress = now_ns();
    // Deliberately separate from `last_progress`: that one also advances on
    // a merely-*attempted* local retransmit send succeeding, which paces
    // the RTO but is not evidence the peer is still there — on loopback a
    // local send to a dead/wrong-keyed peer keeps "succeeding" forever, so
    // reusing it for the overall deadline check below would make this loop
    // literally unbounded against an unreachable peer instead of honoring
    // timeout_ms. This one only moves when `base` is actually confirmed by
    // an Ack.
    uint64_t last_real_progress = now_ns();
    uint64_t last_rtt_echo = 0; // dedupes repeated Ack echoes (see N13 below)
    const uint64_t t0 = now_ns();
    const uint64_t deadline_ns = static_cast<uint64_t>(cfg.timeout_ms) * 1000000ull;
    bool sent_last = false;
    uint64_t acked_bytes = 0; // live progress, published through res->bytes

    // Every early exit tells the receiver, so it stops at once instead of
    // waiting out its own idle timeout.
    const auto give_up = [&](TransferStatus st, bool tell_peer) {
        if (tell_peer) send_stream_close(sock, dst, lane, ss.nonce, kStreamCloseAborted);
        res->retransmits.store(retransmits);
        res->status.store(static_cast<int>(st));
    };

    res->start_ns.store(t0);

    for (;;) {
        if (cancel_requested(cfg)) {
            give_up(TransferStatus::Cancelled, true);
            stop_all->store(true, std::memory_order_release);
            return;
        }
        if (stop_all->load(std::memory_order_acquire)) {
            // A sibling lane heard the receiver abort; make sure this lane's
            // receiver end hears it too, in case its own copy was lost.
            give_up(TransferStatus::PeerAborted, true);
            return;
        }
        cc.poll(now_ns());
        const uint64_t window = std::min<uint64_t>(kWindow, std::max<uint32_t>(4, cc.window()));
        bool did_work = false;

        // A zero-length shard (an empty send_buffer call, or an uneven
        // split across lanes leaving one lane with nothing) still needs its
        // own completion signal: without this, next_offset(0) < shard_bytes
        // (0) is false from the start, the packing loop below never runs,
        // `sent_last` never gets set, and the lane spins until timeout
        // instead of completing immediately.
        const bool need_empty_last = (shard_bytes == 0 && next_seq == 0);
        if ((next_offset < shard_bytes || need_empty_last) && next_seq < base + window) {
            const uint16_t seg = static_cast<uint16_t>(kDataPrefixSize + block +
                                                       (keys.enabled ? kAeadTagLen : 0));
            const size_t max_segs = std::min<size_t>(
                {kGsoBudget / seg, window - (next_seq - base), kMaxGsoSegments});
            size_t packed = 0, bytes_in_batch = 0;

            while (packed < max_segs && (next_offset < shard_bytes || need_empty_last)) {
                const uint64_t remaining = shard_bytes - next_offset;
                if (remaining < block && packed > 0) break;

                const uint16_t len = static_cast<uint16_t>(std::min<uint64_t>(block, remaining));
                const bool is_last = (next_offset + len >= shard_bytes);

                BlockHeader hdr;
                hdr.stream_id = lane;
                hdr.seq_no = next_seq;
                hdr.flags = is_last ? kFlagLastBlock : 0;
                hdr.payload_len = len;
                hdr.offset = next_offset;

                reg.store(next_seq, data + next_offset, len, now_ns(), next_offset, hdr.flags);

                const uint8_t *body = data + next_offset;
                if (keys.enabled) {
                    uint8_t aad[19];
                    const size_t al = build_aad(aad, lane, next_seq, next_offset, hdr.flags);
                    if (!cipher.seal(lane, next_seq, aad, al, data + next_offset, len,
                                     sealed.data())) {
                        break; // fail closed rather than emit plaintext
                    }
                    body = sealed.data();
                    hdr.payload_len = static_cast<uint16_t>(len + kAeadTagLen);
                }

                const size_t n = encode_data_datagram(hdr, now_ns(), body,
                                                      staging.data() + bytes_in_batch,
                                                      staging.size() - bytes_in_batch);
                if (n == 0) break;
                bytes_in_batch += n;
                ++packed;
                ++next_seq;
                next_offset += len;
                if (is_last) { sent_last = true; break; }
                if (n != seg) break;
            }

            if (packed > 0) {
                did_work = true;
                if (packed == 1 ||
                    !sock.send_segmented(staging.data(), bytes_in_batch, seg, dst)) {
                    size_t off = 0;
                    for (size_t i = 0; i < packed && off < bytes_in_batch; ++i) {
                        const size_t n = std::min<size_t>(seg, bytes_in_batch - off);
                        sock.send_to(staging.data() + off, n, dst);
                        off += n;
                    }
                }
            }
        }

        bool loss_seen = false;
        for (int drain = 0; drain < 64; ++drain) {
            size_t got = 0;
            PeerAddr src;
            if (!sock.recv_from(ctl.data(), ctl.size(), &got, &src)) break;
            did_work = true;
            MsgType type;
            if (!peek_msg_type(ctl.data(), got, &type)) continue;

            if (type == MsgType::Ack) {
                Ack ack;
                if (!decode_ack(ctl.data(), got, &ack)) continue;
                if (ack.nonce != ss.nonce) continue; // not this session
                // The Ack proved it holds this session's nonce, so its
                // source is safe to trust even if it differs from `dst` —
                // this is what lets a transfer survive the receiver's NAT
                // mapping changing mid-flight, symmetric to the migration
                // recv_lane does for the sender's address.
                dst = src;
                if (ack.base_seq_no > base) {
                    // Acks are unauthenticated even under PSK (only the
                    // nonce above is checked), so base_seq_no is untrusted
                    // input: clamp to what has actually been sent, or a
                    // forged huge value spins this loop ~2^64 times.
                    const uint64_t new_base = std::min(ack.base_seq_no, next_seq);
                    for (uint64_t s = base; s < new_base; ++s) {
                        // Count the block before confirm() frees its slot.
                        if (const RegistrySlot *sl = reg.lookup(s)) acked_bytes += sl->payload_len;
                        reg.confirm(s);
                    }
                    base = new_base;
                    res->bytes.store(acked_bytes, std::memory_order_relaxed);
                }
                // The receiver only updates its echoed send-time when a new
                // highest-seq block arrives (receiver.cpp) — every
                // periodic/final Ack in between repeats the same value.
                // Treating each repeat as a fresh sample manufactures an
                // ever-growing fake RTT while the sender is stalled on a
                // gap, which stretches the RTO and congestion-control
                // epochs right when they need to stay responsive.
                if (ack.echoed_send_time > 0 && ack.echoed_send_time != last_rtt_echo) {
                    last_rtt_echo = ack.echoed_send_time;
                    const uint64_t t = now_ns();
                    if (t > ack.echoed_send_time) cc.on_rtt_sample(t - ack.echoed_send_time);
                }
            } else if (type == MsgType::Nack) {
                Nack nack;
                if (!decode_nack(ctl.data(), got, &nack)) continue;
                cc.on_loss();
                loss_seen = true;
                for (uint16_t i = 0; i < nack.count; ++i) {
                    const uint64_t seq = nack.missing[i];
                    const RegistrySlot *sl = reg.lookup(seq);
                    if (!sl) continue;
                    BlockHeader hdr;
                    hdr.stream_id = lane;
                    hdr.seq_no = seq;
                    // OR, not overwrite: a retransmitted last block is still
                    // the last block, and the receiver has no other way to
                    // learn a stream finished if this bit is dropped here.
                    hdr.flags = sl->flags | kFlagRetransmission;
                    hdr.payload_len = sl->payload_len;
                    hdr.offset = sl->offset;
                    const uint8_t *body = sl->payload;
                    if (keys.enabled) {
                        uint8_t aad[19];
                        const size_t al = build_aad(aad, lane, seq, hdr.offset, hdr.flags);
                        if (!cipher.seal(lane, seq, aad, al, sl->payload, sl->payload_len,
                                         sealed.data())) {
                            continue;
                        }
                        body = sealed.data();
                        hdr.payload_len = static_cast<uint16_t>(sl->payload_len + kAeadTagLen);
                    }
                    const size_t n =
                        encode_data_datagram(hdr, now_ns(), body, rtx.data(), rtx.size());
                    if (n && sock.send_to(rtx.data(), n, dst)) ++retransmits;
                }
            } else if (type == MsgType::StreamClose) {
                StreamClose sc;
                if (decode_stream_close(ctl.data(), got, &sc) && sc.nonce == ss.nonce &&
                    sc.reason == kStreamCloseAborted) {
                    // The receiver gave up (cancelled or timed out) and said
                    // so: stop now rather than retransmitting at a closed
                    // port until our own timeout.
                    give_up(TransferStatus::PeerAborted, false);
                    stop_all->store(true, std::memory_order_release);
                    return;
                }
            }
        }

        // Retransmission timeout, adaptive to the measured RTT. A fixed 5 ms
        // is right for loopback but catastrophic on any real path: it fires
        // ~RTT/5ms times before the ACK can arrive, so the sender retransmits
        // every in-flight block many times over. The congestion controller
        // already smooths an RTT estimate from the ACK echo; the RTO is
        // ~2*RTT, with a 1 s default until the first sample lands.
        const uint64_t srtt = cc.rtt_ns();
        const uint64_t rto_ns =
            srtt ? std::clamp<uint64_t>(srtt * 2, 5'000'000ull, 1'000'000'000ull)
                 : 1'000'000'000ull;

        const uint64_t t = now_ns();
        const bool base_advanced = (base != last_base);
        if (base_advanced) {
            last_base = base;
            last_progress = t;
            last_real_progress = t;
        } else if (t - last_progress > rto_ns) {
            cc.on_loss();
            const RegistrySlot *sl = reg.lookup(base);
            if (sl) {
                BlockHeader hdr;
                hdr.stream_id = lane;
                hdr.seq_no = base;
                hdr.flags = sl->flags | kFlagRetransmission; // see the NACK path's comment above
                hdr.payload_len = sl->payload_len;
                hdr.offset = sl->offset;
                const uint8_t *body = sl->payload;
                if (keys.enabled) {
                    uint8_t aad[19];
                    const size_t al = build_aad(aad, lane, base, hdr.offset, hdr.flags);
                    if (cipher.seal(lane, base, aad, al, sl->payload, sl->payload_len,
                                    sealed.data())) {
                        body = sealed.data();
                        hdr.payload_len = static_cast<uint16_t>(sl->payload_len + kAeadTagLen);
                    }
                }
                const size_t n =
                    encode_data_datagram(hdr, now_ns(), body, rtx.data(), rtx.size());
                // A refused send (buffer full) must not count as progress, or
                // the block is never retransmitted and the transfer stalls.
                if (n && sock.send_to(rtx.data(), n, dst)) {
                    ++retransmits;
                    last_progress = t;
                } else {
                    timespec nap{0, 200000};
                    nanosleep(&nap, nullptr);
                }
            } else {
                last_progress = t;
            }
        }

        // Block size only grows on a real run of ACK-confirmed batches, not
        // merely on loop iterations (which include idle naps and can rack
        // up kCleanBatchesToGrow well within a single RTT, before any ACK
        // has even had time to arrive — growing past path MTU and
        // fragmenting on an ordinary network, the opposite of wire.hpp's
        // intent).
        if (loss_seen) {
            block = std::min<uint16_t>(cfg.block_size, block_ceiling);
            clean_batches = 0;
        } else if (base_advanced && ++clean_batches >= kCleanBatchesToGrow &&
                  block < block_ceiling) {
            clean_batches = 0;
            block = static_cast<uint16_t>(std::min<uint32_t>(block_ceiling, block * 2u));
        }

        res->retransmits.store(retransmits, std::memory_order_relaxed);
        if (sent_last && base >= next_seq) break;
        // TransferConfig::timeout_ms is documented as "no progress for this
        // long", not a total deadline — `last_real_progress` (only moves on
        // an Ack-confirmed advance of `base`) is what makes this match that
        // contract and the receiver's own idle-based timeout, instead of
        // failing a transfer that is slow but still moving.
        if (t - last_real_progress > deadline_ns) {
            give_up(TransferStatus::Timeout, true);
            return;
        }
        if (!did_work) {
            timespec nap{0, 20000};
            nanosleep(&nap, nullptr);
        }
    }

    res->end_ns.store(now_ns());
    // Everything is acknowledged: let the receiver leave now instead of
    // lingering to re-send Acks we no longer need.
    send_stream_close(sock, dst, lane, ss.nonce, kStreamCloseFinished);
    res->bytes.store(shard_bytes);
    res->retransmits.store(retransmits);
    res->final_block.store(block);
    res->status.store(static_cast<int>(TransferStatus::Ok));
}

// Ranks each status by how much it should dominate the others when lanes
// disagree, so the caller learns about the most actionable problem rather
// than whichever lane's thread happened to store its result last.
// Config/socket/unsupported outrank everything else: nothing could run at
// all, which matters more than a peer lane merely running short. Auth and
// resource-limit issues are security-relevant and outrank ordinary
// transport failures. Timeout outranks Incomplete since it means the lane
// gave up entirely, versus Incomplete's "the loop finished but fell short".
int severity(TransferStatus s) {
    switch (s) {
        case TransferStatus::Ok: return 0;
        case TransferStatus::Incomplete: return 1;
        case TransferStatus::Timeout: return 2;
        // A lane that saw the peer's abort, or our own cancel, explains the
        // others' shortfall better than their Timeout/Incomplete does.
        case TransferStatus::PeerAborted: return 3;
        case TransferStatus::Cancelled: return 4;
        case TransferStatus::AuthFailed: return 5;
        case TransferStatus::ResourceLimit: return 6;
        case TransferStatus::SocketError: return 7;
        case TransferStatus::ConfigError: return 8;
        case TransferStatus::Unsupported: return 8;
    }
    return 0;
}

TransferStatus worst(const std::vector<LaneResult> &lanes) {
    TransferStatus st = TransferStatus::Ok;
    for (const auto &l : lanes) {
        const auto s = static_cast<TransferStatus>(l.status.load());
        if (severity(s) > severity(st)) st = s;
    }
    return st;
}

// Joins every lane thread. While they run, and once more at the end, it
// reports progress through cfg.on_progress from this (the caller's) thread,
// so the callback never races the lanes or itself. `sender_total` is the
// sender's known size; the receiver passes nullptr and learns its total
// from the lanes' StreamStarts.
void wait_for_lanes(const TransferConfig &cfg, std::vector<std::thread> &threads,
                    const std::vector<LaneResult> &results, const uint64_t *sender_total) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    const auto snapshot = [&] {
        TransferProgress p;
        p.lanes_total = static_cast<uint16_t>(results.size());
        bool all_started = true;
        uint64_t expected = 0;
        for (const auto &r : results) {
            p.bytes_done += r.bytes.load(std::memory_order_relaxed);
            p.retransmits += r.retransmits.load(std::memory_order_relaxed);
            if (r.finished.load(std::memory_order_acquire)) ++p.lanes_done;
            if (r.started.load(std::memory_order_acquire)) {
                expected += r.expected.load(std::memory_order_relaxed);
            } else {
                all_started = false;
            }
        }
        p.bytes_total = sender_total ? *sender_total : (all_started ? expected : 0);
        p.seconds = std::chrono::duration<double>(clock::now() - t0).count();
        return p;
    };

    if (cfg.on_progress) {
        const auto interval =
            std::chrono::milliseconds(std::max<uint32_t>(cfg.progress_interval_ms, 10));
        auto next = t0 + interval;
        for (;;) {
            bool all_done = true;
            for (const auto &r : results) {
                if (!r.finished.load(std::memory_order_acquire)) {
                    all_done = false;
                    break;
                }
            }
            if (all_done) break;
            // Short naps rather than one interval-long sleep, so returning
            // to the caller is never delayed by up to a whole interval.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (clock::now() >= next) {
                cfg.on_progress(snapshot());
                next = clock::now() + interval;
            }
        }
    }
    for (auto &t : threads) t.join();
    if (cfg.on_progress) cfg.on_progress(snapshot());
}

void fill_stats(const std::vector<LaneResult> &lanes, uint64_t bytes, TransferStats *out) {
    if (out == nullptr) return;
    uint64_t first = UINT64_MAX, last = 0, rtx = 0, auth = 0;
    uint32_t blk = 0;
    for (const auto &l : lanes) {
        const uint64_t s = l.start_ns.load(), e = l.end_ns.load();
        if (s != 0) first = std::min(first, s);
        last = std::max(last, e);
        rtx += l.retransmits.load();
        auth += l.auth_failures.load();
        blk = std::max(blk, l.final_block.load());
    }
    out->bytes = bytes;
    out->retransmits = rtx;
    out->auth_failures = auth;
    out->final_block_size = static_cast<uint16_t>(blk);
    out->seconds = (first != UINT64_MAX && last > first) ? (last - first) / 1e9 : 0.0;
}

} // namespace

const char *to_string(TransferStatus s) {
    switch (s) {
        case TransferStatus::Ok: return "ok";
        case TransferStatus::ConfigError: return "configuration error";
        case TransferStatus::SocketError: return "socket error";
        case TransferStatus::Timeout: return "timed out";
        case TransferStatus::AuthFailed: return "authentication failed";
        case TransferStatus::Incomplete: return "incomplete transfer";
        case TransferStatus::Unsupported: return "unsupported (built without crypto)";
        case TransferStatus::ResourceLimit: return "peer's claimed size could not be allocated";
        case TransferStatus::Cancelled: return "cancelled";
        case TransferStatus::PeerAborted: return "peer aborted the transfer";
    }
    return "unknown";
}

bool encryption_available() { return session_crypto_available(); }

TransferStatus send_buffer(const TransferConfig &cfg, const uint8_t *data, size_t len,
                           TransferStats *stats) {
    if (cfg.lanes == 0 || cfg.base_port == 0 || cfg.block_size == 0 ||
        (data == nullptr && len > 0)) {
        return TransferStatus::ConfigError;
    }
    Keys keys;
    if (!cfg.pre_shared_key.empty()) {
        if (!session_crypto_available()) return TransferStatus::Unsupported;
        keys.enabled = true;
        // One salt and one key for the whole session; lanes are separated by
        // nonce, not by key, so crypto stays parallel.
        if (!random_bytes(keys.salt, kSessionSaltLen) ||
            !derive_session_key(reinterpret_cast<const uint8_t *>(cfg.pre_shared_key.data()),
                                cfg.pre_shared_key.size(), keys.salt, kSessionSaltLen,
                                keys.key)) {
            return TransferStatus::Unsupported;
        }
    }

    const uint16_t lanes = cfg.lanes;
    const uint64_t shard = (len + lanes - 1) / (lanes ? lanes : 1);
    std::vector<LaneResult> results(lanes);
    std::atomic<bool> stop_all{false};
    std::vector<std::thread> threads;
    threads.reserve(lanes);

    for (uint16_t i = 0; i < lanes; ++i) {
        const uint64_t off = std::min<uint64_t>(static_cast<uint64_t>(i) * shard, len);
        const uint64_t n = std::min<uint64_t>(shard, len - off);
        threads.emplace_back([&cfg, &keys, &results, &stop_all, i, lane_data = data + off, n] {
            send_lane(cfg, i, keys, lane_data, n, &results[i], &stop_all);
            results[i].finished.store(true, std::memory_order_release);
        });
    }
    const uint64_t total = len;
    wait_for_lanes(cfg, threads, results, &total);

    fill_stats(results, len, stats);
    return worst(results);
}

TransferStatus receive_buffer(const TransferConfig &cfg, std::vector<uint8_t> *out,
                              TransferStats *stats) {
    if (out == nullptr || cfg.lanes == 0 || cfg.base_port == 0) {
        return TransferStatus::ConfigError;
    }
    if (!cfg.pre_shared_key.empty() && !session_crypto_available()) {
        return TransferStatus::Unsupported;
    }

    const uint16_t lanes = cfg.lanes;
    std::vector<std::vector<uint8_t>> shards(lanes);
    std::vector<LaneResult> results(lanes);
    std::atomic<bool> stop_all{false};
    std::vector<std::thread> threads;
    threads.reserve(lanes);

    for (uint16_t i = 0; i < lanes; ++i) {
        threads.emplace_back([&cfg, &shards, &results, &stop_all, i] {
            recv_lane(cfg, i, cfg.pre_shared_key, &shards[i], &results[i], &stop_all);
            results[i].finished.store(true, std::memory_order_release);
        });
    }
    wait_for_lanes(cfg, threads, results, nullptr);

    uint64_t total = 0;
    for (const auto &s : shards) total += s.size();

    // A failed lane's shard is sized to what the sender claimed but only
    // partially (or never) written — concatenating it in regardless would
    // hand the caller a full-length buffer that looks complete but is
    // zero-filled garbage wherever that lane fell short. Only a fully
    // successful transfer gets to populate `*out`, matching what
    // receive_file already does with this same status before writing to
    // disk.
    const TransferStatus status = worst(results);
    out->clear();
    if (status == TransferStatus::Ok) {
        out->reserve(total);
        for (const auto &s : shards) out->insert(out->end(), s.begin(), s.end());
    }

    fill_stats(results, total, stats);
    return status;
}

TransferStatus send_file(const TransferConfig &cfg, const std::string &path,
                         TransferStats *stats) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return TransferStatus::ConfigError;
    const auto len = static_cast<uint64_t>(in.tellg());
    in.seekg(0);
    std::vector<uint8_t> buf(len);
    in.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(len));
    if (!in) return TransferStatus::ConfigError;
    return send_buffer(cfg, buf.data(), buf.size(), stats);
}

TransferStatus receive_file(const TransferConfig &cfg, const std::string &path,
                            TransferStats *stats) {
    std::vector<uint8_t> buf;
    const TransferStatus st = receive_buffer(cfg, &buf, stats);
    if (st != TransferStatus::Ok) return st;
    std::ofstream out(path, std::ios::binary);
    if (!out) return TransferStatus::ConfigError;
    out.write(reinterpret_cast<const char *>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    return out ? TransferStatus::Ok : TransferStatus::ConfigError;
}

} // namespace fuse
