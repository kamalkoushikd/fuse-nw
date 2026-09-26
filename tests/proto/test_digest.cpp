#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fuse/proto/digest.hpp"

using namespace fuse::proto;

namespace {

std::vector<uint8_t> bytes(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        v[i] = static_cast<uint8_t>(seed >> 24);
    }
    return v;
}

// Writes `data` to a temporary file and returns an fd open for reading.
int temp_fd(const std::vector<uint8_t> &data) {
    char path[] = "/tmp/fuse_digest_XXXXXX";
    const int fd = ::mkstemp(path);
    ::unlink(path);
    EXPECT_EQ(::write(fd, data.data(), data.size()), static_cast<ssize_t>(data.size()));
    return fd;
}

} // namespace

// The sender hashes its file in memory, the receiver hashes its .part from
// disk: both must get the same answer for every size, including right at
// and around the 64 MiB segment boundaries.
TEST(Digest, MemoryAndFileAgree) {
    constexpr size_t kSeg = 64u << 20;
    for (size_t n : {size_t{0}, size_t{1}, size_t{4096}, kSeg - 1, kSeg, kSeg + 1, 2 * kSeg + 12345}) {
        SCOPED_TRACE("size=" + std::to_string(n));
        const auto data = bytes(n, static_cast<uint32_t>(n));
        uint8_t mem[kFileDigestLen], file[kFileDigestLen];
        segmented_digest(data.data(), data.size(), mem);
        const int fd = temp_fd(data);
        ASSERT_TRUE(segmented_digest_fd(fd, data.size(), file));
        ::close(fd);
        EXPECT_EQ(0, std::memcmp(mem, file, kFileDigestLen));
    }
}

TEST(Digest, AnyChangeChangesTheDigest) {
    auto data = bytes((64u << 20) + 999, 7);
    uint8_t a[kFileDigestLen], b[kFileDigestLen], c[kFileDigestLen];
    segmented_digest(data.data(), data.size(), a);
    data[(64u << 20) + 5] ^= 0x01; // in the second segment
    segmented_digest(data.data(), data.size(), b);
    EXPECT_NE(0, std::memcmp(a, b, kFileDigestLen));
    data[(64u << 20) + 5] ^= 0x01;
    segmented_digest(data.data(), data.size() - 1, c); // same bytes, shorter
    EXPECT_NE(0, std::memcmp(a, c, kFileDigestLen));
}

TEST(Digest, ReadErrorIsReported) {
    uint8_t out[kFileDigestLen];
    const int fd = temp_fd(bytes(1000, 1));
    EXPECT_FALSE(segmented_digest_fd(fd, 5000, out)) << "asked for more than the file holds";
    ::close(fd);
}
