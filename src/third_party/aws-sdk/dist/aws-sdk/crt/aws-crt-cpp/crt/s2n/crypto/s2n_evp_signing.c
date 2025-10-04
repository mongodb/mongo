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

#include "crypto/s2n_evp_signing.h"

#include "crypto/s2n_evp.h"
#include "crypto/s2n_pkey.h"
#include "crypto/s2n_rsa_pss.h"
#include "error/s2n_errno.h"
#include "tls/s2n_signature_algorithms.h"
#include "utils/s2n_safety.h"

DEFINE_POINTER_CLEANUP_FUNC(EVP_PKEY_CTX *, EVP_PKEY_CTX_free);

/*
 * FIPS 140-3 requires that we don't pass raw digest bytes to the libcrypto signing methods.
 * In order to do that, we need to use signing methods that both calculate the digest and
 * perform the signature.
 */

static S2N_RESULT s2n_evp_md_ctx_set_pkey_ctx(EVP_MD_CTX *ctx, EVP_PKEY_CTX *pctx)
{
#ifdef S2N_LIBCRYPTO_SUPPORTS_EVP_MD_CTX_SET_PKEY_CTX
    EVP_MD_CTX_set_pkey_ctx(ctx, pctx);
    return S2N_RESULT_OK;
#else
    RESULT_BAIL(S2N_ERR_UNIMPLEMENTED);
#endif
}

static S2N_RESULT s2n_evp_pkey_set_rsa_pss_saltlen(EVP_PKEY_CTX *pctx)
{
#if defined(S2N_LIBCRYPTO_SUPPORTS_RSA_PSS_SIGNING)
    RESULT_GUARD_OSSL(EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST), S2N_ERR_PKEY_CTX_INIT);
    return S2N_RESULT_OK;
#else
    RESULT_BAIL(S2N_ERR_RSA_PSS_NOT_SUPPORTED);
#endif
}

bool s2n_evp_signing_supported()
{
#ifdef S2N_LIBCRYPTO_SUPPORTS_EVP_MD_CTX_SET_PKEY_CTX
    /* We can only use EVP signing if the hash state has an EVP_MD_CTX
     * that we can pass to the EVP signing methods.
     */
    return s2n_hash_evp_fully_supported();
#else
    return false;
#endif
}

/* If using EVP signing, override the sign and verify pkey methods.
 * The EVP methods can handle all pkey types / signature algorithms.
 */
S2N_RESULT s2n_evp_signing_set_pkey_overrides(struct s2n_pkey *pkey)
{
    if (s2n_evp_signing_supported()) {
        RESULT_ENSURE_REF(pkey);
        pkey->sign = &s2n_evp_sign;
        pkey->verify = &s2n_evp_verify;
    }
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_evp_signing_validate_hash_alg(s2n_signature_algorithm sig_alg, s2n_hash_algorithm hash_alg)
{
    switch (hash_alg) {
        case S2N_HASH_NONE:
        case S2N_HASH_MD5:
            /* MD5 alone is never supported */
            RESULT_BAIL(S2N_ERR_HASH_INVALID_ALGORITHM);
            break;
        case S2N_HASH_MD5_SHA1:
            /* Only RSA supports MD5+SHA1.
             * This should not be a problem, as we only allow MD5+SHA1 when
             * falling back to TLS1.0 or 1.1, which only support RSA.
             */
            RESULT_ENSURE(sig_alg == S2N_SIGNATURE_RSA, S2N_ERR_HASH_INVALID_ALGORITHM);
            break;
        default:
            break;
    }
    /* Hash algorithm must be recognized and supported by EVP_MD */
    RESULT_ENSURE(s2n_hash_alg_to_evp_md(hash_alg) != NULL, S2N_ERR_HASH_INVALID_ALGORITHM);
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_evp_signing_validate_sig_alg(const struct s2n_pkey *key, s2n_signature_algorithm sig_alg)
{
    RESULT_ENSURE_REF(key);

    /* Ensure that the signature algorithm type matches the key type. */
    s2n_pkey_type pkey_type = S2N_PKEY_TYPE_UNKNOWN;
    RESULT_GUARD(s2n_pkey_get_type(key->pkey, &pkey_type));
    s2n_pkey_type sig_alg_type = S2N_PKEY_TYPE_UNKNOWN;
    RESULT_GUARD(s2n_signature_algorithm_get_pkey_type(sig_alg, &sig_alg_type));
    RESULT_ENSURE(pkey_type == sig_alg_type, S2N_ERR_INVALID_SIGNATURE_ALGORITHM);

    return S2N_RESULT_OK;
}

int s2n_evp_sign(const struct s2n_pkey *priv, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *hash_state, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(priv);
    POSIX_ENSURE_REF(hash_state);
    POSIX_ENSURE_REF(signature);
    POSIX_ENSURE(s2n_evp_signing_supported(), S2N_ERR_HASH_NOT_READY);
    POSIX_GUARD_RESULT(s2n_evp_signing_validate_hash_alg(sig_alg, hash_state->alg));

    DEFER_CLEANUP(EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new(priv->pkey, NULL), EVP_PKEY_CTX_free_pointer);
    POSIX_ENSURE_REF(pctx);
    POSIX_GUARD_OSSL(EVP_PKEY_sign_init(pctx), S2N_ERR_PKEY_CTX_INIT);
    POSIX_GUARD_OSSL(S2N_EVP_PKEY_CTX_set_signature_md(pctx, s2n_hash_alg_to_evp_md(hash_state->alg)), S2N_ERR_PKEY_CTX_INIT);

    if (sig_alg == S2N_SIGNATURE_RSA_PSS_RSAE || sig_alg == S2N_SIGNATURE_RSA_PSS_PSS) {
        POSIX_GUARD_OSSL(EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING), S2N_ERR_PKEY_CTX_INIT);
        POSIX_GUARD_RESULT(s2n_evp_pkey_set_rsa_pss_saltlen(pctx));
    }

    EVP_MD_CTX *ctx = hash_state->digest.high_level.evp.ctx;
    POSIX_ENSURE_REF(ctx);
    POSIX_GUARD_RESULT(s2n_evp_md_ctx_set_pkey_ctx(ctx, pctx));

    size_t signature_size = signature->size;
    POSIX_GUARD_OSSL(EVP_DigestSignFinal(ctx, signature->data, &signature_size), S2N_ERR_SIGN);
    POSIX_ENSURE(signature_size <= signature->size, S2N_ERR_SIZE_MISMATCH);
    signature->size = signature_size;
    POSIX_GUARD_RESULT(s2n_evp_md_ctx_set_pkey_ctx(ctx, NULL));
    return S2N_SUCCESS;
}

int s2n_evp_verify(const struct s2n_pkey *pub, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *hash_state, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(pub);
    POSIX_ENSURE_REF(hash_state);
    POSIX_ENSURE_REF(signature);
    POSIX_ENSURE(s2n_evp_signing_supported(), S2N_ERR_HASH_NOT_READY);
    POSIX_GUARD_RESULT(s2n_evp_signing_validate_hash_alg(sig_alg, hash_state->alg));
    POSIX_GUARD_RESULT(s2n_evp_signing_validate_sig_alg(pub, sig_alg));

    DEFER_CLEANUP(EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new(pub->pkey, NULL), EVP_PKEY_CTX_free_pointer);
    POSIX_ENSURE_REF(pctx);
    POSIX_GUARD_OSSL(EVP_PKEY_verify_init(pctx), S2N_ERR_PKEY_CTX_INIT);
    POSIX_GUARD_OSSL(S2N_EVP_PKEY_CTX_set_signature_md(pctx, s2n_hash_alg_to_evp_md(hash_state->alg)), S2N_ERR_PKEY_CTX_INIT);

    if (sig_alg == S2N_SIGNATURE_RSA_PSS_RSAE || sig_alg == S2N_SIGNATURE_RSA_PSS_PSS) {
        POSIX_GUARD_OSSL(EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING), S2N_ERR_PKEY_CTX_INIT);
        POSIX_GUARD_RESULT(s2n_evp_pkey_set_rsa_pss_saltlen(pctx));
    }

    EVP_MD_CTX *ctx = hash_state->digest.high_level.evp.ctx;
    POSIX_ENSURE_REF(ctx);
    POSIX_GUARD_RESULT(s2n_evp_md_ctx_set_pkey_ctx(ctx, pctx));

    POSIX_GUARD_OSSL(EVP_DigestVerifyFinal(ctx, signature->data, signature->size), S2N_ERR_VERIFY_SIGNATURE);
    POSIX_GUARD_RESULT(s2n_evp_md_ctx_set_pkey_ctx(ctx, NULL));
    return S2N_SUCCESS;
}
