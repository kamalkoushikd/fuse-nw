#include "fuse/transfer.hpp"

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#include "fuse/proto/aux.hpp"
#include "fuse/proto/block.hpp"
#include "fuse/proto/congestion.hpp"
#include "fuse/proto/receiver.hpp"
#include "fuse/proto/registry.hpp"
#include "fuse/proto/session_crypto.hpp"
#include "fuse/proto/udp.hpp"

namespace fuse {

using namespace fuse::proto;

namespace {

constexpr uint8_t kWindow = kMaxWindow;
constexpr size_t kRxBatch = 64;
constexpr size_t kGsoBudget = 60000;
constexpr uint32_t kCleanBatchesToGrow = 8;

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
size_t build_aad(uint8_t *out, uint16_t lane, uint64_t seq, uint64_t offset) {
    size_t n = 0;
    n += put_u16(out + n, lane);
    n += put_u64(out + n, seq);
    n += put_u64(out + n, offset);
    return n;
}

struct LaneResult {
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> retransmits{0};
    std::atomic<uint64_t> auth_failures{0};
    std::atomic<uint32_t> final_block{0};
    std::atomic<uint64_t> start_ns{0};
    std::atomic<uint64_t> end_ns{0};
    std::atomic<int> status{static_cast<int>(TransferStatus::Incomplete)};
};

struct Keys {
    bool enabled = false;
    uint8_t key[kSessionKeyLen] = {};
    uint8_t salt[kSessionSaltLen] = {};
};

// --- Receiver lane -------------------------------------------------------

void recv_lane(const TransferConfig &cfg, uint16_t lane, const std::string &psk,
               std::vector<uint8_t> *shard, LaneResult *res) {
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
    bool have_start = false, have_peer = false;
    PeerAddr peer{};
    int idle = 0;
    const int max_idle = static_cast<int>(cfg.timeout_ms / 200) + 5;

    for (;;) {
        PeerAddr src;
        const int got = sock.recv_batch(rx_buf.data(), slot, kRxBatch, lens.data(), &src);
        if (got > 0) {
            idle = 0;
            peer = src;
            have_peer = true;
            if (res->start_ns.load() == 0) res->start_ns.store(now_ns());
        } else if (++idle > max_idle) {
            res->status.store(static_cast<int>(TransferStatus::Timeout));
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
                    have_start = true;
                    shard_bytes = ss.total_bytes;
                    shard->assign(shard_bytes, 0);
                    if (want_crypto) {
                        uint8_t key[kSessionKeyLen];
                        if (!derive_session_key(reinterpret_cast<const uint8_t *>(psk.data()),
                                                psk.size(), ss.session_salt, key) ||
                            !cipher.init(key)) {
                            res->status.store(static_cast<int>(TransferStatus::Unsupported));
                            return;
                        }
                    }
                }
                continue;
            }
            if (type != MsgType::Data || !have_start) continue;

            BlockHeader hdr;
            uint64_t send_time = 0;
            const uint8_t *payload = nullptr;
            if (!decode_data_datagram(dg, dlen, &hdr, &send_time, &payload)) continue;
            if (hdr.flags & kFlagLastBlock) final_seq = hdr.seq_no;

            const uint8_t *body = payload;
            uint16_t body_len = hdr.payload_len;

            if (want_crypto) {
                // Authenticate before admitting the block to the window: a
                // forged block that advanced `base` would slide the window
                // past data never written.
                uint8_t aad[18];
                const size_t al = build_aad(aad, lane, hdr.seq_no, hdr.offset);
                if (payload == nullptr || hdr.payload_len < kAeadTagLen ||
                    !cipher.open(lane, hdr.seq_no, aad, al, payload, hdr.payload_len,
                                 opened.data())) {
                    ++auth_failures;
                    continue;
                }
                body = opened.data();
                body_len = static_cast<uint16_t>(hdr.payload_len - kAeadTagLen);
            }

            // A retransmission arriving after we NACK'd measures one RTT on
            // the receiver's own clock (no cross-host clock comparison).
            if ((hdr.flags & kFlagRetransmission) && nack_sent_ns != 0) {
                const uint64_t sample = now_ns() - nack_sent_ns;
                rtt_est_ns = (rtt_est_ns == 0) ? sample : (rtt_est_ns * 7 + sample) / 8;
                nack_sent_ns = 0;
            }

            if (rx.on_receive(hdr.seq_no, send_time, now_ns()) == ReceiveResult::Accepted &&
                body != nullptr) {
                if (hdr.offset + body_len <= shard->size()) {
                    std::memcpy(shard->data() + hdr.offset, body, body_len);
                    written += body_len;
                }
                ++delivered;
            }
        }

        if (!have_peer || !have_start) continue;

        const uint64_t t = now_ns();
        if (delivered - last_ack_blocks >= 8 || t - last_ack_ns > 200000) {
            last_ack_blocks = delivered;
            last_ack_ns = t;
            const Ack ack = rx.build_ack();
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
            res->end_ns.store(now_ns());
            break;
        }
    }

    for (int i = 0; i < 8; ++i) {
        const Ack ack = rx.build_ack();
        const size_t n = encode_ack(ack, tx.data(), tx.size());
        if (n) sock.send_to(tx.data(), n, peer);
    }

    res->bytes.store(written);
    res->auth_failures.store(auth_failures);
    res->status.store(static_cast<int>(written == shard_bytes ? TransferStatus::Ok
                                                              : TransferStatus::Incomplete));
}

// --- Sender lane ---------------------------------------------------------

void send_lane(const TransferConfig &cfg, uint16_t lane, const Keys &keys, const uint8_t *data,
               uint64_t shard_bytes, LaneResult *res) {
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

    bool started = false;
    for (int attempt = 0; attempt < 200 && !started; ++attempt) {
        const size_t n = encode_stream_start(ss, ctl.data(), ctl.size());
        sock.send_to(ctl.data(), n, dst);
        size_t got = 0;
        if (sock.recv_from(ctl.data(), ctl.size(), &got, nullptr)) {
            MsgType t;
            if (peek_msg_type(ctl.data(), got, &t) && t == MsgType::Ack) started = true;
        }
    }
    if (!started) {
        res->status.store(static_cast<int>(TransferStatus::Timeout));
        return;
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
    const uint64_t t0 = now_ns();
    const uint64_t deadline_ns = static_cast<uint64_t>(cfg.timeout_ms) * 1000000ull;
    bool sent_last = false;

    std::vector<std::pair<uint64_t, uint16_t>> meta;
    meta.reserve(shard_bytes / (cfg.block_size ? cfg.block_size : 1200) + 8);

    res->start_ns.store(t0);

    for (;;) {
        cc.poll(now_ns());
        const uint64_t window = std::min<uint64_t>(kWindow, std::max<uint32_t>(4, cc.window()));
        bool did_work = false;

        if (next_offset < shard_bytes && next_seq < base + window) {
            const uint16_t seg = static_cast<uint16_t>(kDataPrefixSize + block +
                                                       (keys.enabled ? kAeadTagLen : 0));
            const size_t max_segs =
                std::min<size_t>(kGsoBudget / seg, window - (next_seq - base));
            size_t packed = 0, bytes_in_batch = 0;

            while (packed < max_segs && next_offset < shard_bytes) {
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

                reg.store(next_seq, data + next_offset, len, now_ns());
                if (meta.size() <= next_seq) meta.resize(next_seq + 1);
                meta[next_seq] = {next_offset, len};

                const uint8_t *body = data + next_offset;
                if (keys.enabled) {
                    uint8_t aad[18];
                    const size_t al = build_aad(aad, lane, next_seq, next_offset);
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
            if (!sock.recv_from(ctl.data(), ctl.size(), &got, nullptr)) break;
            did_work = true;
            MsgType type;
            if (!peek_msg_type(ctl.data(), got, &type)) continue;

            if (type == MsgType::Ack) {
                Ack ack;
                if (!decode_ack(ctl.data(), got, &ack)) continue;
                if (ack.base_seq_no > base) {
                    for (uint64_t s = base; s < ack.base_seq_no; ++s) reg.confirm(s);
                    base = ack.base_seq_no;
                }
                if (ack.echoed_send_time > 0) {
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
                    if (!sl || seq >= meta.size()) continue;
                    BlockHeader hdr;
                    hdr.stream_id = lane;
                    hdr.seq_no = seq;
                    hdr.flags = kFlagRetransmission;
                    hdr.payload_len = sl->payload_len;
                    hdr.offset = meta[seq].first;
                    const uint8_t *body = sl->payload;
                    if (keys.enabled) {
                        uint8_t aad[18];
                        const size_t al = build_aad(aad, lane, seq, hdr.offset);
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
            }
        }

        if (loss_seen) {
            block = std::min<uint16_t>(cfg.block_size, block_ceiling);
            clean_batches = 0;
        } else if (++clean_batches >= kCleanBatchesToGrow && block < block_ceiling) {
            clean_batches = 0;
            block = static_cast<uint16_t>(std::min<uint32_t>(block_ceiling, block * 2u));
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
        if (base != last_base) {
            last_base = base;
            last_progress = t;
        } else if (t - last_progress > rto_ns) {
            cc.on_loss();
            const RegistrySlot *sl = reg.lookup(base);
            if (sl && base < meta.size()) {
                BlockHeader hdr;
                hdr.stream_id = lane;
                hdr.seq_no = base;
                hdr.flags = kFlagRetransmission;
                hdr.payload_len = sl->payload_len;
                hdr.offset = meta[base].first;
                const uint8_t *body = sl->payload;
                if (keys.enabled) {
                    uint8_t aad[18];
                    const size_t al = build_aad(aad, lane, base, hdr.offset);
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

        if (sent_last && base >= next_seq) break;
        if (t - t0 > deadline_ns) {
            res->status.store(static_cast<int>(TransferStatus::Timeout));
            return;
        }
        if (!did_work) {
            timespec nap{0, 20000};
            nanosleep(&nap, nullptr);
        }
    }

    res->end_ns.store(now_ns());
    res->bytes.store(shard_bytes);
    res->retransmits.store(retransmits);
    res->final_block.store(block);
    res->status.store(static_cast<int>(TransferStatus::Ok));
}

TransferStatus worst(const std::vector<LaneResult> &lanes) {
    TransferStatus st = TransferStatus::Ok;
    for (const auto &l : lanes) {
        const auto s = static_cast<TransferStatus>(l.status.load());
        if (s != TransferStatus::Ok) st = s;
    }
    return st;
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
    }
    return "unknown";
}

bool encryption_available() { return session_crypto_available(); }

TransferStatus send_buffer(const TransferConfig &cfg, const uint8_t *data, size_t len,
                           TransferStats *stats) {
    if (cfg.lanes == 0 || cfg.base_port == 0 || (data == nullptr && len > 0)) {
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
                                cfg.pre_shared_key.size(), keys.salt, keys.key)) {
            return TransferStatus::Unsupported;
        }
    }

    const uint16_t lanes = cfg.lanes;
    const uint64_t shard = (len + lanes - 1) / (lanes ? lanes : 1);
    std::vector<LaneResult> results(lanes);
    std::vector<std::thread> threads;
    threads.reserve(lanes);

    for (uint16_t i = 0; i < lanes; ++i) {
        const uint64_t off = std::min<uint64_t>(static_cast<uint64_t>(i) * shard, len);
        const uint64_t n = std::min<uint64_t>(shard, len - off);
        threads.emplace_back(send_lane, std::cref(cfg), i, std::cref(keys), data + off, n,
                             &results[i]);
    }
    for (auto &t : threads) t.join();

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
    std::vector<std::thread> threads;
    threads.reserve(lanes);

    for (uint16_t i = 0; i < lanes; ++i) {
        threads.emplace_back(recv_lane, std::cref(cfg), i, std::cref(cfg.pre_shared_key),
                             &shards[i], &results[i]);
    }
    for (auto &t : threads) t.join();

    uint64_t total = 0;
    for (const auto &s : shards) total += s.size();
    out->clear();
    out->reserve(total);
    for (const auto &s : shards) out->insert(out->end(), s.begin(), s.end());

    fill_stats(results, total, stats);
    return worst(results);
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
