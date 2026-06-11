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

#include "crypto/s2n_hash.h"

#include "crypto/s2n_fips.h"
#include "crypto/s2n_hmac.h"
#include "error/s2n_errno.h"
#include "utils/s2n_safety.h"

#if S2N_LIBCRYPTO_SUPPORTS_PROVIDERS
static EVP_MD *s2n_evp_mds[S2N_HASH_ALGS_COUNT] = { 0 };
#else
static const EVP_MD *s2n_evp_mds[S2N_HASH_ALGS_COUNT] = { 0 };
#endif

bool s2n_hash_use_custom_md5_sha1()
{
#if defined(S2N_LIBCRYPTO_SUPPORTS_EVP_MD5_SHA1_HASH)
    return false;
#else
    return true;
#endif
}

bool s2n_hash_supports_shake()
{
#if S2N_LIBCRYPTO_SUPPORTS_SHAKE
    return true;
#else
    return false;
#endif
}

S2N_RESULT s2n_hash_algorithms_init()
{
#if S2N_LIBCRYPTO_SUPPORTS_PROVIDERS
    /* openssl-3.0 introduced the concept of providers.
     * After openssl-3.0, the old EVP_sha256()-style methods will still work,
     * but may be inefficient. See
     * https://docs.openssl.org/3.4/man7/ossl-guide-libcrypto-introduction/#performance
     *
     * Additionally, the old style methods do not support property query strings
     * to guide which provider to fetch from. This is important for FIPS, where
     * the default query string of "fips=yes" will need to be overridden for
     * legacy algorithms.
     */
    s2n_evp_mds[S2N_HASH_MD5] = EVP_MD_fetch(NULL, "MD5", "-fips");
    s2n_evp_mds[S2N_HASH_MD5_SHA1] = EVP_MD_fetch(NULL, "MD5-SHA1", "-fips");
    s2n_evp_mds[S2N_HASH_SHA1] = EVP_MD_fetch(NULL, "SHA1", NULL);
    s2n_evp_mds[S2N_HASH_SHA224] = EVP_MD_fetch(NULL, "SHA224", NULL);
    s2n_evp_mds[S2N_HASH_SHA256] = EVP_MD_fetch(NULL, "SHA256", NULL);
    s2n_evp_mds[S2N_HASH_SHA384] = EVP_MD_fetch(NULL, "SHA384", NULL);
    s2n_evp_mds[S2N_HASH_SHA512] = EVP_MD_fetch(NULL, "SHA512", NULL);
    s2n_evp_mds[S2N_HASH_SHAKE256_64] = EVP_MD_fetch(NULL, "SHAKE256", NULL);
#else
    s2n_evp_mds[S2N_HASH_MD5] = EVP_md5();
    s2n_evp_mds[S2N_HASH_SHA1] = EVP_sha1();
    s2n_evp_mds[S2N_HASH_SHA224] = EVP_sha224();
    s2n_evp_mds[S2N_HASH_SHA256] = EVP_sha256();
    s2n_evp_mds[S2N_HASH_SHA384] = EVP_sha384();
    s2n_evp_mds[S2N_HASH_SHA512] = EVP_sha512();
    #if S2N_LIBCRYPTO_SUPPORTS_SHAKE
    s2n_evp_mds[S2N_HASH_SHAKE256_64] = EVP_shake256();
    #endif
    /* Very old libcryptos like openssl-1.0.2 do not support EVP_MD_md5_sha1().
     * We work around that by manually combining MD5 and SHA1, rather than
     * using the composite algorithm.
     */
    #if defined(S2N_LIBCRYPTO_SUPPORTS_EVP_MD5_SHA1_HASH)
    s2n_evp_mds[S2N_HASH_MD5_SHA1] = EVP_md5_sha1();
    #endif
#endif
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_hash_algorithms_cleanup()
{
#if S2N_LIBCRYPTO_SUPPORTS_PROVIDERS
    for (size_t i = 0; i < S2N_HASH_ALGS_COUNT; i++) {
        /* https://docs.openssl.org/3.4/man3/EVP_DigestInit/
         * > Decrements the reference count for the fetched EVP_MD structure.
         * > If the reference count drops to 0 then the structure is freed.
         * > If the argument is NULL, nothing is done.
         */
        EVP_MD_free(s2n_evp_mds[i]);
        s2n_evp_mds[i] = NULL;
    }
#endif
    return S2N_RESULT_OK;
}

const EVP_MD *s2n_hash_alg_to_evp_md(s2n_hash_algorithm alg)
{
    PTR_ENSURE_GTE(alg, 0);
    PTR_ENSURE_LT(alg, S2N_HASH_ALGS_COUNT);
    return s2n_evp_mds[alg];
}

int s2n_hash_digest_size(s2n_hash_algorithm alg, uint8_t *out)
{
    POSIX_ENSURE(S2N_MEM_IS_WRITABLE_CHECK(out, sizeof(*out)), S2N_ERR_PRECONDITION_VIOLATION);
    /* clang-format off */
    switch (alg) {
        case S2N_HASH_NONE:     *out = 0;                    break;
        case S2N_HASH_MD5:      *out = MD5_DIGEST_LENGTH;    break;
        case S2N_HASH_SHA1:     *out = SHA_DIGEST_LENGTH;    break;
        case S2N_HASH_SHA224:   *out = SHA224_DIGEST_LENGTH; break;
        case S2N_HASH_SHA256:   *out = SHA256_DIGEST_LENGTH; break;
        case S2N_HASH_SHA384:   *out = SHA384_DIGEST_LENGTH; break;
        case S2N_HASH_SHA512:   *out = SHA512_DIGEST_LENGTH; break;
        case S2N_HASH_MD5_SHA1: *out = MD5_DIGEST_LENGTH + SHA_DIGEST_LENGTH; break;
        /* SHAKE digests can be variable sized, but for now we only support
         * 64-byte digests to match the behavior of our existing SHA hashes.
         */
        case S2N_HASH_SHAKE256_64: *out = 64; break;
        default:
            POSIX_BAIL(S2N_ERR_HASH_INVALID_ALGORITHM);
    }
    /* clang-format on */
    return S2N_SUCCESS;
}

/* Return true if hash algorithm is available, false otherwise. */
bool s2n_hash_is_available(s2n_hash_algorithm alg)
{
    switch (alg) {
        case S2N_HASH_MD5:
        case S2N_HASH_MD5_SHA1:
        case S2N_HASH_NONE:
        case S2N_HASH_SHA1:
        case S2N_HASH_SHA224:
        case S2N_HASH_SHA256:
        case S2N_HASH_SHA384:
        case S2N_HASH_SHA512:
            return true;
        case S2N_HASH_SHAKE256_64:
            return s2n_hash_supports_shake();
        case S2N_HASH_ALGS_COUNT:
            return false;
    }
    return false;
}

int s2n_hash_is_ready_for_input(struct s2n_hash_state *state)
{
    POSIX_PRECONDITION(s2n_hash_state_validate(state));
    return state->is_ready_for_input;
}

static int s2n_evp_hash_new(struct s2n_hash_state *state)
{
    POSIX_ENSURE_REF(state->digest.high_level.evp.ctx = S2N_EVP_MD_CTX_NEW());
    if (s2n_hash_use_custom_md5_sha1()) {
        POSIX_ENSURE_REF(state->digest.high_level.evp_md5_secondary.ctx = S2N_EVP_MD_CTX_NEW());
    }
    return S2N_SUCCESS;
}

static int s2n_evp_hash_init(struct s2n_hash_state *state, s2n_hash_algorithm alg)
{
    POSIX_ENSURE_REF(state->digest.high_level.evp.ctx);

    if (alg == S2N_HASH_NONE) {
        return S2N_SUCCESS;
    }

    if (alg == S2N_HASH_MD5_SHA1 && s2n_hash_use_custom_md5_sha1()) {
        POSIX_ENSURE_REF(state->digest.high_level.evp_md5_secondary.ctx);
        POSIX_GUARD_OSSL(EVP_DigestInit_ex(state->digest.high_level.evp.ctx,
                                 s2n_hash_alg_to_evp_md(S2N_HASH_SHA1), NULL),
                S2N_ERR_HASH_INIT_FAILED);
        POSIX_GUARD_OSSL(EVP_DigestInit_ex(state->digest.high_level.evp_md5_secondary.ctx,
                                 s2n_hash_alg_to_evp_md(S2N_HASH_MD5), NULL),
                S2N_ERR_HASH_INIT_FAILED);
        return S2N_SUCCESS;
    }

    const EVP_MD *md = s2n_hash_alg_to_evp_md(alg);
    POSIX_ENSURE(md, S2N_ERR_HASH_INVALID_ALGORITHM);
    POSIX_GUARD_OSSL(EVP_DigestInit_ex(state->digest.high_level.evp.ctx, md, NULL),
            S2N_ERR_HASH_INIT_FAILED);

    return S2N_SUCCESS;
}

static int s2n_evp_hash_update(struct s2n_hash_state *state, const void *data, uint32_t size)
{
    if (state->alg == S2N_HASH_NONE) {
        return S2N_SUCCESS;
    }

    POSIX_ENSURE_REF(EVP_MD_CTX_md(state->digest.high_level.evp.ctx));
    POSIX_GUARD_OSSL(EVP_DigestUpdate(state->digest.high_level.evp.ctx, data, size), S2N_ERR_HASH_UPDATE_FAILED);

    if (state->alg == S2N_HASH_MD5_SHA1 && s2n_hash_use_custom_md5_sha1()) {
        POSIX_ENSURE_REF(EVP_MD_CTX_md(state->digest.high_level.evp_md5_secondary.ctx));
        POSIX_GUARD_OSSL(EVP_DigestUpdate(state->digest.high_level.evp_md5_secondary.ctx, data, size), S2N_ERR_HASH_UPDATE_FAILED);
    }

    return S2N_SUCCESS;
}

static int s2n_evp_hash_digest(struct s2n_hash_state *state, void *out, uint32_t size)
{
    unsigned int digest_size = size;
    uint8_t expected_digest_size = 0;
    POSIX_GUARD(s2n_hash_digest_size(state->alg, &expected_digest_size));
    POSIX_ENSURE_EQ(digest_size, expected_digest_size);

    if (state->alg == S2N_HASH_NONE) {
        return S2N_SUCCESS;
    }

    POSIX_ENSURE_REF(EVP_MD_CTX_md(state->digest.high_level.evp.ctx));

    if (state->alg == S2N_HASH_MD5_SHA1 && s2n_hash_use_custom_md5_sha1()) {
        POSIX_ENSURE_REF(EVP_MD_CTX_md(state->digest.high_level.evp_md5_secondary.ctx));

        uint8_t sha1_digest_size = 0;
        POSIX_GUARD(s2n_hash_digest_size(S2N_HASH_SHA1, &sha1_digest_size));

        unsigned int sha1_primary_digest_size = sha1_digest_size;
        unsigned int md5_secondary_digest_size = digest_size - sha1_primary_digest_size;

        POSIX_ENSURE(EVP_MD_CTX_size(state->digest.high_level.evp.ctx) <= sha1_digest_size, S2N_ERR_HASH_DIGEST_FAILED);
        POSIX_ENSURE((size_t) EVP_MD_CTX_size(state->digest.high_level.evp_md5_secondary.ctx) <= md5_secondary_digest_size, S2N_ERR_HASH_DIGEST_FAILED);

        POSIX_GUARD_OSSL(EVP_DigestFinal_ex(state->digest.high_level.evp.ctx, ((uint8_t *) out) + MD5_DIGEST_LENGTH, &sha1_primary_digest_size), S2N_ERR_HASH_DIGEST_FAILED);
        POSIX_GUARD_OSSL(EVP_DigestFinal_ex(state->digest.high_level.evp_md5_secondary.ctx, out, &md5_secondary_digest_size), S2N_ERR_HASH_DIGEST_FAILED);
        return S2N_SUCCESS;
    }

    if (state->alg == S2N_HASH_SHAKE256_64) {
#if S2N_LIBCRYPTO_SUPPORTS_SHAKE
        /* "XOF" stands for "extendable-output functions", and indicates a hash algorithm
         * like SHAKE that can produce digests of any size. When using an XOF algorithm,
         * EVP_DigestFinalXOF should be used instead of EVP_DigestFinal_ex.
         * Calling the wrong digest method will error.
         */
        POSIX_GUARD_OSSL(EVP_DigestFinalXOF(state->digest.high_level.evp.ctx, out, digest_size),
                S2N_ERR_HASH_DIGEST_FAILED);
        return S2N_SUCCESS;
#else
        POSIX_BAIL(S2N_ERR_HASH_INVALID_ALGORITHM);
#endif
    }

    POSIX_ENSURE((size_t) EVP_MD_CTX_size(state->digest.high_level.evp.ctx) <= digest_size, S2N_ERR_HASH_DIGEST_FAILED);
    POSIX_GUARD_OSSL(EVP_DigestFinal_ex(state->digest.high_level.evp.ctx, out, &digest_size), S2N_ERR_HASH_DIGEST_FAILED);
    return S2N_SUCCESS;
}

static int s2n_evp_hash_copy(struct s2n_hash_state *to, struct s2n_hash_state *from)
{
    if (from->alg == S2N_HASH_NONE) {
        return S2N_SUCCESS;
    }

    POSIX_ENSURE_REF(to->digest.high_level.evp.ctx);
    POSIX_GUARD_OSSL(EVP_MD_CTX_copy_ex(to->digest.high_level.evp.ctx, from->digest.high_level.evp.ctx), S2N_ERR_HASH_COPY_FAILED);

    if (from->alg == S2N_HASH_MD5_SHA1 && s2n_hash_use_custom_md5_sha1()) {
        POSIX_ENSURE_REF(to->digest.high_level.evp_md5_secondary.ctx);
        POSIX_GUARD_OSSL(EVP_MD_CTX_copy_ex(to->digest.high_level.evp_md5_secondary.ctx, from->digest.high_level.evp_md5_secondary.ctx), S2N_ERR_HASH_COPY_FAILED);
    }

    return S2N_SUCCESS;
}

static int s2n_evp_hash_reset(struct s2n_hash_state *state)
{
    POSIX_GUARD_OSSL(S2N_EVP_MD_CTX_RESET(state->digest.high_level.evp.ctx), S2N_ERR_HASH_WIPE_FAILED);
    if (state->alg == S2N_HASH_MD5_SHA1 && s2n_hash_use_custom_md5_sha1()) {
        POSIX_GUARD_OSSL(S2N_EVP_MD_CTX_RESET(state->digest.high_level.evp_md5_secondary.ctx), S2N_ERR_HASH_WIPE_FAILED);
    }
    return S2N_SUCCESS;
}

static int s2n_evp_hash_free(struct s2n_hash_state *state)
{
    S2N_EVP_MD_CTX_FREE(state->digest.high_level.evp.ctx);
    state->digest.high_level.evp.ctx = NULL;
    if (s2n_hash_use_custom_md5_sha1()) {
        S2N_EVP_MD_CTX_FREE(state->digest.high_level.evp_md5_secondary.ctx);
        state->digest.high_level.evp_md5_secondary.ctx = NULL;
    }
    return S2N_SUCCESS;
}

static const struct s2n_hash s2n_evp_hash = {
    .alloc = &s2n_evp_hash_new,
    .init = &s2n_evp_hash_init,
    .update = &s2n_evp_hash_update,
    .digest = &s2n_evp_hash_digest,
    .copy = &s2n_evp_hash_copy,
    .reset = &s2n_evp_hash_reset,
    .free = &s2n_evp_hash_free,
};

/* This method looks unnecessary, but our CBMC proofs are
 * dependent on it. Search for:
 * __CPROVER_file_local_s2n_hash_c_s2n_hash_set_evp_impl
 */
static void s2n_hash_set_evp_impl(struct s2n_hash_state *state)
{
    state->hash_impl = &s2n_evp_hash;
}

int s2n_hash_new(struct s2n_hash_state *state)
{
    POSIX_ENSURE_REF(state);

    s2n_hash_set_evp_impl(state);
    POSIX_ENSURE_REF(state->hash_impl->alloc);
    POSIX_GUARD(state->hash_impl->alloc(state));

    state->alg = S2N_HASH_NONE;
    state->is_ready_for_input = 0;
    state->currently_in_hash = 0;
    return S2N_SUCCESS;
}

S2N_RESULT s2n_hash_state_validate(struct s2n_hash_state *state)
{
    RESULT_ENSURE_REF(state);
    return S2N_RESULT_OK;
}

int s2n_hash_init(struct s2n_hash_state *state, s2n_hash_algorithm alg)
{
    POSIX_ENSURE_REF(state);
    POSIX_ENSURE(s2n_hash_is_available(alg), S2N_ERR_HASH_INVALID_ALGORITHM);

    POSIX_ENSURE_REF(state->hash_impl);
    POSIX_ENSURE_REF(state->hash_impl->init);
    POSIX_GUARD(state->hash_impl->init(state, alg));

    state->alg = alg;
    state->is_ready_for_input = 1;
    state->currently_in_hash = 0;
    return S2N_SUCCESS;
}

int s2n_hash_update(struct s2n_hash_state *state, const void *data, uint32_t size)
{
    POSIX_PRECONDITION(s2n_hash_state_validate(state));
    POSIX_ENSURE(S2N_MEM_IS_READABLE(data, size), S2N_ERR_PRECONDITION_VIOLATION);
    POSIX_ENSURE(state->is_ready_for_input, S2N_ERR_HASH_NOT_READY);

    POSIX_ENSURE_REF(state->hash_impl);
    POSIX_ENSURE_REF(state->hash_impl->update);
    POSIX_GUARD(state->hash_impl->update(state, data, size));

    POSIX_ENSURE(size <= (UINT64_MAX - state->currently_in_hash), S2N_ERR_INTEGER_OVERFLOW);
    state->currently_in_hash += size;
    return S2N_SUCCESS;
}

int s2n_hash_digest(struct s2n_hash_state *state, void *out, uint32_t size)
{
    POSIX_PRECONDITION(s2n_hash_state_validate(state));
    POSIX_ENSURE(S2N_MEM_IS_READABLE(out, size), S2N_ERR_PRECONDITION_VIOLATION);
    POSIX_ENSURE(state->is_ready_for_input, S2N_ERR_HASH_NOT_READY);

    POSIX_ENSURE_REF(state->hash_impl);
    POSIX_ENSURE_REF(state->hash_impl->digest);
    POSIX_GUARD(state->hash_impl->digest(state, out, size));

    state->currently_in_hash = 0;
    state->is_ready_for_input = 0;
    return S2N_SUCCESS;
}

int s2n_hash_copy(struct s2n_hash_state *to, struct s2n_hash_state *from)
{
    POSIX_PRECONDITION(s2n_hash_state_validate(to));
    POSIX_PRECONDITION(s2n_hash_state_validate(from));

    POSIX_ENSURE_REF(from->hash_impl);
    POSIX_ENSURE_REF(from->hash_impl->copy);
    POSIX_GUARD(from->hash_impl->copy(to, from));

    to->hash_impl = from->hash_impl;
    to->alg = from->alg;
    to->is_ready_for_input = from->is_ready_for_input;
    to->currently_in_hash = from->currently_in_hash;
    return S2N_SUCCESS;
}

int s2n_hash_reset(struct s2n_hash_state *state)
{
    POSIX_ENSURE_REF(state);
    POSIX_ENSURE_REF(state->hash_impl);
    POSIX_ENSURE_REF(state->hash_impl->reset);
    POSIX_GUARD(state->hash_impl->reset(state));
    POSIX_GUARD(s2n_hash_init(state, state->alg));
    return S2N_SUCCESS;
}

int s2n_hash_free(struct s2n_hash_state *state)
{
    if (state == NULL) {
        return S2N_SUCCESS;
    }

    POSIX_ENSURE_REF(state->hash_impl);
    POSIX_ENSURE_REF(state->hash_impl->free);
    POSIX_GUARD(state->hash_impl->free(state));

    state->alg = S2N_HASH_NONE;
    state->is_ready_for_input = 0;
    state->currently_in_hash = 0;
    return S2N_SUCCESS;
}

int s2n_hash_get_currently_in_hash_total(struct s2n_hash_state *state, uint64_t *out)
{
    POSIX_PRECONDITION(s2n_hash_state_validate(state));
    POSIX_ENSURE(S2N_MEM_IS_WRITABLE_CHECK(out, sizeof(*out)), S2N_ERR_PRECONDITION_VIOLATION);
    POSIX_ENSURE(state->is_ready_for_input, S2N_ERR_HASH_NOT_READY);
    *out = state->currently_in_hash;
    return S2N_SUCCESS;
}
