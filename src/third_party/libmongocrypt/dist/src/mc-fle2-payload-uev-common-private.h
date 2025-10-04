/*
 * Copyright 2023-present MongoDB, Inc.
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

#ifndef MONGOCRYPT_FLE2_UNINDEXED_ENCRYPTED_VALUE_COMMON_PRIVATE_H
#define MONGOCRYPT_FLE2_UNINDEXED_ENCRYPTED_VALUE_COMMON_PRIVATE_H

#include "mc-fle-blob-subtype-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-status-private.h"

/**
 * Deserializes the data in @buf and assigns the parsed values to
 * to the output parameters.
 * Callers are expected to call _mongocrypt_buffer_init() on the
 * @key_uuid and @ciphertext output buffers prior to this call, and
 * call _mongocrypt_buffer_cleanup() afterwards.
 * Returns false and sets @status on error.
 */
bool _mc_FLE2UnindexedEncryptedValueCommon_parse(const _mongocrypt_buffer_t *buf,
                                                 uint8_t *fle_blob_subtype,
                                                 uint8_t *original_bson_type,
                                                 _mongocrypt_buffer_t *key_uuid,
                                                 _mongocrypt_buffer_t *ciphertext,
                                                 mongocrypt_status_t *status);

/**
 * Decrypts @ciphertext onto the @plaintext buffer. The @plaintext
 * pointer is returned on success. The @fle_blob_subtype must be an
 * unindexed type, and determines the decryption algorithm to use.
 * Returns NULL and sets @status on error.
 */
const _mongocrypt_buffer_t *_mc_FLE2UnindexedEncryptedValueCommon_decrypt(_mongocrypt_crypto_t *crypto,
                                                                          mc_fle_blob_subtype_t fle_blob_subtype,
                                                                          const _mongocrypt_buffer_t *key_uuid,
                                                                          bson_type_t original_bson_type,
                                                                          const _mongocrypt_buffer_t *ciphertext,
                                                                          const _mongocrypt_buffer_t *key,
                                                                          _mongocrypt_buffer_t *plaintext,
                                                                          mongocrypt_status_t *status);

/**
 * Encrypts @plaintext onto the @out buffer. The @fle_blob_subtype must
 * be an unindexed type, and determines the encryption algorithm to use.
 * Returns false and sets @status on error.
 */
bool _mc_FLE2UnindexedEncryptedValueCommon_encrypt(_mongocrypt_crypto_t *crypto,
                                                   mc_fle_blob_subtype_t fle_blob_subtype,
                                                   const _mongocrypt_buffer_t *key_uuid,
                                                   bson_type_t original_bson_type,
                                                   const _mongocrypt_buffer_t *plaintext,
                                                   const _mongocrypt_buffer_t *key,
                                                   _mongocrypt_buffer_t *out,
                                                   mongocrypt_status_t *status);

#endif /* MONGOCRYPT_FLE2_UNINDEXED_ENCRYPTED_VALUE_COMMON_PRIVATE_H */
