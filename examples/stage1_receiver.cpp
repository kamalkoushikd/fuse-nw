// Stage 1 datagram-protocol receiver demo.
//
// Binds a UDP port, receives DATA blocks for a single stream, tracks them
// with a bitmask, ACKs progress, and NACKs gaps after a reorder delay.
// Pair with fuse_stage1_sender (run the receiver first). This exercises
// only Stage 1 machinery; there is no handshake yet (that is Stage 2).
//
//   ./fuse_stage1_receiver [port]     (default 48000)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "fuse/proto/aux.hpp"
#include "fuse/proto/block.hpp"
#include "fuse/proto/receiver.hpp"
#include "fuse/proto/udp.hpp"

using namespace fuse::proto;

static uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

int main(int argc, char **argv) {
    // Line-buffer stdout so progress is visible in real time even when
    // piped to a file, and nothing is lost if the process is killed.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::atoi(argv[1])) : 48000;

    constexpr uint16_t kStream = 1;
    constexpr uint8_t  kWindow = 32;
    constexpr uint64_t kReorderDelayNs = 5'000'000;   // 5 ms
    constexpr uint64_t kRenackNs       = 20'000'000;  // 20 ms

    UdpSocket sock;
    if (!sock.open("0.0.0.0", port)) {
        std::fprintf(stderr, "failed to bind UDP port %u\n", port);
        return 1;
    }
    sock.set_nonblocking(true);
    std::printf("stage1_receiver listening on 0.0.0.0:%u (stream %u, window %u)\n",
                port, kStream, kWindow);

    ReceiverStream receiver(kStream, kWindow);
    uint8_t buf[kMaxDatagramSize];
    PeerAddr peer;
    bool have_peer = false;
    uint64_t received = 0;

    for (;;) {
        size_t got = 0;
        PeerAddr src;
        if (sock.recv_from(buf, sizeof(buf), &got, &src)) {
            MsgType type;
            if (!peek_msg_type(buf, got, &type) || type != MsgType::Data) {
                continue;
            }
            BlockHeader hdr;
            uint64_t send_time = 0;
            const uint8_t *payload = nullptr;
            if (!decode_data_datagram(buf, got, &hdr, &send_time, &payload)) {
                continue;
            }
            peer = src;
            have_peer = true;

            ReceiveResult r = receiver.on_receive(hdr.seq_no, send_time, now_ns());
            if (r == ReceiveResult::Accepted) {
                ++received;
                const char *rtx = (hdr.flags & kFlagRetransmission) ? " [retransmit]" : "";
                std::printf("recv seq=%llu%s  base=%llu  received_total=%llu\n",
                            static_cast<unsigned long long>(hdr.seq_no), rtx,
                            static_cast<unsigned long long>(receiver.base_seq_no()),
                            static_cast<unsigned long long>(received));
            }

            // ACK current progress back to the sender.
            Ack ack = receiver.build_ack();
            size_t n = encode_ack(ack, buf, sizeof(buf));
            if (n > 0) {
                sock.send_to(buf, n, peer);
            }
        }

        // Emit NACKs for gaps that have outlived the reorder tolerance.
        if (have_peer) {
            Nack nack;
            uint16_t count = receiver.collect_nacks(now_ns(), kReorderDelayNs, kRenackNs, &nack);
            if (count > 0) {
                for (uint16_t i = 0; i < count; ++i) {
                    std::printf("NACK seq=%llu\n",
                                static_cast<unsigned long long>(nack.missing[i]));
                }
                size_t n = encode_nack(nack, buf, sizeof(buf));
                if (n > 0) {
                    sock.send_to(buf, n, peer);
                }
            }
        }
    }

    return 0;
}
