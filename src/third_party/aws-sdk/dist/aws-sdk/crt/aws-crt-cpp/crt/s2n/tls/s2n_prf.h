/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#pragma once

#include <stdint.h>

#include "crypto/s2n_hash.h"
#include "crypto/s2n_hmac.h"
#include "crypto/s2n_openssl.h"
#include "utils/s2n_blob.h"

/* Enough to support TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384, 2*SHA384_DIGEST_LEN + 2*AES256_KEY_SIZE */
#define S2N_MAX_KEY_BLOCK_LEN 160

#if defined(OPENSSL_IS_AWSLC)
    #define S2N_LIBCRYPTO_SUPPORTS_TLS_PRF 1
#else
    #define S2N_LIBCRYPTO_SUPPORTS_TLS_PRF 0
#endif

union p_hash_state {
    struct s2n_hmac_state s2n_hmac;
    struct s2n_evp_hmac_state evp_hmac;
};

struct s2n_prf_working_space {
    union p_hash_state p_hash;
    uint8_t digest0[S2N_MAX_DIGEST_LEN];
    uint8_t digest1[S2N_MAX_DIGEST_LEN];
};

/* The s2n p_hash implementation is abstracted to allow for separate implementations, using
 * either s2n's formally verified HMAC or OpenSSL's EVP HMAC, for use by the TLS PRF. */
struct s2n_p_hash_hmac {
    int (*alloc)(struct s2n_prf_working_space *ws);
    int (*init)(struct s2n_prf_working_space *ws, s2n_hmac_algorithm alg, struct s2n_blob *secret);
    int (*update)(struct s2n_prf_working_space *ws, const void *data, uint32_t size);
    int (*final)(struct s2n_prf_working_space *ws, void *digest, uint32_t size);
    int (*reset)(struct s2n_prf_working_space *ws);
    int (*cleanup)(struct s2n_prf_working_space *ws);
    int (*free)(struct s2n_prf_working_space *ws);
};

/* TLS key expansion results in an array of contiguous data which is then
 * interpreted as the MAC, KEY and IV for the client and server.
 *
 * The following is the memory layout of the key material:
 *
 *     [ CLIENT_MAC, SERVER_MAC, CLIENT_KEY, SERVER_KEY, CLIENT_IV, SERVER_IV ]
 */
struct s2n_key_material {
    /* key material data resulting from key expansion */
    uint8_t key_block[S2N_MAX_KEY_BLOCK_LEN];

    /* pointers into data representing specific key information */
    struct s2n_blob client_mac;
    struct s2n_blob server_mac;
    struct s2n_blob client_key;
    struct s2n_blob server_key;
    struct s2n_blob client_iv;
    struct s2n_blob server_iv;
};

S2N_RESULT s2n_key_material_init(struct s2n_key_material *key_material, struct s2n_connection *conn);

#include "tls/s2n_connection.h"

S2N_RESULT s2n_prf_new(struct s2n_connection *conn);
S2N_RESULT s2n_prf_wipe(struct s2n_connection *conn);
S2N_RESULT s2n_prf_free(struct s2n_connection *conn);

int s2n_prf(struct s2n_connection *conn, struct s2n_blob *secret, struct s2n_blob *label, struct s2n_blob *seed_a,
        struct s2n_blob *seed_b, struct s2n_blob *seed_c, struct s2n_blob *out);
int s2n_prf_calculate_master_secret(struct s2n_connection *conn, struct s2n_blob *premaster_secret);
int s2n_tls_prf_master_secret(struct s2n_connection *conn, struct s2n_blob *premaster_secret);
int s2n_hybrid_prf_master_secret(struct s2n_connection *conn, struct s2n_blob *premaster_secret);
S2N_RESULT s2n_tls_prf_extended_master_secret(struct s2n_connection *conn, struct s2n_blob *premaster_secret, struct s2n_blob *session_hash, struct s2n_blob *sha1_hash);
S2N_RESULT s2n_prf_get_digest_for_ems(struct s2n_connection *conn, struct s2n_blob *message, s2n_hash_algorithm hash_alg, struct s2n_blob *output);
S2N_RESULT s2n_prf_generate_key_material(struct s2n_connection *conn, struct s2n_key_material *key_material);
int s2n_prf_key_expansion(struct s2n_connection *conn);
int s2n_prf_server_finished(struct s2n_connection *conn);
int s2n_prf_client_finished(struct s2n_connection *conn);

bool s2n_libcrypto_supports_tls_prf();

S2N_RESULT s2n_custom_prf(struct s2n_connection *conn, struct s2n_blob *secret, struct s2n_blob *label,
        struct s2n_blob *seed_a, struct s2n_blob *seed_b, struct s2n_blob *seed_c, struct s2n_blob *out);
S2N_RESULT s2n_libcrypto_prf(struct s2n_connection *conn, struct s2n_blob *secret, struct s2n_blob *label,
        struct s2n_blob *seed_a, struct s2n_blob *seed_b, struct s2n_blob *seed_c, struct s2n_blob *out);
