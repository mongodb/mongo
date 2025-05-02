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

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#include <openssl/params.h>
#endif

bool _native_crypto_initialized = false;

/* _encrypt_with_cipher encrypts @in with the specified OpenSSL cipher.
 * @cipher is a usable EVP_CIPHER, or NULL if early initialization failed.
 * @cipher_description is a human-readable description used when reporting deferred errors from initialization, required
 * if @cipher might be NULL.
 * @key is the input key. @iv is the input IV.
 * @out is the output ciphertext. @out must be allocated by the caller with
 * enough room for the ciphertext.
 * @bytes_written is the number of bytes that were written to @out.
 * Returns false and sets @status on error. @status is required. */
static bool _encrypt_with_cipher(const EVP_CIPHER *cipher, const char *cipher_description, aes_256_args_t args) {
    BSON_ASSERT(args.key);
    BSON_ASSERT(args.in);
    BSON_ASSERT(args.out);
    BSON_ASSERT(args.in->len <= INT_MAX);

    mongocrypt_status_t *status = args.status;
    if (!cipher) {
        BSON_ASSERT(cipher_description);
        CLIENT_ERR("failed to initialize cipher %s", cipher_description);
        return false;
    }

    BSON_ASSERT(NULL == args.iv || (uint32_t)EVP_CIPHER_iv_length(cipher) == args.iv->len);
    BSON_ASSERT((uint32_t)EVP_CIPHER_key_length(cipher) == args.key->len);

    bool ret = false;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    BSON_ASSERT(ctx);

    if (!EVP_EncryptInit_ex(ctx, cipher, NULL /* engine */, args.key->data, NULL == args.iv ? NULL : args.iv->data)) {
        CLIENT_ERR("error in EVP_EncryptInit_ex: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    /* Disable the default OpenSSL padding. */
    EVP_CIPHER_CTX_set_padding(ctx, 0);

    *args.bytes_written = 0;

    int intermediate_bytes_written = 0;
    if (!EVP_EncryptUpdate(ctx, args.out->data, &intermediate_bytes_written, args.in->data, (int)args.in->len)) {
        CLIENT_ERR("error in EVP_EncryptUpdate: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    BSON_ASSERT(intermediate_bytes_written >= 0 && (uint64_t)intermediate_bytes_written <= UINT32_MAX);
    /* intermediate_bytes_written cannot be negative, so int -> uint32_t is OK */
    *args.bytes_written = (uint32_t)intermediate_bytes_written;

    if (!EVP_EncryptFinal_ex(ctx, args.out->data, &intermediate_bytes_written)) {
        CLIENT_ERR("error in EVP_EncryptFinal_ex: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    BSON_ASSERT(UINT32_MAX - *args.bytes_written >= (uint32_t)intermediate_bytes_written);
    *args.bytes_written += (uint32_t)intermediate_bytes_written;

    ret = true;
done:
    EVP_CIPHER_CTX_free(ctx);
    return ret;
}

/* _decrypt_with_cipher decrypts @in with the specified OpenSSL cipher.
 * @cipher is a usable EVP_CIPHER, or NULL if early initialization failed.
 * @cipher_description is a human-readable description used when reporting deferred errors from initialization, required
 * if @cipher might be NULL.
 * @key is the input key. @iv is the input IV.
 * @out is the output plaintext. @out must be allocated by the caller with
 * enough room for the plaintext.
 * @bytes_written is the number of bytes that were written to @out.
 * Returns false and sets @status on error. @status is required. */
static bool _decrypt_with_cipher(const EVP_CIPHER *cipher, const char *cipher_description, aes_256_args_t args) {
    BSON_ASSERT(args.iv);
    BSON_ASSERT(args.key);
    BSON_ASSERT(args.in);
    BSON_ASSERT(args.out);
    BSON_ASSERT(args.in->len <= INT_MAX);

    mongocrypt_status_t *status = args.status;
    if (!cipher) {
        BSON_ASSERT_PARAM(cipher_description);
        CLIENT_ERR("failed to initialize cipher %s", cipher_description);
        return false;
    }

    BSON_ASSERT((uint32_t)EVP_CIPHER_iv_length(cipher) == args.iv->len);
    BSON_ASSERT((uint32_t)EVP_CIPHER_key_length(cipher) == args.key->len);

    bool ret = false;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    BSON_ASSERT(ctx);

    if (!EVP_DecryptInit_ex(ctx, cipher, NULL /* engine */, args.key->data, args.iv->data)) {
        CLIENT_ERR("error in EVP_DecryptInit_ex: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    /* Disable padding. */
    EVP_CIPHER_CTX_set_padding(ctx, 0);

    *args.bytes_written = 0;

    int intermediate_bytes_written = 0;
    if (!EVP_DecryptUpdate(ctx, args.out->data, &intermediate_bytes_written, args.in->data, (int)args.in->len)) {
        CLIENT_ERR("error in EVP_DecryptUpdate: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    BSON_ASSERT(intermediate_bytes_written >= 0 && (uint64_t)intermediate_bytes_written <= UINT32_MAX);
    /* intermediate_bytes_written cannot be negative, so int -> uint32_t is OK */
    *args.bytes_written = (uint32_t)intermediate_bytes_written;

    if (!EVP_DecryptFinal_ex(ctx, args.out->data, &intermediate_bytes_written)) {
        CLIENT_ERR("error in EVP_DecryptFinal_ex: %s", ERR_error_string(ERR_get_error(), NULL));
        goto done;
    }

    BSON_ASSERT(UINT32_MAX - *args.bytes_written >= (uint32_t)intermediate_bytes_written);
    *args.bytes_written += (uint32_t)intermediate_bytes_written;

    ret = true;
done:
    EVP_CIPHER_CTX_free(ctx);
    return ret;
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

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
// Newest libcrypto support: requires EVP_MAC_CTX_dup and EVP_CIPHER_fetch added in OpenSSL 3.0.0

static struct {
    EVP_MAC_CTX *hmac_sha2_256;
    EVP_MAC_CTX *hmac_sha2_512;
    EVP_CIPHER *aes_256_cbc;
    EVP_CIPHER *aes_256_ctr;
    EVP_CIPHER *aes_256_ecb; // For testing only
} _mongocrypt_libcrypto;

EVP_MAC_CTX *_build_hmac_ctx_prototype(const char *digest_name) {
    EVP_MAC *hmac = EVP_MAC_fetch(NULL, OSSL_MAC_NAME_HMAC, NULL);
    if (!hmac) {
        return NULL;
    }

    EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(hmac);
    EVP_MAC_free(hmac);
    if (!ctx) {
        return NULL;
    }

    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, (char *)digest_name, 0),
                           OSSL_PARAM_construct_end()};

    if (EVP_MAC_CTX_set_params(ctx, params)) {
        return ctx;
    } else {
        EVP_MAC_CTX_free(ctx);
        return NULL;
    }
}

/* _hmac_with_ctx_prototype computes an HMAC of @in using an OpenSSL context duplicated from @ctx_prototype.
 * @ctx_description is a human-readable description used when reporting deferred errors from initialization, required
 * if @ctx_prototype might be NULL.
 * @key is the input key.
 * @out is the output. @out must be allocated by the caller with
 * the exact length for the output. E.g. for HMAC 256, @out->len must be 32.
 * Returns false and sets @status on error. @status is required. */
static bool _hmac_with_ctx_prototype(const EVP_MAC_CTX *ctx_prototype,
                                     const char *ctx_description,
                                     const _mongocrypt_buffer_t *key,
                                     const _mongocrypt_buffer_t *in,
                                     _mongocrypt_buffer_t *out,
                                     mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(key->len <= INT_MAX);

    if (!ctx_prototype) {
        BSON_ASSERT_PARAM(ctx_description);
        CLIENT_ERR("failed to initialize algorithm %s", ctx_description);
        return false;
    }

    EVP_MAC_CTX *ctx = EVP_MAC_CTX_dup(ctx_prototype);
    if (ctx) {
        bool ok = EVP_MAC_init(ctx, key->data, key->len, NULL) && EVP_MAC_update(ctx, in->data, in->len)
               && EVP_MAC_final(ctx, out->data, NULL, out->len);
        EVP_MAC_CTX_free(ctx);
        if (ok) {
            return true;
        }
    }
    CLIENT_ERR("HMAC error: %s", ERR_error_string(ERR_get_error(), NULL));
    return false;
}

void _native_crypto_init(void) {
    // Early lookup of digest and cipher algorithms avoids both the lookup overhead itself and the overhead of lock
    // contention in the default OSSL_LIB_CTX.
    //
    // Failures now will store NULL, reporting a client error later.
    //
    // On HMAC fetching:
    //
    // Note that libcrypto sets an additional trap for us regarding MAC algorithms. An early fetch of the HMAC itself
    // won't actually pre-fetch the subalgorithm. The name of the inner digest gets stored as a string, and re-fetched
    // when setting up MAC context parameters. To fetch both the outer and inner algorithms ahead of time, we construct
    // a prototype EVP_MAC_CTX that can be duplicated before each use.
    //
    // On thread safety:
    //
    // This creates objects that are intended to be immutable shared data after initialization. To understand whether
    // this is safe we could consult the OpenSSL documentation but currently it's lacking in specifics about the
    // individual API functions and types. It offers some general guidelines: "Objects are thread-safe as long as the
    // API's being invoked don't modify the object; in this case the parameter is usually marked in the API as C<const>.
    // Not all parameters are marked this way." By inspection, we can see that pre-fetched ciphers and MACs are designed
    // with atomic reference counting support and appear to be intended for safe immutable use. Contexts are normally
    // not safe to share, but these used only as a source for EVP_MAC_CTX_dup() can be treated as immutable.
    //
    // TODO: This could be refactored to live in mongocrypt_t rather than in global data. Currently there's no way to
    // avoid leaking this set of one-time allocations.
    //
    // TODO: Higher performance yet could be achieved by re-using thread local EVP_MAC_CTX, but this requires careful
    // lifecycle management to avoid leaking data. Alternatively, the libmongocrypt API could be modified to include
    // some non-shared but long-lived context suitable for keeping these crypto objects. Alternatively still, it may be
    // worth using a self contained SHA2 HMAC with favorable performance and portability characteristics.

    _mongocrypt_libcrypto.aes_256_cbc = EVP_CIPHER_fetch(NULL, "AES-256-CBC", NULL);
    _mongocrypt_libcrypto.aes_256_ctr = EVP_CIPHER_fetch(NULL, "AES-256-CTR", NULL);
    _mongocrypt_libcrypto.aes_256_ecb = EVP_CIPHER_fetch(NULL, "AES-256-ECB", NULL);
    _mongocrypt_libcrypto.hmac_sha2_256 = _build_hmac_ctx_prototype(OSSL_DIGEST_NAME_SHA2_256);
    _mongocrypt_libcrypto.hmac_sha2_512 = _build_hmac_ctx_prototype(OSSL_DIGEST_NAME_SHA2_512);
    _native_crypto_initialized = true;
}

bool _native_crypto_aes_256_cbc_encrypt(aes_256_args_t args) {
    return _encrypt_with_cipher(_mongocrypt_libcrypto.aes_256_cbc, "AES-256-CBC", args);
}

bool _native_crypto_aes_256_cbc_decrypt(aes_256_args_t args) {
    return _decrypt_with_cipher(_mongocrypt_libcrypto.aes_256_cbc, "AES-256-CBC", args);
}

bool _native_crypto_aes_256_ecb_encrypt(aes_256_args_t args); // -Wmissing-prototypes: for testing only.

bool _native_crypto_aes_256_ecb_encrypt(aes_256_args_t args) {
    return _encrypt_with_cipher(_mongocrypt_libcrypto.aes_256_ecb, "AES-256-ECB", args);
}

bool _native_crypto_aes_256_ctr_encrypt(aes_256_args_t args) {
    return _encrypt_with_cipher(_mongocrypt_libcrypto.aes_256_ctr, "AES-256-CTR", args);
}

bool _native_crypto_aes_256_ctr_decrypt(aes_256_args_t args) {
    return _decrypt_with_cipher(_mongocrypt_libcrypto.aes_256_ctr, "AES-256-CTR", args);
}

bool _native_crypto_hmac_sha_256(const _mongocrypt_buffer_t *key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) {
    return _hmac_with_ctx_prototype(_mongocrypt_libcrypto.hmac_sha2_256, "HMAC-SHA2-256", key, in, out, status);
}

bool _native_crypto_hmac_sha_512(const _mongocrypt_buffer_t *key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) {
    return _hmac_with_ctx_prototype(_mongocrypt_libcrypto.hmac_sha2_512, "HMAC-SHA2-512", key, in, out, status);
}

#else /* OPENSSL_VERSION_NUMBER < 0x30000000L */
// Support for previous libcrypto versions, without early fetch optimization.

#if OPENSSL_VERSION_NUMBER < 0x10100000L || (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x20700000L)
static HMAC_CTX *HMAC_CTX_new(void) {
    return bson_malloc0(sizeof(HMAC_CTX));
}

static void HMAC_CTX_free(HMAC_CTX *ctx) {
    HMAC_CTX_cleanup(ctx);
    bson_free(ctx);
}
#endif

void _native_crypto_init(void) {
    _native_crypto_initialized = true;
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

    if (out->len != (uint32_t)EVP_MD_size(hash)) {
        CLIENT_ERR("out does not contain %d bytes", EVP_MD_size(hash));
        goto done;
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

bool _native_crypto_aes_256_cbc_encrypt(aes_256_args_t args) {
    return _encrypt_with_cipher(EVP_aes_256_cbc(), NULL, args);
}

bool _native_crypto_aes_256_cbc_decrypt(aes_256_args_t args) {
    return _decrypt_with_cipher(EVP_aes_256_cbc(), NULL, args);
}

bool _native_crypto_aes_256_ecb_encrypt(aes_256_args_t args); // -Wmissing-prototypes: for testing only.

bool _native_crypto_aes_256_ecb_encrypt(aes_256_args_t args) {
    return _encrypt_with_cipher(EVP_aes_256_ecb(), NULL, args);
}

bool _native_crypto_aes_256_ctr_encrypt(aes_256_args_t args) {
    return _encrypt_with_cipher(EVP_aes_256_ctr(), NULL, args);
}

bool _native_crypto_aes_256_ctr_decrypt(aes_256_args_t args) {
    return _decrypt_with_cipher(EVP_aes_256_ctr(), NULL, args);
}

bool _native_crypto_hmac_sha_256(const _mongocrypt_buffer_t *key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) {
    return _hmac_with_hash(EVP_sha256(), key, in, out, status);
}

bool _native_crypto_hmac_sha_512(const _mongocrypt_buffer_t *key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) {
    return _hmac_with_hash(EVP_sha512(), key, in, out, status);
}

#endif /* OPENSSL_VERSION_NUMBER */

#endif /* MONGOCRYPT_ENABLE_CRYPTO_LIBCRYPTO */
