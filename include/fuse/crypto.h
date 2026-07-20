#ifndef FUSE_CRYPTO_H
#define FUSE_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "fuse/export.h"
#include "fuse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Built with FUSE_WITH_CRYPTO=1 unless the project was configured with
 * -DFUSE_WITH_CRYPTO=OFF, in which case every function below returns
 * FUSE_ERR_NOT_SUPPORTED. Check this macro to detect the build's
 * capability at compile time; see also fuse_crypto_available() for a
 * runtime check against the linked library. */
#ifndef FUSE_WITH_CRYPTO
#define FUSE_WITH_CRYPTO 0
#endif

typedef enum fuse_hash_algorithm {
    FUSE_HASH_SHA256 = 0
} fuse_hash_algorithm;

#define FUSE_SHA256_LEN 32

FUSE_EXPORT int fuse_crypto_available(void);

/* HKDF-Extract (RFC 5869): condenses `ikm` (keyed by `salt`) into a
 * pseudorandom key of the hash's digest length. `prk_out_len` must
 * equal the digest length for `alg` (32 for FUSE_HASH_SHA256). */
FUSE_EXPORT fuse_status fuse_crypto_hkdf_extract(fuse_hash_algorithm alg,
                                                  const uint8_t *salt, size_t salt_len,
                                                  const uint8_t *ikm, size_t ikm_len,
                                                  uint8_t *prk_out, size_t prk_out_len);

/* HKDF-Expand-Label, as defined by TLS 1.3 (RFC 8446, Section 7.1) and
 * reused by QUIC (RFC 9001, Section 5.1) to turn a secret into
 * purpose-specific key material. The caller supplies both the
 * "protocol" prefix (e.g. "tls13 ") and the label that follows it, so
 * this same primitive can reproduce published TLS 1.3/QUIC test
 * vectors as well as derive fuse's own labeled secrets. */
FUSE_EXPORT fuse_status fuse_crypto_hkdf_expand_label(fuse_hash_algorithm alg,
                                                       const uint8_t *prk, size_t prk_len,
                                                       const char *protocol, size_t protocol_len,
                                                       const char *label, size_t label_len,
                                                       const uint8_t *context, size_t context_len,
                                                       uint8_t *out, size_t out_len);

/* Derives the pair of "initial secrets" fuse uses to protect the
 * handshake's first flight, following the same construction as QUIC's
 * Initial secrets (RFC 9001 Section 5.2): HKDF-Extract the
 * destination connection ID under a fixed, protocol-specific salt,
 * then HKDF-Expand-Label it separately for each direction. Deriving
 * these from a value visible on the wire (the connection ID) - rather
 * than from a pre-shared key - means the Initial keys only obscure the
 * handshake from passive on-path observers; they authenticate nothing
 * on their own. Actual packet protection (AEAD key/iv/hp derivation
 * and encryption) is not yet implemented; see docs/ROADMAP.md.
 *
 * `client_secret_out` and `server_secret_out` must each be at least
 * FUSE_SHA256_LEN bytes. */
FUSE_EXPORT fuse_status fuse_crypto_derive_initial_secrets(const fuse_connection_id *dst_cid,
                                                            uint8_t *client_secret_out, size_t client_secret_len,
                                                            uint8_t *server_secret_out, size_t server_secret_len);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_CRYPTO_H */
