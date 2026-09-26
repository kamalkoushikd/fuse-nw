// Fuse quickstart: receive a file.
//
// Start this BEFORE the sender — it must be bound before the sender's
// opening message arrives.
//
//   fuse_quickstart_recv <bind-address> <port> <out-file> [lanes] [pre-shared-key]
//
// <bind-address> is usually "0.0.0.0" (listen on every interface); give a
// specific address to restrict listening to one interface on a multi-homed
// host. The whole reliable, sharded, optionally-encrypted transfer is the
// single receive_file() call below.

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include <fuse/transfer.hpp>

#include "quickstart_common.hpp"

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <bind-address> <port> <out-file> [lanes] [pre-shared-key]\n",
                     argv[0]);
        return 2;
    }

    fuse::TransferConfig cfg;
    cfg.bind_address = argv[1];
    cfg.base_port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 4) cfg.lanes = static_cast<uint16_t>(std::atoi(argv[4]));
    if (argc > 5) cfg.pre_shared_key = argv[5];

    std::printf("listening on ports %u-%u (%u lanes)%s\n", cfg.base_port,
                cfg.base_port + cfg.lanes - 1, cfg.lanes,
                cfg.pre_shared_key.empty() ? "" : ", encrypted");

    // Ctrl+C cancels cleanly: the sender is told, and <out-file> is left
    // untouched; the partial copy in <out-file>.part lets a re-run resume.
    quickstart::attach(cfg, "received");

    fuse::TransferStats stats;
    const fuse::TransferStatus st = fuse::receive_file(cfg, argv[3], &stats);
    quickstart::finish_progress_line();
    if (st != fuse::TransferStatus::Ok) {
        std::fprintf(stderr, "receive failed: %s\n", fuse::to_string(st));
        const std::string part = std::string(argv[3]) + ".part";
        struct stat sb {};
        if (::stat(part.c_str(), &sb) == 0) {
            std::fprintf(stderr,
                         "partial copy kept in %s — run the same receive and send again to "
                         "resume (delete it to start over)\n",
                         part.c_str());
        }
        return quickstart::exit_code_for(st);
    }

    if (stats.resumed_bytes > 0) {
        std::printf("resumed: %.1f MiB were already here from an earlier run; verified\n",
                    static_cast<double>(stats.resumed_bytes) / (1024.0 * 1024.0));
    }
    std::printf("received %llu bytes in %.3f s (%.1f MB/s) -> %s\n",
                static_cast<unsigned long long>(stats.bytes), stats.seconds,
                stats.throughput_mb_per_s(), argv[3]);
    return 0;
}
