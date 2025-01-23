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
#include <openssl/evp.h>

#include "crypto/s2n_cipher.h"
#include "crypto/s2n_ktls_crypto.h"
#include "tls/s2n_crypto.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

static bool s2n_aead_cipher_aes128_gcm_available(void)
{
#if defined(S2N_LIBCRYPTO_SUPPORTS_EVP_AEAD_TLS)
    return (EVP_aead_aes_128_gcm() ? true : false);
#else
    return (EVP_aes_128_gcm() ? true : false);
#endif
}

static bool s2n_aead_cipher_aes256_gcm_available(void)
{
#if defined(S2N_LIBCRYPTO_SUPPORTS_EVP_AEAD_TLS)
    return (EVP_aead_aes_256_gcm() ? true : false);
#else
    return (EVP_aes_256_gcm() ? true : false);
#endif
}

#if defined(S2N_LIBCRYPTO_SUPPORTS_EVP_AEAD_TLS) /* BoringSSL and AWS-LC AEAD API implementation */

static int s2n_aead_cipher_aes_gcm_encrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_REF(in);
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(iv);
    POSIX_ENSURE_REF(key);
    POSIX_ENSURE_REF(aad);

    /* The size of the |in| blob includes the size of the data and the size of the AES-GCM tag */
    POSIX_ENSURE_GTE(in->size, S2N_TLS_GCM_TAG_LEN);
    POSIX_ENSURE_GTE(out->size, in->size);
    POSIX_ENSURE_EQ(iv->size, S2N_TLS_GCM_IV_LEN);

    /* Adjust input length to account for the Tag length */
    size_t in_len = in->size - S2N_TLS_GCM_TAG_LEN;
    /* out_len is set by EVP_AEAD_CTX_seal and checked post operation */
    size_t out_len = 0;

    POSIX_GUARD_OSSL(EVP_AEAD_CTX_seal(key->evp_aead_ctx, out->data, &out_len, out->size, iv->data, iv->size, in->data, in_len, aad->data, aad->size), S2N_ERR_ENCRYPT);

    S2N_ERROR_IF((in_len + S2N_TLS_GCM_TAG_LEN) != out_len, S2N_ERR_ENCRYPT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes_gcm_decrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_REF(in);
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(iv);
    POSIX_ENSURE_REF(key);
    POSIX_ENSURE_REF(aad);

    POSIX_ENSURE_GTE(in->size, S2N_TLS_GCM_TAG_LEN);
    POSIX_ENSURE_GTE(out->size, in->size - S2N_TLS_GCM_TAG_LEN);
    POSIX_ENSURE_EQ(iv->size, S2N_TLS_GCM_IV_LEN);

    /* out_len is set by EVP_AEAD_CTX_open and checked post operation */
    size_t out_len = 0;

    POSIX_GUARD_OSSL(EVP_AEAD_CTX_open(key->evp_aead_ctx, out->data, &out_len, out->size, iv->data, iv->size, in->data, in->size, aad->data, aad->size), S2N_ERR_DECRYPT);

    S2N_ERROR_IF((in->size - S2N_TLS_GCM_TAG_LEN) != out_len, S2N_ERR_ENCRYPT);

    return S2N_SUCCESS;
}

static S2N_RESULT s2n_aead_cipher_aes128_gcm_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_REF(key);
    RESULT_ENSURE_REF(in);

    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_128_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_128_gcm_tls12(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes256_gcm_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_REF(key);
    RESULT_ENSURE_REF(in);

    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_256_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_256_gcm_tls12(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes128_gcm_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_REF(key);
    RESULT_ENSURE_REF(in);

    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_128_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_128_gcm_tls12(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes256_gcm_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_REF(key);
    RESULT_ENSURE_REF(in);

    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_256_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_256_gcm_tls12(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes128_gcm_set_encryption_key_tls13(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_REF(key);
    RESULT_ENSURE_REF(in);

    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_128_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_128_gcm_tls13(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes256_gcm_set_encryption_key_tls13(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_REF(key);
    RESULT_ENSURE_REF(in);

    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_256_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_256_gcm_tls13(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes128_gcm_set_decryption_key_tls13(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_REF(key);
    RESULT_ENSURE_REF(in);

    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_128_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_128_gcm_tls13(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes256_gcm_set_decryption_key_tls13(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_REF(key);
    RESULT_ENSURE_REF(in);

    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_256_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_256_gcm_tls13(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes_gcm_init(struct s2n_session_key *key)
{
    RESULT_ENSURE_REF(key);

    EVP_AEAD_CTX_zero(key->evp_aead_ctx);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes_gcm_destroy_key(struct s2n_session_key *key)
{
    RESULT_ENSURE_REF(key);

    EVP_AEAD_CTX_cleanup(key->evp_aead_ctx);

    return S2N_RESULT_OK;
}

#else /* Standard AES-GCM implementation */

static int s2n_aead_cipher_aes_gcm_encrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    /* The size of the |in| blob includes the size of the data and the size of the AES-GCM tag */
    POSIX_ENSURE_GTE(in->size, S2N_TLS_GCM_TAG_LEN);
    POSIX_ENSURE_GTE(out->size, in->size);
    POSIX_ENSURE_EQ(iv->size, S2N_TLS_GCM_IV_LEN);

    /* Initialize the IV */
    POSIX_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, NULL, NULL, NULL, iv->data), S2N_ERR_KEY_INIT);

    /* Adjust input length and buffer pointer to account for the Tag length */
    int in_len = in->size - S2N_TLS_GCM_TAG_LEN;
    uint8_t *tag_data = out->data + out->size - S2N_TLS_GCM_TAG_LEN;

    /* out_len is set by EVP_EncryptUpdate and checked post operation */
    int out_len = 0;
    /* Specify the AAD */
    POSIX_GUARD_OSSL(EVP_EncryptUpdate(key->evp_cipher_ctx, NULL, &out_len, aad->data, aad->size), S2N_ERR_ENCRYPT);

    /* Encrypt the data */
    POSIX_GUARD_OSSL(EVP_EncryptUpdate(key->evp_cipher_ctx, out->data, &out_len, in->data, in_len), S2N_ERR_ENCRYPT);

    /* When using AES-GCM, *out_len is the number of bytes written by EVP_EncryptUpdate. Since the tag is not written during this call, we do not take S2N_TLS_GCM_TAG_LEN into account */
    S2N_ERROR_IF(in_len != out_len, S2N_ERR_ENCRYPT);

    /* Finalize */
    POSIX_GUARD_OSSL(EVP_EncryptFinal_ex(key->evp_cipher_ctx, out->data, &out_len), S2N_ERR_ENCRYPT);

    /* write the tag */
    POSIX_GUARD_OSSL(EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_GET_TAG, S2N_TLS_GCM_TAG_LEN, tag_data), S2N_ERR_ENCRYPT);

    /* When using AES-GCM, EVP_EncryptFinal_ex does not write any bytes. So, we should expect *out_len = 0. */
    S2N_ERROR_IF(0 != out_len, S2N_ERR_ENCRYPT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes_gcm_decrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_GTE(in->size, S2N_TLS_GCM_TAG_LEN);
    POSIX_ENSURE_GTE(out->size, in->size);
    POSIX_ENSURE_EQ(iv->size, S2N_TLS_GCM_IV_LEN);

    /* Initialize the IV */
    POSIX_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, NULL, NULL, NULL, iv->data), S2N_ERR_KEY_INIT);

    /* Adjust input length and buffer pointer to account for the Tag length */
    int in_len = in->size - S2N_TLS_GCM_TAG_LEN;
    uint8_t *tag_data = in->data + in->size - S2N_TLS_GCM_TAG_LEN;

    /* Set the TAG */
    POSIX_GUARD_OSSL(EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_SET_TAG, S2N_TLS_GCM_TAG_LEN, tag_data), S2N_ERR_DECRYPT);

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

    return S2N_SUCCESS;
}

static S2N_RESULT s2n_aead_cipher_aes128_gcm_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_128_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, EVP_aes_128_gcm(), NULL, NULL, NULL), S2N_ERR_KEY_INIT);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_SET_IVLEN, S2N_TLS_GCM_IV_LEN, NULL);

    RESULT_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, NULL, NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes256_gcm_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_256_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, EVP_aes_256_gcm(), NULL, NULL, NULL), S2N_ERR_KEY_INIT);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_SET_IVLEN, S2N_TLS_GCM_IV_LEN, NULL);

    RESULT_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, NULL, NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes128_gcm_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_128_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, EVP_aes_128_gcm(), NULL, NULL, NULL), S2N_ERR_KEY_INIT);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_SET_IVLEN, S2N_TLS_GCM_IV_LEN, NULL);

    RESULT_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, NULL, NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes256_gcm_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, S2N_TLS_AES_256_GCM_KEY_LEN);

    RESULT_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, EVP_aes_256_gcm(), NULL, NULL, NULL), S2N_ERR_KEY_INIT);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_SET_IVLEN, S2N_TLS_GCM_IV_LEN, NULL);

    RESULT_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, NULL, NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes128_gcm_set_encryption_key_tls13(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_GUARD(s2n_aead_cipher_aes128_gcm_set_encryption_key(key, in));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes256_gcm_set_encryption_key_tls13(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_GUARD(s2n_aead_cipher_aes256_gcm_set_encryption_key(key, in));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes128_gcm_set_decryption_key_tls13(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_GUARD(s2n_aead_cipher_aes128_gcm_set_decryption_key(key, in));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes256_gcm_set_decryption_key_tls13(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_GUARD(s2n_aead_cipher_aes256_gcm_set_decryption_key(key, in));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes_gcm_init(struct s2n_session_key *key)
{
    RESULT_EVP_CTX_INIT(key->evp_cipher_ctx);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_aead_cipher_aes_gcm_destroy_key(struct s2n_session_key *key)
{
    EVP_CIPHER_CTX_cleanup(key->evp_cipher_ctx);

    return S2N_RESULT_OK;
}

#endif

static S2N_RESULT s2n_tls12_aead_cipher_aes128_gcm_set_ktls_info(
        struct s2n_ktls_crypto_info_inputs *in, struct s2n_ktls_crypto_info *out)
{
    RESULT_ENSURE_REF(in);
    RESULT_ENSURE_REF(out);

    s2n_ktls_crypto_info_tls12_aes_gcm_128 *crypto_info = &out->ciphers.aes_gcm_128;
    crypto_info->info.version = TLS_1_2_VERSION;
    crypto_info->info.cipher_type = TLS_CIPHER_AES_GCM_128;

    RESULT_ENSURE_LTE(sizeof(crypto_info->key), in->key.size);
    RESULT_CHECKED_MEMCPY(crypto_info->key, in->key.data, sizeof(crypto_info->key));
    RESULT_ENSURE_LTE(sizeof(crypto_info->rec_seq), in->seq.size);
    RESULT_CHECKED_MEMCPY(crypto_info->rec_seq, in->seq.data, sizeof(crypto_info->rec_seq));

    /* TLS1.2 uses partially explicit nonces. That means that although part of the
     * nonce is still fixed and implicit (the salt), the remainder is explicit
     * (written into the record) and must be unique per record. The RFC5288 suggests
     * using the sequence number as the explicit part.
     *
     * Therefore, ktls expects the salt to contain the iv derived from the secret
     * and should generate the remainder of the nonce per-record.
     *
     * See the TLS1.2 RFC:
     * - https://datatracker.ietf.org/doc/html/rfc5246#section-6.2.3.3
     * And RFC5288, which defines the TLS1.2 AES-GCM cipher suites:
     * - https://datatracker.ietf.org/doc/html/rfc5288#section-3
     */
    RESULT_ENSURE_LTE(sizeof(crypto_info->salt), in->iv.size);
    RESULT_CHECKED_MEMCPY(crypto_info->salt, in->iv.data, sizeof(crypto_info->salt));

    /* Because TLS1.2 uses partially explicit nonces, the kernel should not
     * use the iv in crypto_info but instead use a unique value for each record.
     *
     * As of this commit, Openssl has chosen to set the TLS1.2 IV to random
     * bytes when sending and all zeroes when receiving:
     * https://github.com/openssl/openssl/blob/de8e0851a1c0d22533801f081781a9f0be56c2c2/ssl/record/methods/ktls_meth.c#L197-L204
     * And GnuTLS has chosen to set the TLS1.2 IV to the sequence number:
     * https://github.com/gnutls/gnutls/blob/3f42ae70a1672673cb8f27c2dd3da1a34d1cbdd7/lib/system/ktls.c#L547-L550
     *
     * We (fairly arbitrarily) choose to also set it to the current sequence number.
     */
    RESULT_ENSURE_LTE(sizeof(crypto_info->iv), in->seq.size);
    RESULT_CHECKED_MEMCPY(crypto_info->iv, in->seq.data, sizeof(crypto_info->iv));

    RESULT_GUARD_POSIX(s2n_blob_init(&out->value, (uint8_t *) (void *) crypto_info,
            sizeof(s2n_ktls_crypto_info_tls12_aes_gcm_128)));
    return S2N_RESULT_OK;
}

/* TLS1.2 AES256 is configured like TLS1.2 AES128, but with a larger key size.
 * See TLS1.2 AES128 for details (particularly a discussion of salt + iv).
 */
static S2N_RESULT s2n_tls12_aead_cipher_aes256_gcm_set_ktls_info(
        struct s2n_ktls_crypto_info_inputs *in, struct s2n_ktls_crypto_info *out)
{
    RESULT_ENSURE_REF(in);
    RESULT_ENSURE_REF(out);

    s2n_ktls_crypto_info_tls12_aes_gcm_256 *crypto_info = &out->ciphers.aes_gcm_256;
    crypto_info->info.version = TLS_1_2_VERSION;
    crypto_info->info.cipher_type = TLS_CIPHER_AES_GCM_256;

    RESULT_ENSURE_LTE(sizeof(crypto_info->key), in->key.size);
    RESULT_CHECKED_MEMCPY(crypto_info->key, in->key.data, sizeof(crypto_info->key));
    RESULT_ENSURE_LTE(sizeof(crypto_info->rec_seq), in->seq.size);
    RESULT_CHECKED_MEMCPY(crypto_info->rec_seq, in->seq.data, sizeof(crypto_info->rec_seq));
    RESULT_ENSURE_LTE(sizeof(crypto_info->salt), in->iv.size);
    RESULT_CHECKED_MEMCPY(crypto_info->salt, in->iv.data, sizeof(crypto_info->salt));
    RESULT_ENSURE_LTE(sizeof(crypto_info->iv), in->seq.size);
    RESULT_CHECKED_MEMCPY(crypto_info->iv, in->seq.data, sizeof(crypto_info->iv));

    RESULT_GUARD_POSIX(s2n_blob_init(&out->value, (uint8_t *) (void *) crypto_info,
            sizeof(s2n_ktls_crypto_info_tls12_aes_gcm_256)));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_tls13_aead_cipher_aes128_gcm_set_ktls_info(
        struct s2n_ktls_crypto_info_inputs *in, struct s2n_ktls_crypto_info *out)
{
    RESULT_ENSURE_REF(in);
    RESULT_ENSURE_REF(out);

    s2n_ktls_crypto_info_tls12_aes_gcm_128 *crypto_info = &out->ciphers.aes_gcm_128;
    crypto_info->info.version = TLS_1_3_VERSION;
    crypto_info->info.cipher_type = TLS_CIPHER_AES_GCM_128;

    RESULT_ENSURE_LTE(sizeof(crypto_info->key), in->key.size);
    RESULT_CHECKED_MEMCPY(crypto_info->key, in->key.data, sizeof(crypto_info->key));
    RESULT_ENSURE_LTE(sizeof(crypto_info->rec_seq), in->seq.size);
    RESULT_CHECKED_MEMCPY(crypto_info->rec_seq, in->seq.data, sizeof(crypto_info->rec_seq));

    /* TLS1.3 uses fully implicit nonces. The fixed, implicit IV value derived from
     * the secret is xored with the sequence number to produce a unique per-record nonce.
     *
     * See the TLS1.3 RFC:
     * - https://www.rfc-editor.org/rfc/rfc8446.html#section-5.3
     *
     * ktls handles this with the same structure as TLS1.2 uses for its partially
     * explicit nonces by splitting the implicit IV between the salt and iv fields.
     */
    size_t salt_size = sizeof(crypto_info->salt);
    RESULT_ENSURE_LTE(salt_size, in->iv.size);
    RESULT_CHECKED_MEMCPY(crypto_info->salt, in->iv.data, salt_size);
    size_t iv_remainder = in->iv.size - salt_size;
    RESULT_ENSURE_LTE(sizeof(crypto_info->iv), iv_remainder);
    RESULT_CHECKED_MEMCPY(crypto_info->iv, in->iv.data + salt_size, sizeof(crypto_info->iv));

    RESULT_GUARD_POSIX(s2n_blob_init(&out->value, (uint8_t *) (void *) crypto_info,
            sizeof(s2n_ktls_crypto_info_tls12_aes_gcm_128)));
    return S2N_RESULT_OK;
}

/* TLS1.3 AES256 is configured like TLS1.3 AES128, but with a larger key size.
 * See TLS1.3 AES128 for details (particularly a discussion of salt + iv).
 */
static S2N_RESULT s2n_tls13_aead_cipher_aes256_gcm_set_ktls_info(
        struct s2n_ktls_crypto_info_inputs *in, struct s2n_ktls_crypto_info *out)
{
    RESULT_ENSURE_REF(in);
    RESULT_ENSURE_REF(out);

    s2n_ktls_crypto_info_tls12_aes_gcm_256 *crypto_info = &out->ciphers.aes_gcm_256;
    crypto_info->info.version = TLS_1_3_VERSION;
    crypto_info->info.cipher_type = TLS_CIPHER_AES_GCM_256;

    RESULT_ENSURE_LTE(sizeof(crypto_info->key), in->key.size);
    RESULT_CHECKED_MEMCPY(crypto_info->key, in->key.data, sizeof(crypto_info->key));
    RESULT_ENSURE_LTE(sizeof(crypto_info->rec_seq), in->seq.size);
    RESULT_CHECKED_MEMCPY(crypto_info->rec_seq, in->seq.data, sizeof(crypto_info->rec_seq));

    size_t salt_size = sizeof(crypto_info->salt);
    RESULT_ENSURE_LTE(salt_size, in->iv.size);
    RESULT_CHECKED_MEMCPY(crypto_info->salt, in->iv.data, salt_size);
    size_t iv_remainder = in->iv.size - salt_size;
    RESULT_ENSURE_LTE(sizeof(crypto_info->iv), iv_remainder);
    RESULT_CHECKED_MEMCPY(crypto_info->iv, in->iv.data + salt_size, sizeof(crypto_info->iv));

    RESULT_GUARD_POSIX(s2n_blob_init(&out->value, (uint8_t *) (void *) crypto_info,
            sizeof(s2n_ktls_crypto_info_tls12_aes_gcm_256)));
    return S2N_RESULT_OK;
}

const struct s2n_cipher s2n_aes128_gcm = {
    .key_material_size = S2N_TLS_AES_128_GCM_KEY_LEN,
    .type = S2N_AEAD,
    .io.aead = {
            .record_iv_size = S2N_TLS_GCM_EXPLICIT_IV_LEN,
            .fixed_iv_size = S2N_TLS_GCM_FIXED_IV_LEN,
            .tag_size = S2N_TLS_GCM_TAG_LEN,
            .decrypt = s2n_aead_cipher_aes_gcm_decrypt,
            .encrypt = s2n_aead_cipher_aes_gcm_encrypt },
    .is_available = s2n_aead_cipher_aes128_gcm_available,
    .init = s2n_aead_cipher_aes_gcm_init,
    .set_encryption_key = s2n_aead_cipher_aes128_gcm_set_encryption_key,
    .set_decryption_key = s2n_aead_cipher_aes128_gcm_set_decryption_key,
    .destroy_key = s2n_aead_cipher_aes_gcm_destroy_key,
    .set_ktls_info = s2n_tls12_aead_cipher_aes128_gcm_set_ktls_info,
};

const struct s2n_cipher s2n_aes256_gcm = {
    .key_material_size = S2N_TLS_AES_256_GCM_KEY_LEN,
    .type = S2N_AEAD,
    .io.aead = {
            .record_iv_size = S2N_TLS_GCM_EXPLICIT_IV_LEN,
            .fixed_iv_size = S2N_TLS_GCM_FIXED_IV_LEN,
            .tag_size = S2N_TLS_GCM_TAG_LEN,
            .decrypt = s2n_aead_cipher_aes_gcm_decrypt,
            .encrypt = s2n_aead_cipher_aes_gcm_encrypt },
    .is_available = s2n_aead_cipher_aes256_gcm_available,
    .init = s2n_aead_cipher_aes_gcm_init,
    .set_encryption_key = s2n_aead_cipher_aes256_gcm_set_encryption_key,
    .set_decryption_key = s2n_aead_cipher_aes256_gcm_set_decryption_key,
    .destroy_key = s2n_aead_cipher_aes_gcm_destroy_key,
    .set_ktls_info = s2n_tls12_aead_cipher_aes256_gcm_set_ktls_info,
};

/* TLS 1.3 GCM ciphers */
const struct s2n_cipher s2n_tls13_aes128_gcm = {
    .key_material_size = S2N_TLS_AES_128_GCM_KEY_LEN,
    .type = S2N_AEAD,
    .io.aead = {
            .record_iv_size = S2N_TLS13_RECORD_IV_LEN,
            .fixed_iv_size = S2N_TLS13_FIXED_IV_LEN,
            .tag_size = S2N_TLS_GCM_TAG_LEN,
            .decrypt = s2n_aead_cipher_aes_gcm_decrypt,
            .encrypt = s2n_aead_cipher_aes_gcm_encrypt },
    .is_available = s2n_aead_cipher_aes128_gcm_available,
    .init = s2n_aead_cipher_aes_gcm_init,
    .set_encryption_key = s2n_aead_cipher_aes128_gcm_set_encryption_key_tls13,
    .set_decryption_key = s2n_aead_cipher_aes128_gcm_set_decryption_key_tls13,
    .destroy_key = s2n_aead_cipher_aes_gcm_destroy_key,
    .set_ktls_info = s2n_tls13_aead_cipher_aes128_gcm_set_ktls_info,
};

const struct s2n_cipher s2n_tls13_aes256_gcm = {
    .key_material_size = S2N_TLS_AES_256_GCM_KEY_LEN,
    .type = S2N_AEAD,
    .io.aead = {
            .record_iv_size = S2N_TLS13_RECORD_IV_LEN,
            .fixed_iv_size = S2N_TLS13_FIXED_IV_LEN,
            .tag_size = S2N_TLS_GCM_TAG_LEN,
            .decrypt = s2n_aead_cipher_aes_gcm_decrypt,
            .encrypt = s2n_aead_cipher_aes_gcm_encrypt },
    .is_available = s2n_aead_cipher_aes256_gcm_available,
    .init = s2n_aead_cipher_aes_gcm_init,
    .set_encryption_key = s2n_aead_cipher_aes256_gcm_set_encryption_key_tls13,
    .set_decryption_key = s2n_aead_cipher_aes256_gcm_set_decryption_key_tls13,
    .destroy_key = s2n_aead_cipher_aes_gcm_destroy_key,
    .set_ktls_info = s2n_tls13_aead_cipher_aes256_gcm_set_ktls_info,
};
