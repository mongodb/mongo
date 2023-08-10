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

#ifndef MONGOCRYPT_CRYPTO_PRIVATE_H
#define MONGOCRYPT_CRYPTO_PRIVATE_H

#include "mongocrypt-buffer-private.h"
#include "mongocrypt.h"

#define MONGOCRYPT_KEY_LEN 96
#define MONGOCRYPT_IV_KEY_LEN 32
#define MONGOCRYPT_MAC_KEY_LEN 32
#define MONGOCRYPT_ENC_KEY_LEN 32
#define MONGOCRYPT_IV_LEN 16
#define MONGOCRYPT_HMAC_SHA512_LEN 64
#define MONGOCRYPT_HMAC_LEN 32
#define MONGOCRYPT_BLOCK_SIZE 16
#define MONGOCRYPT_HMAC_SHA256_LEN 32
#define MONGOCRYPT_TOKEN_KEY_LEN 32

typedef struct {
    int hooks_enabled;
    mongocrypt_crypto_fn aes_256_cbc_encrypt;
    mongocrypt_crypto_fn aes_256_cbc_decrypt;
    mongocrypt_crypto_fn aes_256_ctr_encrypt;
    mongocrypt_crypto_fn aes_256_ctr_decrypt;
    mongocrypt_crypto_fn aes_256_ecb_encrypt;
    mongocrypt_random_fn random;
    mongocrypt_hmac_fn hmac_sha_512;
    mongocrypt_hmac_fn hmac_sha_256;
    mongocrypt_hash_fn sha_256;
    void *ctx;
} _mongocrypt_crypto_t;

typedef uint32_t (*_mongocrypt_ciphertextlen_fn)(uint32_t plaintext_len, mongocrypt_status_t *status);
typedef uint32_t (*_mongocrypt_plaintextlen_fn)(uint32_t ciphertext_len, mongocrypt_status_t *status);
typedef bool (*_mongocrypt_do_encryption_fn)(_mongocrypt_crypto_t *crypto,
                                             const _mongocrypt_buffer_t *iv,
                                             const _mongocrypt_buffer_t *associated_data,
                                             const _mongocrypt_buffer_t *key,
                                             const _mongocrypt_buffer_t *plaintext,
                                             _mongocrypt_buffer_t *ciphertext,
                                             uint32_t *bytes_written,
                                             mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;
typedef bool (*_mongocrypt_do_decryption_fn)(_mongocrypt_crypto_t *crypto,
                                             const _mongocrypt_buffer_t *associated_data,
                                             const _mongocrypt_buffer_t *key,
                                             const _mongocrypt_buffer_t *ciphertext,
                                             _mongocrypt_buffer_t *plaintext,
                                             uint32_t *bytes_written,
                                             mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

/**
 * Defines the application layer protocol to use when
 * encrypting client data values.
 */
typedef struct {
    _mongocrypt_ciphertextlen_fn get_ciphertext_len;
    _mongocrypt_plaintextlen_fn get_plaintext_len;
    _mongocrypt_do_encryption_fn do_encrypt;
    _mongocrypt_do_decryption_fn do_decrypt;
} _mongocrypt_value_encryption_algorithm_t;

// FLE1 algorithm: AES-256-CBC HMAC/SHA-512-256 (SHA-512 truncated to 256 bits)
// Algorithm is documented in [FLE and
// AEAD](https://docs.google.com/document/d/1D8xTXWo1B1dunO0bDZhPdolKTMbbD5fUIgsERubWRmY)
const _mongocrypt_value_encryption_algorithm_t *_mcFLE1Algorithm();

// FLE2 general algorithm: AES-256-CTR HMAC/SHA-256
// Algorithm is documented in [AEAD with
// CTR](https://docs.google.com/document/d/1eCU7R8Kjr-mdyz6eKvhNIDVmhyYQcAaLtTfHeK7a_vE/).
const _mongocrypt_value_encryption_algorithm_t *_mcFLE2AEADAlgorithm();

// FLE2 used with FLE2IndexedEncryptedValue: AES-256-CTR no HMAC
const _mongocrypt_value_encryption_algorithm_t *_mcFLE2Algorithm();

// FLE2AEAD general algorithm: AES-256-CBC HMAC/SHA-256
const _mongocrypt_value_encryption_algorithm_t *_mcFLE2v2AEADAlgorithm();

bool _mongocrypt_random(_mongocrypt_crypto_t *crypto,
                        _mongocrypt_buffer_t *out,
                        uint32_t count,
                        mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

/* Generates a random number in the range [0, exclusive_upper_bound) in out. */
bool _mongocrypt_random_uint64(_mongocrypt_crypto_t *crypto,
                               uint64_t exclusive_upper_bound,
                               uint64_t *out,
                               mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

/* Generates a random number in the range [0, exclusive_upper_bound) in out. */
bool _mongocrypt_random_int64(_mongocrypt_crypto_t *crypto,
                              int64_t exclusive_upper_bound,
                              int64_t *out,
                              mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

/* Returns 0 if equal, non-zero otherwise */
int _mongocrypt_memequal(const void *const b1, const void *const b2, size_t len);

/*
 * _mongocrypt_wrap_key encrypts a DEK with a KEK.

 * kek is an input Key Encryption Key.
 * dek is an input Data Encryption Key.
 * encrypted_dek the result of encrypting dek with kek.
 * encrypted_dek is always initialized.
 * Returns true if no error occurred.
 * Returns false and sets @status if an error occurred.
 */
bool _mongocrypt_wrap_key(_mongocrypt_crypto_t *crypto,
                          _mongocrypt_buffer_t *kek,
                          _mongocrypt_buffer_t *dek,
                          _mongocrypt_buffer_t *encrypted_dek,
                          mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

/*
 * _mongocrypt_unwrap_key decrypts an encrypted DEK with a KEK.
 *
 * kek is an input Key Encryption Key.
 * encrypted_dek is an input encrypted Data Encryption Key.
 * dek is the result of decrypting encrypted_dek with kek.
 * dek is always initialized.
 * Returns true if no error occurred.
 * Returns false and sets @status if an error occurred.
 */
bool _mongocrypt_unwrap_key(_mongocrypt_crypto_t *crypto,
                            _mongocrypt_buffer_t *kek,
                            _mongocrypt_buffer_t *encrypted_dek,
                            _mongocrypt_buffer_t *dek,
                            mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_calculate_deterministic_iv(_mongocrypt_crypto_t *crypto,
                                            const _mongocrypt_buffer_t *key,
                                            const _mongocrypt_buffer_t *plaintext,
                                            const _mongocrypt_buffer_t *associated_data,
                                            _mongocrypt_buffer_t *out,
                                            mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

/*
 * _mongocrypt_hmac_sha_256 computes the HMAC SHA-256.
 *
 * Uses the hmac_sha_256 hook set on @crypto if set, and otherwise
 * calls the native implementation.
 *
 * @out must have length 32 bytes.
 *
 * Returns true if no error occurred.
 * Returns false sets @status if an error occurred.
 */
bool _mongocrypt_hmac_sha_256(_mongocrypt_crypto_t *crypto,
                              const _mongocrypt_buffer_t *key,
                              const _mongocrypt_buffer_t *in,
                              _mongocrypt_buffer_t *out,
                              mongocrypt_status_t *status);

/* Crypto implementations must implement these functions. */

/* This variable must be defined in implementation
   files, and must be set to true when _crypto_init
   is successful. */
extern bool _native_crypto_initialized;

void _native_crypto_init(void);

typedef struct {
    const _mongocrypt_buffer_t *key;
    const _mongocrypt_buffer_t *iv;
    const _mongocrypt_buffer_t *in;
    _mongocrypt_buffer_t *out;
    uint32_t *bytes_written;
    mongocrypt_status_t *status;
} aes_256_args_t;

bool _native_crypto_aes_256_cbc_encrypt(aes_256_args_t args) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _native_crypto_aes_256_cbc_decrypt(aes_256_args_t args) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _native_crypto_hmac_sha_512(const _mongocrypt_buffer_t *key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _native_crypto_random(_mongocrypt_buffer_t *out,
                           uint32_t count,
                           mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _native_crypto_aes_256_ctr_encrypt(aes_256_args_t args) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _native_crypto_aes_256_ctr_decrypt(aes_256_args_t args) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _native_crypto_hmac_sha_256(const _mongocrypt_buffer_t *key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

#endif /* MONGOCRYPT_CRYPTO_PRIVATE_H */
