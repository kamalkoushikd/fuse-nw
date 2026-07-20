#include "fuse/packet.h"

#include <string.h>

#include "fuse/varint.h"

size_t fuse_long_header_encode(const fuse_long_header *hdr, uint8_t *out, size_t out_len) {
    if (!hdr || !out) {
        return 0;
    }
    if (hdr->dst_cid.len > FUSE_CONNECTION_ID_MAX_LEN || hdr->src_cid.len > FUSE_CONNECTION_ID_MAX_LEN) {
        return 0;
    }

    size_t pn_len = fuse_varint_encoded_len(hdr->packet_number);
    if (pn_len == 0) {
        return 0;
    }

    size_t needed = 1 + 4 + 1 + hdr->dst_cid.len + 1 + hdr->src_cid.len + pn_len;
    if (out_len < needed) {
        return 0;
    }

    size_t off = 0;
    out[off++] = (uint8_t)hdr->type;

    out[off++] = (uint8_t)((hdr->version >> 24) & 0xFF);
    out[off++] = (uint8_t)((hdr->version >> 16) & 0xFF);
    out[off++] = (uint8_t)((hdr->version >> 8) & 0xFF);
    out[off++] = (uint8_t)(hdr->version & 0xFF);

    out[off++] = hdr->dst_cid.len;
    memcpy(out + off, hdr->dst_cid.data, hdr->dst_cid.len);
    off += hdr->dst_cid.len;

    out[off++] = hdr->src_cid.len;
    memcpy(out + off, hdr->src_cid.data, hdr->src_cid.len);
    off += hdr->src_cid.len;

    size_t written = fuse_varint_encode(hdr->packet_number, out + off, out_len - off);
    if (written == 0) {
        return 0;
    }
    off += written;

    return off;
}

size_t fuse_long_header_decode(const uint8_t *in, size_t in_len, fuse_long_header *hdr) {
    if (!in || !hdr) {
        return 0;
    }

    size_t off = 0;

    if (in_len < off + 1) {
        return 0;
    }
    uint8_t type = in[off++];
    if (type > FUSE_PACKET_SHORT) {
        return 0;
    }
    hdr->type = (fuse_packet_type)type;

    if (in_len < off + 4) {
        return 0;
    }
    hdr->version = ((uint32_t)in[off] << 24) | ((uint32_t)in[off + 1] << 16) |
                   ((uint32_t)in[off + 2] << 8) | (uint32_t)in[off + 3];
    off += 4;

    if (in_len < off + 1) {
        return 0;
    }
    uint8_t dcil = in[off++];
    if (dcil > FUSE_CONNECTION_ID_MAX_LEN || in_len < off + dcil) {
        return 0;
    }
    memcpy(hdr->dst_cid.data, in + off, dcil);
    hdr->dst_cid.len = dcil;
    off += dcil;

    if (in_len < off + 1) {
        return 0;
    }
    uint8_t scil = in[off++];
    if (scil > FUSE_CONNECTION_ID_MAX_LEN || in_len < off + scil) {
        return 0;
    }
    memcpy(hdr->src_cid.data, in + off, scil);
    hdr->src_cid.len = scil;
    off += scil;

    uint64_t pn = 0;
    size_t pn_len = fuse_varint_decode(in + off, in_len - off, &pn);
    if (pn_len == 0) {
        return 0;
    }
    hdr->packet_number = pn;
    off += pn_len;

    return off;
}
