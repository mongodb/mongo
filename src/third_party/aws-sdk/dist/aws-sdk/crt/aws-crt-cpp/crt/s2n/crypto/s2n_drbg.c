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

#include "crypto/s2n_drbg.h"

#include <openssl/evp.h>
#include <sys/param.h>

#include "utils/s2n_blob.h"
#include "utils/s2n_random.h"
#include "utils/s2n_safety.h"

static bool ignore_prediction_resistance_for_testing = false;

#define s2n_drbg_key_size(drgb)  EVP_CIPHER_CTX_key_length((drbg)->ctx)
#define s2n_drbg_seed_size(drgb) (S2N_DRBG_BLOCK_SIZE + s2n_drbg_key_size(drgb))

/* This function is the same as s2n_increment_sequence_number
    but it does not check for overflow, since overflow is
    acceptable in DRBG */
S2N_RESULT s2n_increment_drbg_counter(struct s2n_blob *counter)
{
    for (uint32_t i = (uint32_t) counter->size; i > 0; i--) {
        counter->data[i - 1] += 1;
        if (counter->data[i - 1]) {
            break;
        }

        /* seq[i] wrapped, so let it carry */
    }
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_drbg_block_encrypt(EVP_CIPHER_CTX *ctx, uint8_t in[S2N_DRBG_BLOCK_SIZE], uint8_t out[S2N_DRBG_BLOCK_SIZE])
{
    RESULT_ENSURE_REF(ctx);

    /* len is set by EVP_EncryptUpdate and checked post operation */
    int len = S2N_DRBG_BLOCK_SIZE;
    RESULT_GUARD_OSSL(EVP_EncryptUpdate(ctx, out, &len, in, S2N_DRBG_BLOCK_SIZE), S2N_ERR_DRBG);
    RESULT_ENSURE_EQ(len, S2N_DRBG_BLOCK_SIZE);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_drbg_bits(struct s2n_drbg *drbg, struct s2n_blob *out)
{
    RESULT_ENSURE_REF(drbg);
    RESULT_ENSURE_REF(drbg->ctx);
    RESULT_ENSURE_REF(out);

    struct s2n_blob value = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&value, drbg->v, sizeof(drbg->v)));
    uint32_t block_aligned_size = out->size - (out->size % S2N_DRBG_BLOCK_SIZE);

    /* Per NIST SP800-90A 10.2.1.2: */
    for (size_t i = 0; i < block_aligned_size; i += S2N_DRBG_BLOCK_SIZE) {
        RESULT_GUARD(s2n_increment_drbg_counter(&value));
        RESULT_GUARD(s2n_drbg_block_encrypt(drbg->ctx, drbg->v, out->data + i));
        drbg->bytes_used += S2N_DRBG_BLOCK_SIZE;
    }

    if (out->size <= block_aligned_size) {
        return S2N_RESULT_OK;
    }

    uint8_t spare_block[S2N_DRBG_BLOCK_SIZE];
    RESULT_GUARD(s2n_increment_drbg_counter(&value));
    RESULT_GUARD(s2n_drbg_block_encrypt(drbg->ctx, drbg->v, spare_block));
    drbg->bytes_used += S2N_DRBG_BLOCK_SIZE;

    RESULT_CHECKED_MEMCPY(out->data + block_aligned_size, spare_block, out->size - block_aligned_size);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_drbg_update(struct s2n_drbg *drbg, struct s2n_blob *provided_data)
{
    RESULT_ENSURE_REF(drbg);
    RESULT_ENSURE_REF(drbg->ctx);
    RESULT_ENSURE_REF(provided_data);

    RESULT_STACK_BLOB(temp_blob, s2n_drbg_seed_size(drgb), S2N_DRBG_MAX_SEED_SIZE);

    RESULT_ENSURE_EQ(provided_data->size, (uint32_t) s2n_drbg_seed_size(drbg));

    RESULT_GUARD(s2n_drbg_bits(drbg, &temp_blob));

    /* XOR in the provided data */
    for (uint32_t i = 0; i < provided_data->size; i++) {
        temp_blob.data[i] ^= provided_data->data[i];
    }

    /* Update the key and value */
    RESULT_GUARD_OSSL(EVP_EncryptInit_ex(drbg->ctx, NULL, NULL, temp_blob.data, NULL), S2N_ERR_DRBG);

    RESULT_CHECKED_MEMCPY(drbg->v, temp_blob.data + s2n_drbg_key_size(drbg), S2N_DRBG_BLOCK_SIZE);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_drbg_mix_in_entropy(struct s2n_drbg *drbg, struct s2n_blob *entropy, struct s2n_blob *ps)
{
    RESULT_ENSURE_REF(drbg);
    RESULT_ENSURE_REF(drbg->ctx);
    RESULT_ENSURE_REF(entropy);

    RESULT_ENSURE_GTE(entropy->size, ps->size);

    for (uint32_t i = 0; i < ps->size; i++) {
        entropy->data[i] ^= ps->data[i];
    }

    RESULT_GUARD(s2n_drbg_update(drbg, entropy));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_drbg_seed(struct s2n_drbg *drbg, struct s2n_blob *ps)
{
    RESULT_STACK_BLOB(blob, s2n_drbg_seed_size(drbg), S2N_DRBG_MAX_SEED_SIZE);

    RESULT_GUARD(s2n_get_seed_entropy(&blob));
    RESULT_GUARD(s2n_drbg_mix_in_entropy(drbg, &blob, ps));

    drbg->bytes_used = 0;

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_drbg_mix(struct s2n_drbg *drbg, struct s2n_blob *ps)
{
    if (s2n_unlikely(ignore_prediction_resistance_for_testing)) {
        RESULT_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);
        return S2N_RESULT_OK;
    }

    RESULT_STACK_BLOB(blob, s2n_drbg_seed_size(drbg), S2N_DRBG_MAX_SEED_SIZE);

    RESULT_GUARD(s2n_get_mix_entropy(&blob));
    RESULT_GUARD(s2n_drbg_mix_in_entropy(drbg, &blob, ps));

    drbg->mixes += 1;

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_drbg_instantiate(struct s2n_drbg *drbg, struct s2n_blob *personalization_string, const s2n_drbg_mode mode)
{
    RESULT_ENSURE_REF(drbg);
    RESULT_ENSURE_REF(personalization_string);

    drbg->ctx = EVP_CIPHER_CTX_new();
    RESULT_GUARD_PTR(drbg->ctx);

    RESULT_EVP_CTX_INIT(drbg->ctx);

    switch (mode) {
        case S2N_AES_128_CTR_NO_DF_PR:
            RESULT_GUARD_OSSL(EVP_EncryptInit_ex(drbg->ctx, EVP_aes_128_ecb(), NULL, NULL, NULL), S2N_ERR_DRBG);
            break;
        case S2N_AES_256_CTR_NO_DF_PR:
            RESULT_GUARD_OSSL(EVP_EncryptInit_ex(drbg->ctx, EVP_aes_256_ecb(), NULL, NULL, NULL), S2N_ERR_DRBG);
            break;
        default:
            RESULT_BAIL(S2N_ERR_DRBG);
    }

    RESULT_ENSURE_LTE(s2n_drbg_key_size(drbg), S2N_DRBG_MAX_KEY_SIZE);
    RESULT_ENSURE_LTE(s2n_drbg_seed_size(drbg), S2N_DRBG_MAX_SEED_SIZE);

    static const uint8_t zero_key[S2N_DRBG_MAX_KEY_SIZE] = { 0 };

    /* Start off with zeroed data, per 10.2.1.3.1 item 4 and 5 */
    memset(drbg->v, 0, sizeof(drbg->v));
    RESULT_GUARD_OSSL(EVP_EncryptInit_ex(drbg->ctx, NULL, NULL, zero_key, NULL), S2N_ERR_DRBG);

    /* Copy the personalization string */
    RESULT_STACK_BLOB(ps, s2n_drbg_seed_size(drbg), S2N_DRBG_MAX_SEED_SIZE);
    RESULT_GUARD_POSIX(s2n_blob_zero(&ps));

    RESULT_CHECKED_MEMCPY(ps.data, personalization_string->data, MIN(ps.size, personalization_string->size));

    /* Seed the DRBG */
    RESULT_GUARD(s2n_drbg_seed(drbg, &ps));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_drbg_generate(struct s2n_drbg *drbg, struct s2n_blob *blob)
{
    RESULT_ENSURE_REF(drbg);
    RESULT_ENSURE_REF(drbg->ctx);

    RESULT_STACK_BLOB(zeros, s2n_drbg_seed_size(drbg), S2N_DRBG_MAX_SEED_SIZE);

    RESULT_ENSURE(blob->size <= S2N_DRBG_GENERATE_LIMIT, S2N_ERR_DRBG_REQUEST_SIZE);

    /* Mix in additional entropy for every randomness generation call. This
     * defense mechanism is referred to as "prediction resistance".
     * If we ever relax this defense, we must:
     *  1. Implement reseeding according to limit specified in
     *     NIST SP800-90A 10.2.1 Table 3.
     *  2. Re-consider whether the current fork detection strategy is still
     *     sufficient.
     */
    RESULT_GUARD(s2n_drbg_mix(drbg, &zeros));
    RESULT_GUARD(s2n_drbg_bits(drbg, blob));
    RESULT_GUARD(s2n_drbg_update(drbg, &zeros));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_drbg_wipe(struct s2n_drbg *drbg)
{
    RESULT_ENSURE_REF(drbg);

    if (drbg->ctx) {
        RESULT_GUARD_OSSL(EVP_CIPHER_CTX_cleanup(drbg->ctx), S2N_ERR_DRBG);

        EVP_CIPHER_CTX_free(drbg->ctx);
        drbg->ctx = NULL;
    }

    *drbg = (struct s2n_drbg){ 0 };
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_drbg_bytes_used(struct s2n_drbg *drbg, uint64_t *bytes_used)
{
    RESULT_ENSURE_REF(drbg);
    RESULT_ENSURE_REF(bytes_used);

    *bytes_used = drbg->bytes_used;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_ignore_prediction_resistance_for_testing(bool ignore_bool)
{
    RESULT_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);

    ignore_prediction_resistance_for_testing = ignore_bool;

    return S2N_RESULT_OK;
}
