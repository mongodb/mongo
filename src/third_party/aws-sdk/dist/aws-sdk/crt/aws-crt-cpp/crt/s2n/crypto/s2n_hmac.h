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
/* this file is patched by sidetrail, clang-format invalidates patches */
/* clang-format off */

#pragma once

#include <stdint.h>

#include "crypto/s2n_hash.h"

typedef enum {
    S2N_HMAC_NONE,
    S2N_HMAC_MD5,
    S2N_HMAC_SHA1,
    S2N_HMAC_SHA224,
    S2N_HMAC_SHA256,
    S2N_HMAC_SHA384,
    S2N_HMAC_SHA512,
    S2N_HMAC_SSLv3_MD5,
    S2N_HMAC_SSLv3_SHA1
} s2n_hmac_algorithm;

struct s2n_hmac_state {
    s2n_hmac_algorithm alg;

    uint16_t hash_block_size;
    uint32_t currently_in_hash_block;
    uint16_t xor_pad_size;
    uint8_t digest_size;

    struct s2n_hash_state inner;
    struct s2n_hash_state inner_just_key;
    struct s2n_hash_state outer;
    struct s2n_hash_state outer_just_key;

    /* key needs to be as large as the biggest block size */
    uint8_t xor_pad[128];

    /* For storing the inner digest */
    uint8_t digest_pad[SHA512_DIGEST_LENGTH];
};

struct s2n_hmac_evp_backup {
    struct s2n_hash_evp_digest inner;
    struct s2n_hash_evp_digest inner_just_key;
    struct s2n_hash_evp_digest outer;
    struct s2n_hash_evp_digest outer_just_key;
};

int s2n_hmac_digest_size(s2n_hmac_algorithm alg, uint8_t *out);
bool s2n_hmac_is_available(s2n_hmac_algorithm alg);
int s2n_hmac_hash_alg(s2n_hmac_algorithm hmac_alg, s2n_hash_algorithm *out);
int s2n_hash_hmac_alg(s2n_hash_algorithm hash_alg, s2n_hmac_algorithm *out);

int s2n_hmac_new(struct s2n_hmac_state *state);
S2N_RESULT s2n_hmac_state_validate(struct s2n_hmac_state *state);
int s2n_hmac_init(struct s2n_hmac_state *state, s2n_hmac_algorithm alg, const void *key, uint32_t klen);
int s2n_hmac_update(struct s2n_hmac_state *state, const void *in, uint32_t size);
int s2n_hmac_digest(struct s2n_hmac_state *state, void *out, uint32_t size);
int s2n_hmac_digest_two_compression_rounds(struct s2n_hmac_state *state, void *out, uint32_t size);
int s2n_hmac_digest_verify(const void *a, const void *b, uint32_t len);
int s2n_hmac_free(struct s2n_hmac_state *state);
int s2n_hmac_reset(struct s2n_hmac_state *state);
int s2n_hmac_copy(struct s2n_hmac_state *to, struct s2n_hmac_state *from);
int s2n_hmac_save_evp_hash_state(struct s2n_hmac_evp_backup* backup, struct s2n_hmac_state* hmac);
int s2n_hmac_restore_evp_hash_state(struct s2n_hmac_evp_backup* backup, struct s2n_hmac_state* hmac);

S2N_RESULT s2n_hmac_md_from_alg(s2n_hmac_algorithm alg, const EVP_MD **md);
