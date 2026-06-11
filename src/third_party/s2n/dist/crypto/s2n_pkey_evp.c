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

#include "crypto/s2n_pkey_evp.h"

#include <openssl/evp.h>
#include <openssl/rsa.h>

#include "crypto/s2n_evp.h"
#include "crypto/s2n_libcrypto.h"
#include "crypto/s2n_pkey.h"
#include "crypto/s2n_rsa_pss.h"
#include "error/s2n_errno.h"
#include "tls/s2n_signature_algorithms.h"
#include "utils/s2n_random.h"
#include "utils/s2n_safety.h"

DEFINE_POINTER_CLEANUP_FUNC(EVP_PKEY_CTX *, EVP_PKEY_CTX_free);

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

static S2N_RESULT s2n_pkey_evp_validate_sig_alg(const struct s2n_pkey *key, s2n_signature_algorithm sig_alg)
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

static EVP_PKEY_CTX *s2n_evp_pkey_ctx_new(EVP_PKEY *pkey, s2n_hash_algorithm hash_alg)
{
    PTR_ENSURE_REF(pkey);
    switch (hash_alg) {
#if S2N_LIBCRYPTO_SUPPORTS_PROVIDERS
        /* For openssl-3.0, pkey methods will do an implicit fetch for the signing
         * algorithm, which includes the hash algorithm. If using a legacy hash
         * algorithm, specify the non-fips version.
         */
        case S2N_HASH_MD5:
        case S2N_HASH_MD5_SHA1:
        case S2N_HASH_SHA1:
            return EVP_PKEY_CTX_new_from_pkey(NULL, pkey, "-fips");
#endif
        default:
            return EVP_PKEY_CTX_new(pkey, NULL);
    }
}

/* Our "digest-and-sign" EVP signing logic is intended to support FIPS 140-3.
 * FIPS 140-3 does not allow signing or verifying externally calculated digests
 * for RSA and ECDSA verify.
 * See https://csrc.nist.gov/Projects/Cryptographic-Algorithm-Validation-Program/Digital-Signatures,
 * and note that "component" tests only exist for ECDSA sign.
 *
 * In order to avoid signing externally calculated digests, we naively would
 * need access to the full message to be signed at the time of signing. That's
 * a problem for TLS1.2, where the client cert verify message requires signing
 * every handshake message sent or received before the client cert verify message.
 * To avoid storing every single handshake message in its entirety, we instead
 * keep a running hash of the messages in an EVP hash state. Then, instead of
 * digesting that hash state, we pass it unmodified to EVP_DigestSignFinal.
 * That would normally not be allowed, since the hash state was initialized without
 * a key using EVP_DigestInit instead of with a key using EVP_DigestSignInit.
 * We make it work by using the EVP_MD_CTX_set_pkey_ctx method to attach a key
 * to an existing hash state.
 *
 * All that means that "digest-and-sign" requires two things:
 * - A single EVP hash state to sign. So we must not use a custom MD5_SHA1 hash,
 *   which doesn't produce a single hash state.
 * - EVP_MD_CTX_set_pkey_ctx to exist and to behave as expected. Existence
 *   alone is not sufficient: the method exists in openssl-3.0-fips, but
 *   it cannot be used to setup a hash state for EVP_DigestSignFinal.
 *
 * Currently only awslc-fips meets both these requirements. New libcryptos
 * should be assumed not to meet these requirements until proven otherwise.
 */
static int s2n_pkey_evp_digest_and_sign(EVP_PKEY_CTX *pctx, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *hash_state, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(pctx);
    POSIX_ENSURE_REF(hash_state);
    POSIX_ENSURE_REF(signature);

    /* Custom MD5_SHA1 involves combining separate MD5 and SHA1 hashes.
     * That involves two hash states instead of the single hash state this
     * method requires.
     */
    POSIX_ENSURE(!s2n_hash_use_custom_md5_sha1(), S2N_ERR_SAFETY);

    /* Not all implementations of EVP_MD_CTX_set_pkey_ctx behave as required
     * by this method. Using EVP_MD_CTX_set_pkey_ctx to convert a hash initialized
     * with EVP_DigestInit to one that can be finalized with EVP_DigestSignFinal
     * is not entirely standard.
     *
     * However, this behavior is known to work with awslc-fips.
     */
    POSIX_ENSURE(s2n_libcrypto_is_awslc_fips(), S2N_ERR_SAFETY);

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

/* See s2n_evp_digest_and_sign for more information */
static bool s2n_pkey_evp_digest_and_sign_is_required(s2n_signature_algorithm sig_alg)
{
    if (sig_alg == S2N_SIGNATURE_MLDSA) {
        /* The FIPS restrictions do not apply to ML-DSA */
        return false;
    }
    return s2n_libcrypto_is_awslc_fips();
}

/* "digest-then-sign" means that we calculate the digest for a hash state,
 * then sign the digest bytes. That is not allowed by FIPS 140-3, but is allowed
 * in all other cases.
 */
static int s2n_pkey_evp_digest_then_sign(EVP_PKEY_CTX *pctx,
        struct s2n_hash_state *hash_state, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(pctx);
    POSIX_ENSURE_REF(hash_state);
    POSIX_ENSURE_REF(signature);

    uint8_t digest_length = 0;
    POSIX_GUARD(s2n_hash_digest_size(hash_state->alg, &digest_length));
    POSIX_ENSURE_LTE(digest_length, S2N_MAX_DIGEST_LEN);

    uint8_t digest_out[S2N_MAX_DIGEST_LEN] = { 0 };
    POSIX_GUARD(s2n_hash_digest(hash_state, digest_out, digest_length));

    size_t signature_size = signature->size;
    POSIX_GUARD_OSSL(EVP_PKEY_sign(pctx, signature->data, &signature_size,
                             digest_out, digest_length),
            S2N_ERR_SIGN);
    POSIX_ENSURE(signature_size <= signature->size, S2N_ERR_SIZE_MISMATCH);
    signature->size = signature_size;

    return S2N_SUCCESS;
}

int s2n_pkey_evp_sign(const struct s2n_pkey *priv, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *hash_state, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(priv);
    POSIX_ENSURE_REF(hash_state);

    DEFER_CLEANUP(EVP_PKEY_CTX *pctx = s2n_evp_pkey_ctx_new(priv->pkey, hash_state->alg), EVP_PKEY_CTX_free_pointer);
    POSIX_ENSURE_REF(pctx);
    POSIX_GUARD_OSSL(EVP_PKEY_sign_init(pctx), S2N_ERR_PKEY_CTX_INIT);

    if (sig_alg != S2N_SIGNATURE_MLDSA) {
        POSIX_GUARD_OSSL(S2N_EVP_PKEY_CTX_set_signature_md(pctx, s2n_hash_alg_to_evp_md(hash_state->alg)), S2N_ERR_PKEY_CTX_INIT);
    }

    if (sig_alg == S2N_SIGNATURE_RSA_PSS_RSAE || sig_alg == S2N_SIGNATURE_RSA_PSS_PSS) {
        POSIX_GUARD_OSSL(EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING), S2N_ERR_PKEY_CTX_INIT);
        POSIX_GUARD_RESULT(s2n_evp_pkey_set_rsa_pss_saltlen(pctx));
    }

    if (s2n_pkey_evp_digest_and_sign_is_required(sig_alg)) {
        POSIX_GUARD(s2n_pkey_evp_digest_and_sign(pctx, sig_alg, hash_state, signature));
    } else {
        POSIX_GUARD(s2n_pkey_evp_digest_then_sign(pctx, hash_state, signature));
    }

    return S2N_SUCCESS;
}

/* See s2n_evp_digest_and_sign for more information */
static int s2n_pkey_evp_digest_and_verify(EVP_PKEY_CTX *pctx, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *hash_state, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(pctx);
    POSIX_ENSURE_REF(hash_state);
    POSIX_ENSURE_REF(signature);

    /* See digest-and-sign requirements */
    POSIX_ENSURE(!s2n_hash_use_custom_md5_sha1(), S2N_ERR_SAFETY);
    POSIX_ENSURE(s2n_libcrypto_is_awslc_fips(), S2N_ERR_SAFETY);

    EVP_MD_CTX *ctx = hash_state->digest.high_level.evp.ctx;
    POSIX_ENSURE_REF(ctx);
    POSIX_GUARD_RESULT(s2n_evp_md_ctx_set_pkey_ctx(ctx, pctx));

    POSIX_GUARD_OSSL(EVP_DigestVerifyFinal(ctx, signature->data, signature->size), S2N_ERR_VERIFY_SIGNATURE);
    POSIX_GUARD_RESULT(s2n_evp_md_ctx_set_pkey_ctx(ctx, NULL));

    return S2N_SUCCESS;
}

/* See s2n_evp_digest_then_sign for more information */
static int s2n_pkey_evp_digest_then_verify(EVP_PKEY_CTX *pctx,
        struct s2n_hash_state *hash_state, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(pctx);
    POSIX_ENSURE_REF(hash_state);
    POSIX_ENSURE_REF(signature);

    uint8_t digest_length = 0;
    POSIX_GUARD(s2n_hash_digest_size(hash_state->alg, &digest_length));
    POSIX_ENSURE_LTE(digest_length, S2N_MAX_DIGEST_LEN);

    uint8_t digest_out[S2N_MAX_DIGEST_LEN] = { 0 };
    POSIX_GUARD(s2n_hash_digest(hash_state, digest_out, digest_length));

    POSIX_GUARD_OSSL(EVP_PKEY_verify(pctx, signature->data, signature->size,
                             digest_out, digest_length),
            S2N_ERR_VERIFY_SIGNATURE);
    return S2N_SUCCESS;
}

int s2n_pkey_evp_verify(const struct s2n_pkey *pub, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *hash_state, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(pub);
    POSIX_ENSURE_REF(hash_state);
    POSIX_ENSURE_REF(signature);
    POSIX_GUARD_RESULT(s2n_pkey_evp_validate_sig_alg(pub, sig_alg));

    DEFER_CLEANUP(EVP_PKEY_CTX *pctx = s2n_evp_pkey_ctx_new(pub->pkey, hash_state->alg), EVP_PKEY_CTX_free_pointer);
    POSIX_ENSURE_REF(pctx);
    POSIX_GUARD_OSSL(EVP_PKEY_verify_init(pctx), S2N_ERR_PKEY_CTX_INIT);

    if (sig_alg != S2N_SIGNATURE_MLDSA) {
        POSIX_GUARD_OSSL(S2N_EVP_PKEY_CTX_set_signature_md(pctx, s2n_hash_alg_to_evp_md(hash_state->alg)), S2N_ERR_PKEY_CTX_INIT);
    }

    if (sig_alg == S2N_SIGNATURE_RSA_PSS_RSAE || sig_alg == S2N_SIGNATURE_RSA_PSS_PSS) {
        POSIX_GUARD_OSSL(EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING), S2N_ERR_PKEY_CTX_INIT);
        POSIX_GUARD_RESULT(s2n_evp_pkey_set_rsa_pss_saltlen(pctx));
    }

    if (s2n_pkey_evp_digest_and_sign_is_required(sig_alg)) {
        POSIX_GUARD(s2n_pkey_evp_digest_and_verify(pctx, sig_alg, hash_state, signature));
    } else {
        POSIX_GUARD(s2n_pkey_evp_digest_then_verify(pctx, hash_state, signature));
    }

    return S2N_SUCCESS;
}

S2N_RESULT s2n_pkey_evp_size(const struct s2n_pkey *pkey, uint32_t *size_out)
{
    RESULT_ENSURE_REF(pkey);
    RESULT_ENSURE_REF(pkey->pkey);
    RESULT_ENSURE_REF(size_out);

    const int size = EVP_PKEY_size(pkey->pkey);
    RESULT_ENSURE_GT(size, 0);
    *size_out = size;

    return S2N_RESULT_OK;
}

int s2n_pkey_evp_encrypt(const struct s2n_pkey *key, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_REF(key);
    POSIX_ENSURE_REF(in);
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(key->pkey);

    s2n_pkey_type type = 0;
    POSIX_GUARD_RESULT(s2n_pkey_get_type(key->pkey, &type));
    POSIX_ENSURE(type == S2N_PKEY_TYPE_RSA, S2N_ERR_UNIMPLEMENTED);

    DEFER_CLEANUP(EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new(key->pkey, NULL), EVP_PKEY_CTX_free_pointer);
    POSIX_ENSURE_REF(pctx);
    POSIX_GUARD_OSSL(EVP_PKEY_encrypt_init(pctx), S2N_ERR_PKEY_CTX_INIT);
    POSIX_GUARD_OSSL(EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING), S2N_ERR_PKEY_CTX_INIT);

    size_t out_size = out->size;
    POSIX_GUARD_OSSL(EVP_PKEY_encrypt(pctx, out->data, &out_size, in->data, in->size), S2N_ERR_ENCRYPT);
    POSIX_ENSURE(out_size == out->size, S2N_ERR_SIZE_MISMATCH);

    return S2N_SUCCESS;
}

int s2n_pkey_evp_decrypt(const struct s2n_pkey *key, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_REF(key);
    POSIX_ENSURE_REF(in);
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(key->pkey);

    s2n_pkey_type type = 0;
    POSIX_GUARD_RESULT(s2n_pkey_get_type(key->pkey, &type));
    POSIX_ENSURE(type == S2N_PKEY_TYPE_RSA, S2N_ERR_UNIMPLEMENTED);

    uint32_t expected_size = 0;
    POSIX_GUARD_RESULT(s2n_pkey_size(key, &expected_size));

    /* RSA decryption requires more output memory than the size of the final decrypted message */
    struct s2n_blob buffer = { 0 };
    uint8_t buffer_bytes[4096] = { 0 };
    POSIX_GUARD(s2n_blob_init(&buffer, buffer_bytes, sizeof(buffer_bytes)));
    POSIX_ENSURE(out->size <= buffer.size, S2N_ERR_NOMEM);
    POSIX_ENSURE(expected_size <= buffer.size, S2N_ERR_NOMEM);

    DEFER_CLEANUP(EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new(key->pkey, NULL), EVP_PKEY_CTX_free_pointer);
    POSIX_ENSURE_REF(pctx);
    POSIX_GUARD_OSSL(EVP_PKEY_decrypt_init(pctx), S2N_ERR_PKEY_CTX_INIT);
    /* The padding is actually RSA_PKCS1_PADDING, but we'll handle the padding later */
    POSIX_GUARD_OSSL(EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_NO_PADDING), S2N_ERR_PKEY_CTX_INIT);

    size_t out_size = buffer.size;
    POSIX_GUARD_OSSL(EVP_PKEY_decrypt(pctx, buffer.data, &out_size, in->data, in->size), S2N_ERR_DECRYPT);
    POSIX_ENSURE(out_size == expected_size, S2N_ERR_SIZE_MISMATCH);

    /* Handle padding in constant time to avoid Bleichenbacher oracles.
     * If the padding is wrong, we return random output rather than failing.
     * That ensures that padding failures are treated the same as wrong outputs.
     */
    POSIX_GUARD_RESULT(s2n_get_public_random_data(out));
    s2n_constant_time_pkcs1_unpad_or_dont(out->data, buffer.data, out_size, out->size);

    return S2N_SUCCESS;
}

S2N_RESULT s2n_pkey_evp_init(struct s2n_pkey *pkey)
{
    RESULT_ENSURE_REF(pkey);
    pkey->size = &s2n_pkey_evp_size;
    pkey->sign = &s2n_pkey_evp_sign;
    pkey->verify = &s2n_pkey_evp_verify;
    pkey->encrypt = s2n_pkey_evp_encrypt;
    pkey->decrypt = s2n_pkey_evp_decrypt;
    return S2N_RESULT_OK;
}
