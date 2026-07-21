// Stage 8 benchmark harness.
//
// Runs a generic synthetic workload — many small loss-tolerant messages
// (LOSSLESS=0, latency-sensitive) plus one large ordered bulk transfer
// (LOSSLESS=1, correctness-sensitive) — across the configurations the work
// plan calls for:
//
//   (a) single-threaded baseline
//   (b) multi-worker, no topology pinning
//   (c) multi-worker, with topology pinning (Stage 6)
//
// each optionally with encryption forced on (Stage 7), so DTLS overhead is
// measured separately from the threading/pinning comparison rather than
// conflated with it.
//
// Metrics: bulk throughput, small-message delivery latency (p50/p99, taken
// from the send timestamp each block already carries), CPU time, and peak
// RSS. The workload is deliberately domain-agnostic.
//
// NOT INCLUDED: the side-by-side comparison against a reference QUIC
// implementation. That requires linking a real QUIC stack (quiche/msquic/
// ngtcp2); until it is wired up, no claim about beating QUIC is supported
// by this harness. See docs/ROADMAP.md.

#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "fuse/proto/block.hpp"
#include "fuse/proto/dtls.hpp"
#include "fuse/proto/reassembly.hpp"
#include "fuse/proto/receiver.hpp"
#include "fuse/proto/registry.hpp"
#include "fuse/proto/setup.hpp"
#include "fuse/proto/topology.hpp"
#include "fuse/proto/udp.hpp"

using namespace fuse::proto;

namespace {

// --- Workload shape (tunable via argv) ---------------------------------
struct Workload {
    uint64_t bulk_bytes = 8ull * 1024 * 1024; // one large ordered transfer
    uint16_t bulk_block = 1200;               // ~MTU-sized blocks
    uint64_t telemetry_msgs = 20000;          // many small loss-tolerant msgs
    uint16_t telemetry_block = 64;
    int lanes = 4;                            // worker lanes for (b)/(c)
};

struct Result {
    std::string name;
    double seconds = 0;
    uint64_t bulk_bytes_rx = 0;
    uint64_t bulk_bytes_tx = 0;
    uint64_t telemetry_rx = 0;
    uint64_t telemetry_tx = 0;
    double p50_us = 0;
    double p99_us = 0;
    double cpu_seconds = 0;
    long peak_rss_kb = 0;

    double throughput_mbps() const {
        return seconds > 0 ? (static_cast<double>(bulk_bytes_rx) / (1024.0 * 1024.0)) / seconds : 0;
    }
    double loss_pct() const {
        uint64_t tx = bulk_bytes_tx + telemetry_tx * 1;
        (void)tx;
        uint64_t sent = telemetry_tx;
        return sent > 0 ? 100.0 * (1.0 - static_cast<double>(telemetry_rx) / sent) : 0.0;
    }
};

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

double cpu_seconds_used() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return (ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6) +
           (ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
}

long peak_rss_kb() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

void set_sock_buffers(int fd, int bytes) {
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

void set_recv_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// One sender/receiver lane: a socket pair plus the blocks assigned to it.
struct Lane {
    UdpSocket tx, rx;
    PeerAddr rx_addr{};
    uint64_t bulk_blocks = 0;
    uint64_t telemetry_blocks = 0;

    // Results filled by the receiver thread.
    std::atomic<uint64_t> bulk_bytes_rx{0};
    std::atomic<uint64_t> telemetry_rx{0};
    std::vector<uint64_t> latencies; // ns, telemetry only
    std::atomic<bool> done{false};
};

constexpr uint16_t kBulkStream = 1;      // LOSSLESS=1, ORDERED=1
constexpr uint16_t kTelemetryStream = 2; // LOSSLESS=0

// Receiver: drains its socket until it has seen everything expected or the
// socket goes quiet. Records telemetry latency from each block's send time.
void receiver_thread(Lane *lane, const Workload &wl, bool encrypted, const std::string &psk) {
    DtlsSession sess;
    DtlsConfig cfg;
    cfg.encryption_required = encrypted;
    cfg.role = DtlsRole::Server;
    cfg.psk_key = psk;
    PeerAddr any{};
    if (sess.configure(cfg, &lane->rx, any) == DtlsStatus::Unsupported) {
        lane->done.store(true);
        return;
    }
    if (encrypted && sess.handshake() != DtlsStatus::Ok) {
        lane->done.store(true);
        return;
    }

    ReceiverStream bulk_rx(kBulkStream, 64, /*lossless=*/true);
    ReceiverStream tele_rx(kTelemetryStream, 64, /*lossless=*/false);

    std::vector<uint8_t> buf(kMaxDatagramSize + 256);
    const uint64_t expected = lane->bulk_blocks + lane->telemetry_blocks;
    uint64_t seen = 0;
    int quiet_rounds = 0;

    while (seen < expected && quiet_rounds < 3) {
        size_t got = 0;
        bool ok;
        if (encrypted) {
            ok = (sess.recv(buf.data(), buf.size(), &got) == DtlsStatus::Ok);
        } else {
            ok = lane->rx.recv_from(buf.data(), buf.size(), &got, nullptr);
        }
        if (!ok) {
            ++quiet_rounds; // socket timeout: sender probably finished
            continue;
        }
        quiet_rounds = 0;

        BlockHeader hdr;
        uint64_t send_time = 0;
        const uint8_t *payload = nullptr;
        if (!decode_data_datagram(buf.data(), got, &hdr, &send_time, &payload)) {
            continue;
        }
        ++seen;

        if (hdr.stream_id == kBulkStream) {
            bulk_rx.on_receive(hdr.seq_no, send_time, now_ns());
            lane->bulk_bytes_rx.fetch_add(hdr.payload_len, std::memory_order_relaxed);
        } else {
            tele_rx.on_receive(hdr.seq_no, send_time, now_ns());
            lane->telemetry_rx.fetch_add(1, std::memory_order_relaxed);
            lane->latencies.push_back(now_ns() - send_time);
        }
    }
    lane->done.store(true);
}

// Sender: interleaves its share of the bulk transfer and the telemetry
// stream, storing every block in a registry as the real data path would.
void sender_thread(Lane *lane, const Workload &wl, bool encrypted, const std::string &psk,
                   int pin_core) {
    if (pin_core >= 0) {
        pin_current_thread_to_core(pin_core);
    }

    DtlsSession sess;
    DtlsConfig cfg;
    cfg.encryption_required = encrypted;
    cfg.role = DtlsRole::Client;
    cfg.psk_key = psk;
    if (sess.configure(cfg, &lane->tx, lane->rx_addr) == DtlsStatus::Unsupported) {
        return;
    }
    if (encrypted && sess.handshake() != DtlsStatus::Ok) {
        return;
    }

    SenderRegistry bulk_reg(kBulkStream, 64);
    SenderRegistry tele_reg(kTelemetryStream, 64);

    std::vector<uint8_t> payload(std::max(wl.bulk_block, wl.telemetry_block), 0xAB);
    std::vector<uint8_t> datagram(kMaxDatagramSize + 256);

    uint64_t bulk_sent = 0, tele_sent = 0;
    const uint64_t total = lane->bulk_blocks + lane->telemetry_blocks;

    for (uint64_t i = 0; i < total; ++i) {
        // Interleave so the latency-sensitive stream is not starved behind
        // the bulk transfer — the mixed-workload case the plan targets.
        const bool send_bulk =
            (tele_sent >= lane->telemetry_blocks) ||
            (bulk_sent < lane->bulk_blocks &&
             (i % 4 != 0)); // ~3:1 bulk:telemetry interleave

        BlockHeader hdr;
        if (send_bulk) {
            hdr.stream_id = kBulkStream;
            hdr.seq_no = bulk_sent;
            hdr.payload_len = wl.bulk_block;
            bulk_reg.store(bulk_sent, payload.data(), wl.bulk_block, now_ns());
            ++bulk_sent;
        } else {
            hdr.stream_id = kTelemetryStream;
            hdr.seq_no = tele_sent;
            hdr.payload_len = wl.telemetry_block;
            tele_reg.store(tele_sent, payload.data(), wl.telemetry_block, now_ns());
            ++tele_sent;
        }

        size_t n = encode_data_datagram(hdr, now_ns(), payload.data(), datagram.data(),
                                        datagram.size());
        if (n == 0) continue;
        if (encrypted) {
            sess.send(datagram.data(), n);
        } else {
            lane->tx.send_to(datagram.data(), n, lane->rx_addr);
        }
    }
}

Result run_config(const std::string &name, const Workload &wl, int lanes, bool pin,
                  bool encrypted) {
    const std::string psk = "fuse-bench-preshared-key";
    const uint64_t bulk_blocks_total = wl.bulk_bytes / wl.bulk_block;

    std::vector<std::unique_ptr<Lane>> lane_objs;
    for (int i = 0; i < lanes; ++i) {
        auto lane = std::make_unique<Lane>();
        if (!lane->tx.open("127.0.0.1", 0) || !lane->rx.open("127.0.0.1", 0)) {
            std::fprintf(stderr, "socket open failed\n");
            return {};
        }
        set_sock_buffers(lane->tx.fd(), 8 << 20);
        set_sock_buffers(lane->rx.fd(), 8 << 20);
        set_recv_timeout(lane->rx.fd(), 400);
        UdpSocket::resolve("127.0.0.1", lane->rx.local_port(), &lane->rx_addr);

        lane->bulk_blocks = bulk_blocks_total / lanes;
        lane->telemetry_blocks = wl.telemetry_msgs / lanes;
        lane->latencies.reserve(lane->telemetry_blocks);
        lane_objs.push_back(std::move(lane));
    }

    double cpu_before = cpu_seconds_used();
    uint64_t t0 = now_ns();

    std::vector<std::thread> rx_threads, tx_threads;
    for (int i = 0; i < lanes; ++i) {
        rx_threads.emplace_back(receiver_thread, lane_objs[i].get(), std::cref(wl), encrypted,
                                std::cref(psk));
    }
    // Give receivers a moment to be ready (and to complete DTLS accept).
    std::this_thread::sleep_for(std::chrono::milliseconds(encrypted ? 60 : 20));
    for (int i = 0; i < lanes; ++i) {
        int core = pin ? (i % static_cast<int>(std::thread::hardware_concurrency())) : -1;
        tx_threads.emplace_back(sender_thread, lane_objs[i].get(), std::cref(wl), encrypted,
                                std::cref(psk), core);
    }
    for (auto &t : tx_threads) t.join();
    for (auto &t : rx_threads) t.join();

    uint64_t t1 = now_ns();

    Result r;
    r.name = name;
    r.seconds = (t1 - t0) / 1e9;
    r.cpu_seconds = cpu_seconds_used() - cpu_before;
    r.peak_rss_kb = peak_rss_kb();

    std::vector<uint64_t> all_lat;
    for (auto &lane : lane_objs) {
        r.bulk_bytes_rx += lane->bulk_bytes_rx.load();
        r.bulk_bytes_tx += lane->bulk_blocks * wl.bulk_block;
        r.telemetry_rx += lane->telemetry_rx.load();
        r.telemetry_tx += lane->telemetry_blocks;
        all_lat.insert(all_lat.end(), lane->latencies.begin(), lane->latencies.end());
    }
    if (!all_lat.empty()) {
        std::sort(all_lat.begin(), all_lat.end());
        r.p50_us = all_lat[all_lat.size() / 2] / 1000.0;
        r.p99_us = all_lat[static_cast<size_t>(all_lat.size() * 0.99)] / 1000.0;
    }
    return r;
}

void print_header(const char *title) {
    std::printf("\n%s\n", title);
    std::printf("%-38s %10s %10s %9s %9s %8s %8s\n", "configuration", "MB/s", "elapsed s",
                "p50 us", "p99 us", "cpu s", "loss %");
    std::printf("%.*s\n", 96,
                "--------------------------------------------------------------------------------"
                "----------------");
}

void print_row(const Result &r) {
    std::printf("%-38s %10.1f %10.2f %9.1f %9.1f %8.2f %8.2f\n", r.name.c_str(),
                r.throughput_mbps(), r.seconds, r.p50_us, r.p99_us, r.cpu_seconds, r.loss_pct());
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    Workload wl;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bulk-mb") == 0 && i + 1 < argc) {
            wl.bulk_bytes = static_cast<uint64_t>(std::atoll(argv[++i])) * 1024 * 1024;
        } else if (std::strcmp(argv[i], "--telemetry") == 0 && i + 1 < argc) {
            wl.telemetry_msgs = static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--lanes") == 0 && i + 1 < argc) {
            wl.lanes = std::atoi(argv[++i]);
        }
    }

    std::printf("fuse benchmark harness (Stage 8)\n");
    std::printf("workload: bulk %llu MiB in %u B blocks (LOSSLESS=1,ORDERED=1) + %llu telemetry "
                "msgs of %u B (LOSSLESS=0)\n",
                static_cast<unsigned long long>(wl.bulk_bytes / (1024 * 1024)), wl.bulk_block,
                static_cast<unsigned long long>(wl.telemetry_msgs), wl.telemetry_block);
    std::printf("host: %u cores; DTLS available: %s\n", std::thread::hardware_concurrency(),
                dtls_available() ? "yes" : "no");

    std::vector<Result> plain, crypt;

    plain.push_back(run_config("(a) single-threaded", wl, 1, false, false));
    plain.push_back(run_config("(b) multi-worker, unpinned", wl, wl.lanes, false, false));
    plain.push_back(run_config("(c) multi-worker, pinned", wl, wl.lanes, true, false));

    print_header("=== Threading / topology comparison (encryption OFF) ===");
    for (const auto &r : plain) print_row(r);

    if (dtls_available()) {
        crypt.push_back(run_config("(a) single-threaded + DTLS", wl, 1, false, true));
        crypt.push_back(run_config("(b) multi-worker, unpinned + DTLS", wl, wl.lanes, false, true));
        crypt.push_back(run_config("(c) multi-worker, pinned + DTLS", wl, wl.lanes, true, true));

        print_header("=== Same configurations with encryption FORCED ON (Stage 7) ===");
        for (const auto &r : crypt) print_row(r);

        print_header("=== Encryption overhead, isolated ===");
        for (size_t i = 0; i < plain.size() && i < crypt.size(); ++i) {
            double tp_delta = plain[i].throughput_mbps() > 0
                                  ? 100.0 * (1.0 - crypt[i].throughput_mbps() /
                                                       plain[i].throughput_mbps())
                                  : 0.0;
            std::printf("%-38s throughput -%.1f%%  (p50 %.1f -> %.1f us)\n", plain[i].name.c_str(),
                        tp_delta, plain[i].p50_us, crypt[i].p50_us);
        }
    } else {
        std::printf("\n(DTLS unavailable in this build: encryption comparison skipped)\n");
    }

    std::printf("\npeak RSS: %ld KiB\n", peak_rss_kb());
    std::printf("\nNOTE: no reference-QUIC baseline is included in this harness yet, so it\n"
                "cannot substantiate any claim of beating QUIC. Wiring one up (quiche/msquic/\n"
                "ngtcp2) is the remaining Stage 8 work.\n");
    return 0;
}
