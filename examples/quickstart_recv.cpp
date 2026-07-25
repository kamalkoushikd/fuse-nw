// Fuse quickstart: receive a file.
//
// Start this BEFORE the sender — it must be bound before the sender's
// opening message arrives.
//
//   fuse_quickstart_recv <port> <out-file> [lanes] [pre-shared-key]
//
// The whole reliable, sharded, optionally-encrypted transfer is the single
// receive_file() call below.

#include <cstdio>
#include <cstdlib>
#include <string>

#include <fuse/transfer.hpp>

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <port> <out-file> [lanes] [pre-shared-key]\n", argv[0]);
        return 2;
    }

    fuse::TransferConfig cfg;
    cfg.bind_address = "0.0.0.0";
    cfg.base_port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 3) cfg.lanes = static_cast<uint16_t>(std::atoi(argv[3]));
    if (argc > 4) cfg.pre_shared_key = argv[4];

    std::printf("listening on ports %u-%u (%u lanes)%s\n", cfg.base_port,
                cfg.base_port + cfg.lanes - 1, cfg.lanes,
                cfg.pre_shared_key.empty() ? "" : ", encrypted");

    fuse::TransferStats stats;
    const fuse::TransferStatus st = fuse::receive_file(cfg, argv[2], &stats);
    if (st != fuse::TransferStatus::Ok) {
        std::fprintf(stderr, "receive failed: %s\n", fuse::to_string(st));
        return 1;
    }

    std::printf("received %llu bytes in %.3f s (%.1f MB/s) -> %s\n",
                static_cast<unsigned long long>(stats.bytes), stats.seconds,
                stats.throughput_mb_per_s(), argv[2]);
    return 0;
}
