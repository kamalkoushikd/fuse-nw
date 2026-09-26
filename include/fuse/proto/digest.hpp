#ifndef FUSE_PROTO_DIGEST_HPP
#define FUSE_PROTO_DIGEST_HPP

// Whole-file digest, used to verify a *resumed* transfer end to end: such a
// file is stitched together from two or more sessions, so a per-block check
// alone can't tell whether the bytes kept from an earlier session still
// belong to the file being sent now.
//
// SHA-256 when the crypto backend is built in. Without it, a
// non-cryptographic 256-bit fallback (four independently seeded FNV-1a
// lanes): it catches accidental mismatches — a stale .part file, a disk
// error — but not a deliberate forgery. digest_is_cryptographic() says
// which one this build uses.

#include <cstddef>
#include <cstdint>

namespace fuse::proto {

inline constexpr size_t kFileDigestLen = 32;

bool digest_is_cryptographic();

class FileDigest {
public:
    FileDigest();
    ~FileDigest();
    FileDigest(const FileDigest &) = delete;
    FileDigest &operator=(const FileDigest &) = delete;

    void update(const uint8_t *data, size_t len);
    // Writes the digest and leaves the object unusable.
    void finish(uint8_t out[kFileDigestLen]);

private:
    void *state_ = nullptr; // wolfSSL wc_Sha256 when available
    uint64_t fnv_[4] = {};  // fallback
};

// One-shot convenience.
void file_digest(const uint8_t *data, size_t len, uint8_t out[kFileDigestLen]);

// The digest a resumed transfer is verified with. A plain SHA-256 runs on
// one core (measured ~390 MB/s with this wolfSSL build — about 5 s per GB
// counting both ends), so instead the data is cut into 64 MiB segments,
// each segment is hashed on its own thread, and the result is the hash of
// the list of segment hashes plus the total length. Just as strong as one
// SHA-256 of the file, but it uses every core. Both ends compute this same
// construction, so it is only comparable with itself — not with sha256sum.
void segmented_digest(const uint8_t *data, uint64_t len, uint8_t out[kFileDigestLen]);
// The same digest over a file's first `len` bytes (read with pread).
// False if the file can't be read.
bool segmented_digest_fd(int fd, uint64_t len, uint8_t out[kFileDigestLen]);

} // namespace fuse::proto

#endif // FUSE_PROTO_DIGEST_HPP
