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

#include <openssl/evp.h>

#include "crypto/s2n_hash.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

#define S2N_DRBG_BLOCK_SIZE    16
#define S2N_DRBG_MAX_KEY_SIZE  32
#define S2N_DRBG_MAX_SEED_SIZE (S2N_DRBG_BLOCK_SIZE + S2N_DRBG_MAX_KEY_SIZE)

/* The maximum size of any one request: from NIST SP800-90A 10.2.1 Table 3 */
#define S2N_DRBG_GENERATE_LIMIT 8192

struct s2n_drbg {
    /* Track how many bytes have been used */
    uint64_t bytes_used;

    EVP_CIPHER_CTX *ctx;

    /* The current DRBG 'value' */
    uint8_t v[S2N_DRBG_BLOCK_SIZE];

    /* Used only by the unit tests: how many times has entropy been mixed in */
    uint64_t mixes;
};

/*
 * S2N_AES_128_CTR_NO_DF_PR is a deterministic random bit generator using AES 128 in counter mode (AES_128_CTR). It does not
 * use a derivation function (NO_DF) on the seed but does have prediction resistance (PR).
 *
 * S2N_AES_256_CTR_NO_DF_PR is a deterministic random bit generator using AES 256 in counter mode (AES_128_CTR). It does not
 * use a derivation function on the seed but does have prediction resistance.
 */
typedef enum {
    S2N_AES_128_CTR_NO_DF_PR,
    S2N_AES_256_CTR_NO_DF_PR
} s2n_drbg_mode;

/* Per NIST SP 800-90C 6.3
 *
 * s2n's DRBG uses prediction resistance and does not support the
 * additional_input parameter (which per 800-90C may be zero).
 *
 * The security strength provided by s2n's DRBG is either 128 or 256 bits
 * depending on the s2n_drbg_mode passed in.
 */
S2N_RESULT s2n_drbg_instantiate(struct s2n_drbg *drbg, struct s2n_blob *personalization_string, const s2n_drbg_mode mode);
S2N_RESULT s2n_drbg_generate(struct s2n_drbg *drbg, struct s2n_blob *returned_bits);
S2N_RESULT s2n_drbg_wipe(struct s2n_drbg *drbg);
S2N_RESULT s2n_drbg_bytes_used(struct s2n_drbg *drbg, uint64_t *bytes_used);
/* Use for testing only */
S2N_RESULT s2n_ignore_prediction_resistance_for_testing(bool true_or_false);
