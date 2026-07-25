// Fuse quickstart: send a file.
//
// Start the receiver first, then:
//
//   fuse_quickstart_send <host> <port> <in-file> [lanes] [pre-shared-key]
//
// The whole reliable, sharded, optionally-encrypted transfer is the single
// send_file() call below.

#include <cstdio>
#include <cstdlib>
#include <string>

#include <fuse/transfer.hpp>

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <host> <port> <in-file> [lanes] [pre-shared-key]\n", argv[0]);
        return 2;
    }

    fuse::TransferConfig cfg;
    cfg.host = argv[1];
    cfg.base_port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 4) cfg.lanes = static_cast<uint16_t>(std::atoi(argv[4]));
    if (argc > 5) cfg.pre_shared_key = argv[5];

    if (!cfg.pre_shared_key.empty() && !fuse::encryption_available()) {
        std::fprintf(stderr, "this build has no crypto backend; refusing to send in the clear\n");
        return 1;
    }

    fuse::TransferStats stats;
    const fuse::TransferStatus st = fuse::send_file(cfg, argv[3], &stats);
    if (st != fuse::TransferStatus::Ok) {
        std::fprintf(stderr, "send failed: %s\n", fuse::to_string(st));
        return 1;
    }

    std::printf("sent %llu bytes in %.3f s (%.1f MB/s), %llu retransmits, block=%u\n",
                static_cast<unsigned long long>(stats.bytes), stats.seconds,
                stats.throughput_mb_per_s(),
                static_cast<unsigned long long>(stats.retransmits), stats.final_block_size);
    return 0;
}
