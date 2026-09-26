#include "fuse/proto/control.hpp"

#include <cstring>

namespace fuse::proto {

namespace {

size_t put_outer(uint8_t *out, MsgType type) {
    size_t off = 0;
    off += put_u8(out + off, kProtocolVersion);
    off += put_u8(out + off, static_cast<uint8_t>(type));
    return off;
}

// Validates the outer header and returns the offset past it, or 0 on a
// version/type/length mismatch. (0 is unambiguous here: a valid outer
// header is always kOuterHeaderSize bytes, never 0.)
size_t check_outer(const uint8_t *in, size_t in_len, MsgType expected) {
    if (in_len < kOuterHeaderSize) {
        return 0;
    }
    if (in[0] != kProtocolVersion) {
        return 0;
    }
    if (static_cast<MsgType>(in[1]) != expected) {
        return 0;
    }
    return kOuterHeaderSize;
}

} // namespace

size_t encode_heartbeat(const Heartbeat &hb, uint8_t *out, size_t out_cap) {
    const size_t total = kOuterHeaderSize + 2 + 8;
    if (out_cap < total) {
        return 0;
    }
    size_t off = put_outer(out, MsgType::Heartbeat);
    off += put_u16(out + off, hb.stream_id);
    off += put_u64(out + off, hb.highest_seq_no);
    return off;
}

bool decode_heartbeat(const uint8_t *in, size_t in_len, Heartbeat *hb) {
    size_t off = check_outer(in, in_len, MsgType::Heartbeat);
    if (off == 0 || in_len < kOuterHeaderSize + 2 + 8) {
        return false;
    }
    off += get_u16(in + off, &hb->stream_id);
    off += get_u64(in + off, &hb->highest_seq_no);
    return true;
}

size_t encode_stream_start(const StreamStart &ss, uint8_t *out, size_t out_cap) {
    const size_t total = kOuterHeaderSize + 2 + 8 + 2 + 8 + 16 + 8 + 8 + 8 + 8;
    if (out_cap < total) {
        return 0;
    }
    size_t off = put_outer(out, MsgType::StreamStart);
    off += put_u16(out + off, ss.stream_id);
    off += put_u64(out + off, ss.total_blocks);
    off += put_u16(out + off, ss.block_size);
    off += put_u64(out + off, ss.total_bytes);
    std::memcpy(out + off, ss.session_salt, sizeof(ss.session_salt));
    off += sizeof(ss.session_salt);
    off += put_u64(out + off, ss.nonce);
    off += put_u64(out + off, ss.stream_base_offset);
    off += put_u64(out + off, ss.file_total_bytes);
    off += put_u64(out + off, ss.file_id);
    return off;
}

bool decode_stream_start(const uint8_t *in, size_t in_len, StreamStart *ss) {
    size_t off = check_outer(in, in_len, MsgType::StreamStart);
    if (off == 0 || in_len < kOuterHeaderSize + 2 + 8 + 2 + 8 + 16 + 8 + 8 + 8 + 8) {
        return false;
    }
    off += get_u16(in + off, &ss->stream_id);
    off += get_u64(in + off, &ss->total_blocks);
    off += get_u16(in + off, &ss->block_size);
    off += get_u64(in + off, &ss->total_bytes);
    std::memcpy(ss->session_salt, in + off, sizeof(ss->session_salt));
    off += sizeof(ss->session_salt);
    off += get_u64(in + off, &ss->nonce);
    off += get_u64(in + off, &ss->stream_base_offset);
    off += get_u64(in + off, &ss->file_total_bytes);
    off += get_u64(in + off, &ss->file_id);
    return true;
}

size_t encode_ack(const Ack &ack, uint8_t *out, size_t out_cap) {
    const size_t total = kOuterHeaderSize + 2 + 8 + kMaskWords * 8 + 8 + 8;
    if (out_cap < total) {
        return 0;
    }
    size_t off = put_outer(out, MsgType::Ack);
    off += put_u16(out + off, ack.stream_id);
    off += put_u64(out + off, ack.base_seq_no);
    for (size_t i = 0; i < kMaskWords; ++i) {
        off += put_u64(out + off, ack.received_bitmask[i]);
    }
    off += put_u64(out + off, ack.echoed_send_time);
    off += put_u64(out + off, ack.nonce);
    return off;
}

bool decode_ack(const uint8_t *in, size_t in_len, Ack *ack) {
    size_t off = check_outer(in, in_len, MsgType::Ack);
    if (off == 0 || in_len < kOuterHeaderSize + 2 + 8 + kMaskWords * 8 + 8 + 8) {
        return false;
    }
    off += get_u16(in + off, &ack->stream_id);
    off += get_u64(in + off, &ack->base_seq_no);
    for (size_t i = 0; i < kMaskWords; ++i) {
        off += get_u64(in + off, &ack->received_bitmask[i]);
    }
    off += get_u64(in + off, &ack->echoed_send_time);
    off += get_u64(in + off, &ack->nonce);
    return true;
}

size_t encode_nack(const Nack &nack, uint8_t *out, size_t out_cap) {
    if (nack.count > kMaxWindow) {
        return 0;
    }
    const size_t total = kOuterHeaderSize + 2 + 2 + static_cast<size_t>(nack.count) * 8;
    if (out_cap < total) {
        return 0;
    }
    size_t off = put_outer(out, MsgType::Nack);
    off += put_u16(out + off, nack.stream_id);
    off += put_u16(out + off, nack.count);
    for (uint16_t i = 0; i < nack.count; ++i) {
        off += put_u64(out + off, nack.missing[i]);
    }
    return off;
}

bool decode_nack(const uint8_t *in, size_t in_len, Nack *nack) {
    size_t off = check_outer(in, in_len, MsgType::Nack);
    if (off == 0 || in_len < kOuterHeaderSize + 2 + 2) {
        return false;
    }
    off += get_u16(in + off, &nack->stream_id);
    off += get_u16(in + off, &nack->count);
    if (nack->count > kMaxWindow) {
        return false;
    }
    if (in_len < kOuterHeaderSize + 2 + 2 + static_cast<size_t>(nack->count) * 8) {
        return false;
    }
    for (uint16_t i = 0; i < nack->count; ++i) {
        off += get_u64(in + off, &nack->missing[i]);
    }
    return true;
}

size_t encode_stream_close(const StreamClose &sc, uint8_t *out, size_t out_cap) {
    const size_t total = kOuterHeaderSize + 2 + 8 + 1;
    if (out_cap < total) {
        return 0;
    }
    size_t off = put_outer(out, MsgType::StreamClose);
    off += put_u16(out + off, sc.stream_id);
    off += put_u64(out + off, sc.nonce);
    off += put_u8(out + off, sc.reason);
    return off;
}

bool decode_stream_close(const uint8_t *in, size_t in_len, StreamClose *sc) {
    size_t off = check_outer(in, in_len, MsgType::StreamClose);
    if (off == 0 || in_len < kOuterHeaderSize + 2 + 8 + 1) {
        return false;
    }
    off += get_u16(in + off, &sc->stream_id);
    off += get_u64(in + off, &sc->nonce);
    off += get_u8(in + off, &sc->reason);
    return true;
}

namespace {
constexpr size_t kResumeRangesFixed = kOuterHeaderSize + 2 + 8 + 1 + 2 + 2 + 2;
}

size_t encode_resume_ranges(const ResumeRanges &rr, uint8_t *out, size_t out_cap) {
    if (rr.count > kMaxResumeRangesPerMsg || rr.parts == 0 || rr.part >= rr.parts) {
        return 0;
    }
    const size_t total = kResumeRangesFixed + static_cast<size_t>(rr.count) * 16;
    if (out_cap < total) {
        return 0;
    }
    size_t off = put_outer(out, MsgType::ResumeRanges);
    off += put_u16(out + off, rr.stream_id);
    off += put_u64(out + off, rr.nonce);
    off += put_u8(out + off, rr.flags);
    off += put_u16(out + off, rr.part);
    off += put_u16(out + off, rr.parts);
    off += put_u16(out + off, rr.count);
    for (uint16_t i = 0; i < rr.count; ++i) {
        off += put_u64(out + off, rr.ranges[i].offset);
        off += put_u64(out + off, rr.ranges[i].length);
    }
    return off;
}

bool decode_resume_ranges(const uint8_t *in, size_t in_len, ResumeRanges *rr) {
    size_t off = check_outer(in, in_len, MsgType::ResumeRanges);
    if (off == 0 || in_len < kResumeRangesFixed) {
        return false;
    }
    off += get_u16(in + off, &rr->stream_id);
    off += get_u64(in + off, &rr->nonce);
    off += get_u8(in + off, &rr->flags);
    off += get_u16(in + off, &rr->part);
    off += get_u16(in + off, &rr->parts);
    off += get_u16(in + off, &rr->count);
    if (rr->count > kMaxResumeRangesPerMsg || rr->parts == 0 || rr->part >= rr->parts ||
        in_len < kResumeRangesFixed + static_cast<size_t>(rr->count) * 16) {
        return false;
    }
    for (uint16_t i = 0; i < rr->count; ++i) {
        off += get_u64(in + off, &rr->ranges[i].offset);
        off += get_u64(in + off, &rr->ranges[i].length);
    }
    return true;
}

size_t encode_digest(const DigestMessage &dm, uint8_t *out, size_t out_cap) {
    const size_t total = kOuterHeaderSize + 2 + 8 + kDigestLen;
    if (out_cap < total) {
        return 0;
    }
    size_t off = put_outer(out, MsgType::FileDigest);
    off += put_u16(out + off, dm.stream_id);
    off += put_u64(out + off, dm.nonce);
    std::memcpy(out + off, dm.digest, kDigestLen);
    return off + kDigestLen;
}

bool decode_digest(const uint8_t *in, size_t in_len, DigestMessage *dm) {
    size_t off = check_outer(in, in_len, MsgType::FileDigest);
    if (off == 0 || in_len < kOuterHeaderSize + 2 + 8 + kDigestLen) {
        return false;
    }
    off += get_u16(in + off, &dm->stream_id);
    off += get_u64(in + off, &dm->nonce);
    std::memcpy(dm->digest, in + off, kDigestLen);
    return true;
}

} // namespace fuse::proto
