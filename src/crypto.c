#include "fuse/crypto.h"

int fuse_crypto_available(void) {
    return FUSE_WITH_CRYPTO;
}

#if FUSE_WITH_CRYPTO

#include <string.h>

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/kdf.h>
#include <wolfssl/wolfcrypt/types.h>

/* fuse's analogue of QUIC's Initial salt (RFC 9001 Section 5.2): a
 * fixed, public constant that binds Initial-packet keys to this
 * protocol so that a middlebox speaking a different protocol version
 * can't casually decrypt or tamper with them. It carries no secrecy of
 * its own. Generated as the first 20 bytes of
 * SHA-256("fuse transport protocol v1 initial salt"). */
static const uint8_t FUSE_INITIAL_SALT[20] = {
    0xb6, 0x6e, 0x3e, 0xc7, 0x19, 0xbd, 0x0c, 0x9f, 0xa1, 0x38,
    0xc2, 0xf0, 0x07, 0x87, 0x91, 0x68, 0xca, 0x1e, 0xfb, 0xfe
};

static const char FUSE_PROTOCOL_LABEL[] = "fuse13 ";
#define FUSE_PROTOCOL_LABEL_LEN (sizeof(FUSE_PROTOCOL_LABEL) - 1)

static int map_digest(fuse_hash_algorithm alg, size_t *digest_len) {
    switch (alg) {
        case FUSE_HASH_SHA256:
            *digest_len = FUSE_SHA256_LEN;
            return WC_HASH_TYPE_SHA256;
        default:
            return -1;
    }
}

fuse_status fuse_crypto_hkdf_extract(fuse_hash_algorithm alg,
                                      const uint8_t *salt, size_t salt_len,
                                      const uint8_t *ikm, size_t ikm_len,
                                      uint8_t *prk_out, size_t prk_out_len) {
    size_t digest_len = 0;
    int digest = map_digest(alg, &digest_len);
    if (digest < 0 || !salt || !ikm || !prk_out || prk_out_len != digest_len) {
        return FUSE_ERR_INVALID_ARGUMENT;
    }

    /* wc_Tls13_HKDF_Extract takes a non-const `ikm`; copy into a
     * bounded local buffer rather than casting away the caller's
     * const-ness. Every current caller (connection IDs, secrets) is
     * well under this bound. */
    if (ikm_len > 256) {
        return FUSE_ERR_INVALID_ARGUMENT;
    }
    uint8_t ikm_copy[256];
    memcpy(ikm_copy, ikm, ikm_len);

    int rc = wc_Tls13_HKDF_Extract(prk_out, salt, (word32)salt_len,
                                    ikm_copy, (word32)ikm_len, digest);
    return rc == 0 ? FUSE_OK : FUSE_ERR_CRYPTO;
}

fuse_status fuse_crypto_hkdf_expand_label(fuse_hash_algorithm alg,
                                          const uint8_t *prk, size_t prk_len,
                                          const char *protocol, size_t protocol_len,
                                          const char *label, size_t label_len,
                                          const uint8_t *context, size_t context_len,
                                          uint8_t *out, size_t out_len) {
    size_t digest_len = 0;
    int digest = map_digest(alg, &digest_len);
    if (digest < 0 || !prk || !protocol || !label || !out || prk_len != digest_len) {
        return FUSE_ERR_INVALID_ARGUMENT;
    }
    if (!context && context_len != 0) {
        return FUSE_ERR_INVALID_ARGUMENT;
    }

    static const uint8_t empty_context[1] = {0};
    const uint8_t *ctx = context ? context : empty_context;

    int rc = wc_Tls13_HKDF_Expand_Label(out, (word32)out_len,
                                         prk, (word32)prk_len,
                                         (const byte *)protocol, (word32)protocol_len,
                                         (const byte *)label, (word32)label_len,
                                         ctx, (word32)context_len,
                                         digest);
    return rc == 0 ? FUSE_OK : FUSE_ERR_CRYPTO;
}

fuse_status fuse_crypto_derive_initial_secrets(const fuse_connection_id *dst_cid,
                                               uint8_t *client_secret_out, size_t client_secret_len,
                                               uint8_t *server_secret_out, size_t server_secret_len) {
    if (!dst_cid || !client_secret_out || !server_secret_out) {
        return FUSE_ERR_INVALID_ARGUMENT;
    }
    if (client_secret_len != FUSE_SHA256_LEN || server_secret_len != FUSE_SHA256_LEN) {
        return FUSE_ERR_INVALID_ARGUMENT;
    }

    uint8_t initial_secret[FUSE_SHA256_LEN];
    fuse_status status = fuse_crypto_hkdf_extract(FUSE_HASH_SHA256,
                                                   FUSE_INITIAL_SALT, sizeof(FUSE_INITIAL_SALT),
                                                   dst_cid->data, dst_cid->len,
                                                   initial_secret, sizeof(initial_secret));
    if (status != FUSE_OK) {
        return status;
    }

    status = fuse_crypto_hkdf_expand_label(FUSE_HASH_SHA256,
                                            initial_secret, sizeof(initial_secret),
                                            FUSE_PROTOCOL_LABEL, FUSE_PROTOCOL_LABEL_LEN,
                                            "client in", 9,
                                            NULL, 0,
                                            client_secret_out, client_secret_len);
    if (status != FUSE_OK) {
        return status;
    }

    return fuse_crypto_hkdf_expand_label(FUSE_HASH_SHA256,
                                          initial_secret, sizeof(initial_secret),
                                          FUSE_PROTOCOL_LABEL, FUSE_PROTOCOL_LABEL_LEN,
                                          "server in", 9,
                                          NULL, 0,
                                          server_secret_out, server_secret_len);
}

#else /* !FUSE_WITH_CRYPTO */

fuse_status fuse_crypto_hkdf_extract(fuse_hash_algorithm alg,
                                      const uint8_t *salt, size_t salt_len,
                                      const uint8_t *ikm, size_t ikm_len,
                                      uint8_t *prk_out, size_t prk_out_len) {
    (void)alg; (void)salt; (void)salt_len; (void)ikm; (void)ikm_len;
    (void)prk_out; (void)prk_out_len;
    return FUSE_ERR_NOT_SUPPORTED;
}

fuse_status fuse_crypto_hkdf_expand_label(fuse_hash_algorithm alg,
                                          const uint8_t *prk, size_t prk_len,
                                          const char *protocol, size_t protocol_len,
                                          const char *label, size_t label_len,
                                          const uint8_t *context, size_t context_len,
                                          uint8_t *out, size_t out_len) {
    (void)alg; (void)prk; (void)prk_len; (void)protocol; (void)protocol_len;
    (void)label; (void)label_len; (void)context; (void)context_len;
    (void)out; (void)out_len;
    return FUSE_ERR_NOT_SUPPORTED;
}

fuse_status fuse_crypto_derive_initial_secrets(const fuse_connection_id *dst_cid,
                                               uint8_t *client_secret_out, size_t client_secret_len,
                                               uint8_t *server_secret_out, size_t server_secret_len) {
    (void)dst_cid; (void)client_secret_out; (void)client_secret_len;
    (void)server_secret_out; (void)server_secret_len;
    return FUSE_ERR_NOT_SUPPORTED;
}

#endif /* FUSE_WITH_CRYPTO */
