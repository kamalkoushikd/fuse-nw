// Userspace network emulator — a UDP impairment relay.
//
// A transport protocol has to be tested against the conditions of a real
// network, not just loopback: packet loss, propagation delay, jitter,
// reordering and duplication. The kernel's `tc netem` is the usual tool,
// but it needs CAP_NET_ADMIN. This relay does the same job entirely in
// userspace, so it works unprivileged and identically for any UDP-based
// protocol — Fuse and the reference QUIC both just point their sender at the
// relay instead of at the receiver.
//
// Topology:  client  <->  relay(this)  <->  server
// The relay listens on `nports` consecutive ports and forwards each to the
// server, applying the configured impairments to datagrams in BOTH
// directions (so ACKs are impaired too, as they are on a real path). One
// thread per port keeps the relay from being the bottleneck at low port
// counts; at multi-gigabit line rates the relay's own per-packet cost does
// cap throughput, so absolute numbers through it are relay-limited — but the
// cap is identical for every protocol, and loss/delay/reorder effects still
// show through differentially. Latency measurements stay valid: the relay
// adds a known, constant one-way delay.
//
//   fuse_netem <listen-base> <nports> <server-host> <server-base> [opts]
//
// Impairment options (all default 0 = a transparent relay):
//   --delay-ms N     one-way propagation delay added each direction
//   --jitter-ms N    uniform +/- jitter on top of the delay (causes reorder)
//   --loss-pct N     percent of datagrams dropped
//   --dup-pct N      percent of datagrams duplicated
//   --reorder-pct N  percent sent immediately while the rest take the delay
//   --seed N         RNG seed, for reproducible runs

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <queue>
#include <random>
#include <thread>
#include <vector>

namespace {

struct Impair {
    double delay_ms = 0;
    double jitter_ms = 0;
    double loss_pct = 0;
    double dup_pct = 0;
    double reorder_pct = 0;
    uint32_t seed = 1;
};

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

constexpr size_t kMaxDg = 65536;

// A datagram waiting for its scheduled release time.
struct Pending {
    uint64_t release_ns;
    bool to_server; // direction this datagram travels
    size_t len;
    std::vector<uint8_t> data;
    bool operator<(const Pending &o) const { return release_ns > o.release_ns; } // min-heap
};

std::atomic<uint64_t> g_forwarded{0};
std::atomic<uint64_t> g_dropped{0};

// Relays one port: client<->server, applying impairments both ways.
void relay_port(uint16_t listen_port, const std::string &server_host, uint16_t server_port,
                Impair im) {
    int lsock = socket(AF_INET, SOCK_DGRAM, 0); // faces the client
    int usock = socket(AF_INET, SOCK_DGRAM, 0); // faces the server
    if (lsock < 0 || usock < 0) return;

    int big = 32 << 20;
    for (int s : {lsock, usock}) {
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    }

    sockaddr_in laddr{};
    laddr.sin_family = AF_INET;
    laddr.sin_addr.s_addr = htonl(INADDR_ANY);
    laddr.sin_port = htons(listen_port);
    if (bind(lsock, reinterpret_cast<sockaddr *>(&laddr), sizeof(laddr)) != 0) {
        std::fprintf(stderr, "netem: bind %u failed\n", listen_port);
        return;
    }

    sockaddr_in saddr{};
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_host.c_str(), &saddr.sin_addr);

    sockaddr_in client{};
    socklen_t client_len = 0;
    bool have_client = false;

    std::mt19937 rng(im.seed ^ (listen_port * 2654435761u));
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_real_distribution<double> jit(-im.jitter_ms, im.jitter_ms);

    std::priority_queue<Pending> queue;
    std::vector<uint8_t> buf(kMaxDg);

    auto schedule = [&](const uint8_t *data, size_t len, bool to_server) {
        // Loss.
        if (im.loss_pct > 0 && unit(rng) * 100.0 < im.loss_pct) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Delay + jitter, unless this datagram is chosen to skip ahead
        // (netem's reorder model: a fraction travel with ~no delay).
        double delay = im.delay_ms;
        if (im.reorder_pct > 0 && unit(rng) * 100.0 < im.reorder_pct) {
            delay = 0;
        } else if (im.jitter_ms > 0) {
            delay += jit(rng);
        }
        if (delay < 0) delay = 0;
        const uint64_t rel = now_ns() + static_cast<uint64_t>(delay * 1e6);

        Pending p;
        p.release_ns = rel;
        p.to_server = to_server;
        p.len = len;
        p.data.assign(data, data + len);
        queue.push(p);

        // Duplication: a second copy at the same release time.
        if (im.dup_pct > 0 && unit(rng) * 100.0 < im.dup_pct) {
            queue.push(p);
        }
    };

    auto forward = [&](const Pending &p) {
        if (p.to_server) {
            sendto(usock, p.data.data(), p.len, 0, reinterpret_cast<sockaddr *>(&saddr),
                   sizeof(saddr));
        } else if (have_client) {
            sendto(lsock, p.data.data(), p.len, 0, reinterpret_cast<sockaddr *>(&client),
                   client_len);
        }
        g_forwarded.fetch_add(1, std::memory_order_relaxed);
    };

    pollfd pfds[2] = {{lsock, POLLIN, 0}, {usock, POLLIN, 0}};

    for (;;) {
        // Wait until either a socket is readable or the next queued datagram
        // is due for release.
        int timeout = -1;
        if (!queue.empty()) {
            const uint64_t t = now_ns();
            const uint64_t r = queue.top().release_ns;
            timeout = (r > t) ? static_cast<int>((r - t) / 1000000ull) : 0;
        }
        poll(pfds, 2, timeout);

        if (pfds[0].revents & POLLIN) {
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            const ssize_t n = recvfrom(lsock, buf.data(), buf.size(), 0,
                                       reinterpret_cast<sockaddr *>(&from), &fl);
            if (n > 0) {
                client = from; // the client is whoever talks to us on lsock
                client_len = fl;
                have_client = true;
                schedule(buf.data(), static_cast<size_t>(n), /*to_server=*/true);
            }
        }
        if (pfds[1].revents & POLLIN) {
            const ssize_t n = recvfrom(usock, buf.data(), buf.size(), 0, nullptr, nullptr);
            if (n > 0) {
                schedule(buf.data(), static_cast<size_t>(n), /*to_server=*/false);
            }
        }

        const uint64_t t = now_ns();
        while (!queue.empty() && queue.top().release_ns <= t) {
            forward(queue.top());
            queue.pop();
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: %s <listen-base> <nports> <server-host> <server-base> [opts]\n"
                     "opts: --delay-ms N --jitter-ms N --loss-pct N --dup-pct N "
                     "--reorder-pct N --seed N\n",
                     argv[0]);
        return 2;
    }
    const uint16_t listen_base = static_cast<uint16_t>(std::atoi(argv[1]));
    const int nports = std::atoi(argv[2]);
    const std::string server_host = argv[3];
    const uint16_t server_base = static_cast<uint16_t>(std::atoi(argv[4]));

    Impair im;
    for (int i = 5; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const double v = std::atof(argv[i + 1]);
        if (k == "--delay-ms") im.delay_ms = v;
        else if (k == "--jitter-ms") im.jitter_ms = v;
        else if (k == "--loss-pct") im.loss_pct = v;
        else if (k == "--dup-pct") im.dup_pct = v;
        else if (k == "--reorder-pct") im.reorder_pct = v;
        else if (k == "--seed") im.seed = static_cast<uint32_t>(v);
    }

    std::printf("netem: %d ports %u+ -> %s:%u+  delay=%.1fms jitter=%.1fms loss=%.2f%% "
                "dup=%.2f%% reorder=%.1f%%\n",
                nports, listen_base, server_host.c_str(), server_base, im.delay_ms, im.jitter_ms,
                im.loss_pct, im.dup_pct, im.reorder_pct);

    std::vector<std::thread> threads;
    for (int i = 0; i < nports; ++i) {
        threads.emplace_back(relay_port, static_cast<uint16_t>(listen_base + i), server_host,
                             static_cast<uint16_t>(server_base + i), im);
    }
    for (auto &t : threads) t.join();
    return 0;
}
