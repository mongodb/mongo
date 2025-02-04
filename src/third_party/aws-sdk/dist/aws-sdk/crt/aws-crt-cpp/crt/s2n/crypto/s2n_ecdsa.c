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

#include "crypto/s2n_ecdsa.h"

#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/x509.h>

#include "crypto/s2n_ecc_evp.h"
#include "crypto/s2n_evp_signing.h"
#include "crypto/s2n_hash.h"
#include "crypto/s2n_openssl.h"
#include "crypto/s2n_pkey.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_compiler.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_random.h"
#include "utils/s2n_result.h"
#include "utils/s2n_safety.h"
#include "utils/s2n_safety_macros.h"

#define S2N_ECDSA_TYPE 0

EC_KEY *s2n_unsafe_ecdsa_get_non_const(const struct s2n_ecdsa_key *ecdsa_key)
{
    PTR_ENSURE_REF(ecdsa_key);

#ifdef S2N_DIAGNOSTICS_PUSH_SUPPORTED
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wcast-qual"
#endif
    EC_KEY *out_ec_key = (EC_KEY *) ecdsa_key->ec_key;
#ifdef S2N_DIAGNOSTICS_POP_SUPPORTED
    #pragma GCC diagnostic pop
#endif

    return out_ec_key;
}

S2N_RESULT s2n_ecdsa_der_signature_size(const struct s2n_pkey *pkey, uint32_t *size_out)
{
    RESULT_ENSURE_REF(pkey);
    RESULT_ENSURE_REF(size_out);

    const struct s2n_ecdsa_key *ecdsa_key = &pkey->key.ecdsa_key;
    RESULT_ENSURE_REF(ecdsa_key->ec_key);

    const int size = ECDSA_size(ecdsa_key->ec_key);
    RESULT_GUARD_POSIX(size);
    *size_out = size;

    return S2N_RESULT_OK;
}

int s2n_ecdsa_sign_digest(const struct s2n_pkey *priv, struct s2n_blob *digest, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(priv);
    POSIX_ENSURE_REF(digest);
    POSIX_ENSURE_REF(signature);

    const s2n_ecdsa_private_key *key = &priv->key.ecdsa_key;
    POSIX_ENSURE_REF(key->ec_key);

    unsigned int signature_size = signature->size;

    /* Safety: ECDSA_sign does not mutate the key */
    POSIX_GUARD_OSSL(ECDSA_sign(S2N_ECDSA_TYPE, digest->data, digest->size, signature->data, &signature_size,
                             s2n_unsafe_ecdsa_get_non_const(key)),
            S2N_ERR_SIGN);
    POSIX_ENSURE(signature_size <= signature->size, S2N_ERR_SIZE_MISMATCH);
    signature->size = signature_size;

    return S2N_SUCCESS;
}

static int s2n_ecdsa_sign(const struct s2n_pkey *priv, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(digest);
    sig_alg_check(sig_alg, S2N_SIGNATURE_ECDSA);

    uint8_t digest_length = 0;
    POSIX_GUARD(s2n_hash_digest_size(digest->alg, &digest_length));
    POSIX_ENSURE_LTE(digest_length, S2N_MAX_DIGEST_LEN);

    uint8_t digest_out[S2N_MAX_DIGEST_LEN] = { 0 };
    POSIX_GUARD(s2n_hash_digest(digest, digest_out, digest_length));

    struct s2n_blob digest_blob = { 0 };
    POSIX_GUARD(s2n_blob_init(&digest_blob, digest_out, digest_length));
    POSIX_GUARD(s2n_ecdsa_sign_digest(priv, &digest_blob, signature));

    POSIX_GUARD(s2n_hash_reset(digest));

    return S2N_SUCCESS;
}

static int s2n_ecdsa_verify(const struct s2n_pkey *pub, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, struct s2n_blob *signature)
{
    sig_alg_check(sig_alg, S2N_SIGNATURE_ECDSA);

    const s2n_ecdsa_public_key *key = &pub->key.ecdsa_key;
    POSIX_ENSURE_REF(key->ec_key);

    uint8_t digest_length = 0;
    POSIX_GUARD(s2n_hash_digest_size(digest->alg, &digest_length));
    POSIX_ENSURE_LTE(digest_length, S2N_MAX_DIGEST_LEN);

    uint8_t digest_out[S2N_MAX_DIGEST_LEN];
    POSIX_GUARD(s2n_hash_digest(digest, digest_out, digest_length));

    /* Safety: ECDSA_verify does not mutate the key */
    /* ECDSA_verify ignores the first parameter */
    POSIX_GUARD_OSSL(ECDSA_verify(0, digest_out, digest_length, signature->data, signature->size,
                             s2n_unsafe_ecdsa_get_non_const(key)),
            S2N_ERR_VERIFY_SIGNATURE);

    POSIX_GUARD(s2n_hash_reset(digest));

    return 0;
}

static int s2n_ecdsa_keys_match(const struct s2n_pkey *pub, const struct s2n_pkey *priv)
{
    uint8_t input[16] = { 1 };
    DEFER_CLEANUP(struct s2n_blob signature = { 0 }, s2n_free);
    DEFER_CLEANUP(struct s2n_hash_state state_in = { 0 }, s2n_hash_free);
    DEFER_CLEANUP(struct s2n_hash_state state_out = { 0 }, s2n_hash_free);

    /* s2n_hash_new only allocates memory when using high-level EVP hashes, currently restricted to FIPS mode. */
    POSIX_GUARD(s2n_hash_new(&state_in));
    POSIX_GUARD(s2n_hash_new(&state_out));

    POSIX_GUARD(s2n_hash_init(&state_in, S2N_HASH_SHA1));
    POSIX_GUARD(s2n_hash_init(&state_out, S2N_HASH_SHA1));
    POSIX_GUARD(s2n_hash_update(&state_in, input, sizeof(input)));
    POSIX_GUARD(s2n_hash_update(&state_out, input, sizeof(input)));

    uint32_t size = 0;
    POSIX_GUARD_RESULT(s2n_ecdsa_der_signature_size(priv, &size));
    POSIX_GUARD(s2n_alloc(&signature, size));

    POSIX_GUARD(s2n_ecdsa_sign(priv, S2N_SIGNATURE_ECDSA, &state_in, &signature));
    POSIX_GUARD(s2n_ecdsa_verify(pub, S2N_SIGNATURE_ECDSA, &state_out, &signature));

    return 0;
}

static int s2n_ecdsa_key_free(struct s2n_pkey *pkey)
{
    POSIX_ENSURE_REF(pkey);
    struct s2n_ecdsa_key *ecdsa_key = &pkey->key.ecdsa_key;
    if (ecdsa_key->ec_key == NULL) {
        return S2N_SUCCESS;
    }

    /* Safety: freeing the key owned by this object */
    EC_KEY_free(s2n_unsafe_ecdsa_get_non_const(ecdsa_key));
    ecdsa_key->ec_key = NULL;

    return S2N_SUCCESS;
}

static int s2n_ecdsa_check_key_exists(const struct s2n_pkey *pkey)
{
    const struct s2n_ecdsa_key *ecdsa_key = &pkey->key.ecdsa_key;
    POSIX_ENSURE_REF(ecdsa_key->ec_key);
    return 0;
}

S2N_RESULT s2n_evp_pkey_to_ecdsa_private_key(s2n_ecdsa_private_key *ecdsa_key, EVP_PKEY *evp_private_key)
{
    const EC_KEY *ec_key = EVP_PKEY_get1_EC_KEY(evp_private_key);
    RESULT_ENSURE(ec_key != NULL, S2N_ERR_DECODE_PRIVATE_KEY);

    ecdsa_key->ec_key = ec_key;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_evp_pkey_to_ecdsa_public_key(s2n_ecdsa_public_key *ecdsa_key, EVP_PKEY *evp_public_key)
{
    const EC_KEY *ec_key = EVP_PKEY_get1_EC_KEY(evp_public_key);
    RESULT_ENSURE(ec_key != NULL, S2N_ERR_DECODE_CERTIFICATE);

    ecdsa_key->ec_key = ec_key;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_ecdsa_pkey_init(struct s2n_pkey *pkey)
{
    pkey->size = &s2n_ecdsa_der_signature_size;
    pkey->sign = &s2n_ecdsa_sign;
    pkey->verify = &s2n_ecdsa_verify;
    pkey->encrypt = NULL; /* No function for encryption */
    pkey->decrypt = NULL; /* No function for decryption */
    pkey->match = &s2n_ecdsa_keys_match;
    pkey->free = &s2n_ecdsa_key_free;
    pkey->check_key = &s2n_ecdsa_check_key_exists;
    RESULT_GUARD(s2n_evp_signing_set_pkey_overrides(pkey));
    return S2N_RESULT_OK;
}

int s2n_ecdsa_pkey_matches_curve(const struct s2n_ecdsa_key *ecdsa_key, const struct s2n_ecc_named_curve *curve)
{
    POSIX_ENSURE_REF(ecdsa_key);
    POSIX_ENSURE_REF(ecdsa_key->ec_key);
    POSIX_ENSURE_REF(curve);

    const EC_KEY *key = ecdsa_key->ec_key;
    int curve_id = EC_GROUP_get_curve_name(EC_KEY_get0_group(key));
    POSIX_ENSURE_EQ(curve_id, curve->libcrypto_nid);

    return 0;
}
