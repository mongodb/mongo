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

#include <openssl/aes.h>

#include "crypto/s2n_cipher.h"
#include "crypto/s2n_openssl.h"
#include "error/s2n_errno.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

static bool s2n_cbc_cipher_aes128_available(void)
{
    return (EVP_aes_128_cbc() ? true : false);
}

static bool s2n_cbc_cipher_aes256_available(void)
{
    return (EVP_aes_256_cbc() ? true : false);
}

static int s2n_cbc_cipher_aes_encrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_GTE(out->size, in->size);

    POSIX_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, NULL, NULL, NULL, iv->data), S2N_ERR_KEY_INIT);

    /* len is set by EVP_EncryptUpdate and checked post operation */
    int len = 0;
    POSIX_GUARD_OSSL(EVP_EncryptUpdate(key->evp_cipher_ctx, out->data, &len, in->data, in->size), S2N_ERR_ENCRYPT);
    POSIX_ENSURE((int64_t) len == (int64_t) in->size, S2N_ERR_ENCRYPT);

    return 0;
}

int s2n_cbc_cipher_aes_decrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_GTE(out->size, in->size);

    POSIX_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, NULL, NULL, NULL, iv->data), S2N_ERR_KEY_INIT);

    /* len is set by EVP_DecryptUpdate. It is not checked here but padding is manually removed and therefore
     * the decryption operation is validated. */
    int len = 0;
    POSIX_GUARD_OSSL(EVP_DecryptUpdate(key->evp_cipher_ctx, out->data, &len, in->data, in->size), S2N_ERR_DECRYPT);

    return 0;
}

S2N_RESULT s2n_cbc_cipher_aes128_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 128 / 8);

    /* Always returns 1 */
    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    RESULT_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, EVP_aes_128_cbc(), NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_cbc_cipher_aes128_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 128 / 8);

    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    RESULT_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, EVP_aes_128_cbc(), NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_cbc_cipher_aes256_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 256 / 8);

    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    RESULT_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, EVP_aes_256_cbc(), NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_cbc_cipher_aes256_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 256 / 8);

    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    RESULT_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, EVP_aes_256_cbc(), NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_cbc_cipher_aes_init(struct s2n_session_key *key)
{
    RESULT_EVP_CTX_INIT(key->evp_cipher_ctx);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_cbc_cipher_aes_destroy_key(struct s2n_session_key *key)
{
    EVP_CIPHER_CTX_cleanup(key->evp_cipher_ctx);

    return S2N_RESULT_OK;
}

const struct s2n_cipher s2n_aes128 = {
    .key_material_size = 16,
    .type = S2N_CBC,
    .io.cbc = {
            .block_size = 16,
            .record_iv_size = 16,
            .decrypt = s2n_cbc_cipher_aes_decrypt,
            .encrypt = s2n_cbc_cipher_aes_encrypt },
    .is_available = s2n_cbc_cipher_aes128_available,
    .init = s2n_cbc_cipher_aes_init,
    .set_decryption_key = s2n_cbc_cipher_aes128_set_decryption_key,
    .set_encryption_key = s2n_cbc_cipher_aes128_set_encryption_key,
    .destroy_key = s2n_cbc_cipher_aes_destroy_key,
};

const struct s2n_cipher s2n_aes256 = {
    .key_material_size = 32,
    .type = S2N_CBC,
    .io.cbc = {
            .block_size = 16,
            .record_iv_size = 16,
            .decrypt = s2n_cbc_cipher_aes_decrypt,
            .encrypt = s2n_cbc_cipher_aes_encrypt },
    .is_available = s2n_cbc_cipher_aes256_available,
    .init = s2n_cbc_cipher_aes_init,
    .set_decryption_key = s2n_cbc_cipher_aes256_set_decryption_key,
    .set_encryption_key = s2n_cbc_cipher_aes256_set_encryption_key,
    .destroy_key = s2n_cbc_cipher_aes_destroy_key,
};
