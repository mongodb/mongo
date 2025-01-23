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

#include "crypto/s2n_hkdf.h"

#include "crypto/s2n_fips.h"
#include "crypto/s2n_hmac.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

#ifdef S2N_LIBCRYPTO_SUPPORTS_HKDF
    #include <openssl/hkdf.h>
#endif

#define MAX_DIGEST_SIZE 64 /* Current highest is SHA512 */
#define MAX_HKDF_ROUNDS 255

/* Reference: RFC 5869 */

struct s2n_hkdf_impl {
    int (*hkdf)(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
            const struct s2n_blob *key, const struct s2n_blob *info, struct s2n_blob *output);
    int (*hkdf_extract)(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
            const struct s2n_blob *key, struct s2n_blob *pseudo_rand_key);
    int (*hkdf_expand)(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *pseudo_rand_key,
            const struct s2n_blob *info, struct s2n_blob *output);
};

static int s2n_custom_hkdf_extract(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
        const struct s2n_blob *key, struct s2n_blob *pseudo_rand_key)
{
    uint8_t hmac_size = 0;
    POSIX_GUARD(s2n_hmac_digest_size(alg, &hmac_size));
    POSIX_ENSURE(hmac_size <= pseudo_rand_key->size, S2N_ERR_HKDF_OUTPUT_SIZE);
    pseudo_rand_key->size = hmac_size;

    POSIX_GUARD(s2n_hmac_init(hmac, alg, salt->data, salt->size));
    POSIX_GUARD(s2n_hmac_update(hmac, key->data, key->size));
    POSIX_GUARD(s2n_hmac_digest(hmac, pseudo_rand_key->data, pseudo_rand_key->size));

    POSIX_GUARD(s2n_hmac_reset(hmac));

    return S2N_SUCCESS;
}

static int s2n_custom_hkdf_expand(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg,
        const struct s2n_blob *pseudo_rand_key, const struct s2n_blob *info, struct s2n_blob *output)
{
    uint8_t prev[MAX_DIGEST_SIZE] = { 0 };

    uint32_t done_len = 0;
    uint8_t hash_len = 0;
    POSIX_GUARD(s2n_hmac_digest_size(alg, &hash_len));
    POSIX_ENSURE_GT(hash_len, 0);
    uint32_t total_rounds = output->size / hash_len;
    if (output->size % hash_len) {
        total_rounds++;
    }

    POSIX_ENSURE(total_rounds > 0, S2N_ERR_HKDF_OUTPUT_SIZE);
    POSIX_ENSURE(total_rounds <= MAX_HKDF_ROUNDS, S2N_ERR_HKDF_OUTPUT_SIZE);

    for (uint32_t curr_round = 1; curr_round <= total_rounds; curr_round++) {
        uint32_t cat_len = 0;
        POSIX_GUARD(s2n_hmac_init(hmac, alg, pseudo_rand_key->data, pseudo_rand_key->size));
        if (curr_round != 1) {
            POSIX_GUARD(s2n_hmac_update(hmac, prev, hash_len));
        }
        POSIX_GUARD(s2n_hmac_update(hmac, info->data, info->size));
        POSIX_GUARD(s2n_hmac_update(hmac, &curr_round, 1));
        POSIX_GUARD(s2n_hmac_digest(hmac, prev, hash_len));

        cat_len = hash_len;
        if (done_len + hash_len > output->size) {
            cat_len = output->size - done_len;
        }

        POSIX_CHECKED_MEMCPY(output->data + done_len, prev, cat_len);

        done_len += cat_len;

        POSIX_GUARD(s2n_hmac_reset(hmac));
    }

    return S2N_SUCCESS;
}

static int s2n_custom_hkdf(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
        const struct s2n_blob *key, const struct s2n_blob *info, struct s2n_blob *output)
{
    uint8_t prk_pad[MAX_DIGEST_SIZE] = { 0 };
    struct s2n_blob pseudo_rand_key = { 0 };
    POSIX_GUARD(s2n_blob_init(&pseudo_rand_key, prk_pad, sizeof(prk_pad)));

    POSIX_GUARD(s2n_custom_hkdf_extract(hmac, alg, salt, key, &pseudo_rand_key));
    POSIX_GUARD(s2n_custom_hkdf_expand(hmac, alg, &pseudo_rand_key, info, output));

    return S2N_SUCCESS;
}

const struct s2n_hkdf_impl s2n_custom_hkdf_impl = {
    .hkdf = &s2n_custom_hkdf,
    .hkdf_extract = &s2n_custom_hkdf_extract,
    .hkdf_expand = &s2n_custom_hkdf_expand,
};

#ifdef S2N_LIBCRYPTO_SUPPORTS_HKDF
static int s2n_libcrypto_hkdf_extract(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
        const struct s2n_blob *key, struct s2n_blob *pseudo_rand_key)
{
    const EVP_MD *digest = NULL;
    POSIX_GUARD_RESULT(s2n_hmac_md_from_alg(alg, &digest));

    /* The out_len argument of HKDF_extract is set to the number of bytes written to out_key, and
     * is not used to ensure that out_key is large enough to contain the PRK. Ensure that the PRK
     * output will fit in the blob.
     */
    uint8_t hmac_size = 0;
    POSIX_GUARD(s2n_hmac_digest_size(alg, &hmac_size));
    POSIX_ENSURE(hmac_size <= pseudo_rand_key->size, S2N_ERR_HKDF_OUTPUT_SIZE);

    size_t bytes_written = 0;
    POSIX_GUARD_OSSL(HKDF_extract(pseudo_rand_key->data, &bytes_written, digest, key->data, key->size,
                             salt->data, salt->size),
            S2N_ERR_HKDF);

    /* HKDF_extract updates the out_len argument based on the digest size. Update the blob's size based on this. */
    POSIX_ENSURE_LTE(bytes_written, pseudo_rand_key->size);
    pseudo_rand_key->size = bytes_written;

    return S2N_SUCCESS;
}

static int s2n_libcrypto_hkdf_expand(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg,
        const struct s2n_blob *pseudo_rand_key, const struct s2n_blob *info, struct s2n_blob *output)
{
    POSIX_ENSURE(output->size > 0, S2N_ERR_HKDF_OUTPUT_SIZE);

    const EVP_MD *digest = NULL;
    POSIX_GUARD_RESULT(s2n_hmac_md_from_alg(alg, &digest));

    POSIX_GUARD_OSSL(HKDF_expand(output->data, output->size, digest, pseudo_rand_key->data, pseudo_rand_key->size,
                             info->data, info->size),
            S2N_ERR_HKDF);

    return S2N_SUCCESS;
}

static int s2n_libcrypto_hkdf(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
        const struct s2n_blob *key, const struct s2n_blob *info, struct s2n_blob *output)
{
    POSIX_ENSURE(output->size > 0, S2N_ERR_HKDF_OUTPUT_SIZE);

    const EVP_MD *digest = NULL;
    POSIX_GUARD_RESULT(s2n_hmac_md_from_alg(alg, &digest));

    POSIX_GUARD_OSSL(HKDF(output->data, output->size, digest, key->data, key->size, salt->data, salt->size,
                             info->data, info->size),
            S2N_ERR_HKDF);

    return S2N_SUCCESS;
}
#else
static int s2n_libcrypto_hkdf_extract(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
        const struct s2n_blob *key, struct s2n_blob *pseudo_rand_key)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}

static int s2n_libcrypto_hkdf_expand(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg,
        const struct s2n_blob *pseudo_rand_key, const struct s2n_blob *info, struct s2n_blob *output)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}

static int s2n_libcrypto_hkdf(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
        const struct s2n_blob *key, const struct s2n_blob *info, struct s2n_blob *output)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}
#endif /* S2N_LIBCRYPTO_SUPPORTS_HKDF */

const struct s2n_hkdf_impl s2n_libcrypto_hkdf_impl = {
    .hkdf = &s2n_libcrypto_hkdf,
    .hkdf_extract = &s2n_libcrypto_hkdf_extract,
    .hkdf_expand = &s2n_libcrypto_hkdf_expand,
};

static const struct s2n_hkdf_impl *s2n_get_hkdf_implementation()
{
    /* By default, s2n-tls uses a custom HKDF implementation. When operating in FIPS mode, the
     * FIPS-validated libcrypto implementation is used instead, if an implementation is provided.
     */
    if (s2n_is_in_fips_mode() && s2n_libcrypto_supports_hkdf()) {
        return &s2n_libcrypto_hkdf_impl;
    }

    return &s2n_custom_hkdf_impl;
}

int s2n_hkdf_extract(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
        const struct s2n_blob *key, struct s2n_blob *pseudo_rand_key)
{
    POSIX_ENSURE_REF(hmac);
    POSIX_ENSURE_REF(salt);
    POSIX_ENSURE_REF(key);
    POSIX_ENSURE_REF(pseudo_rand_key);

    const struct s2n_hkdf_impl *hkdf_implementation = s2n_get_hkdf_implementation();
    POSIX_ENSURE_REF(hkdf_implementation);

    POSIX_GUARD(hkdf_implementation->hkdf_extract(hmac, alg, salt, key, pseudo_rand_key));

    return S2N_SUCCESS;
}

static int s2n_hkdf_expand(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *pseudo_rand_key,
        const struct s2n_blob *info, struct s2n_blob *output)
{
    POSIX_ENSURE_REF(hmac);
    POSIX_ENSURE_REF(pseudo_rand_key);
    POSIX_ENSURE_REF(info);
    POSIX_ENSURE_REF(output);

    const struct s2n_hkdf_impl *hkdf_implementation = s2n_get_hkdf_implementation();
    POSIX_ENSURE_REF(hkdf_implementation);

    POSIX_GUARD(hkdf_implementation->hkdf_expand(hmac, alg, pseudo_rand_key, info, output));

    return S2N_SUCCESS;
}

int s2n_hkdf_expand_label(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *secret, const struct s2n_blob *label,
        const struct s2n_blob *context, struct s2n_blob *output)
{
    POSIX_ENSURE_REF(label);
    POSIX_ENSURE_REF(context);
    POSIX_ENSURE_REF(output);

    /* Per RFC8446: 7.1, a HKDF label is a 2 byte length field, and two 1...255 byte arrays with a one byte length field each. */
    uint8_t hkdf_label_buf[2 + 256 + 256];
    struct s2n_blob hkdf_label_blob = { 0 };
    struct s2n_stuffer hkdf_label = { 0 };

    POSIX_ENSURE_LTE(label->size, S2N_MAX_HKDF_EXPAND_LABEL_LENGTH);

    POSIX_GUARD(s2n_blob_init(&hkdf_label_blob, hkdf_label_buf, sizeof(hkdf_label_buf)));
    POSIX_GUARD(s2n_stuffer_init(&hkdf_label, &hkdf_label_blob));
    POSIX_GUARD(s2n_stuffer_write_uint16(&hkdf_label, output->size));
    POSIX_GUARD(s2n_stuffer_write_uint8(&hkdf_label, label->size + sizeof("tls13 ") - 1));
    POSIX_GUARD(s2n_stuffer_write_str(&hkdf_label, "tls13 "));
    POSIX_GUARD(s2n_stuffer_write(&hkdf_label, label));
    POSIX_GUARD(s2n_stuffer_write_uint8(&hkdf_label, context->size));
    POSIX_GUARD(s2n_stuffer_write(&hkdf_label, context));

    hkdf_label_blob.size = s2n_stuffer_data_available(&hkdf_label);
    POSIX_GUARD(s2n_hkdf_expand(hmac, alg, secret, &hkdf_label_blob, output));

    return S2N_SUCCESS;
}

int s2n_hkdf(struct s2n_hmac_state *hmac, s2n_hmac_algorithm alg, const struct s2n_blob *salt,
        const struct s2n_blob *key, const struct s2n_blob *info, struct s2n_blob *output)
{
    POSIX_ENSURE_REF(hmac);
    POSIX_ENSURE_REF(salt);
    POSIX_ENSURE_REF(key);
    POSIX_ENSURE_REF(info);
    POSIX_ENSURE_REF(output);

    const struct s2n_hkdf_impl *hkdf_implementation = s2n_get_hkdf_implementation();
    POSIX_ENSURE_REF(hkdf_implementation);

    POSIX_GUARD(hkdf_implementation->hkdf(hmac, alg, salt, key, info, output));

    return S2N_SUCCESS;
}

bool s2n_libcrypto_supports_hkdf()
{
#ifdef S2N_LIBCRYPTO_SUPPORTS_HKDF
    return true;
#else
    return false;
#endif
}
