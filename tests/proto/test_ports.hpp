#ifndef FUSE_TEST_PORTS_HPP
#define FUSE_TEST_PORTS_HPP

// Picks a block of consecutive free UDP ports on 127.0.0.1 for one test.
//
// ctest runs every test case in its own process, so a per-process counter
// starting at a fixed port gave every process the same ports and made
// `ctest -j` collide. The starting point is spread by pid, and each
// candidate block is probed by actually binding it before use.

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "fuse/proto/udp.hpp"

namespace fuse_test {

inline uint16_t free_port_block(uint16_t count) {
    constexpr uint32_t kFirst = 30000, kSpan = 20000, kStride = 64;
    static std::atomic<uint32_t> cursor{(static_cast<uint32_t>(getpid()) * 7919u) % kSpan};
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const uint32_t off = (cursor.fetch_add(kStride) % (kSpan - kStride));
        const uint16_t base = static_cast<uint16_t>(kFirst + off);
        std::vector<fuse::proto::UdpSocket> probe(count);
        bool ok = true;
        for (uint16_t i = 0; i < count && ok; ++i) {
            ok = probe[i].open("127.0.0.1", static_cast<uint16_t>(base + i));
        }
        if (ok) return base;
    }
    return 0;
}

} // namespace fuse_test

#endif // FUSE_TEST_PORTS_HPP
