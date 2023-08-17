/*
 * Copyright 2022-present MongoDB, Inc.
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

#ifndef MONGOCRYPT_FLE2_UNINDEXED_ENCRYPTED_VALUE_PRIVATE_H
#define MONGOCRYPT_FLE2_UNINDEXED_ENCRYPTED_VALUE_PRIVATE_H

#include "mongocrypt-buffer-private.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-status-private.h"

/**
 * FLE2UnindexedEncryptedValue represents an FLE2 unindexed encrypted value.
 * It is created client side.
 *
 * FLE2UnindexedEncryptedValue has the following data layout:
 *
 * struct {
 *   uint8_t fle_blob_subtype = 6;
 *   uint8_t key_uuid[16];
 *   uint8_t original_bson_type;
 *   uint8_t ciphertext[ciphertext_length];
 * } FLE2UnindexedEncryptedValue;
 *
 * ciphertext is the output of:
 *    EncryptAEAD_AES_256_CTR_HMAC_SHA_256(
 *       key=K_Key,
 *       plaintext=ClientValue,
 *       associated_data=(fle_blob_subtype || key_uuid || original_bson_type))
 */

typedef struct _mc_FLE2UnindexedEncryptedValue_t mc_FLE2UnindexedEncryptedValue_t;

mc_FLE2UnindexedEncryptedValue_t *mc_FLE2UnindexedEncryptedValue_new(void);

bool mc_FLE2UnindexedEncryptedValue_parse(mc_FLE2UnindexedEncryptedValue_t *uev,
                                          const _mongocrypt_buffer_t *buf,
                                          mongocrypt_status_t *status);

/* mc_FLE2UnindexedEncryptedValue_get_original_bson_type returns
 * original_bson_type. Returns 0 and sets @status on error.
 * It is an error to call before mc_FLE2UnindexedEncryptedValue_parse. */
bson_type_t mc_FLE2UnindexedEncryptedValue_get_original_bson_type(const mc_FLE2UnindexedEncryptedValue_t *uev,
                                                                  mongocrypt_status_t *status);

/* mc_FLE2UnindexedEncryptedValue_get_key_uuid returns key_uuid. Returns
 * NULL and sets @status on error. It is an error to call before
 * mc_FLE2UnindexedEncryptedValue_parse. */
const _mongocrypt_buffer_t *mc_FLE2UnindexedEncryptedValue_get_key_uuid(const mc_FLE2UnindexedEncryptedValue_t *uev,
                                                                        mongocrypt_status_t *status);

/* mc_FLE2UnindexedEncryptedValue_decrypt decrypts ciphertext.
 * Returns NULL and sets @status on error. It is an error to call before
 * mc_FLE2UnindexedEncryptedValue_parse. */
const _mongocrypt_buffer_t *mc_FLE2UnindexedEncryptedValue_decrypt(_mongocrypt_crypto_t *crypto,
                                                                   mc_FLE2UnindexedEncryptedValue_t *uev,
                                                                   const _mongocrypt_buffer_t *key,
                                                                   mongocrypt_status_t *status);

/* mc_FLE2UnindexedEncryptedValue_encrypt outputs the ciphertext field of
 * FLEUnindexedEncryptedValue into @out. Returns false and sets @status on
 * error. */
bool mc_FLE2UnindexedEncryptedValue_encrypt(_mongocrypt_crypto_t *crypto,
                                            const _mongocrypt_buffer_t *key_uuid,
                                            bson_type_t original_bson_type,
                                            const _mongocrypt_buffer_t *plaintext,
                                            const _mongocrypt_buffer_t *key,
                                            _mongocrypt_buffer_t *out,
                                            mongocrypt_status_t *status);

void mc_FLE2UnindexedEncryptedValue_destroy(mc_FLE2UnindexedEncryptedValue_t *uev);

#endif /* MONGOCRYPT_FLE2_UNINDEXED_ENCRYPTED_VALUE_PRIVATE_H */
