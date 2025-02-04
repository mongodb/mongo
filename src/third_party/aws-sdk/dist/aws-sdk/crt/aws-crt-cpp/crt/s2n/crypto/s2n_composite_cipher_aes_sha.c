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
#include <openssl/sha.h>

#include "crypto/s2n_cipher.h"
#include "crypto/s2n_fips.h"
#include "crypto/s2n_openssl.h"
#include "tls/s2n_crypto.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

/* LibreSSL and BoringSSL support the cipher, but the interface is different from Openssl's. We
 * should define a separate s2n_cipher struct for LibreSSL and BoringSSL.
 */
#if !defined(LIBRESSL_VERSION_NUMBER) && !defined(OPENSSL_IS_BORINGSSL)
    /* Symbols for AES-SHA1-CBC composite ciphers were added in Openssl 1.0.1
     * These composite ciphers exhibit erratic behavior in LibreSSL releases.
     */
    #if S2N_OPENSSL_VERSION_AT_LEAST(1, 0, 1)
        #define S2N_AES_SHA1_COMPOSITE_AVAILABLE
    #endif
    #if defined(AWSLC_API_VERSION) && (AWSLC_API_VERSION <= 17)
        #undef S2N_AES_SHA1_COMPOSITE_AVAILABLE
    #endif
    /* Symbols for AES-SHA256-CBC composite ciphers were added in Openssl 1.0.2
     * See https://www.openssl.org/news/cl102.txt
     * These composite ciphers exhibit erratic behavior in LibreSSL releases.
     */
    #if S2N_OPENSSL_VERSION_AT_LEAST(1, 0, 2)
        #define S2N_AES_SHA256_COMPOSITE_AVAILABLE
    #endif
    #if defined(AWSLC_API_VERSION) && (AWSLC_API_VERSION <= 17)
        #undef S2N_AES_SHA256_COMPOSITE_AVAILABLE
    #endif
#endif

/* Silly accessors, but we avoid using version macro guards in multiple places */
static const EVP_CIPHER *s2n_evp_aes_128_cbc_hmac_sha1(void)
{
#if defined(S2N_AES_SHA1_COMPOSITE_AVAILABLE)
    return EVP_aes_128_cbc_hmac_sha1();
#else
    return NULL;
#endif
}

static const EVP_CIPHER *s2n_evp_aes_256_cbc_hmac_sha1(void)
{
#if defined(S2N_AES_SHA1_COMPOSITE_AVAILABLE)
    return EVP_aes_256_cbc_hmac_sha1();
#else
    return NULL;
#endif
}

static const EVP_CIPHER *s2n_evp_aes_128_cbc_hmac_sha256(void)
{
#if defined(S2N_AES_SHA256_COMPOSITE_AVAILABLE)
    return EVP_aes_128_cbc_hmac_sha256();
#else
    return NULL;
#endif
}

static const EVP_CIPHER *s2n_evp_aes_256_cbc_hmac_sha256(void)
{
#if defined(S2N_AES_SHA256_COMPOSITE_AVAILABLE)
    return EVP_aes_256_cbc_hmac_sha256();
#else
    return NULL;
#endif
}

static bool s2n_composite_cipher_aes128_sha_available(void)
{
    /* EVP_aes_128_cbc_hmac_sha1() returns NULL if the implementations aren't available.
     * See https://github.com/openssl/openssl/blob/master/crypto/evp/e_aes_cbc_hmac_sha1.c#L952
     *
     * Composite ciphers cannot be used when FIPS mode is set. Ciphers require the
     * EVP_CIPH_FLAG_FIPS OpenSSL flag to be set for use when in FIPS mode, and composite
     * ciphers cause OpenSSL errors due to the lack of the flag.
     */
    return (!s2n_is_in_fips_mode() && s2n_evp_aes_128_cbc_hmac_sha1() ? true : false);
}

static bool s2n_composite_cipher_aes256_sha_available(void)
{
    /* Composite ciphers cannot be used when FIPS mode is set. Ciphers require the
     * EVP_CIPH_FLAG_FIPS OpenSSL flag to be set for use when in FIPS mode, and composite
     * ciphers cause OpenSSL errors due to the lack of the flag.
     */
    return (!s2n_is_in_fips_mode() && s2n_evp_aes_256_cbc_hmac_sha1() ? true : false);
}

static bool s2n_composite_cipher_aes128_sha256_available(void)
{
    /* Composite ciphers cannot be used when FIPS mode is set. Ciphers require the
     * EVP_CIPH_FLAG_FIPS OpenSSL flag to be set for use when in FIPS mode, and composite
     * ciphers cause OpenSSL errors due to the lack of the flag.
     */
    return (!s2n_is_in_fips_mode() && s2n_evp_aes_128_cbc_hmac_sha256() ? true : false);
}

static bool s2n_composite_cipher_aes256_sha256_available(void)
{
    /* Composite ciphers cannot be used when FIPS mode is set. Ciphers require the
     * EVP_CIPH_FLAG_FIPS OpenSSL flag to be set for use when in FIPS mode, and composite
     * ciphers cause OpenSSL errors due to the lack of the flag.
     */
    return (!s2n_is_in_fips_mode() && s2n_evp_aes_256_cbc_hmac_sha256() ? true : false);
}

static int s2n_composite_cipher_aes_sha_initial_hmac(struct s2n_session_key *key, uint8_t *sequence_number, uint8_t content_type,
        uint16_t protocol_version, uint16_t payload_and_eiv_len, int *extra)
{
    /* BoringSSL and AWS-LC(AWSLC_API_VERSION <= 17) do not support these composite ciphers with the existing EVP API, and they took out the
     * constants used below. This method should never be called with BoringSSL or AWS-LC(AWSLC_API_VERSION <= 17) because the isAvaliable checked
     * will fail. Instead of defining a possibly dangerous default or hard coding this to 0x16 error out with BoringSSL and AWS-LC(AWSLC_API_VERSION <= 17).
     */
#if defined(OPENSSL_IS_BORINGSSL) || (defined(AWSLC_API_VERSION) && (AWSLC_API_VERSION <= 17))
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
#else
    uint8_t ctrl_buf[S2N_TLS12_AAD_LEN];
    struct s2n_blob ctrl_blob = { 0 };
    POSIX_GUARD(s2n_blob_init(&ctrl_blob, ctrl_buf, S2N_TLS12_AAD_LEN));
    struct s2n_stuffer ctrl_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&ctrl_stuffer, &ctrl_blob));

    POSIX_GUARD(s2n_stuffer_write_bytes(&ctrl_stuffer, sequence_number, S2N_TLS_SEQUENCE_NUM_LEN));
    POSIX_GUARD(s2n_stuffer_write_uint8(&ctrl_stuffer, content_type));
    POSIX_GUARD(s2n_stuffer_write_uint8(&ctrl_stuffer, protocol_version / 10));
    POSIX_GUARD(s2n_stuffer_write_uint8(&ctrl_stuffer, protocol_version % 10));
    POSIX_GUARD(s2n_stuffer_write_uint16(&ctrl_stuffer, payload_and_eiv_len));

    /* This will unnecessarily mangle the input buffer, which is fine since it's temporary
     * Return value will be length of digest, padding, and padding length byte.
     * See https://github.com/openssl/openssl/blob/master/crypto/evp/e_aes_cbc_hmac_sha1.c#L814
     * and https://github.com/openssl/openssl/blob/4f0c475719defd7c051964ef9964cc6e5b3a63bf/ssl/record/ssl3_record.c#L743
     */
    int ctrl_ret = EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_AEAD_TLS1_AAD, S2N_TLS12_AAD_LEN, ctrl_buf);

    S2N_ERROR_IF(ctrl_ret <= 0, S2N_ERR_INITIAL_HMAC);

    *extra = ctrl_ret;
    return 0;
#endif
}

static int s2n_composite_cipher_aes_sha_encrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_EQ(out->size, in->size);

    POSIX_GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, NULL, NULL, NULL, iv->data), S2N_ERR_KEY_INIT);

    /* len is set by EVP_EncryptUpdate and checked post operation */
    int len = 0;
    POSIX_GUARD_OSSL(EVP_EncryptUpdate(key->evp_cipher_ctx, out->data, &len, in->data, in->size), S2N_ERR_ENCRYPT);

    POSIX_ENSURE((int64_t) len == (int64_t) in->size, S2N_ERR_ENCRYPT);

    return 0;
}

static int s2n_composite_cipher_aes_sha_decrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_EQ(out->size, in->size);
    POSIX_GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, NULL, NULL, NULL, iv->data), S2N_ERR_KEY_INIT);

    /* len is set by EVP_DecryptUpdate. It is not checked here but padding is manually removed and therefore
     * the decryption operation is validated. */
    int len = 0;
    POSIX_GUARD_OSSL(EVP_DecryptUpdate(key->evp_cipher_ctx, out->data, &len, in->data, in->size), S2N_ERR_DECRYPT);

    return 0;
}

static int s2n_composite_cipher_aes_sha_set_mac_write_key(struct s2n_session_key *key, uint8_t *mac_key, uint32_t mac_size)
{
    POSIX_ENSURE_EQ(mac_size, SHA_DIGEST_LENGTH);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_AEAD_SET_MAC_KEY, mac_size, mac_key);

    return 0;
}

static int s2n_composite_cipher_aes_sha256_set_mac_write_key(struct s2n_session_key *key, uint8_t *mac_key, uint32_t mac_size)
{
    POSIX_ENSURE_EQ(mac_size, SHA256_DIGEST_LENGTH);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_AEAD_SET_MAC_KEY, mac_size, mac_key);

    return 0;
}

static S2N_RESULT s2n_composite_cipher_aes128_sha_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 16);

    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    EVP_EncryptInit_ex(key->evp_cipher_ctx, s2n_evp_aes_128_cbc_hmac_sha1(), NULL, in->data, NULL);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_composite_cipher_aes128_sha_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 16);

    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    EVP_DecryptInit_ex(key->evp_cipher_ctx, s2n_evp_aes_128_cbc_hmac_sha1(), NULL, in->data, NULL);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_composite_cipher_aes256_sha_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 32);

    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    EVP_EncryptInit_ex(key->evp_cipher_ctx, s2n_evp_aes_256_cbc_hmac_sha1(), NULL, in->data, NULL);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_composite_cipher_aes256_sha_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 32);

    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    EVP_DecryptInit_ex(key->evp_cipher_ctx, s2n_evp_aes_256_cbc_hmac_sha1(), NULL, in->data, NULL);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_composite_cipher_aes128_sha256_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 16);

    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    EVP_EncryptInit_ex(key->evp_cipher_ctx, s2n_evp_aes_128_cbc_hmac_sha256(), NULL, in->data, NULL);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_composite_cipher_aes128_sha256_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 16);

    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    EVP_DecryptInit_ex(key->evp_cipher_ctx, s2n_evp_aes_128_cbc_hmac_sha256(), NULL, in->data, NULL);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_composite_cipher_aes256_sha256_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 32);

    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    EVP_EncryptInit_ex(key->evp_cipher_ctx, s2n_evp_aes_256_cbc_hmac_sha256(), NULL, in->data, NULL);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_composite_cipher_aes256_sha256_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    RESULT_ENSURE_EQ(in->size, 32);

    EVP_CIPHER_CTX_set_padding(key->evp_cipher_ctx, 0);
    EVP_DecryptInit_ex(key->evp_cipher_ctx, s2n_evp_aes_256_cbc_hmac_sha256(), NULL, in->data, NULL);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_composite_cipher_aes_sha_init(struct s2n_session_key *key)
{
    RESULT_EVP_CTX_INIT(key->evp_cipher_ctx);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_composite_cipher_aes_sha_destroy_key(struct s2n_session_key *key)
{
    EVP_CIPHER_CTX_cleanup(key->evp_cipher_ctx);

    return S2N_RESULT_OK;
}

const struct s2n_cipher s2n_aes128_sha = {
    .key_material_size = 16,
    .type = S2N_COMPOSITE,
    .io.comp = {
            .block_size = 16,
            .record_iv_size = 16,
            .mac_key_size = SHA_DIGEST_LENGTH,
            .decrypt = s2n_composite_cipher_aes_sha_decrypt,
            .encrypt = s2n_composite_cipher_aes_sha_encrypt,
            .set_mac_write_key = s2n_composite_cipher_aes_sha_set_mac_write_key,
            .initial_hmac = s2n_composite_cipher_aes_sha_initial_hmac },
    .is_available = s2n_composite_cipher_aes128_sha_available,
    .init = s2n_composite_cipher_aes_sha_init,
    .set_encryption_key = s2n_composite_cipher_aes128_sha_set_encryption_key,
    .set_decryption_key = s2n_composite_cipher_aes128_sha_set_decryption_key,
    .destroy_key = s2n_composite_cipher_aes_sha_destroy_key,
};

const struct s2n_cipher s2n_aes256_sha = {
    .key_material_size = 32,
    .type = S2N_COMPOSITE,
    .io.comp = {
            .block_size = 16,
            .record_iv_size = 16,
            .mac_key_size = SHA_DIGEST_LENGTH,
            .decrypt = s2n_composite_cipher_aes_sha_decrypt,
            .encrypt = s2n_composite_cipher_aes_sha_encrypt,
            .set_mac_write_key = s2n_composite_cipher_aes_sha_set_mac_write_key,
            .initial_hmac = s2n_composite_cipher_aes_sha_initial_hmac },
    .is_available = s2n_composite_cipher_aes256_sha_available,
    .init = s2n_composite_cipher_aes_sha_init,
    .set_encryption_key = s2n_composite_cipher_aes256_sha_set_encryption_key,
    .set_decryption_key = s2n_composite_cipher_aes256_sha_set_decryption_key,
    .destroy_key = s2n_composite_cipher_aes_sha_destroy_key,
};

const struct s2n_cipher s2n_aes128_sha256 = {
    .key_material_size = 16,
    .type = S2N_COMPOSITE,
    .io.comp = {
            .block_size = 16,
            .record_iv_size = 16,
            .mac_key_size = SHA256_DIGEST_LENGTH,
            .decrypt = s2n_composite_cipher_aes_sha_decrypt,
            .encrypt = s2n_composite_cipher_aes_sha_encrypt,
            .set_mac_write_key = s2n_composite_cipher_aes_sha256_set_mac_write_key,
            .initial_hmac = s2n_composite_cipher_aes_sha_initial_hmac },
    .is_available = s2n_composite_cipher_aes128_sha256_available,
    .init = s2n_composite_cipher_aes_sha_init,
    .set_encryption_key = s2n_composite_cipher_aes128_sha256_set_encryption_key,
    .set_decryption_key = s2n_composite_cipher_aes128_sha256_set_decryption_key,
    .destroy_key = s2n_composite_cipher_aes_sha_destroy_key,
};

const struct s2n_cipher s2n_aes256_sha256 = {
    .key_material_size = 32,
    .type = S2N_COMPOSITE,
    .io.comp = {
            .block_size = 16,
            .record_iv_size = 16,
            .mac_key_size = SHA256_DIGEST_LENGTH,
            .decrypt = s2n_composite_cipher_aes_sha_decrypt,
            .encrypt = s2n_composite_cipher_aes_sha_encrypt,
            .set_mac_write_key = s2n_composite_cipher_aes_sha256_set_mac_write_key,
            .initial_hmac = s2n_composite_cipher_aes_sha_initial_hmac },
    .is_available = s2n_composite_cipher_aes256_sha256_available,
    .init = s2n_composite_cipher_aes_sha_init,
    .set_encryption_key = s2n_composite_cipher_aes256_sha256_set_encryption_key,
    .set_decryption_key = s2n_composite_cipher_aes256_sha256_set_decryption_key,
    .destroy_key = s2n_composite_cipher_aes_sha_destroy_key,
};
