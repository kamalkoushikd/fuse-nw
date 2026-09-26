#include "fuse/mux_transfer.hpp"

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include "fuse/proto/block.hpp"
#include "fuse/proto/congestion.hpp"
#include "fuse/proto/control.hpp"
#include "fuse/proto/orchestrator.hpp"
#include "fuse/proto/receiver.hpp"
#include "fuse/proto/registry.hpp"
#include "fuse/proto/send_queue.hpp"
#include "fuse/proto/setup.hpp"
#include "fuse/proto/udp.hpp"

namespace fuse {

using namespace fuse::proto;

namespace {

constexpr size_t kRxBatch = 32;
constexpr uint64_t kStreamStartRtoNs = 50'000'000;

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

uint64_t random_nonce() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return rng();
}

// Same ranking as transfer.cpp's worst() — duplicated rather than shared
// since transfer.cpp is deliberately left untouched by this module.
int severity(TransferStatus s) {
    switch (s) {
        case TransferStatus::Ok: return 0;
        case TransferStatus::Incomplete: return 1;
        case TransferStatus::Timeout: return 2;
        case TransferStatus::AuthFailed: return 3;
        case TransferStatus::ResourceLimit: return 4;
        case TransferStatus::SocketError: return 5;
        case TransferStatus::ConfigError: return 6;
        case TransferStatus::Unsupported: return 6;
    }
    return 0;
}

TransferStatus worst_of(const std::vector<TransferStatus> &statuses) {
    TransferStatus st = TransferStatus::Ok;
    for (const auto &s : statuses) {
        if (severity(s) > severity(st)) st = s;
    }
    return st;
}

// --- Per-stream state ------------------------------------------------------
//
// One instance per logical stream, built once (after the SETUP handshake)
// and never resized — safe for any worker to index into streams_[] without
// a lock. Everything else is guarded by this stream's own `mu`, the same
// one-mutex-per-connection shape sdk.cpp's fuse_conn already uses. `done`
// and the progress clock are atomics so the outer completion-poll loop can
// read them without taking the lock every stream, every 20ms.

struct SenderStreamState {
    const uint16_t stream_id;
    const uint16_t block_size;
    const uint16_t window_size;

    std::mutex mu;
    SenderRegistry reg;
    CongestionController cc;
    SendQueue retransmit_backlog; // NACK-driven; coalesce=false matches LOSSLESS

    const uint8_t *data = nullptr; // points into the caller's buffer; not owned
    uint64_t shard_bytes = 0;
    uint64_t tx_base = 0;
    uint64_t tx_next_seq = 0;
    uint64_t next_offset = 0;
    bool sent_last = false;

    uint64_t nonce = 0;
    bool start_acked = false;
    uint64_t last_start_send_ns = 0;

    // RTO pacing only. Deliberately NOT what the outer timeout check reads
    // (see last_real_progress_ns below) — conflating the two is exactly the
    // sender-side timeout bug found and fixed in transfer.cpp this session:
    // this clock also advances on a merely-attempted local retransmit, which
    // is not evidence the peer is still there.
    uint64_t last_progress_ns = 0;
    uint64_t last_rtt_echo_ns = 0;
    uint64_t retransmit_count = 0;

    std::atomic<bool> done{false};
    // Only moves on an Ack-confirmed advance of tx_base. What the overall
    // per-transfer timeout is measured against.
    std::atomic<uint64_t> last_real_progress_ns{0};

    SenderStreamState(uint16_t id, uint16_t bsize, uint16_t wsize)
        : stream_id(id),
          block_size(bsize),
          window_size(wsize),
          reg(id, wsize),
          cc(wsize, /*enabled=*/true, 1, 16),
          retransmit_backlog(wsize, /*coalesce=*/false) {}
};

struct ReceiverStreamState {
    const uint16_t stream_id;
    const uint16_t window_size;

    std::mutex mu;
    ReceiverStream rx;
    std::vector<uint8_t> shard; // resized once, on this stream's StreamStart
    uint64_t shard_bytes = 0;
    uint64_t written = 0;
    uint64_t delivered = 0;
    uint64_t last_ack_delivered = 0;
    uint64_t final_seq = UINT64_MAX;
    bool have_start = false;
    bool resource_limit = false; // StreamStart claimed a size we couldn't allocate
    uint64_t peer_nonce = 0;

    uint64_t rtt_est_ns = 0;
    uint64_t nack_sent_ns = 0;
    uint64_t last_ack_ns = 0;

    std::atomic<bool> done{false};
    std::atomic<uint64_t> last_progress_ns{0}; // moves on any Accepted datagram for this stream

    ReceiverStreamState(uint16_t id, uint16_t wsize)
        : stream_id(id), window_size(wsize), rx(id, wsize, /*lossless=*/true) {}
};

// --- Sender ------------------------------------------------------------

class MuxSender {
public:
    MuxSender(const MuxTransferConfig &cfg, const uint8_t *data, size_t len, TransferStats *stats,
             MuxTransferStats *mux_stats)
        : cfg_(cfg), data_(data), len_(len), stats_(stats), mux_stats_(mux_stats) {}

    TransferStatus run();

private:
    bool run_setup_handshake();
    void build_streams();
    bool try_send_one(SenderStreamState &s);
    void dispatch_incoming(const uint8_t *dg, size_t dlen);
    bool worker_task(uint16_t);
    bool all_done() const;

    const MuxTransferConfig &cfg_;
    const uint8_t *data_;
    size_t len_;
    TransferStats *stats_;
    MuxTransferStats *mux_stats_;

    UdpSocket sock_;
    PeerAddr dst_{};

    // Guards recv_batch()'s call-scratch (udp.cpp's BatchScratch is mutable
    // per-socket state, unsynchronized — see mux_transfer's design doc) and
    // this object's rxbuf_/rxlens_/rxsrcs_, which only the reader-role
    // winner ever touches.
    std::mutex rx_mutex_;
    std::vector<uint8_t> rxbuf_;
    std::vector<size_t> rxlens_;
    std::vector<PeerAddr> rxsrcs_;

    std::vector<std::unique_ptr<SenderStreamState>> streams_;
    std::atomic<uint32_t> cursor_{0};

    // Kept alive past the handshake, not just a run_setup_handshake() local:
    // is_matched() becoming true doesn't mean the receiver has actually
    // gotten our FINACK yet (UDP, no guarantee) — if it hasn't, the
    // receiver's own SetupResponder retries HASH-REPLY on a timer. If that
    // retry arrives after we've already moved to the data phase and this
    // object no longer exists, we can never re-emit FINACK, the receiver's
    // handshake can never complete, and the whole transfer fails even
    // though we ourselves believe SETUP succeeded. dispatch_incoming keeps
    // feeding stray SetupHashReply datagrams to this for exactly that case.
    std::unique_ptr<SetupInitiator> init_;

    uint64_t start_ns_ = 0;
    uint64_t end_ns_ = 0;
};

bool MuxSender::run_setup_handshake() {
    SetupPayload payload;
    payload.num_workers = 1; // vestigial once negotiated -- see header comment
    payload.num_streams = cfg_.num_streams;
    for (uint16_t i = 0; i < cfg_.num_streams; ++i) {
        StreamConfig &s = payload.streams[i];
        s.stream_id = i;
        s.worker_id = 0;
        s.stream_flags = kStreamFlagLossless;
        s.block_size = cfg_.block_size;
        s.window_size = cfg_.window_size;
    }

    init_ = std::make_unique<SetupInitiator>(payload);
    uint8_t out[kMaxSetupDatagramSize];
    uint8_t in[kMaxSetupDatagramSize];

    size_t n = init_->start(out, sizeof(out), now_ns());
    if (n == 0 || !sock_.send_to(out, n, dst_)) return false;

    const uint64_t deadline = now_ns() + static_cast<uint64_t>(cfg_.timeout_ms) * 1000000ull;
    while (!init_->is_matched() && !init_->is_failed()) {
        if (now_ns() > deadline) return false;
        size_t got = 0;
        if (sock_.recv_from(in, sizeof(in), &got, nullptr)) {
            n = init_->on_datagram(in, got, out, sizeof(out), now_ns());
        } else {
            n = init_->on_timeout(out, sizeof(out), now_ns());
        }
        if (n > 0) sock_.send_to(out, n, dst_);
    }
    return init_->is_matched();
}

void MuxSender::build_streams() {
    const uint16_t n = cfg_.num_streams;
    const uint64_t shard = (len_ + n - 1) / (n ? n : 1);
    streams_.reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
        auto s = std::make_unique<SenderStreamState>(i, cfg_.block_size, cfg_.window_size);
        const uint64_t off = std::min<uint64_t>(static_cast<uint64_t>(i) * shard, len_);
        const uint64_t sz = std::min<uint64_t>(shard, len_ - off);
        s->data = data_ + off;
        s->shard_bytes = sz;
        s->nonce = random_nonce();
        const uint64_t t = now_ns();
        s->last_progress_ns = t;
        s->last_real_progress_ns.store(t, std::memory_order_relaxed);
        streams_.push_back(std::move(s));
    }
}

bool MuxSender::try_send_one(SenderStreamState &s) {
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.done.load(std::memory_order_relaxed)) return false;

    const uint64_t t = now_ns();

    if (!s.start_acked) {
        // Written as t < last + rto rather than t - last < rto: with a
        // cross-thread `t`/last_start_send_ns pair (see worst_stall's
        // comment above), subtraction can wrap; addition on the RHS can't.
        if (s.last_start_send_ns != 0 && t < s.last_start_send_ns + kStreamStartRtoNs) {
            return false;
        }
        StreamStart ss;
        ss.stream_id = s.stream_id;
        ss.total_blocks = 0;
        ss.block_size = s.block_size;
        ss.total_bytes = s.shard_bytes;
        ss.nonce = s.nonce;
        uint8_t ctl[kMaxAuxDatagramSize + 64];
        const size_t n = encode_stream_start(ss, ctl, sizeof(ctl));
        if (n == 0) return false;
        sock_.send_to(ctl, n, dst_);
        s.last_start_send_ns = t;
        return true;
    }

    s.cc.poll(t);

    uint64_t seq = 0;
    if (s.retransmit_backlog.pop(seq)) {
        const RegistrySlot *slot = s.reg.lookup(seq);
        if (slot != nullptr) {
            BlockHeader hdr;
            hdr.stream_id = s.stream_id;
            hdr.seq_no = seq;
            // OR, not overwrite: a retransmitted last block is still the
            // last block, and the receiver has no other way to learn a
            // stream finished if this bit is dropped here.
            hdr.flags = slot->flags | kFlagRetransmission;
            hdr.payload_len = slot->payload_len;
            hdr.offset = slot->offset;
            uint8_t dgram[kMaxDatagramSize];
            const size_t n = encode_data_datagram(hdr, now_ns(), slot->payload, dgram, sizeof(dgram));
            if (n && sock_.send_to(dgram, n, dst_)) ++s.retransmit_count;
        }
        return true;
    }

    const uint64_t srtt = s.cc.rtt_ns();
    const uint64_t rto_ns = srtt ? std::clamp<uint64_t>(srtt * 2, 5'000'000ull, 1'000'000'000ull)
                                  : 1'000'000'000ull;
    if (s.tx_base < s.tx_next_seq && t > s.last_progress_ns + rto_ns) {
        const RegistrySlot *slot = s.reg.lookup(s.tx_base);
        if (slot != nullptr) {
            BlockHeader hdr;
            hdr.stream_id = s.stream_id;
            hdr.seq_no = s.tx_base;
            hdr.flags = slot->flags | kFlagRetransmission; // see the backlog path's comment above
            hdr.payload_len = slot->payload_len;
            hdr.offset = slot->offset;
            uint8_t dgram[kMaxDatagramSize];
            const size_t n = encode_data_datagram(hdr, now_ns(), slot->payload, dgram, sizeof(dgram));
            if (n && sock_.send_to(dgram, n, dst_)) {
                ++s.retransmit_count;
                s.cc.on_loss();
            }
        }
        s.last_progress_ns = t; // pacing clock: don't retry every call while waiting out the RTO
        return true;
    }

    const bool need_empty_last = (s.shard_bytes == 0 && s.tx_next_seq == 0);
    const uint32_t window = std::clamp<uint32_t>(s.cc.window(), 4, s.window_size);
    if ((s.next_offset < s.shard_bytes || need_empty_last) && s.tx_next_seq < s.tx_base + window) {
        const uint64_t remaining = s.shard_bytes - s.next_offset;
        const uint16_t blen = static_cast<uint16_t>(std::min<uint64_t>(s.block_size, remaining));
        const bool is_last = (s.next_offset + blen >= s.shard_bytes);

        BlockHeader hdr;
        hdr.stream_id = s.stream_id;
        hdr.seq_no = s.tx_next_seq;
        hdr.flags = is_last ? kFlagLastBlock : 0;
        hdr.payload_len = blen;
        hdr.offset = s.next_offset;

        s.reg.store(s.tx_next_seq, s.data + s.next_offset, blen, now_ns(), s.next_offset, hdr.flags);

        uint8_t dgram[kMaxDatagramSize];
        const size_t n =
            encode_data_datagram(hdr, now_ns(), s.data + s.next_offset, dgram, sizeof(dgram));
        if (n == 0) return false;
        sock_.send_to(dgram, n, dst_);

        ++s.tx_next_seq;
        s.next_offset += blen;
        if (is_last) s.sent_last = true;
        return true;
    }

    return false;
}

void MuxSender::dispatch_incoming(const uint8_t *dg, size_t dlen) {
    MsgType type;
    if (!peek_msg_type(dg, dlen, &type)) return;

    if (type == MsgType::Ack) {
        Ack ack;
        if (!decode_ack(dg, dlen, &ack) || ack.stream_id >= streams_.size()) return;
        SenderStreamState &s = *streams_[ack.stream_id];
        std::lock_guard<std::mutex> lk(s.mu);
        if (ack.nonce != s.nonce) return; // not this attempt's session
        s.start_acked = true;
        if (ack.base_seq_no > s.tx_base) {
            // Acks are unauthenticated (no crypto in this module at all):
            // clamp to what was actually sent, mirroring the same fix
            // applied to transfer.cpp/sdk.cpp this session.
            const uint64_t new_base = std::min(ack.base_seq_no, s.tx_next_seq);
            for (uint64_t seq = s.tx_base; seq < new_base; ++seq) s.reg.confirm(seq);
            s.tx_base = new_base;
            const uint64_t t = now_ns();
            s.last_progress_ns = t;
            s.last_real_progress_ns.store(t, std::memory_order_release);
        }
        if (ack.echoed_send_time > 0 && ack.echoed_send_time != s.last_rtt_echo_ns) {
            s.last_rtt_echo_ns = ack.echoed_send_time;
            const uint64_t t = now_ns();
            if (t > ack.echoed_send_time) s.cc.on_rtt_sample(t - ack.echoed_send_time);
        }
        if (s.tx_base >= s.tx_next_seq && s.sent_last) {
            s.done.store(true, std::memory_order_release);
        }
    } else if (type == MsgType::Nack) {
        Nack nack;
        if (!decode_nack(dg, dlen, &nack) || nack.stream_id >= streams_.size()) return;
        SenderStreamState &s = *streams_[nack.stream_id];
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.done.load(std::memory_order_relaxed)) return;
        s.cc.on_loss();
        for (uint16_t i = 0; i < nack.count; ++i) {
            s.retransmit_backlog.push(nack.missing[i]); // best-effort; a full push just isn't queued
        }
    } else if (type == MsgType::SetupHashReply && init_) {
        // The receiver's own retry for a FINACK that never arrived (see
        // init_'s comment). on_datagram() re-validates the hash itself and
        // only returns non-zero (a FINACK to resend) for a genuine match —
        // safe to call unconditionally here.
        uint8_t out[kMaxSetupDatagramSize];
        const size_t n = init_->on_datagram(dg, dlen, out, sizeof(out), now_ns());
        if (n > 0) sock_.send_to(out, n, dst_);
    }
}

bool MuxSender::worker_task(uint16_t) {
    bool did_work = false;
    {
        std::unique_lock<std::mutex> rx_lock(rx_mutex_, std::try_to_lock);
        if (rx_lock.owns_lock()) {
            const size_t slot = kMaxDatagramSize + 64;
            const int got =
                sock_.recv_batch(rxbuf_.data(), slot, kRxBatch, rxlens_.data(), rxsrcs_.data());
            for (int i = 0; i < got; ++i) {
                dispatch_incoming(rxbuf_.data() + static_cast<size_t>(i) * slot, rxlens_[i]);
                did_work = true;
            }
        }
    }

    const uint16_t n = static_cast<uint16_t>(streams_.size());
    if (n == 0) return did_work;
    const uint32_t start = cursor_.fetch_add(1, std::memory_order_relaxed) % n;
    for (uint16_t k = 0; k < n; ++k) {
        SenderStreamState &s = *streams_[(start + k) % n];
        if (s.done.load(std::memory_order_relaxed)) continue;
        if (try_send_one(s)) {
            did_work = true;
            break;
        }
    }
    return did_work;
}

bool MuxSender::all_done() const {
    for (const auto &s : streams_) {
        if (!s->done.load(std::memory_order_relaxed)) return false;
    }
    return true;
}

TransferStatus MuxSender::run() {
    if (cfg_.num_streams == 0 || cfg_.num_streams > kMaxStreams || cfg_.port == 0 ||
        cfg_.block_size == 0 || cfg_.window_size == 0 || (data_ == nullptr && len_ > 0)) {
        return TransferStatus::ConfigError;
    }
    if (!sock_.open("0.0.0.0", 0)) return TransferStatus::SocketError;
    if (!UdpSocket::resolve(cfg_.host.c_str(), cfg_.port, &dst_)) return TransferStatus::ConfigError;
    set_bufs(sock_.fd(), 32 << 20);
    set_timeout_us(sock_.fd(), 50000);

    if (!run_setup_handshake()) return TransferStatus::Timeout;

    build_streams();

    const size_t slot = kMaxDatagramSize + 64;
    rxbuf_.assign(kRxBatch * slot, 0);
    rxlens_.assign(kRxBatch, 0);
    rxsrcs_.assign(kRxBatch, PeerAddr{});
    set_timeout_us(sock_.fd(), 200);

    start_ns_ = now_ns();

    OrchestratorConfig occfg;
    occfg.min_workers = cfg_.min_workers;
    occfg.max_workers = cfg_.max_workers;
    occfg.target_utilization = cfg_.target_utilization;
    occfg.scale_in_utilization = cfg_.scale_in_utilization;
    occfg.stabilization_ns = cfg_.stabilization_ns;

    WorkerOrchestrator orch(occfg, [this](uint16_t id) { return worker_task(id); });
    orch.start();

    std::atomic<bool> stop_ticker{false};
    std::thread ticker([&] {
        while (!stop_ticker.load(std::memory_order_relaxed)) {
            orch.tick(now_ns());
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    });

    const uint64_t deadline_ns = static_cast<uint64_t>(cfg_.timeout_ms) * 1000000ull;
    bool timed_out = false;
    uint16_t peak_workers = 0;
    for (;;) {
        peak_workers = std::max(peak_workers, orch.worker_count());
        if (all_done()) break;
        const uint64_t t = now_ns();
        uint64_t worst_stall = 0;
        for (const auto &s : streams_) {
            if (s->done.load(std::memory_order_relaxed)) continue;
            const uint64_t last = s->last_real_progress_ns.load(std::memory_order_acquire);
            // t and `last` can come from clock_gettime() calls on different
            // threads/cores; CLOCK_MONOTONIC guarantees each thread's own
            // reads never go backward, but doesn't guarantee two threads'
            // concurrent reads are strictly ordered to sub-microsecond
            // precision on every platform (observed: a several-hundred-ns
            // "last > t" reading). Unsigned subtraction on that wraps to
            // ~UINT64_MAX, which instantly exceeds any real deadline and
            // fails a transfer that was never actually stalled at all.
            if (t > last) worst_stall = std::max(worst_stall, t - last);
        }
        if (worst_stall > deadline_ns) {
            timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    stop_ticker.store(true, std::memory_order_relaxed);
    ticker.join();
    const OrchestratorStats ostats = orch.stats();
    orch.stop(); // drains every worker; only the calling thread touches streams_ from here

    end_ns_ = now_ns();

    std::vector<TransferStatus> results;
    uint64_t total_bytes = 0, total_retransmits = 0;
    for (const auto &s : streams_) {
        const TransferStatus st = s->done.load(std::memory_order_relaxed)
                                       ? TransferStatus::Ok
                                       : (timed_out ? TransferStatus::Timeout
                                                    : TransferStatus::Incomplete);
        results.push_back(st);
        total_bytes += s->shard_bytes;
        total_retransmits += s->retransmit_count;
    }

    if (stats_ != nullptr) {
        stats_->bytes = total_bytes;
        stats_->retransmits = total_retransmits;
        stats_->auth_failures = 0;
        stats_->final_block_size = cfg_.block_size;
        stats_->seconds = (end_ns_ > start_ns_) ? static_cast<double>(end_ns_ - start_ns_) / 1e9 : 0.0;
    }
    if (mux_stats_ != nullptr) {
        mux_stats_->peak_workers = peak_workers;
        mux_stats_->scale_outs = ostats.scale_outs;
        mux_stats_->scale_ins = ostats.scale_ins;
    }

    return worst_of(results);
}

// --- Receiver ------------------------------------------------------------

class MuxReceiver {
public:
    MuxReceiver(const MuxTransferConfig &cfg, std::vector<uint8_t> *out, TransferStats *stats,
               MuxTransferStats *mux_stats)
        : cfg_(cfg), out_(out), stats_(stats), mux_stats_(mux_stats) {}

    TransferStatus run();

private:
    bool run_setup_handshake();
    void build_streams(const SetupPayload &negotiated);
    bool try_service_one(ReceiverStreamState &s);
    void dispatch_incoming(const uint8_t *dg, size_t dlen);
    bool worker_task(uint16_t);
    bool all_done() const;

    const MuxTransferConfig &cfg_;
    std::vector<uint8_t> *out_;
    TransferStats *stats_;
    MuxTransferStats *mux_stats_;

    UdpSocket sock_;
    PeerAddr peer_{};
    bool have_peer_ = false;

    std::mutex rx_mutex_;
    std::vector<uint8_t> rxbuf_;
    std::vector<size_t> rxlens_;
    std::vector<PeerAddr> rxsrcs_;

    std::vector<std::unique_ptr<ReceiverStreamState>> streams_;
    std::atomic<uint32_t> cursor_{0};

    uint64_t start_ns_ = 0;
    uint64_t end_ns_ = 0;
};

bool MuxReceiver::run_setup_handshake() {
    SetupResponder resp;
    uint8_t in[kMaxSetupDatagramSize];
    uint8_t out[kMaxSetupDatagramSize];

    const uint64_t deadline = now_ns() + static_cast<uint64_t>(cfg_.timeout_ms) * 1000000ull;
    while (!resp.is_complete() && !resp.is_failed()) {
        if (now_ns() > deadline) return false;
        size_t got = 0;
        PeerAddr from;
        size_t n = 0;
        if (sock_.recv_from(in, sizeof(in), &got, &from)) {
            if (!have_peer_) {
                peer_ = from;
                have_peer_ = true;
            }
            n = resp.on_datagram(in, got, out, sizeof(out), now_ns());
        } else {
            n = resp.on_timeout(out, sizeof(out), now_ns());
        }
        if (n > 0) sock_.send_to(out, n, peer_);
    }
    if (!resp.is_complete()) return false;
    build_streams(resp.config());
    return true;
}

void MuxReceiver::build_streams(const SetupPayload &negotiated) {
    const uint16_t n = negotiated.num_streams;
    streams_.reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
        const StreamConfig &sc = negotiated.streams[i];
        auto s = std::make_unique<ReceiverStreamState>(sc.stream_id, sc.window_size);
        s->last_progress_ns.store(now_ns(), std::memory_order_relaxed);
        streams_.push_back(std::move(s));
    }
}

bool MuxReceiver::try_service_one(ReceiverStreamState &s) {
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.done.load(std::memory_order_relaxed) || !s.have_start) return false;

    const uint64_t t = now_ns();
    bool did = false;

    if (s.delivered - s.last_ack_delivered >= 8 || t > s.last_ack_ns + 200000) {
        s.last_ack_delivered = s.delivered;
        s.last_ack_ns = t;
        Ack ack = s.rx.build_ack();
        ack.stream_id = s.stream_id;
        ack.nonce = s.peer_nonce;
        uint8_t ctl[kMaxAuxDatagramSize + 64];
        const size_t n = encode_ack(ack, ctl, sizeof(ctl));
        if (n) sock_.send_to(ctl, n, peer_);
        did = true;
    }

    const uint64_t reorder_ns = (s.rtt_est_ns == 0)
                                     ? 3'000'000
                                     : std::clamp<uint64_t>(s.rtt_est_ns / 2, 1'000'000, 100'000'000);
    const uint64_t renack_ns =
        (s.rtt_est_ns == 0) ? 40'000'000
                            : std::clamp<uint64_t>(s.rtt_est_ns * 3 / 2, 20'000'000, 500'000'000);
    Nack nack;
    if (s.rx.collect_nacks(t, reorder_ns, renack_ns, &nack) > 0) {
        nack.stream_id = s.stream_id;
        uint8_t ctl[kMaxAuxDatagramSize + 64];
        const size_t n = encode_nack(nack, ctl, sizeof(ctl));
        if (n) sock_.send_to(ctl, n, peer_);
        s.nack_sent_ns = t;
        did = true;
    }

    if (s.final_seq != UINT64_MAX && s.rx.base_seq_no() > s.final_seq) {
        s.done.store(true, std::memory_order_release);
    }

    return did;
}

void MuxReceiver::dispatch_incoming(const uint8_t *dg, size_t dlen) {
    MsgType type;
    if (!peek_msg_type(dg, dlen, &type)) return;

    if (type == MsgType::StreamStart) {
        StreamStart ss;
        if (!decode_stream_start(dg, dlen, &ss) || ss.stream_id >= streams_.size()) return;
        ReceiverStreamState &s = *streams_[ss.stream_id];
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.have_start) return;
        // ss.total_bytes is an unauthenticated wire value — a hostile or
        // corrupt claim near UINT64_MAX must not crash the process via an
        // uncaught allocation failure. Same guard as transfer.cpp's
        // recv_lane this session.
        try {
            s.shard.assign(ss.total_bytes, 0);
        } catch (const std::bad_alloc &) {
            s.resource_limit = true;
            s.done.store(true, std::memory_order_release);
            return;
        } catch (const std::length_error &) {
            s.resource_limit = true;
            s.done.store(true, std::memory_order_release);
            return;
        }
        s.have_start = true;
        s.peer_nonce = ss.nonce;
        s.shard_bytes = ss.total_bytes;
        return;
    }

    if (type != MsgType::Data) return;
    BlockHeader hdr;
    uint64_t send_time = 0;
    const uint8_t *payload = nullptr;
    if (!decode_data_datagram(dg, dlen, &hdr, &send_time, &payload)) return;
    if (hdr.stream_id >= streams_.size()) return;
    ReceiverStreamState &s = *streams_[hdr.stream_id];
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.have_start) return;

    const ReceiveResult rr = s.rx.on_receive(hdr.seq_no, send_time, now_ns());
    if (rr != ReceiveResult::Accepted) return;

    if ((hdr.flags & kFlagRetransmission) && s.nack_sent_ns != 0) {
        const uint64_t now = now_ns();
        if (now > s.nack_sent_ns) {
            const uint64_t sample = now - s.nack_sent_ns;
            s.rtt_est_ns = (s.rtt_est_ns == 0) ? sample : (s.rtt_est_ns * 7 + sample) / 8;
        }
        s.nack_sent_ns = 0;
    }

    // Checked this way round so `offset + payload_len` (wire fields, this
    // module has no auth at all) never gets computed with wraparound risk —
    // same bounds-check shape as the offset-overflow fix applied to
    // transfer.cpp/sdk.cpp this session.
    if (payload != nullptr && hdr.offset <= s.shard.size() &&
        hdr.payload_len <= s.shard.size() - hdr.offset) {
        std::memcpy(s.shard.data() + hdr.offset, payload, hdr.payload_len);
        s.written += hdr.payload_len;
    }
    ++s.delivered;
    if (hdr.flags & kFlagLastBlock) s.final_seq = hdr.seq_no;
    s.last_progress_ns.store(now_ns(), std::memory_order_release);
}

bool MuxReceiver::worker_task(uint16_t) {
    bool did_work = false;
    {
        std::unique_lock<std::mutex> rx_lock(rx_mutex_, std::try_to_lock);
        if (rx_lock.owns_lock()) {
            const size_t slot = kMaxDatagramSize + 64;
            const int got =
                sock_.recv_batch(rxbuf_.data(), slot, kRxBatch, rxlens_.data(), rxsrcs_.data());
            for (int i = 0; i < got; ++i) {
                dispatch_incoming(rxbuf_.data() + static_cast<size_t>(i) * slot, rxlens_[i]);
                did_work = true;
            }
        }
    }

    const uint16_t n = static_cast<uint16_t>(streams_.size());
    if (n == 0) return did_work;
    const uint32_t start = cursor_.fetch_add(1, std::memory_order_relaxed) % n;
    for (uint16_t k = 0; k < n; ++k) {
        ReceiverStreamState &s = *streams_[(start + k) % n];
        if (s.done.load(std::memory_order_relaxed)) continue;
        if (try_service_one(s)) {
            did_work = true;
            break;
        }
    }
    return did_work;
}

bool MuxReceiver::all_done() const {
    for (const auto &s : streams_) {
        if (!s->done.load(std::memory_order_relaxed)) return false;
    }
    return true;
}

TransferStatus MuxReceiver::run() {
    if (out_ == nullptr || cfg_.num_streams == 0 || cfg_.num_streams > kMaxStreams ||
        cfg_.port == 0 || cfg_.block_size == 0 || cfg_.window_size == 0) {
        return TransferStatus::ConfigError;
    }
    if (!sock_.open(cfg_.bind_address.c_str(), cfg_.port)) return TransferStatus::SocketError;
    set_bufs(sock_.fd(), 32 << 20);
    set_timeout_us(sock_.fd(), 50000);

    if (!run_setup_handshake()) return TransferStatus::Timeout;

    const size_t slot = kMaxDatagramSize + 64;
    rxbuf_.assign(kRxBatch * slot, 0);
    rxlens_.assign(kRxBatch, 0);
    rxsrcs_.assign(kRxBatch, PeerAddr{});
    set_timeout_us(sock_.fd(), 200);

    start_ns_ = now_ns();

    OrchestratorConfig occfg;
    occfg.min_workers = cfg_.min_workers;
    occfg.max_workers = cfg_.max_workers;
    occfg.target_utilization = cfg_.target_utilization;
    occfg.scale_in_utilization = cfg_.scale_in_utilization;
    occfg.stabilization_ns = cfg_.stabilization_ns;

    WorkerOrchestrator orch(occfg, [this](uint16_t id) { return worker_task(id); });
    orch.start();

    std::atomic<bool> stop_ticker{false};
    std::thread ticker([&] {
        while (!stop_ticker.load(std::memory_order_relaxed)) {
            orch.tick(now_ns());
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    });

    const uint64_t deadline_ns = static_cast<uint64_t>(cfg_.timeout_ms) * 1000000ull;
    bool timed_out = false;
    uint16_t peak_workers = 0;
    for (;;) {
        peak_workers = std::max(peak_workers, orch.worker_count());
        if (all_done()) break;
        const uint64_t t = now_ns();
        uint64_t worst_stall = 0;
        for (const auto &s : streams_) {
            if (s->done.load(std::memory_order_relaxed)) continue;
            const uint64_t last = s->last_progress_ns.load(std::memory_order_acquire);
            // See the matching comment in MuxSender::run(): t and `last` can
            // be read on different threads/cores, and unsigned subtraction
            // of a `last` that reads a few hundred ns ahead of `t` would
            // wrap to ~UINT64_MAX and falsely trip the deadline check.
            if (t > last) worst_stall = std::max(worst_stall, t - last);
        }
        if (worst_stall > deadline_ns) {
            timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    stop_ticker.store(true, std::memory_order_relaxed);
    ticker.join();
    const OrchestratorStats ostats = orch.stats();
    orch.stop();

    // Single-threaded tail: a final burst of Acks per completed stream,
    // mirroring recv_lane's post-loop flush -- best effort, so a lost final
    // Ack doesn't strand a sender that already delivered everything.
    for (const auto &s : streams_) {
        if (!s->have_start || s->resource_limit) continue;
        for (int i = 0; i < 6; ++i) {
            Ack ack = s->rx.build_ack();
            ack.stream_id = s->stream_id;
            ack.nonce = s->peer_nonce;
            uint8_t ctl[kMaxAuxDatagramSize + 64];
            const size_t n = encode_ack(ack, ctl, sizeof(ctl));
            if (n) sock_.send_to(ctl, n, peer_);
        }
    }

    end_ns_ = now_ns();

    std::vector<TransferStatus> results;
    uint64_t total_bytes = 0;
    for (const auto &s : streams_) {
        TransferStatus st;
        if (s->resource_limit) {
            st = TransferStatus::ResourceLimit;
        } else if (s->have_start && s->written == s->shard_bytes) {
            st = TransferStatus::Ok;
        } else {
            st = timed_out ? TransferStatus::Timeout : TransferStatus::Incomplete;
        }
        results.push_back(st);
        total_bytes += s->written;
    }

    const TransferStatus status = worst_of(results);

    out_->clear();
    if (status == TransferStatus::Ok) {
        uint64_t total = 0;
        for (const auto &s : streams_) total += s->shard.size();
        out_->reserve(total);
        for (const auto &s : streams_) out_->insert(out_->end(), s->shard.begin(), s->shard.end());
    }

    if (stats_ != nullptr) {
        stats_->bytes = total_bytes;
        stats_->retransmits = 0; // this module's retransmit counter lives sender-side
        stats_->auth_failures = 0;
        stats_->final_block_size = cfg_.block_size;
        stats_->seconds = (end_ns_ > start_ns_) ? static_cast<double>(end_ns_ - start_ns_) / 1e9 : 0.0;
    }
    if (mux_stats_ != nullptr) {
        mux_stats_->peak_workers = peak_workers;
        mux_stats_->scale_outs = ostats.scale_outs;
        mux_stats_->scale_ins = ostats.scale_ins;
    }

    return status;
}

} // namespace

TransferStatus send_multiplexed(const MuxTransferConfig &config, const uint8_t *data, size_t len,
                                TransferStats *stats, MuxTransferStats *mux_stats) {
    MuxSender sender(config, data, len, stats, mux_stats);
    return sender.run();
}

TransferStatus receive_multiplexed(const MuxTransferConfig &config, std::vector<uint8_t> *out,
                                   TransferStats *stats, MuxTransferStats *mux_stats) {
    MuxReceiver receiver(config, out, stats, mux_stats);
    return receiver.run();
}

} // namespace fuse
