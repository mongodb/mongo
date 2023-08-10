/*
 * Copyright 2019-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Comments in this implementation refer to:
 * [MCGREW] https://tools.ietf.org/html/draft-mcgrew-aead-aes-cbc-hmac-sha2-05
 */

#include "../mongocrypt-crypto-private.h"
#include "../mongocrypt-log-private.h"
#include "../mongocrypt-private.h"

#ifdef MONGOCRYPT_ENABLE_CRYPTO_LIBCRYPTO

#include <bson/bson.h>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#if OPENSSL_VERSION_NUMBER < 0x10100000L || (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x20700000L)

static HMAC_CTX *HMAC_CTX_new(void) {
    return bson_malloc0(sizeof(HMAC_CTX));
}

static void HMAC_CTX_free(HMAC_CTX *ctx) {
    HMAC_CTX_cleanup(ctx);
    bson_free(ctx);
}
#endif

bool _native_crypto_initialized = false;

void _native_crypto_init(void) {
    _native_crypto_initialized = true;
}

/* _encrypt_with_cipher encrypts @in with the OpenSSL cipher specified by
 * @cipher.
 * @key is the input key. @iv is the input IV.
 * @out is the output ciphertext. @out must be allocated by the caller with
 * enough room for the ciphertext.
 * @bytes_written is the number of bytes that were written to @out.
 * Returns false and sets @status on error. @status is required. */
static bool _encrypt_with_cipher(const EVP_CIPHER *cipher, aes_256_args_t args) {
    EVP_CIPHER_CTX *ctx;
    bool ret = false;
    int intermediate_bytes_written;
    mongocrypt_status_t *status = args.status;

    ctx = EVP_CIPHER_CTX_new();

    BSON_ASSERT(args.key);
    BSON_ASSERT(args.in);
    BSON_ASSERT(args.out);
    BSON_ASSERT(ctx);
    BSON_ASSERT(cipher);
    BSON_ASSERT(NULL == args.iv || EVP_CIPHER_iv_length(cipher) == args.iv->len);
    BSON_ASSERT(EVP_CIPHER_key_length(cipher) == args.key->len);
    BSON_ASSERT(args.in->len <= INT_MAX);

    if (!EVP_EncryptInit_ex(ctx, cipher, NULL /* engine */, args.key->data, NULL == args.iv ? NULL : args.iv->data)) {
        CLIENT_ERR("error in EVP_EncryptInit_ex: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    /* Disable the default OpenSSL padding. */
    EVP_CIPHER_CTX_set_padding(ctx, 0);

    *args.bytes_written = 0;
    if (!EVP_EncryptUpdate(ctx, args.out->data, &intermediate_bytes_written, args.in->data, (int)args.in->len)) {
        CLIENT_ERR("error in EVP_EncryptUpdate: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    /* intermediate_bytes_written cannot be negative, so int -> uint32_t is OK */
    *args.bytes_written = (uint32_t)intermediate_bytes_written;

    if (!EVP_EncryptFinal_ex(ctx, args.out->data, &intermediate_bytes_written)) {
        CLIENT_ERR("error in EVP_EncryptFinal_ex: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    BSON_ASSERT(UINT32_MAX - *args.bytes_written >= intermediate_bytes_written);
    *args.bytes_written += (uint32_t)intermediate_bytes_written;

    ret = true;
done:
    EVP_CIPHER_CTX_free(ctx);
    return ret;
}

/* _decrypt_with_cipher decrypts @in with the OpenSSL cipher specified by
 * @cipher.
 * @key is the input key. @iv is the input IV.
 * @out is the output plaintext. @out must be allocated by the caller with
 * enough room for the plaintext.
 * @bytes_written is the number of bytes that were written to @out.
 * Returns false and sets @status on error. @status is required. */
static bool _decrypt_with_cipher(const EVP_CIPHER *cipher, aes_256_args_t args) {
    EVP_CIPHER_CTX *ctx;
    bool ret = false;
    int intermediate_bytes_written;
    mongocrypt_status_t *status = args.status;

    ctx = EVP_CIPHER_CTX_new();

    BSON_ASSERT_PARAM(cipher);
    BSON_ASSERT(args.iv);
    BSON_ASSERT(args.key);
    BSON_ASSERT(args.in);
    BSON_ASSERT(args.out);
    BSON_ASSERT(EVP_CIPHER_iv_length(cipher) == args.iv->len);
    BSON_ASSERT(EVP_CIPHER_key_length(cipher) == args.key->len);
    BSON_ASSERT(args.in->len <= INT_MAX);

    if (!EVP_DecryptInit_ex(ctx, cipher, NULL /* engine */, args.key->data, args.iv->data)) {
        CLIENT_ERR("error in EVP_DecryptInit_ex: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    /* Disable padding. */
    EVP_CIPHER_CTX_set_padding(ctx, 0);

    *args.bytes_written = 0;

    if (!EVP_DecryptUpdate(ctx, args.out->data, &intermediate_bytes_written, args.in->data, (int)args.in->len)) {
        CLIENT_ERR("error in EVP_DecryptUpdate: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    /* intermediate_bytes_written cannot be negative, so int -> uint32_t is OK */
    *args.bytes_written = (uint32_t)intermediate_bytes_written;

    if (!EVP_DecryptFinal_ex(ctx, args.out->data, &intermediate_bytes_written)) {
        CLIENT_ERR("error in EVP_DecryptFinal_ex: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    BSON_ASSERT(UINT32_MAX - *args.bytes_written >= intermediate_bytes_written);
    *args.bytes_written += (uint32_t)intermediate_bytes_written;

    ret = true;
done:
    EVP_CIPHER_CTX_free(ctx);
    return ret;
}

bool _native_crypto_aes_256_cbc_encrypt(aes_256_args_t args) {
    return _encrypt_with_cipher(EVP_aes_256_cbc(), args);
}

bool _native_crypto_aes_256_cbc_decrypt(aes_256_args_t args) {
    return _decrypt_with_cipher(EVP_aes_256_cbc(), args);
}

bool _native_crypto_aes_256_ecb_encrypt(aes_256_args_t args) {
    return _encrypt_with_cipher(EVP_aes_256_ecb(), args);
}

/* _hmac_with_hash computes an HMAC of @in with the OpenSSL hash specified by
 * @hash.
 * @key is the input key.
 * @out is the output. @out must be allocated by the caller with
 * the exact length for the output. E.g. for HMAC 256, @out->len must be 32.
 * Returns false and sets @status on error. @status is required. */
static bool _hmac_with_hash(const EVP_MD *hash,
                            const _mongocrypt_buffer_t *key,
                            const _mongocrypt_buffer_t *in,
                            _mongocrypt_buffer_t *out,
                            mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(hash);
    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(key->len <= INT_MAX);

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    if (!HMAC(hash, key->data, (int)key->len, in->data, in->len, out->data, NULL /* unused out len */)) {
        CLIENT_ERR("error initializing HMAC: %s", ERR_error_string(ERR_get_error(), NULL));
        return false;
    }
    return true;
#else
    HMAC_CTX *ctx;
    bool ret = false;

    ctx = HMAC_CTX_new();

    if (out->len != EVP_MD_size(hash)) {
        CLIENT_ERR("out does not contain %d bytes", EVP_MD_size(hash));
        return false;
    }

    if (!HMAC_Init_ex(ctx, key->data, (int)key->len, hash, NULL /* engine */)) {
        CLIENT_ERR("error initializing HMAC: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    if (!HMAC_Update(ctx, in->data, in->len)) {
        CLIENT_ERR("error updating HMAC: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    if (!HMAC_Final(ctx, out->data, NULL /* unused out len */)) {
        CLIENT_ERR("error finalizing: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    ret = true;
done:
    HMAC_CTX_free(ctx);
    return ret;
#endif
}

bool _native_crypto_hmac_sha_512(const _mongocrypt_buffer_t *key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) {
    return _hmac_with_hash(EVP_sha512(), key, in, out, status);
}

bool _native_crypto_random(_mongocrypt_buffer_t *out, uint32_t count, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(count <= INT_MAX);

    int ret = RAND_bytes(out->data, (int)count);
    /* From man page: "RAND_bytes() and RAND_priv_bytes() return 1 on success, -1
     * if not supported by the current RAND method, or 0 on other failure. The
     * error code can be obtained by ERR_get_error(3)" */
    if (ret == -1) {
        CLIENT_ERR("secure random IV not supported: %s", ERR_error_string(ERR_get_error(), NULL));
        return false;
    } else if (ret == 0) {
        CLIENT_ERR("failed to generate random IV: %s", ERR_error_string(ERR_get_error(), NULL));
        return false;
    }
    return true;
}

bool _native_crypto_aes_256_ctr_encrypt(aes_256_args_t args) {
    return _encrypt_with_cipher(EVP_aes_256_ctr(), args);
}

bool _native_crypto_aes_256_ctr_decrypt(aes_256_args_t args) {
    return _decrypt_with_cipher(EVP_aes_256_ctr(), args);
}

bool _native_crypto_hmac_sha_256(const _mongocrypt_buffer_t *key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) {
    return _hmac_with_hash(EVP_sha256(), key, in, out, status);
}

#endif /* MONGOCRYPT_ENABLE_CRYPTO_LIBCRYPTO */
