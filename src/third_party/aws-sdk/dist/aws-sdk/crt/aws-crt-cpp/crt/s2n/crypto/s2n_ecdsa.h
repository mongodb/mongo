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

#include <openssl/ecdsa.h>
#include <stdint.h>

#include "api/s2n.h"
#include "crypto/s2n_ecc_evp.h"
#include "crypto/s2n_hash.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_blob.h"

/* Forward declaration to avoid the circular dependency with s2n_pkey.h */
struct s2n_pkey;

struct s2n_ecdsa_key {
    /*
     * Starting in openssl_3, `EVP_PKEY_get1_EC_KEY` and `EVP_PKEY_get0_EC_KEY`
     * functions return a pre-cached copy of the underlying key. This means that any
     * mutations are not reflected back onto the underlying key.
     *
     * The `const` identifier is present to help ensure that the key is not mutated.
     * Usecases which require a non-const EC_KEY (some openssl functions), should
     * use `s2n_unsafe_ecdsa_get_non_const` while ensuring that the usage is safe.
     */
    const EC_KEY *ec_key;
};

typedef struct s2n_ecdsa_key s2n_ecdsa_public_key;
typedef struct s2n_ecdsa_key s2n_ecdsa_private_key;

S2N_RESULT s2n_ecdsa_pkey_init(struct s2n_pkey *pkey);
int s2n_ecdsa_pkey_matches_curve(const struct s2n_ecdsa_key *ecdsa_key, const struct s2n_ecc_named_curve *curve);

S2N_RESULT s2n_evp_pkey_to_ecdsa_public_key(s2n_ecdsa_public_key *ecdsa_key, EVP_PKEY *pkey);
S2N_RESULT s2n_evp_pkey_to_ecdsa_private_key(s2n_ecdsa_private_key *ecdsa_key, EVP_PKEY *pkey);
