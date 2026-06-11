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

#include "crypto/s2n_hmac.h"
#include "tls/s2n_connection.h"
#include "utils/s2n_blob.h"

/* Enough to support TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384, 2*SHA384_DIGEST_LEN + 2*AES256_KEY_SIZE */
#define S2N_MAX_KEY_BLOCK_LEN 160

union p_hash_state {
    struct s2n_hmac_state s2n_hmac;
};

struct s2n_prf_working_space {
    union p_hash_state p_hash;
    uint8_t digest0[S2N_MAX_DIGEST_LEN];
    uint8_t digest1[S2N_MAX_DIGEST_LEN];
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

S2N_RESULT s2n_prf_new(struct s2n_connection *conn);
S2N_RESULT s2n_prf_wipe(struct s2n_connection *conn);
S2N_RESULT s2n_prf_free(struct s2n_connection *conn);

int s2n_prf_calculate_master_secret(struct s2n_connection *conn, struct s2n_blob *premaster_secret);
int s2n_prf_hybrid_master_secret(struct s2n_connection *conn, struct s2n_blob *premaster_secret);
S2N_RESULT s2n_prf_generate_key_material(struct s2n_connection *conn, struct s2n_key_material *key_material);
int s2n_prf_key_expansion(struct s2n_connection *conn);
int s2n_prf_server_finished(struct s2n_connection *conn);
int s2n_prf_client_finished(struct s2n_connection *conn);
