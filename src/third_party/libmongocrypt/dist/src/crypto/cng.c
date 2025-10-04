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

#include "../mongocrypt-crypto-private.h"
#include "../mongocrypt-private.h"
#include <stdint.h>

#ifdef MONGOCRYPT_ENABLE_CRYPTO_CNG

#include <bcrypt.h>

static BCRYPT_ALG_HANDLE _algo_sha512_hmac = 0;
static BCRYPT_ALG_HANDLE _algo_sha256_hmac = 0;
static BCRYPT_ALG_HANDLE _algo_aes256_cbc = 0;
static BCRYPT_ALG_HANDLE _algo_aes256_ecb = 0;
static DWORD _aes256_key_blob_length;
static DWORD _aes256_block_length;

static BCRYPT_ALG_HANDLE _random;

#define STATUS_SUCCESS 0

bool _native_crypto_initialized = false;

void _native_crypto_init(void) {
    DWORD cbOutput;
    NTSTATUS nt_status;

    /* Note, there is no mechanism for libmongocrypt to close these providers,
     * If we ever add such a mechanism, call BCryptCloseAlgorithmProvider.
     */
    nt_status = BCryptOpenAlgorithmProvider(&_algo_sha512_hmac,
                                            BCRYPT_SHA512_ALGORITHM,
                                            MS_PRIMITIVE_PROVIDER,
                                            BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (nt_status != STATUS_SUCCESS) {
        return;
    }

    nt_status = BCryptOpenAlgorithmProvider(&_algo_sha256_hmac,
                                            BCRYPT_SHA256_ALGORITHM,
                                            MS_PRIMITIVE_PROVIDER,
                                            BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (nt_status != STATUS_SUCCESS) {
        return;
    }

    nt_status = BCryptOpenAlgorithmProvider(&_algo_aes256_cbc, BCRYPT_AES_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    if (nt_status != STATUS_SUCCESS) {
        return;
    }

    nt_status = BCryptSetProperty(_algo_aes256_cbc,
                                  BCRYPT_CHAINING_MODE,
                                  (PUCHAR)(BCRYPT_CHAIN_MODE_CBC),
                                  (ULONG)(sizeof(wchar_t) * wcslen(BCRYPT_CHAIN_MODE_CBC)),
                                  0);
    if (nt_status != STATUS_SUCCESS) {
        return;
    }

    nt_status = BCryptOpenAlgorithmProvider(&_algo_aes256_ecb, BCRYPT_AES_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    if (nt_status != STATUS_SUCCESS) {
        return;
    }

    nt_status = BCryptSetProperty(_algo_aes256_ecb,
                                  BCRYPT_CHAINING_MODE,
                                  (PUCHAR)(BCRYPT_CHAIN_MODE_ECB),
                                  (ULONG)(sizeof(wchar_t) * wcslen(BCRYPT_CHAIN_MODE_ECB)),
                                  0);
    if (nt_status != STATUS_SUCCESS) {
        return;
    }

    cbOutput = sizeof(_aes256_key_blob_length);
    nt_status = BCryptGetProperty(_algo_aes256_cbc,
                                  BCRYPT_OBJECT_LENGTH,
                                  (PUCHAR)(&_aes256_key_blob_length),
                                  cbOutput,
                                  &cbOutput,
                                  0);
    if (nt_status != STATUS_SUCCESS) {
        return;
    }

    cbOutput = sizeof(_aes256_block_length);
    nt_status = BCryptGetProperty(_algo_aes256_cbc,
                                  BCRYPT_BLOCK_LENGTH,
                                  (PUCHAR)(&_aes256_block_length),
                                  cbOutput,
                                  &cbOutput,
                                  0);
    if (nt_status != STATUS_SUCCESS) {
        return;
    }

    nt_status = BCryptOpenAlgorithmProvider(&_random, BCRYPT_RNG_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    if (nt_status != STATUS_SUCCESS) {
        return;
    }

    _native_crypto_initialized = true;
}

typedef struct {
    unsigned char *key_object;
    uint32_t key_object_length;

    BCRYPT_KEY_HANDLE key_handle;

    unsigned char *iv;
    uint32_t iv_len;
} cng_encrypt_state;

static void _crypto_state_destroy(cng_encrypt_state *state);

static cng_encrypt_state *
_crypto_state_init(const _mongocrypt_buffer_t *key, const _mongocrypt_buffer_t *iv, mongocrypt_status_t *status) {
    cng_encrypt_state *state;
    uint32_t keyBlobLength;
    unsigned char *keyBlob;
    BCRYPT_KEY_DATA_BLOB_HEADER blobHeader;
    NTSTATUS nt_status;

    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(iv);

    keyBlob = NULL;

    state = bson_malloc0(sizeof(*state));
    BSON_ASSERT(state);

    state->key_handle = INVALID_HANDLE_VALUE;

    /* Initialize key storage buffer */
    state->key_object = bson_malloc0(_aes256_key_blob_length);
    BSON_ASSERT(state->key_object);

    state->key_object_length = _aes256_key_blob_length;

    /* Allocate temporary buffer for key import */
    BSON_ASSERT(sizeof(BCRYPT_KEY_DATA_BLOB_HEADER) + key->len <= UINT32_MAX);
    keyBlobLength = sizeof(BCRYPT_KEY_DATA_BLOB_HEADER) + key->len;
    keyBlob = bson_malloc0(keyBlobLength);
    BSON_ASSERT(keyBlob);

    blobHeader.dwMagic = BCRYPT_KEY_DATA_BLOB_MAGIC;
    blobHeader.dwVersion = BCRYPT_KEY_DATA_BLOB_VERSION1;
    blobHeader.cbKeyData = key->len;

    memcpy(keyBlob, &blobHeader, sizeof(BCRYPT_KEY_DATA_BLOB_HEADER));

    memcpy(keyBlob + sizeof(BCRYPT_KEY_DATA_BLOB_HEADER), key->data, key->len);

    nt_status = BCryptImportKey(_algo_aes256_cbc,
                                NULL,
                                BCRYPT_KEY_DATA_BLOB,
                                &(state->key_handle),
                                state->key_object,
                                state->key_object_length,
                                keyBlob,
                                keyBlobLength,
                                0);
    if (nt_status != STATUS_SUCCESS) {
        CLIENT_ERR("Import Key Failed: 0x%x", (int)nt_status);
        goto fail;
    }

    bson_free(keyBlob);

    state->iv = bson_malloc0(iv->len);
    BSON_ASSERT(state->iv);

    state->iv_len = iv->len;
    memcpy(state->iv, iv->data, iv->len);

    return state;
fail:
    _crypto_state_destroy(state);
    bson_free(keyBlob);

    return NULL;
}

static void _crypto_state_destroy(cng_encrypt_state *state) {
    if (state) {
        /* Free the key handle before the key_object that contains it */
        if (state->key_handle != INVALID_HANDLE_VALUE) {
            BCryptDestroyKey(state->key_handle);
        }

        bson_free(state->key_object);
        bson_free(state->iv);
        bson_free(state);
    }
}

bool _native_crypto_aes_256_cbc_encrypt(aes_256_args_t args) {
    BSON_ASSERT(args.in);
    BSON_ASSERT(args.out);

    bool ret = false;
    mongocrypt_status_t *status = args.status;
    cng_encrypt_state *state = _crypto_state_init(args.key, args.iv, status);

    BSON_ASSERT(state);

    NTSTATUS nt_status;

    nt_status = BCryptEncrypt(state->key_handle,
                              (PUCHAR)(args.in->data),
                              args.in->len,
                              NULL,
                              state->iv,
                              state->iv_len,
                              args.out->data,
                              args.out->len,
                              args.bytes_written,
                              0);

    if (nt_status != STATUS_SUCCESS) {
        CLIENT_ERR("error initializing cipher: 0x%x", (int)nt_status);
        goto done;
    }

    ret = true;
done:
    _crypto_state_destroy(state);
    return ret;
}

bool _native_crypto_aes_256_cbc_decrypt(aes_256_args_t args) {
    BSON_ASSERT(args.in);
    BSON_ASSERT(args.out);

    bool ret = false;
    mongocrypt_status_t *status = args.status;
    cng_encrypt_state *state = _crypto_state_init(args.key, args.iv, status);

    BSON_ASSERT(state);

    NTSTATUS nt_status;

    nt_status = BCryptDecrypt(state->key_handle,
                              (PUCHAR)(args.in->data),
                              args.in->len,
                              NULL,
                              state->iv,
                              state->iv_len,
                              args.out->data,
                              args.out->len,
                              args.bytes_written,
                              0);

    if (nt_status != STATUS_SUCCESS) {
        CLIENT_ERR("error initializing cipher: 0x%x", (int)nt_status);
        goto done;
    }

    ret = true;
done:
    _crypto_state_destroy(state);
    return ret;
}

/* _hmac_with_algorithm computes an HMAC of @in with the algorithm specified by
 * @hAlgorithm.
 * @key is the input key.
 * @out is the output. @out must be allocated by the caller with
 * the expected length @expect_out_len for the output.
 * Returns false and sets @status on error. @status is required. */
static bool _hmac_with_algorithm(BCRYPT_ALG_HANDLE hAlgorithm,
                                 const _mongocrypt_buffer_t *key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 uint32_t expect_out_len,
                                 mongocrypt_status_t *status) {
    bool ret = false;
    BCRYPT_HASH_HANDLE hHash;
    NTSTATUS nt_status;

    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    if (out->len != expect_out_len) {
        CLIENT_ERR("out does not contain " PRIu32 " bytes", expect_out_len);
        return false;
    }

    nt_status = BCryptCreateHash(hAlgorithm, &hHash, NULL, 0, (PUCHAR)key->data, (ULONG)key->len, 0);
    if (nt_status != STATUS_SUCCESS) {
        CLIENT_ERR("error initializing hmac: 0x%x", (int)nt_status);
        /* Only call BCryptDestroyHash if BCryptCreateHash succeeded. */
        return false;
    }

    nt_status = BCryptHashData(hHash, (PUCHAR)in->data, (ULONG)in->len, 0);
    if (nt_status != STATUS_SUCCESS) {
        CLIENT_ERR("error hashing data: 0x%x", (int)nt_status);
        goto done;
    }

    nt_status = BCryptFinishHash(hHash, out->data, out->len, 0);
    if (nt_status != STATUS_SUCCESS) {
        CLIENT_ERR("error finishing hmac: 0x%x", (int)nt_status);
        goto done;
    }

    ret = true;
done:
    (void)BCryptDestroyHash(hHash);
    return ret;
}

bool _native_crypto_hmac_sha_512(const _mongocrypt_buffer_t *key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) {
    return _hmac_with_algorithm(_algo_sha512_hmac, key, in, out, MONGOCRYPT_HMAC_SHA512_LEN, status);
}

bool _native_crypto_random(_mongocrypt_buffer_t *out, uint32_t count, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(out);

    NTSTATUS nt_status = BCryptGenRandom(_random, out->data, count, 0);
    if (nt_status != STATUS_SUCCESS) {
        CLIENT_ERR("BCryptGenRandom Failed: 0x%x", (int)nt_status);
        return false;
    }

    return true;
}

typedef struct {
    BCRYPT_KEY_HANDLE key_handle;

    unsigned char *input_block;
    uint32_t input_block_len;

    unsigned char *output_block;
    uint32_t output_block_len;
    uint32_t output_block_ptr;
} cng_ctr_encrypt_state;

static bool _cng_ctr_crypto_generate(cng_ctr_encrypt_state *state, mongocrypt_status_t *status) {
    BSON_ASSERT(state);

    uint32_t bytesEncrypted = 0;
    NTSTATUS nt_status = BCryptEncrypt(state->key_handle,
                                       state->input_block,
                                       state->input_block_len,
                                       NULL,
                                       NULL,
                                       0,
                                       state->output_block,
                                       state->output_block_len,
                                       &bytesEncrypted,
                                       0);
    if (nt_status != STATUS_SUCCESS) {
        CLIENT_ERR("error encrypting: 0x%x", (int)nt_status);
        return false;
    }
    BSON_ASSERT(bytesEncrypted);
    state->output_block_ptr = 0;
    return true;
}

static void _cng_ctr_crypto_advance(cng_ctr_encrypt_state *state) {
    BSON_ASSERT_PARAM(state);

    /* Assert rather than return false/NULL since this function's type is void */
    BSON_ASSERT(sizeof(BCRYPT_KEY_DATA_BLOB_HEADER) <= UINT32_MAX - state->input_block_len);

    uint32_t carry = 1;
    for (int i = (int)state->input_block_len - 1; i >= 0 && carry != 0; --i) {
        uint32_t bpp = (uint32_t)(state->input_block[i]) + carry;
        carry = bpp >> 8;
        state->input_block[i] = bpp & 0xFF;
    }
}

static bool _cng_ctr_crypto_next(cng_ctr_encrypt_state *state, mongocrypt_status_t *status, unsigned char *mask) {
    BSON_ASSERT_PARAM(state);
    BSON_ASSERT_PARAM(mask);

    if (state->output_block_ptr >= state->output_block_len) {
        _cng_ctr_crypto_advance(state);
        if (!_cng_ctr_crypto_generate(state, status)) {
            return false;
        };
    }

    *mask = state->output_block[state->output_block_ptr];
    ++state->output_block_ptr;

    return true;
}

static void _cng_ctr_crypto_state_destroy(cng_ctr_encrypt_state *state);

static cng_ctr_encrypt_state *_cng_ctr_crypto_state_init(const _mongocrypt_buffer_t *key,
                                                         const _mongocrypt_buffer_t *iv,
                                                         mongocrypt_status_t *status) {
    cng_ctr_encrypt_state *state;
    uint32_t keyBlobLength;
    unsigned char *keyBlob;
    BCRYPT_KEY_DATA_BLOB_HEADER blobHeader;
    NTSTATUS nt_status;

    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(iv);

    keyBlob = NULL;

    state = bson_malloc0(sizeof(*state));
    BSON_ASSERT(state);

    if (UINT32_MAX - key->len < sizeof(BCRYPT_KEY_DATA_BLOB_HEADER)) {
        CLIENT_ERR("key is too long");
        goto fail;
    }

    state->key_handle = INVALID_HANDLE_VALUE;

    /* Initialize input storage buffer */
    state->input_block = bson_malloc0(_aes256_block_length);
    BSON_ASSERT(state->input_block);
    state->input_block_len = _aes256_block_length;
    BSON_ASSERT(iv->len == _aes256_block_length);
    memcpy(state->input_block, iv->data, iv->len);

    /* Initialize output storage buffer */
    state->output_block = bson_malloc0(_aes256_block_length);
    BSON_ASSERT(state->output_block);
    state->output_block_len = _aes256_block_length;
    state->output_block_ptr = 0;

    /* Allocate temporary buffer for key import */
    keyBlobLength = sizeof(BCRYPT_KEY_DATA_BLOB_HEADER) + key->len;
    keyBlob = bson_malloc0(keyBlobLength);
    BSON_ASSERT(keyBlob);

    blobHeader.dwMagic = BCRYPT_KEY_DATA_BLOB_MAGIC;
    blobHeader.dwVersion = BCRYPT_KEY_DATA_BLOB_VERSION1;
    blobHeader.cbKeyData = key->len;

    memcpy(keyBlob, &blobHeader, sizeof(BCRYPT_KEY_DATA_BLOB_HEADER));

    memcpy(keyBlob + sizeof(BCRYPT_KEY_DATA_BLOB_HEADER), key->data, key->len);

    nt_status = BCryptImportKey(_algo_aes256_ecb,
                                NULL,
                                BCRYPT_KEY_DATA_BLOB,
                                &(state->key_handle),
                                NULL,
                                0,
                                keyBlob,
                                keyBlobLength,
                                0);
    if (nt_status != STATUS_SUCCESS) {
        CLIENT_ERR("Import Key Failed: 0x%x", (int)nt_status);
        goto fail;
    }

    bson_free(keyBlob);

    if (!_cng_ctr_crypto_generate(state, status)) {
        goto fail;
    }

    return state;
fail:
    _cng_ctr_crypto_state_destroy(state);
    bson_free(keyBlob);

    return NULL;
}

static void _cng_ctr_crypto_state_destroy(cng_ctr_encrypt_state *state) {
    if (state) {
        if (state->key_handle != INVALID_HANDLE_VALUE) {
            BCryptDestroyKey(state->key_handle);
        }

        bson_free(state->input_block);
        bson_free(state->output_block);
        bson_free(state);
    }
}

bool _native_crypto_aes_256_ctr_encrypt(aes_256_args_t args) {
    bool ret = false;
    cng_ctr_encrypt_state *state = NULL;
    mongocrypt_status_t *status = args.status;

    BSON_ASSERT(args.in && args.in->data);
    BSON_ASSERT(args.out && args.out->data);
    if (args.out->len < args.in->len) {
        CLIENT_ERR("Output buffer is too small");
        goto fail;
    }

    state = _cng_ctr_crypto_state_init(args.key, args.iv, status);
    if (!state) {
        goto fail;
    }

    for (uint32_t i = 0; i < args.in->len; ++i) {
        unsigned char mask;
        if (!_cng_ctr_crypto_next(state, status, &mask)) {
            goto fail;
        }
        args.out->data[i] = args.in->data[i] ^ mask;
    }

    if (args.bytes_written) {
        *args.bytes_written = args.in->len;
    }

    ret = true;

fail:
    _cng_ctr_crypto_state_destroy(state);
    return ret;
}

bool _native_crypto_aes_256_ctr_decrypt(aes_256_args_t args) {
    bool ret = false;
    cng_ctr_encrypt_state *state = NULL;
    mongocrypt_status_t *status = args.status;

    BSON_ASSERT(args.in && args.in->data);
    BSON_ASSERT(args.out && args.out->data);
    if (args.out->len < args.in->len) {
        CLIENT_ERR("Output buffer is too small");
        goto fail;
    }

    state = _cng_ctr_crypto_state_init(args.key, args.iv, status);
    if (!state) {
        goto fail;
    }

    for (uint32_t i = 0; i < args.in->len; ++i) {
        unsigned char mask;
        if (!_cng_ctr_crypto_next(state, status, &mask)) {
            goto fail;
        }
        args.out->data[i] = args.in->data[i] ^ mask;
    }

    if (args.bytes_written) {
        *args.bytes_written = args.in->len;
    }

    ret = true;

fail:
    _cng_ctr_crypto_state_destroy(state);
    return ret;
}

bool _native_crypto_hmac_sha_256(const _mongocrypt_buffer_t *key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) {
    return _hmac_with_algorithm(_algo_sha256_hmac, key, in, out, MONGOCRYPT_HMAC_SHA256_LEN, status);
}

#endif /* MONGOCRYPT_ENABLE_CRYPTO_CNG */
