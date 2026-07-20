#include "fuse/varint.h"

size_t fuse_varint_encoded_len(uint64_t value) {
    if (value <= 0x3F) {
        return 1;
    }
    if (value <= 0x3FFF) {
        return 2;
    }
    if (value <= 0x3FFFFFFF) {
        return 4;
    }
    if (value <= 0x3FFFFFFFFFFFFFFFULL) {
        return 8;
    }
    return 0;
}

size_t fuse_varint_encode(uint64_t value, uint8_t *out, size_t out_len) {
    if (!out) {
        return 0;
    }

    size_t len = fuse_varint_encoded_len(value);
    if (len == 0 || out_len < len) {
        return 0;
    }

    uint8_t prefix_bits;
    switch (len) {
        case 1: prefix_bits = 0; break;
        case 2: prefix_bits = 1; break;
        case 4: prefix_bits = 2; break;
        default: prefix_bits = 3; break;
    }

    for (size_t i = 0; i < len; i++) {
        out[len - 1 - i] = (uint8_t)(value & 0xFF);
        value >>= 8;
    }
    out[0] |= (uint8_t)(prefix_bits << 6);

    return len;
}

size_t fuse_varint_decode(const uint8_t *in, size_t in_len, uint64_t *value) {
    if (!in || !value || in_len < 1) {
        return 0;
    }

    uint8_t first = in[0];
    size_t len = (size_t)1 << (first >> 6);
    if (in_len < len) {
        return 0;
    }

    uint64_t v = first & 0x3F;
    for (size_t i = 1; i < len; i++) {
        v = (v << 8) | in[i];
    }

    *value = v;
    return len;
}
