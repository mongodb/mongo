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

#include <openssl/evp.h>

#include "crypto/s2n_cipher.h"
#include "crypto/s2n_openssl.h"
#include "tls/s2n_crypto.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

/* We support two different backing implementations of ChaCha20-Poly1305: one
 * implementation for OpenSSL (>= 1.1.0, see
 * https://www.openssl.org/news/cl110.txt) and one implementation for BoringSSL
 * and AWS-LC. LibreSSL supports ChaCha20-Poly1305, but the interface is
 * different.
 * Note, the order in the if/elif below matters because both BoringSSL and
 * AWS-LC define OPENSSL_VERSION_NUMBER. */
#if defined(OPENSSL_IS_BORINGSSL) || defined(OPENSSL_IS_AWSLC)
    #define S2N_CHACHA20_POLY1305_AVAILABLE_BSSL_AWSLC
#elif (S2N_OPENSSL_VERSION_AT_LEAST(1, 1, 0) && !defined(LIBRESSL_VERSION_NUMBER))
    #define S2N_CHACHA20_POLY1305_AVAILABLE_OSSL
#endif

static bool s2n_aead_chacha20_poly1305_available(void)
{
#if defined(S2N_CHACHA20_POLY1305_AVAILABLE_OSSL) || defined(S2N_CHACHA20_POLY1305_AVAILABLE_BSSL_AWSLC)
    return true;
#else
    return false;
#endif
}

#if defined(S2N_CHACHA20_POLY1305_AVAILABLE_OSSL) /* OpenSSL implementation */

static int s2n_aead_chacha20_poly1305_encrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_GTE(in->size, S2N_TLS_CHACHA20_POLY1305_TAG_LEN);
    /* The size of the |in| blob includes the size of the data and the size of the ChaCha20-Poly1305 tag */
    POSIX_ENSURE_GTE(out->size, in->size);
    POSIX_ENSURE_EQ(iv->size, S2N_TLS_CHACHA20_POLY1305_IV_LEN);

    /* Initialize the IV */
    POSIX_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, NULL, NULL, NULL, iv->data), S2N_ERR_KEY_INIT);

    /* Adjust input length and buffer pointer to account for the Tag length */
    int in_len = in->size - S2N_TLS_CHACHA20_POLY1305_TAG_LEN;
    uint8_t *tag_data = out->data + out->size - S2N_TLS_CHACHA20_POLY1305_TAG_LEN;

    /* out_len is set by EVP_EncryptUpdate and checked post operation */
    int out_len = 0;
    /* Specify the AAD */
    POSIX_GUARD_OSSL(EVP_EncryptUpdate(key->evp_cipher_ctx, NULL, &out_len, aad->data, aad->size), S2N_ERR_ENCRYPT);

    /* Encrypt the data */
    POSIX_GUARD_OSSL(EVP_EncryptUpdate(key->evp_cipher_ctx, out->data, &out_len, in->data, in_len), S2N_ERR_ENCRYPT);

    /* For OpenSSL 1.1.0 and 1.1.1, when using ChaCha20-Poly1305, *out_len is the number of bytes written by EVP_EncryptUpdate. Since the tag is not written during this call, we do not take S2N_TLS_CHACHA20_POLY1305_TAG_LEN into account */
    S2N_ERROR_IF(in_len != out_len, S2N_ERR_ENCRYPT);

    /* Finalize */
    POSIX_GUARD_OSSL(EVP_EncryptFinal_ex(key->evp_cipher_ctx, out->data, &out_len), S2N_ERR_ENCRYPT);

    /* Write the tag */
    POSIX_GUARD_OSSL(EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_AEAD_GET_TAG, S2N_TLS_CHACHA20_POLY1305_TAG_LEN, tag_data), S2N_ERR_ENCRYPT);

    /* For OpenSSL 1.1.0 and 1.1.1, when using ChaCha20-Poly1305, EVP_EncryptFinal_ex does not write any bytes. So, we should expect *out_len = 0. */
    S2N_ERROR_IF(0 != out_len, S2N_ERR_ENCRYPT);

    return 0;
}

static int s2n_aead_chacha20_poly1305_decrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_GTE(in->size, S2N_TLS_CHACHA20_POLY1305_TAG_LEN);
    POSIX_ENSURE_GTE(out->size, in->size - S2N_TLS_CHACHA20_POLY1305_TAG_LEN);
    POSIX_ENSURE_EQ(iv->size, S2N_TLS_CHACHA20_POLY1305_IV_LEN);

    /* Initialize the IV */
    POSIX_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, NULL, NULL, NULL, iv->data), S2N_ERR_KEY_INIT);

    /* Adjust input length and buffer pointer to account for the Tag length */
    int in_len = in->size - S2N_TLS_CHACHA20_POLY1305_TAG_LEN;
    uint8_t *tag_data = in->data + in->size - S2N_TLS_CHACHA20_POLY1305_TAG_LEN;

    /* Set the TAG */
    POSIX_GUARD_OSSL(EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_SET_TAG, S2N_TLS_CHACHA20_POLY1305_TAG_LEN, tag_data), S2N_ERR_DECRYPT);

    /* out_len is set by EVP_DecryptUpdate. While we verify the content of out_len in
     * s2n_aead_chacha20_poly1305_encrypt, we refrain from this here. This is to avoid
     * doing any branching before the ciphertext is verified. */
    int out_len = 0;
    /* Specify the AAD */
    POSIX_GUARD_OSSL(EVP_DecryptUpdate(key->evp_cipher_ctx, NULL, &out_len, aad->data, aad->size), S2N_ERR_DECRYPT);

    int evp_decrypt_rc = 1;
    /* Decrypt the data, but don't short circuit tag verification. EVP_Decrypt* return 0 on failure, 1 for success. */
    evp_decrypt_rc &= EVP_DecryptUpdate(key->evp_cipher_ctx, out->data, &out_len, in->data, in_len);

    /* Verify the tag */
    evp_decrypt_rc &= EVP_DecryptFinal_ex(key->evp_cipher_ctx, out->data, &out_len);

    S2N_ERROR_IF(evp_decrypt_rc != 1, S2N_ERR_DECRYPT);

    return 0;
}

static S2N_RESULT s2n_aead_chacha20_poly1305_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, S2N_TLS_CHACHA20_POLY1305_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL), S2N_ERR_KEY_INIT);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_AEAD_SET_IVLEN, S2N_TLS_CHACHA20_POLY1305_IV_LEN, NULL);

    RESULT_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, NULL, NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_chacha20_poly1305_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, S2N_TLS_CHACHA20_POLY1305_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL), S2N_ERR_KEY_INIT);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_AEAD_SET_IVLEN, S2N_TLS_CHACHA20_POLY1305_IV_LEN, NULL);

    RESULT_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, NULL, NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_chacha20_poly1305_init(struct s2n_session_key *key)
{
    RESULT_EVP_CTX_INIT(key->evp_cipher_ctx);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_chacha20_poly1305_destroy_key(struct s2n_session_key *key)
{
    EVP_CIPHER_CTX_cleanup(key->evp_cipher_ctx);

    return S2N_RESULT_OK;
}

#elif defined(S2N_CHACHA20_POLY1305_AVAILABLE_BSSL_AWSLC) /* BoringSSL and AWS-LC implementation */

static int s2n_aead_chacha20_poly1305_encrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_GTE(in->size, S2N_TLS_CHACHA20_POLY1305_TAG_LEN);
    /* The size of the |in| blob includes the size of the data and the size of the ChaCha20-Poly1305 tag */
    POSIX_ENSURE_GTE(out->size, in->size);
    POSIX_ENSURE_EQ(iv->size, S2N_TLS_CHACHA20_POLY1305_IV_LEN);

    /* Adjust input length to account for the Tag length */
    size_t in_len = in->size - S2N_TLS_CHACHA20_POLY1305_TAG_LEN;
    /* out_len is set by EVP_AEAD_CTX_seal and checked post operation */
    size_t out_len = 0;

    POSIX_GUARD_OSSL(EVP_AEAD_CTX_seal(key->evp_aead_ctx, out->data, &out_len, out->size, iv->data, iv->size, in->data, in_len, aad->data, aad->size), S2N_ERR_ENCRYPT);

    S2N_ERROR_IF((in_len + S2N_TLS_CHACHA20_POLY1305_TAG_LEN) != out_len, S2N_ERR_ENCRYPT);

    return 0;
}

static int s2n_aead_chacha20_poly1305_decrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_GTE(in->size, S2N_TLS_CHACHA20_POLY1305_TAG_LEN);
    POSIX_ENSURE_GTE(out->size, in->size - S2N_TLS_CHACHA20_POLY1305_TAG_LEN);
    POSIX_ENSURE_EQ(iv->size, S2N_TLS_CHACHA20_POLY1305_IV_LEN);

    /* out_len is set by EVP_AEAD_CTX_open and checked post operation */
    size_t out_len = 0;

    POSIX_GUARD_OSSL(EVP_AEAD_CTX_open(key->evp_aead_ctx, out->data, &out_len, out->size, iv->data, iv->size, in->data, in->size, aad->data, aad->size), S2N_ERR_DECRYPT);

    S2N_ERROR_IF((in->size - S2N_TLS_CHACHA20_POLY1305_TAG_LEN) != out_len, S2N_ERR_ENCRYPT);

    return 0;
}

static S2N_RESULT s2n_aead_chacha20_poly1305_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, S2N_TLS_CHACHA20_POLY1305_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_chacha20_poly1305(), in->data, in->size, S2N_TLS_CHACHA20_POLY1305_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_chacha20_poly1305_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, S2N_TLS_CHACHA20_POLY1305_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_chacha20_poly1305(), in->data, in->size, S2N_TLS_CHACHA20_POLY1305_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_chacha20_poly1305_init(struct s2n_session_key *key)
{
    EVP_AEAD_CTX_zero(key->evp_aead_ctx);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_chacha20_poly1305_destroy_key(struct s2n_session_key *key)
{
    EVP_AEAD_CTX_cleanup(key->evp_aead_ctx);

    return S2N_RESULT_OK;
}

#else /* No ChaCha20-Poly1305 implementation exists for chosen cryptographic provider (E.g Openssl 1.0.x) */

static int s2n_aead_chacha20_poly1305_encrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_BAIL(S2N_ERR_ENCRYPT);
}

static int s2n_aead_chacha20_poly1305_decrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_BAIL(S2N_ERR_DECRYPT);
}

static S2N_RESULT s2n_aead_chacha20_poly1305_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_BAIL(S2N_ERR_KEY_INIT);
}

static S2N_RESULT s2n_aead_chacha20_poly1305_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_BAIL(S2N_ERR_KEY_INIT);
}

static S2N_RESULT s2n_aead_chacha20_poly1305_init(struct s2n_session_key *key)
{
    RESULT_BAIL(S2N_ERR_KEY_INIT);
}

static S2N_RESULT s2n_aead_chacha20_poly1305_destroy_key(struct s2n_session_key *key)
{
    RESULT_BAIL(S2N_ERR_KEY_DESTROY);
}

#endif

const struct s2n_cipher s2n_chacha20_poly1305 = {
    .key_material_size = S2N_TLS_CHACHA20_POLY1305_KEY_LEN,
    .type = S2N_AEAD,
    .io.aead = {
            .record_iv_size = S2N_TLS_CHACHA20_POLY1305_EXPLICIT_IV_LEN,
            .fixed_iv_size = S2N_TLS_CHACHA20_POLY1305_FIXED_IV_LEN,
            .tag_size = S2N_TLS_CHACHA20_POLY1305_TAG_LEN,
            .decrypt = s2n_aead_chacha20_poly1305_decrypt,
            .encrypt = s2n_aead_chacha20_poly1305_encrypt },
    .is_available = s2n_aead_chacha20_poly1305_available,
    .init = s2n_aead_chacha20_poly1305_init,
    .set_encryption_key = s2n_aead_chacha20_poly1305_set_encryption_key,
    .set_decryption_key = s2n_aead_chacha20_poly1305_set_decryption_key,
    .destroy_key = s2n_aead_chacha20_poly1305_destroy_key,
};
