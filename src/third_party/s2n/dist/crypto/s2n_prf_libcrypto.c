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

#include "crypto/s2n_prf_libcrypto.h"

#include "crypto/s2n_hash.h"
#include "error/s2n_errno.h"
#include "tls/s2n_connection.h"
#include "utils/s2n_safety.h"

#if defined(OPENSSL_IS_AWSLC)

/* The AWSLC TLS PRF API is exported in all AWSLC versions. However, in the AWSLC FIPS branch, this
 * API is defined in a private header:
 * https://github.com/aws/aws-lc/blob/d251b365b73a6e6acff6ee634aa8f077f23cdea4/crypto/fipsmodule/tls/internal.h#L27
 *
 * AWSLC has committed to this API definition, and the API has been added to a public header in the
 * main branch: https://github.com/aws/aws-lc/pull/1033. As such, this API is forward-declared in
 * order to make it accessible to s2n-tls when linked to AWSLC-FIPS.
 */
int CRYPTO_tls1_prf(const EVP_MD *digest,
        uint8_t *out, size_t out_len,
        const uint8_t *secret, size_t secret_len,
        const char *label, size_t label_len,
        const uint8_t *seed1, size_t seed1_len,
        const uint8_t *seed2, size_t seed2_len);

S2N_RESULT s2n_prf_libcrypto(struct s2n_connection *conn,
        struct s2n_blob *secret, struct s2n_blob *label,
        struct s2n_blob *seed_a, struct s2n_blob *seed_b, struct s2n_blob *seed_c,
        struct s2n_blob *out)
{
    const EVP_MD *digest = NULL;
    if (conn->actual_protocol_version < S2N_TLS12) {
        /* md5_sha1 is a digest that indicates both MD5 and SHA1 should be used in the PRF calculation.
         * This is needed for pre-TLS12 PRFs.
         */
        digest = EVP_md5_sha1();
    } else {
        RESULT_GUARD(s2n_hmac_md_from_alg(conn->secure->cipher_suite->prf_alg, &digest));
    }
    RESULT_ENSURE_REF(digest);

    DEFER_CLEANUP(struct s2n_stuffer seed_b_stuffer = { 0 }, s2n_stuffer_free);
    size_t seed_b_len = 0;
    uint8_t *seed_b_data = NULL;

    if (seed_b != NULL) {
        struct s2n_blob seed_b_blob = { 0 };
        RESULT_GUARD_POSIX(s2n_blob_init(&seed_b_blob, seed_b->data, seed_b->size));
        RESULT_GUARD_POSIX(s2n_stuffer_init_written(&seed_b_stuffer, &seed_b_blob));

        if (seed_c != NULL) {
            /* The AWSLC TLS PRF implementation only provides two seed arguments. If three seeds
             * were provided, pass in the third seed by concatenating it with the second seed.
             */
            RESULT_GUARD_POSIX(s2n_stuffer_alloc(&seed_b_stuffer, seed_b->size + seed_c->size));
            RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(&seed_b_stuffer, seed_b->data, seed_b->size));
            RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(&seed_b_stuffer, seed_c->data, seed_c->size));
        }

        seed_b_len = s2n_stuffer_data_available(&seed_b_stuffer);
        seed_b_data = s2n_stuffer_raw_read(&seed_b_stuffer, seed_b_len);
        RESULT_ENSURE_REF(seed_b_data);
    }

    RESULT_GUARD_OSSL(CRYPTO_tls1_prf(digest,
                              out->data, out->size,
                              secret->data, secret->size,
                              (const char *) label->data, label->size,
                              seed_a->data, seed_a->size,
                              seed_b_data, seed_b_len),
            S2N_ERR_PRF_DERIVE);

    return S2N_RESULT_OK;
}

#elif S2N_OPENSSL_VERSION_AT_LEAST(3, 0, 0)

    #include "crypto/s2n_kdf.h"

S2N_RESULT s2n_prf_libcrypto(struct s2n_connection *conn,
        struct s2n_blob *secret, struct s2n_blob *label,
        struct s2n_blob *seed_a, struct s2n_blob *seed_b, struct s2n_blob *seed_c,
        struct s2n_blob *out)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(secret);
    RESULT_ENSURE_REF(label);
    RESULT_ENSURE_REF(seed_a);
    RESULT_ENSURE_REF(out);

    struct s2n_blob empty_seed = { 0 };
    if (!seed_b) {
        seed_b = &empty_seed;
    }
    if (!seed_c) {
        seed_c = &empty_seed;
    }

    /* Openssl limits the size of the seed to 1024 bytes, including the label.
     * This would be an issue for TLS1.2 PQ, which uses full keyshares as seeds.
     * However, s2n-tls doesn't support PQ with Openssl, so this limitation will
     * never affect customers.
     *
     * As of this commit, EVP_KDF_derive will fail silently (without logging any
     * error) if the seed is too large. This check adds visibility.
     */
    uint64_t seed_total_size = label->size + seed_a->size + seed_b->size + seed_c->size;
    RESULT_ENSURE(seed_total_size <= 1024, S2N_ERR_PRF_INVALID_SEED);

    const char *digest_name = "MD5-SHA1";
    const char *fetch_properties = "-fips";

    if (conn->actual_protocol_version == S2N_TLS12) {
        fetch_properties = "";

        RESULT_ENSURE_REF(conn->secure);
        RESULT_ENSURE_REF(conn->secure->cipher_suite);
        s2n_hmac_algorithm prf_alg = conn->secure->cipher_suite->prf_alg;

        const EVP_MD *digest = NULL;
        RESULT_GUARD(s2n_hmac_md_from_alg(prf_alg, &digest));
        RESULT_ENSURE_REF(digest);
        digest_name = EVP_MD_get0_name(digest);
        RESULT_ENSURE_REF(digest_name);
    }

    /* As an optimization, we should be able to fetch and cache this EVP_KDF*
     * once when s2n_init is called.
     */
    DEFER_CLEANUP(EVP_KDF *prf_impl = EVP_KDF_fetch(NULL, "TLS1-PRF", fetch_properties),
            EVP_KDF_free_pointer);
    RESULT_ENSURE(prf_impl, S2N_ERR_PRF_INVALID_ALGORITHM);

    DEFER_CLEANUP(EVP_KDF_CTX *prf_ctx = EVP_KDF_CTX_new(prf_impl),
            EVP_KDF_CTX_free_pointer);
    RESULT_ENSURE_REF(prf_ctx);

    OSSL_PARAM params[] = {
        /* Casting away the const is safe because providers are forbidden from
         * modifying any OSSL_PARAM value other than return_size.
         * Even the examples in the Openssl documentation cast const strings to
         * non-const void pointers when setting up OSSL_PARAMs.
         */
        S2N_OSSL_PARAM_STR(OSSL_KDF_PARAM_PROPERTIES, (void *) (uintptr_t) fetch_properties),
        S2N_OSSL_PARAM_STR(OSSL_KDF_PARAM_DIGEST, (void *) (uintptr_t) digest_name),
        S2N_OSSL_PARAM_BLOB(OSSL_KDF_PARAM_SECRET, secret),
        /* "TLS1-PRF" handles the label like just another seed */
        S2N_OSSL_PARAM_BLOB(OSSL_KDF_PARAM_SEED, label),
        S2N_OSSL_PARAM_BLOB(OSSL_KDF_PARAM_SEED, seed_a),
        S2N_OSSL_PARAM_BLOB(OSSL_KDF_PARAM_SEED, seed_b),
        S2N_OSSL_PARAM_BLOB(OSSL_KDF_PARAM_SEED, seed_c),
        OSSL_PARAM_END,
    };

    RESULT_GUARD_OSSL(EVP_KDF_derive(prf_ctx, out->data, out->size, params),
            S2N_ERR_PRF_DERIVE);
    return S2N_RESULT_OK;
}

#else

S2N_RESULT s2n_prf_libcrypto(struct s2n_connection *conn,
        struct s2n_blob *secret, struct s2n_blob *label,
        struct s2n_blob *seed_a, struct s2n_blob *seed_b, struct s2n_blob *seed_c,
        struct s2n_blob *out)
{
    RESULT_BAIL(S2N_ERR_FIPS_MODE_UNSUPPORTED);
}

#endif
