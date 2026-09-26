#include "fuse/proto/digest.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <functional>
#include <new>
#include <thread>
#include <vector>

#include "fuse/proto/wire.hpp"

#if FUSE_PROTO_WITH_DTLS
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/sha256.h>
#endif

namespace fuse::proto {

bool digest_is_cryptographic() { return FUSE_PROTO_WITH_DTLS != 0; }

#if FUSE_PROTO_WITH_DTLS

FileDigest::FileDigest() {
    auto *sha = new (std::nothrow) wc_Sha256{};
    if (sha != nullptr && wc_InitSha256(sha) != 0) {
        delete sha;
        sha = nullptr;
    }
    state_ = sha;
}

FileDigest::~FileDigest() {
    if (state_ != nullptr) {
        auto *sha = static_cast<wc_Sha256 *>(state_);
        wc_Sha256Free(sha);
        delete sha;
    }
}

void FileDigest::update(const uint8_t *data, size_t len) {
    if (state_ == nullptr || len == 0) return;
    // wolfSSL takes a 32-bit length; feed very large buffers in pieces.
    while (len > 0) {
        const word32 n = len > (1u << 30) ? (1u << 30) : static_cast<word32>(len);
        wc_Sha256Update(static_cast<wc_Sha256 *>(state_), data, n);
        data += n;
        len -= n;
    }
}

void FileDigest::finish(uint8_t out[kFileDigestLen]) {
    // A failed init leaves an all-zero digest, which can't match a real
    // SHA-256 by accident — verification then fails closed.
    std::memset(out, 0, kFileDigestLen);
    if (state_ != nullptr) wc_Sha256Final(static_cast<wc_Sha256 *>(state_), out);
}

#else

FileDigest::FileDigest() {
    for (int i = 0; i < 4; ++i) fnv_[i] = 1469598103934665603ull ^ (0x9E3779B97F4A7C15ull * (i + 1));
}

FileDigest::~FileDigest() = default;

void FileDigest::update(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        for (uint64_t &h : fnv_) {
            h ^= data[i];
            h *= 1099511628211ull;
        }
    }
}

void FileDigest::finish(uint8_t out[kFileDigestLen]) {
    for (int i = 0; i < 4; ++i) {
        for (int b = 0; b < 8; ++b) out[i * 8 + b] = static_cast<uint8_t>(fnv_[i] >> (56 - 8 * b));
    }
}

#endif

void file_digest(const uint8_t *data, size_t len, uint8_t out[kFileDigestLen]) {
    FileDigest d;
    d.update(data, len);
    d.finish(out);
}

namespace {

constexpr uint64_t kSegment = 64ull << 20;

// Hashes every segment with `hash_segment` on a few threads (handing out
// segments through one atomic counter), then hashes the segment hashes and
// the length together.
bool segmented(uint64_t len,
               const std::function<bool(uint64_t off, uint64_t n, uint8_t out[kFileDigestLen])>
                   &hash_segment,
               uint8_t out[kFileDigestLen]) {
    const uint64_t segments = std::max<uint64_t>(1, (len + kSegment - 1) / kSegment);
    std::vector<std::array<uint8_t, kFileDigestLen>> seg(segments);
    std::atomic<uint64_t> next{0};
    std::atomic<bool> ok{true};
    const auto work = [&] {
        for (uint64_t i; (i = next.fetch_add(1)) < segments;) {
            const uint64_t off = i * kSegment;
            const uint64_t n = std::min(kSegment, len - std::min(off, len));
            if (!hash_segment(off, n, seg[i].data())) ok.store(false);
        }
    };
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned nthreads = static_cast<unsigned>(std::min<uint64_t>({hw, segments, 16}));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nthreads; ++t) pool.emplace_back(work);
    work();
    for (auto &t : pool) t.join();

    FileDigest d;
    for (const auto &h : seg) d.update(h.data(), h.size());
    uint8_t len_be[8];
    put_u64(len_be, len);
    d.update(len_be, sizeof(len_be));
    d.finish(out);
    return ok.load();
}

} // namespace

void segmented_digest(const uint8_t *data, uint64_t len, uint8_t out[kFileDigestLen]) {
    segmented(
        len,
        [data](uint64_t off, uint64_t n, uint8_t h[kFileDigestLen]) {
            file_digest(data + off, static_cast<size_t>(n), h);
            return true;
        },
        out);
}

bool segmented_digest_fd(int fd, uint64_t len, uint8_t out[kFileDigestLen]) {
    return segmented(
        len,
        [fd](uint64_t off, uint64_t n, uint8_t h[kFileDigestLen]) {
            FileDigest d;
            std::vector<uint8_t> buf(std::min<uint64_t>(n, 4u << 20));
            for (uint64_t done = 0; done < n;) {
                const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), n - done));
                const ssize_t got = ::pread(fd, buf.data(), want, static_cast<off_t>(off + done));
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) return false;
                d.update(buf.data(), static_cast<size_t>(got));
                done += static_cast<uint64_t>(got);
            }
            d.finish(h);
            return true;
        },
        out);
}

} // namespace fuse::proto
