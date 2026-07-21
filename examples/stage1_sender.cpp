// Stage 1 datagram-protocol sender demo.
//
// Sends a fixed number of DATA blocks for one stream, stores each in a
// registry, and services NACKs by retransmitting the exact block from the
// registry. Pass --drop N to deliberately withhold seq_no N on its first
// send, so the receiver's NACK / this sender's retransmit path is visibly
// exercised end to end. Run fuse_stage1_receiver first.
//
//   ./fuse_stage1_sender [host] [port] [--drop N] [--count N]
//   defaults: 127.0.0.1 48000, no drop, count 32

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "fuse/proto/aux.hpp"
#include "fuse/proto/block.hpp"
#include "fuse/proto/registry.hpp"
#include "fuse/proto/udp.hpp"

using namespace fuse::proto;

static uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    const char *host = "127.0.0.1";
    uint16_t port = 48000;
    int64_t drop_seq = -1;
    uint64_t count = 32;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--drop") == 0 && i + 1 < argc) {
            drop_seq = std::atoll(argv[++i]);
        } else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (positional == 0) {
            host = argv[i];
            ++positional;
        } else if (positional == 1) {
            port = static_cast<uint16_t>(std::atoi(argv[i]));
            ++positional;
        }
    }

    constexpr uint16_t kStream = 1;
    constexpr uint8_t  kWindow = 32;
    constexpr uint16_t kLen    = 1200;

    UdpSocket sock;
    if (!sock.open("0.0.0.0", 0)) {
        std::fprintf(stderr, "failed to open sender socket\n");
        return 1;
    }
    sock.set_nonblocking(true);

    PeerAddr dst;
    if (!UdpSocket::resolve(host, port, &dst)) {
        std::fprintf(stderr, "bad destination %s:%u\n", host, port);
        return 1;
    }

    SenderRegistry registry(kStream, kWindow);
    uint8_t buf[kMaxDatagramSize];
    uint8_t payload[kLen];

    std::printf("stage1_sender -> %s:%u  count=%llu drop=%lld\n", host, port,
                static_cast<unsigned long long>(count),
                static_cast<long long>(drop_seq));

    // Send all blocks, withholding drop_seq on its first pass.
    for (uint64_t seq = 0; seq < count; ++seq) {
        for (uint16_t i = 0; i < kLen; ++i) payload[i] = static_cast<uint8_t>(seq * 31 + i);
        registry.store(seq, payload, kLen, now_ns());

        if (static_cast<int64_t>(seq) == drop_seq) {
            std::printf("send seq=%llu  [deliberately dropped]\n",
                        static_cast<unsigned long long>(seq));
        } else {
            BlockHeader hdr;
            hdr.stream_id = kStream;
            hdr.seq_no = seq;
            hdr.flags = (seq + 1 == count) ? kFlagLastBlock : 0;
            hdr.payload_len = kLen;
            size_t n = encode_data_datagram(hdr, now_ns(), payload, buf, sizeof(buf));
            if (n > 0) sock.send_to(buf, n, dst);
        }

        // Drain any NACKs/ACKs that have come back and service NACKs.
        size_t got = 0;
        while (sock.recv_from(buf, sizeof(buf), &got, nullptr)) {
            MsgType type;
            if (!peek_msg_type(buf, got, &type)) continue;
            if (type == MsgType::Nack) {
                Nack nack;
                if (!decode_nack(buf, got, &nack)) continue;
                for (uint16_t i = 0; i < nack.count; ++i) {
                    const RegistrySlot *slot = registry.lookup(nack.missing[i]);
                    if (!slot) {
                        std::printf("NACK seq=%llu UNRECOVERABLE (evicted)\n",
                                    static_cast<unsigned long long>(nack.missing[i]));
                        continue;
                    }
                    BlockHeader hdr;
                    hdr.stream_id = kStream;
                    hdr.seq_no = slot->seq_no;
                    hdr.flags = kFlagRetransmission;
                    hdr.payload_len = slot->payload_len;
                    size_t n = encode_data_datagram(hdr, slot->send_time_ns, slot->payload,
                                                    buf, sizeof(buf));
                    if (n > 0) {
                        sock.send_to(buf, n, dst);
                        std::printf("retransmit seq=%llu\n",
                                    static_cast<unsigned long long>(slot->seq_no));
                    }
                }
            } else if (type == MsgType::Ack) {
                Ack ack;
                if (decode_ack(buf, got, &ack)) {
                    registry.confirm(ack.base_seq_no);
                }
            }
        }
    }

    // Keep servicing NACKs briefly so the last blocks / any drop can recover.
    uint64_t deadline = now_ns() + 500'000'000ull; // 500 ms
    while (now_ns() < deadline) {
        size_t got = 0;
        if (!sock.recv_from(buf, sizeof(buf), &got, nullptr)) {
            continue;
        }
        MsgType type;
        if (!peek_msg_type(buf, got, &type) || type != MsgType::Nack) continue;
        Nack nack;
        if (!decode_nack(buf, got, &nack)) continue;
        for (uint16_t i = 0; i < nack.count; ++i) {
            const RegistrySlot *slot = registry.lookup(nack.missing[i]);
            if (!slot) continue;
            BlockHeader hdr;
            hdr.stream_id = kStream;
            hdr.seq_no = slot->seq_no;
            hdr.flags = kFlagRetransmission;
            hdr.payload_len = slot->payload_len;
            size_t n = encode_data_datagram(hdr, slot->send_time_ns, slot->payload,
                                            buf, sizeof(buf));
            if (n > 0) {
                sock.send_to(buf, n, dst);
                std::printf("retransmit seq=%llu (late)\n",
                            static_cast<unsigned long long>(slot->seq_no));
            }
        }
    }

    std::printf("stage1_sender done\n");
    return 0;
}
