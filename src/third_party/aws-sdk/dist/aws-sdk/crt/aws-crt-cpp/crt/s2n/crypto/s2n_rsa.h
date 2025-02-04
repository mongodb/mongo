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

#include <openssl/rsa.h>
#include <stdint.h>

#include "api/s2n.h"
#include "crypto/s2n_hash.h"
#include "utils/s2n_blob.h"

/* Forward declaration to avoid the circular dependency with s2n_pkey.h */
struct s2n_pkey;

struct s2n_rsa_key {
    /*
     * Starting in openssl_3, `EVP_PKEY_get1_RSA` and `EVP_PKEY_get0_RSA` functions
     * return a pre-cached copy of the underlying key. This means that any mutations
     * are not reflected back onto the underlying key.
     *
     * The `const` identifier is present to help ensure that the key is not mutated.
     * Usecases which require a non-const RSA key (some openssl functions), should
     * use `s2n_unsafe_rsa_get_non_const` while ensuring that the usage is safe.
     */
    const RSA *rsa;
};

RSA *s2n_unsafe_rsa_get_non_const(const struct s2n_rsa_key *rsa_key);

typedef struct s2n_rsa_key s2n_rsa_public_key;
typedef struct s2n_rsa_key s2n_rsa_private_key;

S2N_RESULT s2n_rsa_pkey_init(struct s2n_pkey *pkey);

S2N_RESULT s2n_evp_pkey_to_rsa_public_key(s2n_rsa_public_key *rsa_key, EVP_PKEY *pkey);
S2N_RESULT s2n_evp_pkey_to_rsa_private_key(s2n_rsa_private_key *rsa_key, EVP_PKEY *pkey);
