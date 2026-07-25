// Latency and goodput benchmark.
//
// Throughput alone is the least interesting number a transport can report:
// it says how fast bulk data moves when nothing else is happening. What
// actually distinguishes a transport is what happens to a small,
// latency-sensitive message while a bulk transfer is saturating the link.
//
// This harness runs both at once:
//
//   * a BULK stream (LOSSLESS=1-style, MTU-adaptive blocks) driving the link
//     as hard as it can, and
//   * a PROBE stream of small timestamped messages (LOSSLESS=0) sent at a
//     fixed interval on its own lane.
//
// It reports the probe latency distribution both idle and under bulk load.
// The difference between those two is the number that matters: it is
// exactly the head-of-line / shared-congestion coupling that per-stream
// congestion control is supposed to eliminate.
//
// Goodput is reported separately from throughput. Goodput counts only
// application payload actually delivered; the wire figure includes block
// headers, AEAD tags and retransmissions. The ratio is the protocol's real
// efficiency, which a raw MB/s number hides.
//
// One-way latency is meaningful here only because both ends share a clock
// (same host, CLOCK_MONOTONIC). Across hosts this would need RTT/2 or PTP.
//
//   fuse_latbench recv <base-port> <bulk-lanes>
//   fuse_latbench send <host> <base-port> <bulk-lanes> <seconds> <probe-hz>

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "fuse/proto/block.hpp"
#include "fuse/proto/udp.hpp"

using namespace fuse::proto;

namespace {

constexpr uint16_t kBulkStream = 1;
constexpr uint16_t kProbeStream = 900;
constexpr uint16_t kProbeBytes = 64;
constexpr uint16_t kBulkBlock = 1200; // MTU-safe, so this is not a loopback-only result
constexpr size_t kGsoBudget = 60000;
constexpr size_t kRxBatch = 64;

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

void set_bufs(int fd, int b) {
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &b, sizeof(b));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &b, sizeof(b));
}
void set_timeout_us(int fd, long us) {
    timeval tv{};
    tv.tv_sec = us / 1000000;
    tv.tv_usec = us % 1000000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

double pct(std::vector<uint64_t> &v, double p) {
    if (v.empty()) return 0.0;
    size_t i = static_cast<size_t>(p * (v.size() - 1));
    return v[i] / 1000.0; // ns -> us
}

struct Shared {
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> bulk_payload_rx{0}; // goodput numerator
    std::atomic<uint64_t> bulk_wire_rx{0};    // includes headers
    std::atomic<uint64_t> probes_rx{0};
};

// --- Receiver ------------------------------------------------------------

void bulk_recv(uint16_t port, Shared *sh) {
    UdpSocket s;
    if (!s.open("0.0.0.0", port)) return;
    set_bufs(s.fd(), 32 << 20);
    set_timeout_us(s.fd(), 200000);

    const size_t slot = kMaxDatagramSize + 64;
    std::vector<uint8_t> buf(kRxBatch * slot);
    std::vector<size_t> lens(kRxBatch);

    while (!sh->stop.load(std::memory_order_relaxed)) {
        int got = s.recv_batch(buf.data(), slot, kRxBatch, lens.data(), nullptr);
        for (int i = 0; i < got; ++i) {
            BlockHeader h;
            uint64_t st = 0;
            const uint8_t *p = nullptr;
            if (!decode_data_datagram(buf.data() + i * slot, lens[i], &h, &st, &p)) continue;
            sh->bulk_payload_rx.fetch_add(h.payload_len, std::memory_order_relaxed);
            sh->bulk_wire_rx.fetch_add(lens[i], std::memory_order_relaxed);
        }
    }
}

void probe_recv(uint16_t port, Shared *sh, std::vector<uint64_t> *lat) {
    UdpSocket s;
    if (!s.open("0.0.0.0", port)) return;
    set_bufs(s.fd(), 8 << 20);
    set_timeout_us(s.fd(), 200000);

    std::vector<uint8_t> buf(kMaxDatagramSize + 64);
    while (!sh->stop.load(std::memory_order_relaxed)) {
        size_t n = 0;
        if (!s.recv_from(buf.data(), buf.size(), &n, nullptr)) continue;
        BlockHeader h;
        uint64_t sent = 0;
        const uint8_t *p = nullptr;
        if (!decode_data_datagram(buf.data(), n, &h, &sent, &p)) continue;
        if (h.stream_id != kProbeStream) continue;
        const uint64_t t = now_ns();
        if (t > sent) lat->push_back(t - sent); // one-way: shared clock
        sh->probes_rx.fetch_add(1, std::memory_order_relaxed);
    }
}

// --- Sender --------------------------------------------------------------

void bulk_send(const std::string &host, uint16_t port, uint16_t lane, Shared *sh,
               std::atomic<uint64_t> *sent_payload) {
    UdpSocket s;
    if (!s.open("0.0.0.0", 0)) return;
    set_bufs(s.fd(), 32 << 20);
    PeerAddr dst;
    if (!UdpSocket::resolve(host.c_str(), port, &dst)) return;

    std::vector<uint8_t> payload(kBulkBlock, 0xC3);
    std::vector<uint8_t> staging(kGsoBudget + kMaxDatagramSize);
    const uint16_t seg = static_cast<uint16_t>(kDataPrefixSize + kBulkBlock);
    const size_t per_batch = kGsoBudget / seg;

    uint64_t seq = 0;
    while (!sh->stop.load(std::memory_order_relaxed)) {
        size_t bytes = 0;
        for (size_t i = 0; i < per_batch; ++i) {
            BlockHeader h;
            h.stream_id = kBulkStream;
            h.seq_no = seq;
            h.payload_len = kBulkBlock;
            h.offset = seq * kBulkBlock;
            size_t n = encode_data_datagram(h, now_ns(), payload.data(), staging.data() + bytes,
                                            staging.size() - bytes);
            if (n == 0) break;
            bytes += n;
            ++seq;
        }
        if (bytes == 0) break;
        if (!s.send_segmented(staging.data(), bytes, seg, dst)) {
            size_t off = 0;
            while (off < bytes) {
                size_t n = std::min<size_t>(seg, bytes - off);
                s.send_to(staging.data() + off, n, dst);
                off += n;
            }
        }
        sent_payload->fetch_add(per_batch * kBulkBlock, std::memory_order_relaxed);
    }
    (void)lane;
}

// Probes go out on their own lane at a fixed rate, so their latency reflects
// queueing/scheduling rather than their own send rate.
void probe_send(const std::string &host, uint16_t port, Shared *sh, uint32_t hz,
                std::atomic<uint64_t> *sent) {
    UdpSocket s;
    if (!s.open("0.0.0.0", 0)) return;
    PeerAddr dst;
    if (!UdpSocket::resolve(host.c_str(), port, &dst)) return;

    std::vector<uint8_t> payload(kProbeBytes, 0x5A);
    std::vector<uint8_t> dg(kMaxDatagramSize);
    const uint64_t interval = 1000000000ull / (hz ? hz : 1);
    uint64_t seq = 0, next = now_ns();

    while (!sh->stop.load(std::memory_order_relaxed)) {
        const uint64_t t = now_ns();
        if (t < next) {
            timespec nap{0, 50000};
            nanosleep(&nap, nullptr);
            continue;
        }
        next += interval;

        BlockHeader h;
        h.stream_id = kProbeStream;
        h.seq_no = seq++;
        h.payload_len = kProbeBytes;
        h.offset = 0;
        // Timestamp is written as late as possible so the measurement is of
        // the network path, not of our own encode cost.
        size_t n = encode_data_datagram(h, now_ns(), payload.data(), dg.data(), dg.size());
        if (n) {
            s.send_to(dg.data(), n, dst);
            sent->fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void report(const char *label, std::vector<uint64_t> lat, uint64_t sent, uint64_t recvd) {
    std::sort(lat.begin(), lat.end());
    const double loss = sent ? 100.0 * (1.0 - static_cast<double>(recvd) / sent) : 0.0;
    std::printf("%-22s n=%-7zu p50=%7.1f  p90=%7.1f  p99=%7.1f  p99.9=%7.1f  max=%8.1f  loss=%.2f%%\n",
                label, lat.size(), pct(lat, 0.50), pct(lat, 0.90), pct(lat, 0.99),
                pct(lat, 0.999), lat.empty() ? 0.0 : lat.back() / 1000.0, loss);
}

int run_receiver(uint16_t base, uint16_t lanes, uint64_t probes_expected) {
    Shared sh;
    std::vector<uint64_t> lat;
    lat.reserve(1 << 20);

    std::vector<std::thread> th;
    for (uint16_t i = 0; i < lanes; ++i) {
        th.emplace_back(bulk_recv, static_cast<uint16_t>(base + i), &sh);
    }
    std::thread probe(probe_recv, static_cast<uint16_t>(base + 100), &sh, &lat);

    // Run until the sender goes quiet.
    uint64_t last = 0;
    int idle = 0;
    while (idle < 30) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        uint64_t cur = sh.bulk_payload_rx.load() + sh.probes_rx.load();
        idle = (cur == last) ? idle + 1 : 0;
        last = cur;
    }
    sh.stop.store(true);
    for (auto &t : th) t.join();
    probe.join();

    const uint64_t pay = sh.bulk_payload_rx.load();
    const uint64_t wire = sh.bulk_wire_rx.load();
    // Goodput counts only application payload; the wire figure includes
    // every block header. The ratio is the protocol's real efficiency, which
    // a raw MB/s number hides.
    const double eff = wire ? 100.0 * static_cast<double>(pay) / static_cast<double>(wire) : 0.0;
    std::printf("RECV bulk_goodput_bytes=%llu wire_bytes=%llu efficiency=%.2f%% probes=%llu\n",
                (unsigned long long)pay, (unsigned long long)wire, eff,
                (unsigned long long)sh.probes_rx.load());
    report("probe-latency(us)", lat, probes_expected, sh.probes_rx.load());
    return 0;
}

int run_sender(const std::string &host, uint16_t base, uint16_t lanes, double seconds,
               uint32_t hz, bool loaded) {
    Shared sh;
    std::atomic<uint64_t> probe_sent{0}, bulk_sent{0};

    std::vector<std::thread> th;
    if (loaded) {
        for (uint16_t i = 0; i < lanes; ++i) {
            th.emplace_back(bulk_send, std::cref(host), static_cast<uint16_t>(base + i), i, &sh,
                            &bulk_sent);
        }
    }
    std::thread probe(probe_send, std::cref(host), static_cast<uint16_t>(base + 100), &sh, hz,
                      &probe_sent);

    const uint64_t t0 = now_ns();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    sh.stop.store(true);
    for (auto &t : th) t.join();
    probe.join();
    const double secs = (now_ns() - t0) / 1e9;

    std::printf("SEND phase=%s probes_sent=%llu bulk_payload_sent=%llu "
                "offered_goodput=%.1f MB/s elapsed=%.2f\n",
                loaded ? "loaded" : "idle", (unsigned long long)probe_sent.load(),
                (unsigned long long)bulk_sent.load(),
                (bulk_sent.load() / (1024.0 * 1024.0)) / secs, secs);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage:\n  %s recv <base-port> <bulk-lanes> [expected-probes]\n"
                     "  %s send <host> <base-port> <bulk-lanes> <seconds> <probe-hz> <loaded01>\n",
                     argv[0], argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "recv") == 0 && argc >= 4) {
        const uint64_t expected = (argc >= 5) ? std::strtoull(argv[4], nullptr, 10) : 0;
        return run_receiver(static_cast<uint16_t>(std::atoi(argv[2])),
                            static_cast<uint16_t>(std::atoi(argv[3])), expected);
    }
    if (std::strcmp(argv[1], "send") == 0 && argc >= 8) {
        return run_sender(argv[2], static_cast<uint16_t>(std::atoi(argv[3])),
                          static_cast<uint16_t>(std::atoi(argv[4])), std::atof(argv[5]),
                          static_cast<uint32_t>(std::atoi(argv[6])),
                          std::atoi(argv[7]) != 0);
    }
    std::fprintf(stderr, "bad arguments\n");
    return 2;
}
