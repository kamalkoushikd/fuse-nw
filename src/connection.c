#include "fuse/connection.h"

#include <stdlib.h>

#if FUSE_WITH_CRYPTO
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/random.h>
#endif

struct fuse_connection {
    fuse_connection_state state;
    fuse_connection_id local_cid;
};

static void fill_random_cid(fuse_connection_id *cid) {
    cid->len = FUSE_CONNECTION_ID_MAX_LEN;

#if FUSE_WITH_CRYPTO
    WC_RNG rng;
    if (wc_InitRng(&rng) == 0) {
        wc_RNG_GenerateBlock(&rng, cid->data, cid->len);
        wc_FreeRng(&rng);
        return;
    }
#endif

    /* Fallback used when built without crypto: not cryptographically
     * secure, only enough to avoid an all-zero connection ID. */
    for (uint8_t i = 0; i < cid->len; i++) {
        cid->data[i] = (uint8_t)(rand() & 0xFF);
    }
}

fuse_connection *fuse_connection_new(void) {
    fuse_connection *conn = malloc(sizeof(*conn));
    if (!conn) {
        return NULL;
    }

    conn->state = FUSE_CONN_IDLE;
    fill_random_cid(&conn->local_cid);

    return conn;
}

void fuse_connection_free(fuse_connection *conn) {
    free(conn);
}

fuse_connection_state fuse_connection_get_state(const fuse_connection *conn) {
    return conn ? conn->state : FUSE_CONN_CLOSED;
}

const fuse_connection_id *fuse_connection_local_cid(const fuse_connection *conn) {
    return conn ? &conn->local_cid : NULL;
}
