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
#include "crypto/s2n_openssl.h"
#include "error/s2n_errno.h"
#include "utils/s2n_safety.h"

static bool s2n_use_custom_md5_sha1()
{
#if defined(S2N_LIBCRYPTO_SUPPORTS_EVP_MD5_SHA1_HASH)
    return false;
#else
    return true;
#endif
}

static bool s2n_use_evp_impl()
{
    return s2n_is_in_fips_mode();
}

bool s2n_hash_evp_fully_supported()
{
    return s2n_use_evp_impl() && !s2n_use_custom_md5_sha1();
}

const EVP_MD *s2n_hash_alg_to_evp_md(s2n_hash_algorithm alg)
{
    switch (alg) {
        case S2N_HASH_MD5:
            return EVP_md5();
        case S2N_HASH_SHA1:
            return EVP_sha1();
        case S2N_HASH_SHA224:
            return EVP_sha224();
        case S2N_HASH_SHA256:
            return EVP_sha256();
        case S2N_HASH_SHA384:
            return EVP_sha384();
        case S2N_HASH_SHA512:
            return EVP_sha512();
#if defined(S2N_LIBCRYPTO_SUPPORTS_EVP_MD5_SHA1_HASH)
        case S2N_HASH_MD5_SHA1:
            return EVP_md5_sha1();
#endif
        default:
            return NULL;
    }
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
        default:
            POSIX_BAIL(S2N_ERR_HASH_INVALID_ALGORITHM);
    }
    /* clang-format on */
    return S2N_SUCCESS;
}

/* NOTE: s2n_hash_const_time_get_currently_in_hash_block takes advantage of the fact that
 * hash_block_size is a power of 2. This is true for all hashes we currently support
 * If this ever becomes untrue, this would require fixing*/
int s2n_hash_block_size(s2n_hash_algorithm alg, uint64_t *block_size)
{
    POSIX_ENSURE(S2N_MEM_IS_WRITABLE_CHECK(block_size, sizeof(*block_size)), S2N_ERR_PRECONDITION_VIOLATION);
    /* clang-format off */
    switch (alg) {
            case S2N_HASH_NONE:       *block_size = 64;   break;
            case S2N_HASH_MD5:        *block_size = 64;   break;
            case S2N_HASH_SHA1:       *block_size = 64;   break;
            case S2N_HASH_SHA224:     *block_size = 64;   break;
            case S2N_HASH_SHA256:     *block_size = 64;   break;
            case S2N_HASH_SHA384:     *block_size = 128;  break;
            case S2N_HASH_SHA512:     *block_size = 128;  break;
            case S2N_HASH_MD5_SHA1:   *block_size = 64;   break;
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
            /* return false if in FIPS mode, as MD5 algs are not available in FIPS mode. */
            return !s2n_is_in_fips_mode();
        case S2N_HASH_NONE:
        case S2N_HASH_SHA1:
        case S2N_HASH_SHA224:
        case S2N_HASH_SHA256:
        case S2N_HASH_SHA384:
        case S2N_HASH_SHA512:
            return true;
        case S2N_HASH_SENTINEL:
            return false;
    }
    return false;
}

int s2n_hash_is_ready_for_input(struct s2n_hash_state *state)
{
    POSIX_PRECONDITION(s2n_hash_state_validate(state));
    return state->is_ready_for_input;
}

static int s2n_low_level_hash_new(struct s2n_hash_state *state)
{
    /* s2n_hash_new will always call the corresponding implementation of the s2n_hash
     * being used. For the s2n_low_level_hash implementation, new is a no-op.
     */

    *state = (struct s2n_hash_state){ 0 };
    return S2N_SUCCESS;
}

static int s2n_low_level_hash_init(struct s2n_hash_state *state, s2n_hash_algorithm alg)
{
    switch (alg) {
        case S2N_HASH_NONE:
            break;
        case S2N_HASH_MD5:
            POSIX_GUARD_OSSL(MD5_Init(&state->digest.low_level.md5), S2N_ERR_HASH_INIT_FAILED);
            break;
        case S2N_HASH_SHA1:
            POSIX_GUARD_OSSL(SHA1_Init(&state->digest.low_level.sha1), S2N_ERR_HASH_INIT_FAILED);
            break;
        case S2N_HASH_SHA224:
            POSIX_GUARD_OSSL(SHA224_Init(&state->digest.low_level.sha224), S2N_ERR_HASH_INIT_FAILED);
            break;
        case S2N_HASH_SHA256:
            POSIX_GUARD_OSSL(SHA256_Init(&state->digest.low_level.sha256), S2N_ERR_HASH_INIT_FAILED);
            break;
        case S2N_HASH_SHA384:
            POSIX_GUARD_OSSL(SHA384_Init(&state->digest.low_level.sha384), S2N_ERR_HASH_INIT_FAILED);
            break;
        case S2N_HASH_SHA512:
            POSIX_GUARD_OSSL(SHA512_Init(&state->digest.low_level.sha512), S2N_ERR_HASH_INIT_FAILED);
            break;
        case S2N_HASH_MD5_SHA1:
            POSIX_GUARD_OSSL(SHA1_Init(&state->digest.low_level.md5_sha1.sha1), S2N_ERR_HASH_INIT_FAILED);
            POSIX_GUARD_OSSL(MD5_Init(&state->digest.low_level.md5_sha1.md5), S2N_ERR_HASH_INIT_FAILED);
            break;

        default:
            POSIX_BAIL(S2N_ERR_HASH_INVALID_ALGORITHM);
    }

    state->alg = alg;
    state->is_ready_for_input = 1;
    state->currently_in_hash = 0;

    return 0;
}

static int s2n_low_level_hash_update(struct s2n_hash_state *state, const void *data, uint32_t size)
{
    POSIX_ENSURE(state->is_ready_for_input, S2N_ERR_HASH_NOT_READY);

    switch (state->alg) {
        case S2N_HASH_NONE:
            break;
        case S2N_HASH_MD5:
            POSIX_GUARD_OSSL(MD5_Update(&state->digest.low_level.md5, data, size), S2N_ERR_HASH_UPDATE_FAILED);
            break;
        case S2N_HASH_SHA1:
            POSIX_GUARD_OSSL(SHA1_Update(&state->digest.low_level.sha1, data, size), S2N_ERR_HASH_UPDATE_FAILED);
            break;
        case S2N_HASH_SHA224:
            POSIX_GUARD_OSSL(SHA224_Update(&state->digest.low_level.sha224, data, size), S2N_ERR_HASH_UPDATE_FAILED);
            break;
        case S2N_HASH_SHA256:
            POSIX_GUARD_OSSL(SHA256_Update(&state->digest.low_level.sha256, data, size), S2N_ERR_HASH_UPDATE_FAILED);
            break;
        case S2N_HASH_SHA384:
            POSIX_GUARD_OSSL(SHA384_Update(&state->digest.low_level.sha384, data, size), S2N_ERR_HASH_UPDATE_FAILED);
            break;
        case S2N_HASH_SHA512:
            POSIX_GUARD_OSSL(SHA512_Update(&state->digest.low_level.sha512, data, size), S2N_ERR_HASH_UPDATE_FAILED);
            break;
        case S2N_HASH_MD5_SHA1:
            POSIX_GUARD_OSSL(SHA1_Update(&state->digest.low_level.md5_sha1.sha1, data, size), S2N_ERR_HASH_UPDATE_FAILED);
            POSIX_GUARD_OSSL(MD5_Update(&state->digest.low_level.md5_sha1.md5, data, size), S2N_ERR_HASH_UPDATE_FAILED);
            break;
        default:
            POSIX_BAIL(S2N_ERR_HASH_INVALID_ALGORITHM);
    }

    POSIX_ENSURE(size <= (UINT64_MAX - state->currently_in_hash), S2N_ERR_INTEGER_OVERFLOW);
    state->currently_in_hash += size;

    return S2N_SUCCESS;
}

static int s2n_low_level_hash_digest(struct s2n_hash_state *state, void *out, uint32_t size)
{
    POSIX_ENSURE(state->is_ready_for_input, S2N_ERR_HASH_NOT_READY);

    switch (state->alg) {
        case S2N_HASH_NONE:
            break;
        case S2N_HASH_MD5:
            POSIX_ENSURE_EQ(size, MD5_DIGEST_LENGTH);
            POSIX_GUARD_OSSL(MD5_Final(out, &state->digest.low_level.md5), S2N_ERR_HASH_DIGEST_FAILED);
            break;
        case S2N_HASH_SHA1:
            POSIX_ENSURE_EQ(size, SHA_DIGEST_LENGTH);
            POSIX_GUARD_OSSL(SHA1_Final(out, &state->digest.low_level.sha1), S2N_ERR_HASH_DIGEST_FAILED);
            break;
        case S2N_HASH_SHA224:
            POSIX_ENSURE_EQ(size, SHA224_DIGEST_LENGTH);
            POSIX_GUARD_OSSL(SHA224_Final(out, &state->digest.low_level.sha224), S2N_ERR_HASH_DIGEST_FAILED);
            break;
        case S2N_HASH_SHA256:
            POSIX_ENSURE_EQ(size, SHA256_DIGEST_LENGTH);
            POSIX_GUARD_OSSL(SHA256_Final(out, &state->digest.low_level.sha256), S2N_ERR_HASH_DIGEST_FAILED);
            break;
        case S2N_HASH_SHA384:
            POSIX_ENSURE_EQ(size, SHA384_DIGEST_LENGTH);
            POSIX_GUARD_OSSL(SHA384_Final(out, &state->digest.low_level.sha384), S2N_ERR_HASH_DIGEST_FAILED);
            break;
        case S2N_HASH_SHA512:
            POSIX_ENSURE_EQ(size, SHA512_DIGEST_LENGTH);
            POSIX_GUARD_OSSL(SHA512_Final(out, &state->digest.low_level.sha512), S2N_ERR_HASH_DIGEST_FAILED);
            break;
        case S2N_HASH_MD5_SHA1:
            POSIX_ENSURE_EQ(size, MD5_DIGEST_LENGTH + SHA_DIGEST_LENGTH);
            POSIX_GUARD_OSSL(SHA1_Final(((uint8_t *) out) + MD5_DIGEST_LENGTH, &state->digest.low_level.md5_sha1.sha1), S2N_ERR_HASH_DIGEST_FAILED);
            POSIX_GUARD_OSSL(MD5_Final(out, &state->digest.low_level.md5_sha1.md5), S2N_ERR_HASH_DIGEST_FAILED);
            break;
        default:
            POSIX_BAIL(S2N_ERR_HASH_INVALID_ALGORITHM);
    }

    state->currently_in_hash = 0;
    state->is_ready_for_input = 0;
    return 0;
}

static int s2n_low_level_hash_copy(struct s2n_hash_state *to, struct s2n_hash_state *from)
{
    POSIX_CHECKED_MEMCPY(to, from, sizeof(struct s2n_hash_state));
    return 0;
}

static int s2n_low_level_hash_reset(struct s2n_hash_state *state)
{
    /* hash_init resets the ready_for_input and currently_in_hash fields. */
    return s2n_low_level_hash_init(state, state->alg);
}

static int s2n_low_level_hash_free(struct s2n_hash_state *state)
{
    /* s2n_hash_free will always call the corresponding implementation of the s2n_hash
     * being used. For the s2n_low_level_hash implementation, free is a no-op.
     */
    state->is_ready_for_input = 0;
    return S2N_SUCCESS;
}

static int s2n_evp_hash_new(struct s2n_hash_state *state)
{
    POSIX_ENSURE_REF(state->digest.high_level.evp.ctx = S2N_EVP_MD_CTX_NEW());
    if (s2n_use_custom_md5_sha1()) {
        POSIX_ENSURE_REF(state->digest.high_level.evp_md5_secondary.ctx = S2N_EVP_MD_CTX_NEW());
    }

    state->is_ready_for_input = 0;
    state->currently_in_hash = 0;

    return S2N_SUCCESS;
}

static int s2n_evp_hash_allow_md5_for_fips(struct s2n_hash_state *state)
{
    /* This is only to be used for s2n_hash_states that will require MD5 to be used
     * to comply with the TLS 1.0 and 1.1 RFC's for the PRF. MD5 cannot be used
     * outside of the TLS 1.0 and 1.1 PRF when in FIPS mode. When needed, this must
     * be called prior to s2n_hash_init().
     */
    POSIX_GUARD(s2n_digest_allow_md5_for_fips(&state->digest.high_level.evp));
    if (s2n_use_custom_md5_sha1()) {
        POSIX_GUARD(s2n_digest_allow_md5_for_fips(&state->digest.high_level.evp_md5_secondary));
    }
    return S2N_SUCCESS;
}

static int s2n_evp_hash_init(struct s2n_hash_state *state, s2n_hash_algorithm alg)
{
    POSIX_ENSURE_REF(state->digest.high_level.evp.ctx);

    state->alg = alg;
    state->is_ready_for_input = 1;
    state->currently_in_hash = 0;

    if (alg == S2N_HASH_NONE) {
        return S2N_SUCCESS;
    }

    if (alg == S2N_HASH_MD5_SHA1 && s2n_use_custom_md5_sha1()) {
        POSIX_ENSURE_REF(state->digest.high_level.evp_md5_secondary.ctx);
        POSIX_GUARD_OSSL(EVP_DigestInit_ex(state->digest.high_level.evp.ctx, EVP_sha1(), NULL), S2N_ERR_HASH_INIT_FAILED);
        POSIX_GUARD_OSSL(EVP_DigestInit_ex(state->digest.high_level.evp_md5_secondary.ctx, EVP_md5(), NULL), S2N_ERR_HASH_INIT_FAILED);
        return S2N_SUCCESS;
    }

    POSIX_ENSURE_REF(s2n_hash_alg_to_evp_md(alg));
    POSIX_GUARD_OSSL(EVP_DigestInit_ex(state->digest.high_level.evp.ctx, s2n_hash_alg_to_evp_md(alg), NULL), S2N_ERR_HASH_INIT_FAILED);
    return S2N_SUCCESS;
}

static int s2n_evp_hash_update(struct s2n_hash_state *state, const void *data, uint32_t size)
{
    POSIX_ENSURE(state->is_ready_for_input, S2N_ERR_HASH_NOT_READY);
    POSIX_ENSURE(size <= (UINT64_MAX - state->currently_in_hash), S2N_ERR_INTEGER_OVERFLOW);
    state->currently_in_hash += size;

    if (state->alg == S2N_HASH_NONE) {
        return S2N_SUCCESS;
    }

    POSIX_ENSURE_REF(EVP_MD_CTX_md(state->digest.high_level.evp.ctx));
    POSIX_GUARD_OSSL(EVP_DigestUpdate(state->digest.high_level.evp.ctx, data, size), S2N_ERR_HASH_UPDATE_FAILED);

    if (state->alg == S2N_HASH_MD5_SHA1 && s2n_use_custom_md5_sha1()) {
        POSIX_ENSURE_REF(EVP_MD_CTX_md(state->digest.high_level.evp_md5_secondary.ctx));
        POSIX_GUARD_OSSL(EVP_DigestUpdate(state->digest.high_level.evp_md5_secondary.ctx, data, size), S2N_ERR_HASH_UPDATE_FAILED);
    }

    return S2N_SUCCESS;
}

static int s2n_evp_hash_digest(struct s2n_hash_state *state, void *out, uint32_t size)
{
    POSIX_ENSURE(state->is_ready_for_input, S2N_ERR_HASH_NOT_READY);

    state->currently_in_hash = 0;
    state->is_ready_for_input = 0;

    unsigned int digest_size = size;
    uint8_t expected_digest_size = 0;
    POSIX_GUARD(s2n_hash_digest_size(state->alg, &expected_digest_size));
    POSIX_ENSURE_EQ(digest_size, expected_digest_size);

    if (state->alg == S2N_HASH_NONE) {
        return S2N_SUCCESS;
    }

    POSIX_ENSURE_REF(EVP_MD_CTX_md(state->digest.high_level.evp.ctx));

    if (state->alg == S2N_HASH_MD5_SHA1 && s2n_use_custom_md5_sha1()) {
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

    POSIX_ENSURE((size_t) EVP_MD_CTX_size(state->digest.high_level.evp.ctx) <= digest_size, S2N_ERR_HASH_DIGEST_FAILED);
    POSIX_GUARD_OSSL(EVP_DigestFinal_ex(state->digest.high_level.evp.ctx, out, &digest_size), S2N_ERR_HASH_DIGEST_FAILED);
    return S2N_SUCCESS;
}

static int s2n_evp_hash_copy(struct s2n_hash_state *to, struct s2n_hash_state *from)
{
    to->hash_impl = from->hash_impl;
    to->alg = from->alg;
    to->is_ready_for_input = from->is_ready_for_input;
    to->currently_in_hash = from->currently_in_hash;

    if (from->alg == S2N_HASH_NONE) {
        return S2N_SUCCESS;
    }

    POSIX_ENSURE_REF(to->digest.high_level.evp.ctx);
    POSIX_GUARD_OSSL(EVP_MD_CTX_copy_ex(to->digest.high_level.evp.ctx, from->digest.high_level.evp.ctx), S2N_ERR_HASH_COPY_FAILED);

    if (from->alg == S2N_HASH_MD5_SHA1 && s2n_use_custom_md5_sha1()) {
        POSIX_ENSURE_REF(to->digest.high_level.evp_md5_secondary.ctx);
        POSIX_GUARD_OSSL(EVP_MD_CTX_copy_ex(to->digest.high_level.evp_md5_secondary.ctx, from->digest.high_level.evp_md5_secondary.ctx), S2N_ERR_HASH_COPY_FAILED);
    }

    bool is_md5_allowed_for_fips = false;
    POSIX_GUARD_RESULT(s2n_digest_is_md5_allowed_for_fips(&from->digest.high_level.evp, &is_md5_allowed_for_fips));
    if (is_md5_allowed_for_fips && (from->alg == S2N_HASH_MD5 || from->alg == S2N_HASH_MD5_SHA1)) {
        POSIX_GUARD(s2n_hash_allow_md5_for_fips(to));
    }
    return S2N_SUCCESS;
}

static int s2n_evp_hash_reset(struct s2n_hash_state *state)
{
    int reset_md5_for_fips = 0;
    bool is_md5_allowed_for_fips = false;
    POSIX_GUARD_RESULT(s2n_digest_is_md5_allowed_for_fips(&state->digest.high_level.evp, &is_md5_allowed_for_fips));
    if ((state->alg == S2N_HASH_MD5 || state->alg == S2N_HASH_MD5_SHA1) && is_md5_allowed_for_fips) {
        reset_md5_for_fips = 1;
    }

    POSIX_GUARD_OSSL(S2N_EVP_MD_CTX_RESET(state->digest.high_level.evp.ctx), S2N_ERR_HASH_WIPE_FAILED);
    if (state->alg == S2N_HASH_MD5_SHA1 && s2n_use_custom_md5_sha1()) {
        POSIX_GUARD_OSSL(S2N_EVP_MD_CTX_RESET(state->digest.high_level.evp_md5_secondary.ctx), S2N_ERR_HASH_WIPE_FAILED);
    }

    if (reset_md5_for_fips) {
        POSIX_GUARD(s2n_hash_allow_md5_for_fips(state));
    }

    /* hash_init resets the ready_for_input and currently_in_hash fields. */
    return s2n_evp_hash_init(state, state->alg);
}

static int s2n_evp_hash_free(struct s2n_hash_state *state)
{
    S2N_EVP_MD_CTX_FREE(state->digest.high_level.evp.ctx);
    state->digest.high_level.evp.ctx = NULL;

    if (s2n_use_custom_md5_sha1()) {
        S2N_EVP_MD_CTX_FREE(state->digest.high_level.evp_md5_secondary.ctx);
        state->digest.high_level.evp_md5_secondary.ctx = NULL;
    }

    state->is_ready_for_input = 0;
    return S2N_SUCCESS;
}

static const struct s2n_hash s2n_low_level_hash = {
    .alloc = &s2n_low_level_hash_new,
    .allow_md5_for_fips = NULL,
    .init = &s2n_low_level_hash_init,
    .update = &s2n_low_level_hash_update,
    .digest = &s2n_low_level_hash_digest,
    .copy = &s2n_low_level_hash_copy,
    .reset = &s2n_low_level_hash_reset,
    .free = &s2n_low_level_hash_free,
};

static const struct s2n_hash s2n_evp_hash = {
    .alloc = &s2n_evp_hash_new,
    .allow_md5_for_fips = &s2n_evp_hash_allow_md5_for_fips,
    .init = &s2n_evp_hash_init,
    .update = &s2n_evp_hash_update,
    .digest = &s2n_evp_hash_digest,
    .copy = &s2n_evp_hash_copy,
    .reset = &s2n_evp_hash_reset,
    .free = &s2n_evp_hash_free,
};

static int s2n_hash_set_impl(struct s2n_hash_state *state)
{
    state->hash_impl = &s2n_low_level_hash;
    if (s2n_use_evp_impl()) {
        state->hash_impl = &s2n_evp_hash;
    }
    return S2N_SUCCESS;
}

int s2n_hash_new(struct s2n_hash_state *state)
{
    POSIX_ENSURE_REF(state);
    /* Set hash_impl on initial hash creation.
     * When in FIPS mode, the EVP API's must be used for hashes.
     */
    POSIX_GUARD(s2n_hash_set_impl(state));

    POSIX_ENSURE_REF(state->hash_impl->alloc);

    POSIX_GUARD(state->hash_impl->alloc(state));
    return S2N_SUCCESS;
}

S2N_RESULT s2n_hash_state_validate(struct s2n_hash_state *state)
{
    RESULT_ENSURE_REF(state);
    return S2N_RESULT_OK;
}

int s2n_hash_allow_md5_for_fips(struct s2n_hash_state *state)
{
    POSIX_ENSURE_REF(state);
    /* Ensure that hash_impl is set, as it may have been reset for s2n_hash_state on s2n_connection_wipe.
     * When in FIPS mode, the EVP API's must be used for hashes.
     */
    POSIX_GUARD(s2n_hash_set_impl(state));

    POSIX_ENSURE_REF(state->hash_impl->allow_md5_for_fips);

    return state->hash_impl->allow_md5_for_fips(state);
}

int s2n_hash_init(struct s2n_hash_state *state, s2n_hash_algorithm alg)
{
    POSIX_ENSURE_REF(state);
    /* Ensure that hash_impl is set, as it may have been reset for s2n_hash_state on s2n_connection_wipe.
     * When in FIPS mode, the EVP API's must be used for hashes.
     */
    POSIX_GUARD(s2n_hash_set_impl(state));

    bool is_md5_allowed_for_fips = false;
    POSIX_GUARD_RESULT(s2n_digest_is_md5_allowed_for_fips(&state->digest.high_level.evp, &is_md5_allowed_for_fips));

    if (s2n_hash_is_available(alg) || ((alg == S2N_HASH_MD5 || alg == S2N_HASH_MD5_SHA1) && is_md5_allowed_for_fips)) {
        /* s2n will continue to initialize an "unavailable" hash when s2n is in FIPS mode and
         * FIPS is forcing the hash to be made available.
         */
        POSIX_ENSURE_REF(state->hash_impl->init);

        return state->hash_impl->init(state, alg);
    } else {
        POSIX_BAIL(S2N_ERR_HASH_INVALID_ALGORITHM);
    }
}

int s2n_hash_update(struct s2n_hash_state *state, const void *data, uint32_t size)
{
    POSIX_PRECONDITION(s2n_hash_state_validate(state));
    POSIX_ENSURE(S2N_MEM_IS_READABLE(data, size), S2N_ERR_PRECONDITION_VIOLATION);
    POSIX_ENSURE_REF(state->hash_impl->update);

    return state->hash_impl->update(state, data, size);
}

int s2n_hash_digest(struct s2n_hash_state *state, void *out, uint32_t size)
{
    POSIX_PRECONDITION(s2n_hash_state_validate(state));
    POSIX_ENSURE(S2N_MEM_IS_READABLE(out, size), S2N_ERR_PRECONDITION_VIOLATION);
    POSIX_ENSURE_REF(state->hash_impl->digest);

    return state->hash_impl->digest(state, out, size);
}

int s2n_hash_copy(struct s2n_hash_state *to, struct s2n_hash_state *from)
{
    POSIX_PRECONDITION(s2n_hash_state_validate(to));
    POSIX_PRECONDITION(s2n_hash_state_validate(from));
    POSIX_ENSURE_REF(from->hash_impl->copy);

    return from->hash_impl->copy(to, from);
}

int s2n_hash_reset(struct s2n_hash_state *state)
{
    POSIX_ENSURE_REF(state);
    /* Ensure that hash_impl is set, as it may have been reset for s2n_hash_state on s2n_connection_wipe.
     * When in FIPS mode, the EVP API's must be used for hashes.
     */
    POSIX_GUARD(s2n_hash_set_impl(state));

    POSIX_ENSURE_REF(state->hash_impl->reset);

    return state->hash_impl->reset(state);
}

int s2n_hash_free(struct s2n_hash_state *state)
{
    if (state == NULL) {
        return S2N_SUCCESS;
    }
    /* Ensure that hash_impl is set, as it may have been reset for s2n_hash_state on s2n_connection_wipe.
     * When in FIPS mode, the EVP API's must be used for hashes.
     */
    POSIX_GUARD(s2n_hash_set_impl(state));

    POSIX_ENSURE_REF(state->hash_impl->free);

    return state->hash_impl->free(state);
}

int s2n_hash_get_currently_in_hash_total(struct s2n_hash_state *state, uint64_t *out)
{
    POSIX_PRECONDITION(s2n_hash_state_validate(state));
    POSIX_ENSURE(S2N_MEM_IS_WRITABLE_CHECK(out, sizeof(*out)), S2N_ERR_PRECONDITION_VIOLATION);
    POSIX_ENSURE(state->is_ready_for_input, S2N_ERR_HASH_NOT_READY);

    *out = state->currently_in_hash;
    return S2N_SUCCESS;
}

/* Calculate, in constant time, the number of bytes currently in the hash_block */
int s2n_hash_const_time_get_currently_in_hash_block(struct s2n_hash_state *state, uint64_t *out)
{
    POSIX_PRECONDITION(s2n_hash_state_validate(state));
    POSIX_ENSURE(S2N_MEM_IS_WRITABLE_CHECK(out, sizeof(*out)), S2N_ERR_PRECONDITION_VIOLATION);
    POSIX_ENSURE(state->is_ready_for_input, S2N_ERR_HASH_NOT_READY);
    uint64_t hash_block_size = 0;
    POSIX_GUARD(s2n_hash_block_size(state->alg, &hash_block_size));

    /* Requires that hash_block_size is a power of 2. This is true for all hashes we currently support
     * If this ever becomes untrue, this would require fixing this*/
    *out = state->currently_in_hash & (hash_block_size - 1);
    return S2N_SUCCESS;
}
