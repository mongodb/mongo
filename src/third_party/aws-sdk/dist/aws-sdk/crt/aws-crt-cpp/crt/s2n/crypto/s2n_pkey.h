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

#include "crypto/s2n_ecdsa.h"
#include "crypto/s2n_hash.h"
#include "crypto/s2n_rsa.h"
#include "crypto/s2n_signature.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

/* Public/Private Key Type */
typedef enum {
    S2N_PKEY_TYPE_UNKNOWN = -1,
    S2N_PKEY_TYPE_RSA = 0,
    S2N_PKEY_TYPE_ECDSA,
    S2N_PKEY_TYPE_RSA_PSS,
    S2N_PKEY_TYPE_SENTINEL
} s2n_pkey_type;

/* Structure that models a public or private key and type-specific operations */
struct s2n_pkey {
    /* Legacy OpenSSL APIs operate on specific keys, but the more recent
     * APIs all operate on EVP_PKEY. Let's store both for backwards compatibility. */
    union {
        struct s2n_rsa_key rsa_key;
        struct s2n_ecdsa_key ecdsa_key;
    } key;
    EVP_PKEY *pkey;

    S2N_RESULT (*size)(const struct s2n_pkey *key, uint32_t *size_out);
    int (*sign)(const struct s2n_pkey *priv_key, s2n_signature_algorithm sig_alg,
            struct s2n_hash_state *digest, struct s2n_blob *signature);
    int (*verify)(const struct s2n_pkey *pub_key, s2n_signature_algorithm sig_alg,
            struct s2n_hash_state *digest, struct s2n_blob *signature);
    int (*encrypt)(const struct s2n_pkey *key, struct s2n_blob *in, struct s2n_blob *out);
    int (*decrypt)(const struct s2n_pkey *key, struct s2n_blob *in, struct s2n_blob *out);
    int (*match)(const struct s2n_pkey *pub_key, const struct s2n_pkey *priv_key);
    int (*free)(struct s2n_pkey *key);
    int (*check_key)(const struct s2n_pkey *key);
};

int s2n_pkey_zero_init(struct s2n_pkey *pkey);
S2N_RESULT s2n_pkey_setup_for_type(struct s2n_pkey *pkey, s2n_pkey_type pkey_type);
int s2n_pkey_check_key_exists(const struct s2n_pkey *pkey);

S2N_RESULT s2n_pkey_size(const struct s2n_pkey *pkey, uint32_t *size_out);
int s2n_pkey_sign(const struct s2n_pkey *pkey, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, struct s2n_blob *signature);
int s2n_pkey_verify(const struct s2n_pkey *pkey, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, struct s2n_blob *signature);
int s2n_pkey_encrypt(const struct s2n_pkey *pkey, struct s2n_blob *in, struct s2n_blob *out);
int s2n_pkey_decrypt(const struct s2n_pkey *pkey, struct s2n_blob *in, struct s2n_blob *out);
int s2n_pkey_match(const struct s2n_pkey *pub_key, const struct s2n_pkey *priv_key);
int s2n_pkey_free(struct s2n_pkey *pkey);

S2N_RESULT s2n_asn1der_to_private_key(struct s2n_pkey *priv_key, struct s2n_blob *asn1der, int type_hint);
S2N_RESULT s2n_asn1der_to_public_key_and_type(struct s2n_pkey *pub_key, s2n_pkey_type *pkey_type, struct s2n_blob *asn1der);
S2N_RESULT s2n_pkey_get_type(EVP_PKEY *evp_pkey, s2n_pkey_type *pkey_type);
S2N_RESULT s2n_pkey_from_x509(X509 *cert, struct s2n_pkey *pub_key_out,
        s2n_pkey_type *pkey_type_out);
