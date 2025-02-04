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

#include "crypto/s2n_rsa.h"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <stdint.h>

#include "crypto/s2n_drbg.h"
#include "crypto/s2n_evp_signing.h"
#include "crypto/s2n_hash.h"
#include "crypto/s2n_pkey.h"
#include "crypto/s2n_rsa_signing.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_compiler.h"
#include "utils/s2n_random.h"
#include "utils/s2n_result.h"
#include "utils/s2n_safety.h"

RSA *s2n_unsafe_rsa_get_non_const(const struct s2n_rsa_key *rsa_key)
{
    PTR_ENSURE_REF(rsa_key);

#ifdef S2N_DIAGNOSTICS_PUSH_SUPPORTED
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wcast-qual"
#endif
    RSA *out_rsa_key = (RSA *) rsa_key->rsa;
#ifdef S2N_DIAGNOSTICS_POP_SUPPORTED
    #pragma GCC diagnostic pop
#endif

    return out_rsa_key;
}

static S2N_RESULT s2n_rsa_modulus_check(const RSA *rsa)
{
/* RSA was made opaque starting in Openssl 1.1.0 */
#if S2N_OPENSSL_VERSION_AT_LEAST(1, 1, 0)
    const BIGNUM *n = NULL;
    /* RSA still owns the memory for n */
    RSA_get0_key(rsa, &n, NULL, NULL);
    RESULT_ENSURE_REF(n);
#else
    RESULT_ENSURE_REF(rsa->n);
#endif
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_rsa_encrypted_size(const struct s2n_pkey *pkey, uint32_t *size_out)
{
    RESULT_ENSURE_REF(pkey);
    RESULT_ENSURE_REF(size_out);

    const struct s2n_rsa_key *rsa_key = &pkey->key.rsa_key;
    RESULT_ENSURE_REF(rsa_key->rsa);

    RESULT_GUARD(s2n_rsa_modulus_check(rsa_key->rsa));

    const int size = RSA_size(rsa_key->rsa);
    RESULT_GUARD_POSIX(size);
    *size_out = size;

    return S2N_RESULT_OK;
}

static int s2n_rsa_sign(const struct s2n_pkey *priv, s2n_signature_algorithm sig_alg, struct s2n_hash_state *digest,
        struct s2n_blob *signature)
{
    switch (sig_alg) {
        case S2N_SIGNATURE_RSA:
            return s2n_rsa_pkcs1v15_sign(priv, digest, signature);
        case S2N_SIGNATURE_RSA_PSS_RSAE:
            return s2n_rsa_pss_sign(priv, digest, signature);
        default:
            POSIX_BAIL(S2N_ERR_INVALID_SIGNATURE_ALGORITHM);
    }

    return S2N_SUCCESS;
}

static int s2n_rsa_verify(const struct s2n_pkey *pub, s2n_signature_algorithm sig_alg, struct s2n_hash_state *digest,
        struct s2n_blob *signature)
{
    switch (sig_alg) {
        case S2N_SIGNATURE_RSA:
            return s2n_rsa_pkcs1v15_verify(pub, digest, signature);
        case S2N_SIGNATURE_RSA_PSS_RSAE:
            return s2n_rsa_pss_verify(pub, digest, signature);
        default:
            POSIX_BAIL(S2N_ERR_INVALID_SIGNATURE_ALGORITHM);
    }

    return S2N_SUCCESS;
}

static int s2n_rsa_encrypt(const struct s2n_pkey *pub, struct s2n_blob *in, struct s2n_blob *out)
{
    uint32_t size = 0;
    POSIX_GUARD_RESULT(s2n_rsa_encrypted_size(pub, &size));
    S2N_ERROR_IF(out->size < size, S2N_ERR_NOMEM);

    const s2n_rsa_public_key *pub_key = &pub->key.rsa_key;

    /* Safety: RSA_public_encrypt does not mutate the key */
    int r = RSA_public_encrypt(in->size, (unsigned char *) in->data, (unsigned char *) out->data,
            s2n_unsafe_rsa_get_non_const(pub_key), RSA_PKCS1_PADDING);
    POSIX_ENSURE(r >= 0, S2N_ERR_ENCRYPT);
    POSIX_ENSURE((int64_t) r == (int64_t) out->size, S2N_ERR_SIZE_MISMATCH);

    return 0;
}

static int s2n_rsa_decrypt(const struct s2n_pkey *priv, struct s2n_blob *in, struct s2n_blob *out)
{
    unsigned char intermediate[4096];
    uint32_t expected_size = 0;

    POSIX_GUARD_RESULT(s2n_rsa_encrypted_size(priv, &expected_size));

    S2N_ERROR_IF(expected_size > sizeof(intermediate), S2N_ERR_NOMEM);
    S2N_ERROR_IF(out->size > sizeof(intermediate), S2N_ERR_NOMEM);

    POSIX_GUARD_RESULT(s2n_get_public_random_data(out));

    const s2n_rsa_private_key *priv_key = &priv->key.rsa_key;

    /* Safety: RSA_private_decrypt does not mutate the key */
    int r = RSA_private_decrypt(in->size, (unsigned char *) in->data, intermediate,
            s2n_unsafe_rsa_get_non_const(priv_key), RSA_NO_PADDING);
    POSIX_ENSURE(r >= 0, S2N_ERR_DECRYPT);
    POSIX_ENSURE((int64_t) r == (int64_t) expected_size, S2N_ERR_SIZE_MISMATCH);

    s2n_constant_time_pkcs1_unpad_or_dont(out->data, intermediate, r, out->size);

    return 0;
}

static int s2n_rsa_keys_match(const struct s2n_pkey *pub, const struct s2n_pkey *priv)
{
    uint8_t plain_inpad[36] = { 1 }, plain_outpad[36] = { 0 }, encpad[8192];
    struct s2n_blob plain_in = { 0 }, plain_out = { 0 }, enc = { 0 };

    plain_in.data = plain_inpad;
    plain_in.size = sizeof(plain_inpad);

    enc.data = encpad;
    POSIX_GUARD_RESULT(s2n_rsa_encrypted_size(pub, &enc.size));
    POSIX_ENSURE_LTE(enc.size, sizeof(encpad));
    POSIX_GUARD(s2n_rsa_encrypt(pub, &plain_in, &enc));

    plain_out.data = plain_outpad;
    plain_out.size = sizeof(plain_outpad);
    POSIX_GUARD(s2n_rsa_decrypt(priv, &enc, &plain_out));

    POSIX_ENSURE(s2n_constant_time_equals(plain_in.data, plain_out.data, plain_in.size), S2N_ERR_KEY_MISMATCH);

    return 0;
}

static int s2n_rsa_key_free(struct s2n_pkey *pkey)
{
    POSIX_ENSURE_REF(pkey);
    struct s2n_rsa_key *rsa_key = &pkey->key.rsa_key;
    if (rsa_key->rsa == NULL) {
        return S2N_SUCCESS;
    }

    /* Safety: freeing the key owned by this object */
    RSA_free(s2n_unsafe_rsa_get_non_const(rsa_key));
    rsa_key->rsa = NULL;

    return S2N_SUCCESS;
}

static int s2n_rsa_check_key_exists(const struct s2n_pkey *pkey)
{
    const struct s2n_rsa_key *rsa_key = &pkey->key.rsa_key;
    POSIX_ENSURE_REF(rsa_key->rsa);
    return 0;
}

S2N_RESULT s2n_evp_pkey_to_rsa_public_key(s2n_rsa_public_key *rsa_key, EVP_PKEY *evp_public_key)
{
    const RSA *rsa = EVP_PKEY_get1_RSA(evp_public_key);
    RESULT_ENSURE(rsa != NULL, S2N_ERR_DECODE_CERTIFICATE);

    rsa_key->rsa = rsa;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_evp_pkey_to_rsa_private_key(s2n_rsa_private_key *rsa_key, EVP_PKEY *evp_private_key)
{
    const RSA *rsa = EVP_PKEY_get1_RSA(evp_private_key);
    RESULT_ENSURE(rsa != NULL, S2N_ERR_DECODE_PRIVATE_KEY);

    rsa_key->rsa = rsa;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_rsa_pkey_init(struct s2n_pkey *pkey)
{
    pkey->size = &s2n_rsa_encrypted_size;
    pkey->sign = &s2n_rsa_sign;
    pkey->verify = &s2n_rsa_verify;
    pkey->encrypt = &s2n_rsa_encrypt;
    pkey->decrypt = &s2n_rsa_decrypt;
    pkey->match = &s2n_rsa_keys_match;
    pkey->free = &s2n_rsa_key_free;
    pkey->check_key = &s2n_rsa_check_key_exists;
    RESULT_GUARD(s2n_evp_signing_set_pkey_overrides(pkey));
    return S2N_RESULT_OK;
}
