// Reliable, sharded, parallel file transfer over Fuse.
//
// The file is split into N contiguous shards, one per lane. Each lane is a
// fully independent reliable stream — its own socket pair, sender registry,
// receiver window, ACK/NACK loop and congestion controller — so N lanes
// multiply the achievable packet rate rather than contending for one. The
// receiver reassembles each shard into its own buffer and the shards are
// stitched back together in lane order at the end.
//
// Three things make this fast, in descending order of impact:
//
//   1. Batched syscalls. One sendto() per datagram caps throughput at a
//      packet rate (~213k/s here) that has nothing to do with bandwidth.
//      The sender packs many equal-sized datagrams into one buffer and
//      hands them to the kernel with UDP GSO in a single syscall; the
//      receiver drains with recvmmsg. Wire packets stay MTU-sized.
//   2. Sharding across lanes, which multiplies that ceiling by N.
//   3. Adaptive block size: on a link that stays clean the sender grows its
//      block size, so each syscall and each window carries more bytes; on
//      loss it backs off. Blocks carry an explicit byte offset (wire v2) so
//      the size can change mid-stream without the receiver misplacing data.
//
//   fuse_filebench recv <base-port> <lanes> <out-file>
//   fuse_filebench send <host> <base-port> <lanes> <in-file> [block_size]

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "fuse/proto/aux.hpp"
#include "fuse/proto/block.hpp"
#include "fuse/proto/congestion.hpp"
#include "fuse/proto/hash.hpp"
#include "fuse/proto/receiver.hpp"
#include "fuse/proto/registry.hpp"
#include "fuse/proto/udp.hpp"

using namespace fuse::proto;

namespace {

constexpr uint8_t kWindow = kMaxWindow; // 64: the ACK bitmask width
constexpr size_t kRxBatch = 64;         // datagrams per recvmmsg
constexpr size_t kGsoBudget = 60000;    // bytes per GSO sendmsg (<64 KiB)

// Adaptive block-size bounds. The floor is a conventional MTU-safe payload;
// the ceiling only pays off where the path can carry it (loopback, jumbo
// frames), which is precisely why it is probed rather than assumed.
constexpr uint16_t kBlockMin = kDefaultPayloadSize;
constexpr uint16_t kBlockMax = kMaxPayloadSize;
constexpr uint32_t kCleanBatchesToGrow = 8;

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

void set_recv_timeout_us(int fd, long us) {
    timeval tv{};
    tv.tv_sec = us / 1000000;
    tv.tv_usec = us % 1000000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void set_sock_buffers(int fd, int bytes) {
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

struct LaneStats {
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> retransmits{0};
    std::atomic<uint64_t> batches{0};
    std::atomic<uint32_t> final_block{0};
    std::atomic<bool> ok{false};
    // Measured from this lane's first datagram to its last, so the figure
    // excludes the idle wait for the peer to start.
    std::atomic<uint64_t> start_ns{0};
    std::atomic<uint64_t> end_ns{0};
};

// --- Receiver lane -------------------------------------------------------

void recv_lane(uint16_t port, uint16_t lane, std::vector<uint8_t> *shard, LaneStats *stats) {
    UdpSocket sock;
    if (!sock.open("0.0.0.0", port)) {
        std::fprintf(stderr, "lane %u: bind failed on %u\n", lane, port);
        return;
    }
    set_sock_buffers(sock.fd(), 32 << 20);
    set_recv_timeout_us(sock.fd(), 200000);

    ReceiverStream rx(lane, kWindow, /*lossless=*/true);

    uint64_t shard_bytes = 0;
    bool have_start = false;
    uint64_t final_seq = UINT64_MAX; // learned from the last block's flag
    uint64_t bytes_written = 0;

    const size_t slot = kMaxDatagramSize + 64;
    std::vector<uint8_t> rx_buf(kRxBatch * slot);
    std::vector<size_t> lens(kRxBatch);
    std::vector<uint8_t> tx_buf(kMaxAuxDatagramSize + 64);

    PeerAddr peer{};
    bool have_peer = false;
    uint64_t delivered = 0, last_ack_blocks = 0, last_ack_ns = 0;
    int idle = 0;

    for (;;) {
        PeerAddr src;
        int got = sock.recv_batch(rx_buf.data(), slot, kRxBatch, lens.data(), &src);

        if (got > 0) {
            idle = 0;
            peer = src;
            have_peer = true;
            if (stats->start_ns.load() == 0) {
                stats->start_ns.store(now_ns()); // first datagram on this lane
            }
        } else {
            if (++idle > 25) {
                std::fprintf(stderr, "lane %u: timed out (base=%llu final=%llu)\n", lane,
                             (unsigned long long)rx.base_seq_no(),
                             (unsigned long long)final_seq);
                return;
            }
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
                }
                continue;
            }
            if (type != MsgType::Data || !have_start) continue;

            BlockHeader hdr;
            uint64_t send_time = 0;
            const uint8_t *payload = nullptr;
            if (!decode_data_datagram(dg, dlen, &hdr, &send_time, &payload)) continue;

            if (hdr.flags & kFlagLastBlock) {
                final_seq = hdr.seq_no; // the stream's extent, learned on arrival
            }

            if (rx.on_receive(hdr.seq_no, send_time, now_ns()) == ReceiveResult::Accepted &&
                payload != nullptr) {
                // Explicit offset, so a mid-stream block-size change cannot
                // misplace this payload.
                if (hdr.offset + hdr.payload_len <= shard->size()) {
                    std::memcpy(shard->data() + hdr.offset, payload, hdr.payload_len);
                    bytes_written += hdr.payload_len;
                }
                ++delivered;
            }
        }

        if (!have_peer || !have_start) continue;

        const uint64_t t = now_ns();
        if (delivered - last_ack_blocks >= 8 || t - last_ack_ns > 200000) {
            last_ack_blocks = delivered;
            last_ack_ns = t;
            Ack ack = rx.build_ack();
            size_t n = encode_ack(ack, tx_buf.data(), tx_buf.size());
            if (n) sock.send_to(tx_buf.data(), n, peer);
        }

        Nack nack;
        if (rx.collect_nacks(t, /*reorder=*/300000, /*renack=*/2000000, &nack) > 0) {
            size_t n = encode_nack(nack, tx_buf.data(), tx_buf.size());
            if (n) sock.send_to(tx_buf.data(), n, peer);
        }

        // Done once every block up to the announced last one has arrived.
        if (final_seq != UINT64_MAX && rx.base_seq_no() > final_seq) {
            stats->end_ns.store(now_ns());
            break;
        }
    }

    // Final ACKs so the sender can retire the lane promptly.
    for (int i = 0; i < 8; ++i) {
        Ack ack = rx.build_ack();
        size_t n = encode_ack(ack, tx_buf.data(), tx_buf.size());
        if (n) sock.send_to(tx_buf.data(), n, peer);
    }

    stats->bytes.store(bytes_written);
    stats->ok.store(bytes_written == shard_bytes);
}

// --- Sender lane ---------------------------------------------------------

void send_lane(const std::string &host, uint16_t port, uint16_t lane, const uint8_t *data,
               uint64_t shard_bytes, uint16_t start_block, LaneStats *stats) {
    UdpSocket sock;
    if (!sock.open("0.0.0.0", 0)) return;
    set_sock_buffers(sock.fd(), 32 << 20);
    set_recv_timeout_us(sock.fd(), 200);

    PeerAddr dst;
    if (!UdpSocket::resolve(host.c_str(), port, &dst)) return;

    SenderRegistry reg(lane, kWindow);
    // Grow aggressively: the window also bounds how many datagrams can be
    // packed into one GSO batch, so an over-conservative window throttles
    // batching as well as in-flight bytes.
    CongestionController cc(kWindow, true, /*clean_windows_to_grow=*/1, /*grow_step=*/16);

    std::vector<uint8_t> staging(kGsoBudget + kMaxDatagramSize);
    std::vector<uint8_t> ctl(kMaxAuxDatagramSize + 64);

    // Announce the shard. total_blocks is 0 = "not known up front", because
    // an adaptive block size means the block count is not fixed in advance;
    // the receiver learns the extent from the last block's flag instead.
    StreamStart ss;
    ss.stream_id = lane;
    ss.total_blocks = 0;
    ss.block_size = start_block;
    ss.total_bytes = shard_bytes;
    bool started = false;
    for (int attempt = 0; attempt < 200 && !started; ++attempt) {
        size_t n = encode_stream_start(ss, ctl.data(), ctl.size());
        sock.send_to(ctl.data(), n, dst);
        size_t got = 0;
        if (sock.recv_from(ctl.data(), ctl.size(), &got, nullptr)) {
            MsgType t;
            if (peek_msg_type(ctl.data(), got, &t) && t == MsgType::Ack) started = true;
        }
    }
    if (!started) {
        std::fprintf(stderr, "lane %u: receiver never acknowledged stream start\n", lane);
        return;
    }

    sock.set_nonblocking(true);

    uint16_t block = start_block;
    uint32_t clean_batches = 0;
    uint64_t base = 0, next_seq = 0, next_offset = 0;
    uint64_t retransmits = 0, batches = 0;
    uint64_t last_base = 0, last_progress = now_ns();
    const uint64_t t0 = now_ns();
    bool sent_last = false;

    // seq -> offset/len, so a NACK can be served without recomputing a
    // position that a block-size change would have invalidated.
    std::vector<std::pair<uint64_t, uint16_t>> sent_meta;
    sent_meta.reserve(shard_bytes / kBlockMin + 8);

    while (!sent_last || base <= (next_seq == 0 ? 0 : next_seq - 1)) {
        cc.poll(now_ns());
        const uint64_t window = std::min<uint64_t>(kWindow, std::max<uint32_t>(4, cc.window()));
        bool did_work = false;

        // --- Pack a GSO batch of equal-sized datagrams -------------------
        // Every segment in one sendmsg must be the same size, so a batch
        // uses a single block size; the size may change between batches.
        if (next_offset < shard_bytes && next_seq < base + window) {
            const uint16_t seg = static_cast<uint16_t>(kDataPrefixSize + block);
            size_t max_segs = std::min<size_t>(kGsoBudget / seg, window - (next_seq - base));
            size_t packed = 0, bytes_in_batch = 0;

            while (packed < max_segs && next_offset < shard_bytes) {
                const uint64_t remaining = shard_bytes - next_offset;
                if (remaining < block && packed > 0) break; // short tail: send alone

                const uint16_t len = static_cast<uint16_t>(std::min<uint64_t>(block, remaining));
                const bool is_last = (next_offset + len >= shard_bytes);

                BlockHeader hdr;
                hdr.stream_id = lane;
                hdr.seq_no = next_seq;
                hdr.flags = is_last ? kFlagLastBlock : 0;
                hdr.payload_len = len;
                hdr.offset = next_offset;

                reg.store(next_seq, data + next_offset, len, now_ns());
                if (sent_meta.size() <= next_seq) sent_meta.resize(next_seq + 1);
                sent_meta[next_seq] = {next_offset, len};

                size_t n = encode_data_datagram(hdr, now_ns(), data + next_offset,
                                                staging.data() + bytes_in_batch,
                                                staging.size() - bytes_in_batch);
                if (n == 0) break;
                bytes_in_batch += n;
                ++packed;
                ++next_seq;
                next_offset += len;
                if (is_last) { sent_last = true; break; }
                if (n != seg) break; // short block ends the homogeneous batch
            }

            if (packed > 0) {
                did_work = true;
                ++batches;
                if (g_drop_pct > 0) {
                    // Loss injection: send datagram-by-datagram, deliberately
                    // withholding some, to exercise NACK recovery and the
                    // block-size back-off without needing a netem qdisc.
                    size_t off = 0;
                    for (size_t i = 0; i < packed && off < bytes_in_batch; ++i) {
                        size_t n = std::min<size_t>(seg, bytes_in_batch - off);
                        if ((std::rand() % 100) >= g_drop_pct) {
                            sock.send_to(staging.data() + off, n, dst);
                        }
                        off += n;
                    }
                } else if (packed == 1 ||
                           !sock.send_segmented(staging.data(), bytes_in_batch, seg, dst)) {
                    // GSO unsupported or a single/ragged batch: send each.
                    size_t off = 0;
                    for (size_t i = 0; i < packed && off < bytes_in_batch; ++i) {
                        size_t n = std::min<size_t>(seg, bytes_in_batch - off);
                        sock.send_to(staging.data() + off, n, dst);
                        off += n;
                    }
                }
            }
        }

        // --- Feedback ----------------------------------------------------
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
                    uint64_t t = now_ns();
                    if (t > ack.echoed_send_time) cc.on_rtt_sample(t - ack.echoed_send_time);
                }
            } else if (type == MsgType::Nack) {
                Nack nack;
                if (!decode_nack(ctl.data(), got, &nack)) continue;
                cc.on_loss();
                loss_seen = true;
                for (uint16_t i = 0; i < nack.count; ++i) {
                    const uint64_t seq = nack.missing[i];
                    const RegistrySlot *slot = reg.lookup(seq);
                    if (!slot || seq >= sent_meta.size()) continue;
                    BlockHeader hdr;
                    hdr.stream_id = lane;
                    hdr.seq_no = seq;
                    hdr.flags = kFlagRetransmission;
                    hdr.payload_len = slot->payload_len;
                    hdr.offset = sent_meta[seq].first;
                    size_t n = encode_data_datagram(hdr, now_ns(), slot->payload, ctl.data(),
                                                    ctl.size());
                    if (n) sock.send_to(ctl.data(), n, dst);
                    ++retransmits;
                }
            }
        }

        // --- Adapt block size --------------------------------------------
        // Stable link: grow, so each syscall and each window carries more.
        // Any loss: fall straight back to the MTU-safe floor, since an
        // oversized block is the first thing to suspect.
        if (loss_seen) {
            block = kBlockMin;
            clean_batches = 0;
        } else if (++clean_batches >= kCleanBatchesToGrow && block < kBlockMax) {
            clean_batches = 0;
            block = static_cast<uint16_t>(std::min<uint32_t>(kBlockMax, block * 2u));
        }

        // --- Retransmission timeout --------------------------------------
        const uint64_t t = now_ns();
        if (base != last_base) {
            last_base = base;
            last_progress = t;
        } else if (t - last_progress > 5000000) {
            cc.on_loss();
            const RegistrySlot *slot = reg.lookup(base);
            if (slot && base < sent_meta.size()) {
                BlockHeader hdr;
                hdr.stream_id = lane;
                hdr.seq_no = base;
                hdr.flags = kFlagRetransmission;
                hdr.payload_len = slot->payload_len;
                hdr.offset = sent_meta[base].first;
                size_t n = encode_data_datagram(hdr, now_ns(), slot->payload, ctl.data(),
                                                ctl.size());
                if (n) sock.send_to(ctl.data(), n, dst);
                ++retransmits;
            }
            last_progress = t;
        }

        if (t - t0 > 120ull * 1000000000ull) {
            std::fprintf(stderr, "lane %u: timed out at base=%llu/%llu\n", lane,
                         (unsigned long long)base, (unsigned long long)next_seq);
            return;
        }
        if (sent_last && base >= next_seq) break;

        if (!did_work) {
            timespec nap{0, 20000};
            nanosleep(&nap, nullptr);
        }
    }

    stats->retransmits.store(retransmits);
    stats->batches.store(batches);
    stats->final_block.store(block);
    stats->bytes.store(shard_bytes);
    stats->ok.store(true);
}

// --- Drivers -------------------------------------------------------------

int run_receiver(uint16_t base_port, uint16_t lanes, const std::string &out_path) {
    std::vector<std::vector<uint8_t>> shards(lanes);
    std::vector<LaneStats> stats(lanes);

    std::vector<std::thread> threads;
    for (uint16_t i = 0; i < lanes; ++i) {
        threads.emplace_back(recv_lane, static_cast<uint16_t>(base_port + i), i, &shards[i],
                             &stats[i]);
    }
    for (auto &t : threads) t.join();

    // Wall time of the transfer itself: earliest first-datagram to latest
    // completion across lanes, so the idle wait for the sender to appear is
    // not charged to the protocol.
    uint64_t first_start = UINT64_MAX, last_end = 0;
    for (auto &s : stats) {
        const uint64_t st = s.start_ns.load(), en = s.end_ns.load();
        if (st != 0) first_start = std::min(first_start, st);
        last_end = std::max(last_end, en);
    }
    const double secs =
        (first_start != UINT64_MAX && last_end > first_start) ? (last_end - first_start) / 1e9 : 0.0;

    // Stitch the shards back together in lane order.
    uint64_t total = 0;
    for (auto &s : shards) total += s.size();
    std::vector<uint8_t> file;
    file.reserve(total);
    for (auto &s : shards) file.insert(file.end(), s.begin(), s.end());

    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(file.data()),
              static_cast<std::streamsize>(file.size()));
    out.close();

    bool all_ok = true;
    for (auto &s : stats) all_ok = all_ok && s.ok.load();

    std::printf("RECV bytes=%llu lanes=%u elapsed=%.4f throughput=%.1f MB/s ok=%d "
                "checksum=%016llx\n",
                (unsigned long long)file.size(), lanes, secs,
                (file.size() / (1024.0 * 1024.0)) / secs, all_ok ? 1 : 0,
                (unsigned long long)hash64(file.data(), file.size()));
    return all_ok ? 0 : 1;
}

int run_sender(const std::string &host, uint16_t base_port, uint16_t lanes,
               const std::string &in_path, uint16_t block) {
    std::ifstream in(in_path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", in_path.c_str());
        return 1;
    }
    const uint64_t total_bytes = static_cast<uint64_t>(in.tellg());
    in.seekg(0);
    std::vector<uint8_t> file(total_bytes);
    in.read(reinterpret_cast<char *>(file.data()), static_cast<std::streamsize>(total_bytes));
    in.close();

    // Contiguous shards: lane i carries [i*shard, ...).
    const uint64_t shard = (total_bytes + lanes - 1) / lanes;
    std::vector<LaneStats> stats(lanes);

    const uint64_t t0 = now_ns();
    std::vector<std::thread> threads;
    for (uint16_t i = 0; i < lanes; ++i) {
        const uint64_t off = std::min<uint64_t>(i * shard, total_bytes);
        const uint64_t len = std::min<uint64_t>(shard, total_bytes - off);
        threads.emplace_back(send_lane, std::cref(host), static_cast<uint16_t>(base_port + i), i,
                             file.data() + off, len, block, &stats[i]);
    }
    for (auto &t : threads) t.join();
    const double secs = (now_ns() - t0) / 1e9;

    uint64_t rtx = 0, batches = 0;
    uint32_t max_block = 0;
    bool all_ok = true;
    for (auto &s : stats) {
        rtx += s.retransmits.load();
        batches += s.batches.load();
        max_block = std::max(max_block, s.final_block.load());
        all_ok = all_ok && s.ok.load();
    }

    std::printf("SEND bytes=%llu lanes=%u elapsed=%.4f throughput=%.1f MB/s retransmits=%llu "
                "batches=%llu final_block=%u ok=%d\n",
                (unsigned long long)total_bytes, lanes, secs,
                (total_bytes / (1024.0 * 1024.0)) / secs, (unsigned long long)rtx,
                (unsigned long long)batches, max_block, all_ok ? 1 : 0);
    return all_ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage:\n  %s recv <base-port> <lanes> <out-file>\n"
                     "  %s send <host> <base-port> <lanes> <in-file> [block_size]\n",
                     argv[0], argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "recv") == 0 && argc >= 5) {
        return run_receiver(static_cast<uint16_t>(std::atoi(argv[2])),
                            static_cast<uint16_t>(std::atoi(argv[3])), argv[4]);
    }
    if (std::strcmp(argv[1], "send") == 0 && argc >= 6) {
        uint16_t block = (argc >= 7) ? static_cast<uint16_t>(std::atoi(argv[6])) : kBlockMin;
        block = std::clamp<uint16_t>(block, 64, kBlockMax);
        return run_sender(argv[2], static_cast<uint16_t>(std::atoi(argv[3])),
                          static_cast<uint16_t>(std::atoi(argv[4])), argv[5], block);
    }
    std::fprintf(stderr, "bad arguments\n");
    return 2;
}
