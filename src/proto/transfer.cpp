#include "fuse/transfer.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
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
#include "fuse/proto/spsc_ring.hpp"
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

// --- Receive pipeline ----------------------------------------------------
//
// Per lane, one *reader* thread owns the socket and the lane's window
// (ReceiverStream). It receives datagrams, drops blocks it already has, and
// hands each new data block to a consumer thread through a lock-free
// single-producer/single-consumer ring (fuse/proto/spsc_ring.hpp). The
// consumer authenticates/decrypts the block and writes it straight to its
// final place in the output (a Sink) — no per-lane buffer to stitch
// together later, and no waiting for earlier gaps to be retransmitted
// before later blocks reach the disk. The result comes back to the reader
// on a second ring, and only then does the reader admit the block to its
// window — so the window, and therefore every ACK the sender sees, only
// ever covers bytes that are authentic and already written.
//
//   reader(lane i) ── ring[i][k] ──► writer k   ── done[i][k] ──► reader(lane i)
//                  ── rtx_ring[i] ─► rtx thread ── rtx_done[i] ─► reader(lane i)
//
// A new block goes to writer ((seq / kRouteRun) % K) — whole runs of
// consecutive blocks to the same writer, so it can write each run with one
// pwritev — and one sequence number never reaches two writers;
// retransmissions go to the dedicated retransmission thread. Every ring has exactly one producer and one consumer, so the
// whole pipeline needs no mutex: only acquire/release on the ring counters.
// A full ring never blocks the reader — it drops the datagram and the
// normal NACK/RTO recovery resends it.

// Where received bytes end up. write() may be called concurrently from
// several consumer threads, always for disjoint byte ranges.
class Sink {
public:
    virtual ~Sink() = default;
    // Called once, before any write, with the transfer's total size.
    virtual bool prepare(uint64_t total_bytes) = 0;
    // Writes the iovecs back to back starting at `offset`. May modify iov.
    virtual bool write(uint64_t offset, iovec *iov, int iovcnt, size_t bytes) = 0;
    // Called once at the end; `ok` says whether the transfer succeeded.
    // Returns false only if a successful transfer could not be committed.
    virtual bool finish(bool ok) = 0;
};

// receive_buffer: straight into the caller's vector.
class MemorySink final : public Sink {
public:
    explicit MemorySink(std::vector<uint8_t> *out) : out_(out) { out_->clear(); }

    bool prepare(uint64_t total_bytes) override {
        // total_bytes is the sender's unauthenticated claim: a hostile value
        // must fail this transfer, not throw out of a lane thread.
        try {
            out_->assign(total_bytes, 0);
        } catch (const std::bad_alloc &) {
            return false;
        } catch (const std::length_error &) {
            return false;
        }
        return true;
    }

    bool write(uint64_t offset, iovec *iov, int iovcnt, size_t) override {
        uint8_t *p = out_->data() + offset;
        for (int i = 0; i < iovcnt; ++i) {
            std::memcpy(p, iov[i].iov_base, iov[i].iov_len);
            p += iov[i].iov_len;
        }
        return true;
    }

    bool finish(bool ok) override {
        if (!ok) out_->clear(); // only a complete transfer populates *out
        return true;
    }

private:
    std::vector<uint8_t> *out_;
};

// receive_file: blocks go to "<path>.part" as they arrive; the finished file
// is synced and then renamed over <path>, so <path> is only ever either
// untouched or complete. A failed transfer removes the .part file.
class FileSink final : public Sink {
public:
    explicit FileSink(std::string path) : path_(std::move(path)), part_(path_ + ".part") {}

    ~FileSink() override {
        if (fd_ >= 0) { // finish() never ran: don't leave a partial file behind
            ::close(fd_);
            ::unlink(part_.c_str());
        }
    }

    bool open() {
        fd_ = ::open(part_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        return fd_ >= 0;
    }

    bool prepare(uint64_t total_bytes) override {
        if (total_bytes > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) return false;
        // Sets the final size up front (sparse, so it costs no disk yet);
        // blocks then land at their own offsets in any order.
        return ::ftruncate(fd_, static_cast<off_t>(total_bytes)) == 0;
    }

    bool write(uint64_t offset, iovec *iov, int iovcnt, size_t bytes) override {
        while (bytes > 0) {
            const ssize_t n = ::pwritev(fd_, iov, iovcnt, static_cast<off_t>(offset));
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;
            // A short write: skip what was written and retry the rest.
            size_t left = static_cast<size_t>(n);
            offset += left;
            bytes -= left;
            while (iovcnt > 0 && left >= iov->iov_len) {
                left -= iov->iov_len;
                ++iov;
                --iovcnt;
            }
            if (iovcnt > 0 && left > 0) {
                iov->iov_base = static_cast<uint8_t *>(iov->iov_base) + left;
                iov->iov_len -= left;
            }
        }
        return true;
    }

    bool finish(bool ok) override {
        if (fd_ < 0) return !ok;
        // Sync before the rename, or a crash could leave <path> renamed but
        // missing data that was still only in the page cache.
        const bool synced = ok && ::fdatasync(fd_) == 0;
        const bool closed = ::close(fd_) == 0;
        fd_ = -1;
        if (ok && synced && closed && ::rename(part_.c_str(), path_.c_str()) == 0) return true;
        ::unlink(part_.c_str());
        return !ok;
    }

private:
    std::string path_, part_;
    int fd_ = -1;
};

// One data block on its way from a reader to a consumer.
struct PacketSlot {
    PeerAddr src;           // where it came from (the reader re-anchors to it once accepted)
    uint64_t lane_base = 0; // where this lane starts in the output
    uint64_t lane_total = 0;
    uint32_t len = 0;
    uint8_t dg[kMaxDatagramSize]; // the raw datagram (deliberately uninitialised)
};

enum class WriteResult : uint8_t { Written, AuthFailed, IoFailed };

// A consumer's verdict on one block, on its way back to the reader.
struct Completion {
    PeerAddr src;
    uint64_t seq = 0;
    uint64_t send_time = 0;
    uint32_t bytes = 0; // plaintext bytes written (0 for an empty or out-of-range block)
    uint8_t flags = 0;
    WriteResult result = WriteResult::Written;
};

using PacketRing = SpscRing<PacketSlot>;
using DoneRing = SpscRing<Completion>;

// Written once by a lane's reader (before it dispatches that lane's first
// block), read by consumers. The ring's release/acquire already orders the
// key before any block that needs it; `ready` just makes that explicit.
struct LaneKey {
    uint8_t key[kSessionKeyLen] = {};
    std::atomic<bool> ready{false};
};

struct RxPipeline {
    RxPipeline(uint16_t lane_count, uint16_t writer_count, uint32_t slots, Sink *out)
        : lanes(lane_count), writers(writer_count), sink(out), keys(new LaneKey[lane_count]) {
        const size_t rtx_slots = std::max<uint32_t>(16, slots / 4); // retransmits are rare
        for (size_t i = 0; i < static_cast<size_t>(lanes) * writers; ++i) {
            rings.push_back(std::make_unique<PacketRing>(slots));
            dones.push_back(std::make_unique<DoneRing>(slots));
        }
        for (uint16_t i = 0; i < lanes; ++i) {
            rtx_rings.push_back(std::make_unique<PacketRing>(rtx_slots));
            rtx_dones.push_back(std::make_unique<DoneRing>(rtx_slots));
        }
    }

    PacketRing &ring(uint16_t lane, uint16_t k) { return *rings[lane * writers + k]; }
    DoneRing &done(uint16_t lane, uint16_t k) { return *dones[lane * writers + k]; }

    const uint16_t lanes;
    const uint16_t writers;
    Sink *const sink;
    std::vector<std::unique_ptr<PacketRing>> rings, rtx_rings;
    std::vector<std::unique_ptr<DoneRing>> dones, rtx_dones;
    std::unique_ptr<LaneKey[]> keys;

    // The transfer's total size, claimed by the first valid StreamStart
    // (UINT64_MAX until then). Every other lane must agree with it. Also
    // what the receiver reports as its progress total.
    std::atomic<uint64_t> file_total{UINT64_MAX};
    // Set by whichever lane won file_total, after sink->prepare().
    static constexpr int kSinkPending = 0, kSinkReady = 1, kSinkFailed = 2;
    std::atomic<int> sink_state{kSinkPending};
    std::atomic<bool> io_failed{false};
    // Set once every reader has exited: consumers stop (anything still
    // queued is by then only duplicates nobody is waiting for).
    std::atomic<bool> stop{false};
};

// One consumer input: a lane's ring into this consumer and the ring back.
struct Channel {
    uint16_t lane;
    PacketRing *in;
    DoneRing *out;
};

constexpr size_t kWriteBatch = 64;
// New blocks are routed to writers in runs of this many consecutive
// sequence numbers, not round-robin: consecutive blocks of a lane are
// consecutive in the output, so a writer that gets a whole run can write it
// with one large pwritev. Round-robin (seq % K) made every writer's blocks
// non-adjacent, so each block became its own small write — measured 2-4x
// slower on a real disk, and far worse as K grew.
constexpr uint64_t kRouteRun = kWriteBatch;

// Per-consumer scratch, reused for every batch.
struct ConsumerState {
    explicit ConsumerState(uint16_t lanes) : ciphers(lanes), plain(kWriteBatch * kMaxPayloadSize) {}
    std::vector<std::unique_ptr<LaneCipher>> ciphers; // this thread's own, per lane
    std::vector<uint8_t> plain;                       // decrypted blocks of one batch
    iovec iov[kWriteBatch];
};

// This thread's cipher for `lane`, created on first use (LaneCipher is
// thread-confined, so each consumer keeps its own). nullptr if the lane's
// key isn't available.
LaneCipher *cipher_for(RxPipeline &p, ConsumerState &cs, uint16_t lane) {
    if (!cs.ciphers[lane]) {
        if (!p.keys[lane].ready.load(std::memory_order_acquire)) return nullptr;
        auto c = std::make_unique<LaneCipher>();
        if (!c->init(p.keys[lane].key)) return nullptr;
        cs.ciphers[lane] = std::move(c);
    }
    return cs.ciphers[lane].get();
}

// Processes up to one batch from a channel: authenticate/decrypt each block,
// write runs of contiguous blocks with one call each, then report every
// block's outcome back to the reader. Returns false if there was nothing to do.
bool drain_channel(RxPipeline &p, ConsumerState &cs, Channel &ch, bool want_crypto) {
    // Never take more blocks than there is room to report on.
    const size_t n = std::min({ch.in->readable(), ch.out->writable(), kWriteBatch});
    if (n == 0) return false;

    struct Item {
        uint64_t off;
        const uint8_t *data;
        uint32_t len;
    } items[kWriteBatch];

    for (size_t i = 0; i < n; ++i) {
        const PacketSlot &s = ch.in->slot_for_read(i);
        Completion &c = ch.out->slot_for_write(i);
        c.src = s.src;
        c.bytes = 0;
        c.result = WriteResult::Written;
        items[i].len = 0;

        BlockHeader hdr;
        uint64_t send_time = 0;
        const uint8_t *payload = nullptr;
        if (!decode_data_datagram(s.dg, s.len, &hdr, &send_time, &payload)) {
            c.result = WriteResult::AuthFailed; // the reader already checked; defensive only
            continue;
        }
        c.seq = hdr.seq_no;
        c.send_time = send_time;
        c.flags = hdr.flags;

        const uint8_t *body = payload;
        uint32_t body_len = hdr.payload_len;
        if (want_crypto) {
            // `flags` rides in the AAD (not just lane/seq/offset) so
            // LastBlock can't be flipped on an otherwise-authentic block to
            // end the lane early.
            LaneCipher *cipher = cipher_for(p, cs, ch.lane);
            uint8_t aad[19];
            const size_t al = build_aad(aad, ch.lane, hdr.seq_no, hdr.offset, hdr.flags);
            uint8_t *out = cs.plain.data() + i * kMaxPayloadSize;
            if (cipher == nullptr || payload == nullptr || hdr.payload_len < kAeadTagLen ||
                !cipher->open(ch.lane, hdr.seq_no, aad, al, payload, hdr.payload_len, out)) {
                c.result = WriteResult::AuthFailed;
                continue;
            }
            body = out;
            body_len = hdr.payload_len - kAeadTagLen;
        }

        // Checked this way round so `hdr.offset + body_len` (wire fields,
        // attacker-controlled) is never computed: an offset near UINT64_MAX
        // would wrap that sum small enough to pass a naive bounds check.
        // An out-of-range block is still reported as received (it keeps
        // the window moving, as before), it just isn't written.
        if (body != nullptr && body_len > 0 && hdr.offset <= s.lane_total &&
            body_len <= s.lane_total - hdr.offset) {
            items[i] = {s.lane_base + hdr.offset, body, body_len};
            c.bytes = body_len;
        }
    }

    // Coalesce blocks that are contiguous in the output into one write.
    for (size_t i = 0; i < n;) {
        if (items[i].len == 0) {
            ++i;
            continue;
        }
        size_t j = i;
        uint64_t end = items[i].off;
        size_t bytes = 0;
        int cnt = 0;
        while (j < n && items[j].len != 0 && items[j].off == end) {
            cs.iov[cnt].iov_base = const_cast<uint8_t *>(items[j].data);
            cs.iov[cnt].iov_len = items[j].len;
            end += items[j].len;
            bytes += items[j].len;
            ++cnt;
            ++j;
        }
        if (!p.sink->write(items[i].off, cs.iov, cnt, bytes)) {
            p.io_failed.store(true, std::memory_order_release);
            for (size_t k = i; k < j; ++k) {
                Completion &c = ch.out->slot_for_write(k);
                c.result = WriteResult::IoFailed;
                c.bytes = 0;
            }
        }
        i = j;
    }

    ch.out->commit(n);  // outcomes to the reader...
    ch.in->consume(n);  // ...and the slots back to it
    return true;
}

// A consumer thread: writer k serves channel k of every lane; the
// retransmission thread serves every lane's retransmission channel.
void consumer_main(RxPipeline *p, std::vector<Channel> channels, bool want_crypto) {
    ConsumerState cs(p->lanes);
    unsigned idle = 0;
    while (!p->stop.load(std::memory_order_acquire)) {
        bool did = false;
        for (Channel &ch : channels) did |= drain_channel(*p, cs, ch, want_crypto);
        if (did) {
            idle = 0;
        } else if (++idle < 64) {
            std::this_thread::yield();
        } else {
            // Nothing to do for a while (between transfers' bursts, or at
            // the tail): back off to a short nap instead of spinning a core.
            timespec nap{0, 50000};
            nanosleep(&nap, nullptr);
        }
    }
}

// --- Receiver lane (the reader) ------------------------------------------

// `stop_all` is shared by every lane of one call: a lane that is cancelled or
// told the peer aborted sets it, and its siblings then stop too — so one
// lane whose abort datagrams were all lost doesn't sit out its full timeout.
void recv_lane(const TransferConfig &cfg, uint16_t lane, RxPipeline *p, LaneResult *res,
               std::atomic<bool> *stop_all) {
    UdpSocket sock;
    if (!sock.open(cfg.bind_address.c_str(), static_cast<uint16_t>(cfg.base_port + lane))) {
        res->status.store(static_cast<int>(TransferStatus::SocketError));
        return;
    }
    set_bufs(sock.fd(), 32 << 20);
    // While blocks are out with consumers the reader must come back quickly
    // to collect their results (the ACKs the sender is waiting for depend on
    // them), so the receive timeout is short then and longer when idle.
    long cur_timeout_us = -1;
    const auto use_timeout_us = [&](long us) {
        if (us != cur_timeout_us) {
            set_timeout_us(sock.fd(), us);
            cur_timeout_us = us;
        }
    };

    ReceiverStream rx(lane, kWindow, /*lossless=*/true);
    const bool want_crypto = !cfg.pre_shared_key.empty();

    const size_t slot = kMaxDatagramSize + 64;
    std::vector<uint8_t> rx_buf(kRxBatch * slot);
    std::vector<size_t> lens(kRxBatch);
    std::vector<PeerAddr> srcs(kRxBatch);
    std::vector<uint8_t> tx(kMaxAuxDatagramSize + 64);

    // dispatched[seq % kWindow] == seq while that block is with a consumer,
    // so a duplicate arriving meanwhile isn't handed out (and written) twice.
    // Every block in flight is inside the window, so no two collide.
    std::vector<uint64_t> dispatched(kWindow, UINT64_MAX);
    uint64_t pending = 0; // blocks handed to consumers, result not yet back

    uint64_t lane_base = 0, shard_bytes = 0, final_seq = UINT64_MAX, written = 0;
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
    // exiting immediately, in case the Acks already sent were all lost and
    // the sender is still waiting for one. It leaves as soon as the sender's
    // "finished" close arrives, or the sender goes quiet (it got an Ack and
    // left without the close reaching us), or after kCloseGraceNs at most.
    uint64_t completed_at_ns = 0;
    constexpr uint64_t kCloseGraceNs = 1'500'000'000ull;
    constexpr uint64_t kQuietAfterCompletionNs = 400'000'000ull;
    bool have_start = false, have_peer = false;
    // Set from the sender's StreamClose: "finished" lets a completed lane
    // exit at once instead of lingering; "aborted" ends the lane now.
    bool peer_finished = false, peer_aborted = false;
    uint64_t peer_nonce = 0; // echoed in every Ack once StreamStart arrives
    PeerAddr peer{};
    // No datagram at all for this long means the sender is gone.
    const uint64_t idle_limit_ns = static_cast<uint64_t>(cfg.timeout_ms) * 1000000ull + 1'000'000'000ull;
    uint64_t last_rx_ns = now_ns();

    // An active MITM/corruption attack fails most blocks' auth check rather
    // than merely dropping some — that pattern is distinguishable from
    // ordinary loss (which NACK/retransmit already resolves) and deserves a
    // status the caller can act on differently from "just incomplete" or
    // "just timed out". Shared between every exit path below, since a
    // wrong-key transfer never completes and can only leave via a timeout or
    // an abort. Require a minimum sample size so a couple of early failures
    // during key/salt setup don't misreport a healthy transfer.
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

    // Takes a consumer's verdicts on dispatched blocks. Only here — after
    // the block is authentic and written — does it enter the window.
    const auto drain_results = [&](DoneRing &d) {
        const size_t n = d.readable();
        for (size_t j = 0; j < n; ++j) {
            const Completion &c = d.slot_for_read(j);
            --pending;
            uint64_t &mark = dispatched[c.seq % kWindow];
            if (mark == c.seq) mark = UINT64_MAX;

            if (c.result == WriteResult::AuthFailed) {
                ++auth_failures;
                continue;
            }
            if (c.result == WriteResult::IoFailed) continue; // io_failed ends the lane

            // flags is trustworthy now that the block cleared the auth check
            // (or in plaintext mode, where nothing claims otherwise).
            if (c.flags & kFlagLastBlock) final_seq = c.seq;

            // `Accepted` means this block is both authentic (if encrypted)
            // AND new, not a replay of something already delivered. Only
            // then is its source trusted as where to send ACKs: that is
            // what lets a transfer survive the sender's NAT mapping changing
            // mid-flight, while an attacker resending a captured datagram
            // from their own address still can't redirect the ACKs.
            if (rx.on_receive(c.seq, c.send_time, now_ns()) == ReceiveResult::Accepted) {
                peer = c.src;
                have_peer = true;
                // A retransmission arriving after we NACK'd measures one RTT
                // on the receiver's own clock (no cross-host comparison).
                if ((c.flags & kFlagRetransmission) && nack_sent_ns != 0) {
                    const uint64_t sample = now_ns() - nack_sent_ns;
                    rtt_est_ns = (rtt_est_ns == 0) ? sample : (rtt_est_ns * 7 + sample) / 8;
                    nack_sent_ns = 0;
                }
                written += c.bytes;
                ++delivered;
            }
        }
        if (n) d.consume(n);
    };

    for (;;) {
        // Polled once per loop; the receive timeout (at most 20 ms) bounds
        // how long a cancel can go unnoticed.
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
        if (p->io_failed.load(std::memory_order_acquire)) {
            give_up(TransferStatus::IoError);
            stop_all->store(true, std::memory_order_release);
            return;
        }

        use_timeout_us(pending > 0 ? 200 : 20000);
        const int got = sock.recv_batch(rx_buf.data(), slot, kRxBatch, lens.data(), srcs.data());
        if (got > 0) {
            last_rx_ns = now_ns();
            if (res->start_ns.load() == 0) res->start_ns.store(last_rx_ns);
        } else if (now_ns() - last_rx_ns > idle_limit_ns) {
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
                    // Every size here is the sender's unauthenticated claim.
                    // Ordered so nothing can overflow: the lane must fit
                    // inside the transfer.
                    if (ss.total_bytes > ss.file_total_bytes ||
                        ss.stream_base_offset > ss.file_total_bytes - ss.total_bytes) {
                        continue;
                    }
                    // The first lane to get here sizes the output; every
                    // other lane must agree on the total.
                    uint64_t claimed = UINT64_MAX;
                    if (p->file_total.compare_exchange_strong(claimed, ss.file_total_bytes,
                                                              std::memory_order_acq_rel)) {
                        const bool ok = p->sink->prepare(ss.file_total_bytes);
                        p->sink_state.store(ok ? RxPipeline::kSinkReady : RxPipeline::kSinkFailed,
                                            std::memory_order_release);
                    } else if (claimed != ss.file_total_bytes) {
                        continue; // disagrees with the transfer the other lanes joined
                    }
                    const int sink = p->sink_state.load(std::memory_order_acquire);
                    if (sink == RxPipeline::kSinkFailed) {
                        // A hostile or corrupt size claim must fail cleanly.
                        res->status.store(static_cast<int>(TransferStatus::ResourceLimit));
                        return;
                    }
                    // Another lane is still preparing the output: don't Ack
                    // yet; the sender retries its StreamStart.
                    if (sink != RxPipeline::kSinkReady) continue;

                    if (want_crypto) {
                        LaneKey &k = p->keys[lane];
                        const std::string &psk = cfg.pre_shared_key;
                        if (!derive_session_key(reinterpret_cast<const uint8_t *>(psk.data()),
                                                psk.size(), ss.session_salt, kSessionSaltLen,
                                                k.key)) {
                            res->status.store(static_cast<int>(TransferStatus::Unsupported));
                            return;
                        }
                        k.ready.store(true, std::memory_order_release);
                    }
                    have_start = true;
                    peer_nonce = ss.nonce;
                    peer = srcs[i];
                    have_peer = true;
                    lane_base = ss.stream_base_offset;
                    shard_bytes = ss.total_bytes;
                    res->expected.store(shard_bytes, std::memory_order_relaxed);
                    res->started.store(true, std::memory_order_release);
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
                    // the pre-session state, where any first StreamStart
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

            // Hand out only blocks we don't have and aren't already
            // processing. (Nothing here is trusted yet: authentication
            // happens in the consumer, and the window only moves on its
            // verdict.)
            const uint64_t base = rx.base_seq_no();
            if (hdr.seq_no < base) continue;
            const uint64_t rel = hdr.seq_no - base;
            if (rel >= kWindow || rx.is_received(rel)) continue;
            uint64_t &mark = dispatched[hdr.seq_no % kWindow];
            if (mark == hdr.seq_no) continue;

            PacketRing &ring = (hdr.flags & kFlagRetransmission)
                                   ? *p->rtx_rings[lane]
                                   : p->ring(lane, static_cast<uint16_t>((hdr.seq_no / kRouteRun) %
                                                                          p->writers));
            // Consumer behind: drop rather than stall the socket; the block
            // is NACKed and resent like any other loss.
            if (ring.writable() == 0) continue;
            PacketSlot &s = ring.slot_for_write(0);
            s.src = srcs[i];
            s.lane_base = lane_base;
            s.lane_total = shard_bytes;
            s.len = static_cast<uint32_t>(dlen);
            std::memcpy(s.dg, dg, dlen);
            ring.commit(1);
            mark = hdr.seq_no;
            ++pending;
        }

        for (uint16_t k = 0; k < p->writers; ++k) drain_results(p->done(lane, k));
        drain_results(*p->rtx_dones[lane]);
        res->bytes.store(written, std::memory_order_relaxed);

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
                // The transfer's own duration ends here, not after the
                // linger below, so the reported throughput isn't diluted by it.
                res->end_ns.store(t);
            }
            // The sender's "finished" close means it has every Ack it needs:
            // leave now. Otherwise linger in case our Acks were lost.
            if (peer_finished) break;
            if (t - completed_at_ns > kCloseGraceNs ||
                t - std::max(completed_at_ns, last_rx_ns) >= kQuietAfterCompletionNs) {
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

// `stop_all`: see recv_lane. `lane_base`/`file_total` place this lane's
// shard within the whole transfer, so the receiver can write it in place.
void send_lane(const TransferConfig &cfg, uint16_t lane, const Keys &keys, const uint8_t *data,
               uint64_t shard_bytes, uint64_t lane_base, uint64_t file_total, LaneResult *res,
               std::atomic<bool> *stop_all) {
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
    ss.stream_base_offset = lane_base;
    ss.file_total_bytes = file_total;
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
        case TransferStatus::IoError: return 7;
        case TransferStatus::SocketError: return 8;
        case TransferStatus::ConfigError: return 9;
        case TransferStatus::Unsupported: return 9;
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
                    const std::vector<LaneResult> &results, const uint64_t *sender_total,
                    const std::atomic<uint64_t> *receiver_total = nullptr) {
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
        if (sender_total) {
            p.bytes_total = *sender_total;
        } else if (receiver_total &&
                   receiver_total->load(std::memory_order_acquire) != UINT64_MAX) {
            p.bytes_total = receiver_total->load(std::memory_order_relaxed);
        } else {
            p.bytes_total = all_started ? expected : 0;
        }
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

// Runs a whole receive into `sink`: starts the consumers (K writers + the
// retransmission thread) and one reader per lane, reports progress while
// they run, then shuts the pipeline down and commits (or discards) the
// output. Config has already been validated.
TransferStatus receive_to_sink(const TransferConfig &cfg, Sink *sink, TransferStats *stats) {
    const uint16_t lanes = cfg.lanes;
    const uint16_t writers = std::max<uint16_t>(1, cfg.writer_threads);
    const uint32_t slots = std::clamp<uint32_t>(cfg.ring_slots, 16, 1u << 16);
    const bool want_crypto = !cfg.pre_shared_key.empty();
    RxPipeline pipe(lanes, writers, slots, sink);

    std::vector<std::thread> consumers;
    consumers.reserve(writers + 1u);
    for (uint16_t k = 0; k < writers; ++k) {
        std::vector<Channel> channels;
        for (uint16_t i = 0; i < lanes; ++i) channels.push_back({i, &pipe.ring(i, k), &pipe.done(i, k)});
        consumers.emplace_back(consumer_main, &pipe, std::move(channels), want_crypto);
    }
    {
        std::vector<Channel> channels;
        for (uint16_t i = 0; i < lanes; ++i) {
            channels.push_back({i, pipe.rtx_rings[i].get(), pipe.rtx_dones[i].get()});
        }
        consumers.emplace_back(consumer_main, &pipe, std::move(channels), want_crypto);
    }

    std::vector<LaneResult> results(lanes);
    std::atomic<bool> stop_all{false};
    std::vector<std::thread> readers;
    readers.reserve(lanes);
    for (uint16_t i = 0; i < lanes; ++i) {
        readers.emplace_back([&cfg, &pipe, &results, &stop_all, i] {
            recv_lane(cfg, i, &pipe, &results[i], &stop_all);
            results[i].finished.store(true, std::memory_order_release);
        });
    }
    wait_for_lanes(cfg, readers, results, nullptr, &pipe.file_total);

    // Every reader has exited, so nobody is waiting on anything still
    // queued: stop the consumers before the rings they read are destroyed.
    pipe.stop.store(true, std::memory_order_release);
    for (auto &c : consumers) c.join();

    TransferStatus status = worst(results);
    if (!sink->finish(status == TransferStatus::Ok)) status = TransferStatus::IoError;

    uint64_t total = 0;
    for (const auto &r : results) total += r.bytes.load();
    fill_stats(results, total, stats);
    return status;
}

TransferStatus check_receive_config(const TransferConfig &cfg) {
    if (cfg.lanes == 0 || cfg.base_port == 0) return TransferStatus::ConfigError;
    if (!cfg.pre_shared_key.empty() && !session_crypto_available()) {
        return TransferStatus::Unsupported;
    }
    return TransferStatus::Ok;
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
        case TransferStatus::IoError: return "could not write the output";
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
        threads.emplace_back([&cfg, &keys, &results, &stop_all, i, lane_data = data + off, n, off,
                              len] {
            send_lane(cfg, i, keys, lane_data, n, off, len, &results[i], &stop_all);
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
    if (out == nullptr) return TransferStatus::ConfigError;
    const TransferStatus st = check_receive_config(cfg);
    if (st != TransferStatus::Ok) return st;
    // Blocks are written straight into *out; it is only left populated if
    // the whole transfer succeeds.
    MemorySink sink(out);
    return receive_to_sink(cfg, &sink, stats);
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
    const TransferStatus st = check_receive_config(cfg);
    if (st != TransferStatus::Ok) return st;
    // Opened before the transfer starts, so an unwritable destination fails
    // at once instead of after the whole file has crossed the network.
    FileSink sink(path);
    if (!sink.open()) return TransferStatus::ConfigError;
    return receive_to_sink(cfg, &sink, stats);
}

} // namespace fuse
