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

#ifndef MONGOCRYPT_INDEXED_ENCRYPTED_VALUE_PRIVATE_V2_H
#define MONGOCRYPT_INDEXED_ENCRYPTED_VALUE_PRIVATE_V2_H

#include "mc-tokens-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-status-private.h"

/*
 * FLE2IndexedEqualityEncryptedValueV2 and FLE2IndexedRangeEncryptedValueV2
 * share a common internal implementation.  Accessors such as add/get_[SK]_Key
 * may be called for either type and produce appropriate results,
 * however the _parse() method is unique per type.
 *
 * Lifecycle:
 * 1. mc_FLE2IndexedEncryptedValueV2_init
 * 2. mc_FLE2Indexed(Equality|Range)EncryptedValueV2_parse
 * 3. mc_FLE2IndexedEncryptedValueV2_get_S_KeyId
 * 4. mc_FLE2IndexedEncryptedValueV2_add_S_Key
 * 5. mc_FLE2IndexedEncryptedValueV2_get_K_KeyId
 * 6. mc_FLE2IndexedEncryptedValueV2_add_K_Key
 * 7. mc_FLE2IndexedEncryptedValueV2_get_ClientValue
 * 8. mc_FLE2IndexedEncryptedValueV2_destroy
 */

typedef struct _mc_FLE2IndexedEncryptedValueV2_t mc_FLE2IndexedEncryptedValueV2_t;

mc_FLE2IndexedEncryptedValueV2_t *mc_FLE2IndexedEncryptedValueV2_new(void);

bool mc_FLE2IndexedEncryptedValueV2_parse(mc_FLE2IndexedEncryptedValueV2_t *iev,
                                          const _mongocrypt_buffer_t *buf,
                                          mongocrypt_status_t *status);

bson_type_t mc_FLE2IndexedEncryptedValueV2_get_bson_value_type(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                               mongocrypt_status_t *status);

const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValueV2_get_S_KeyId(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                                       mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValueV2_add_S_Key(_mongocrypt_crypto_t *crypto,
                                              mc_FLE2IndexedEncryptedValueV2_t *iev,
                                              const _mongocrypt_buffer_t *S_Key,
                                              mongocrypt_status_t *status);

const _mongocrypt_buffer_t *
mc_FLE2IndexedEncryptedValueV2_get_ClientEncryptedValue(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                        mongocrypt_status_t *status);

const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValueV2_get_K_KeyId(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                                       mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValueV2_add_K_Key(_mongocrypt_crypto_t *crypto,
                                              mc_FLE2IndexedEncryptedValueV2_t *iev,
                                              const _mongocrypt_buffer_t *K_Key,
                                              mongocrypt_status_t *status);

const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValueV2_get_ClientValue(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                                           mongocrypt_status_t *status);

void mc_FLE2IndexedEncryptedValueV2_destroy(mc_FLE2IndexedEncryptedValueV2_t *iev);

/*
 * FLE2IndexedEqualityEncryptedValueV2 has the following data layout:
 *
 * struct FLE2IndexedEqualityEncryptedValueV2 {
 *   uint8_t fle_blob_subtype = 14;
 *   uint8_t S_KeyId[16];
 *   uint8_t original_bson_type;
 *   uint8_t ServerEncryptedValue[ServerEncryptedValue.length];
 *   FLE2TagAndEncryptedMetadataBlock metadata;
 * }
 *
 * ServerEncryptedValue :=
 *   EncryptCTR(ServerEncryptionToken, K_KeyId || ClientEncryptedValue)
 * ClientEncryptedValue := EncryptCBCAEAD(K_Key, clientValue, AD=K_KeyId)
 *
 * The MetadataBlock is ignored by libmongocrypt,
 *   but has the following structure and a fixed size of 96 octets:
 *
 * struct FLE2TagAndEncryptedMetadataBlock {
 *   uint8_t encryptedCount[32]; // EncryptCTR(countEncryptionToken,
 *                               //            count || contentionFactor)
 *   uint8_t tag[32];            // HMAC-SHA256(count, edcTwiceDerived)
 *   uint8_t encryptedZeros[32]; // EncryptCTR(zerosEncryptionToken, 0*)
 * }
 */

bool mc_FLE2IndexedEqualityEncryptedValueV2_parse(mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                  const _mongocrypt_buffer_t *buf,
                                                  mongocrypt_status_t *status);

/*
 * FLE2IndexedRangeEncryptedValueV2 has the following data layout:
 *
 * struct FLE2IndexedRangeEncryptedValueV2 {
 *   uint8_t fle_blob_subtype = 15;
 *   uint8_t S_KeyId[16];
 *   uint8_t original_bson_type;
 *   uint8_t edge_count;
 *   uint8_t ServerEncryptedValue[ServerEncryptedValue.length];
 *   FLE2TagAndEncryptedMetadataBlock metadata[edge_count];
 * }
 *
 * Note that this format differs from FLE2IndexedEqualityEncryptedValueV2
 * in only two ways:
 * 1/ `edge_count` is introduced as an octet following `original_bson_type`.
 * 2/ Rather than a single metadata block, we have {edge_count} blocks.
 *
 * Since libmongocrypt ignores metadata blocks, we can ignore most all
 * differences between Equality and Range types for IndexedEncrypted data.
 */

bool mc_FLE2IndexedRangeEncryptedValueV2_parse(mc_FLE2IndexedEncryptedValueV2_t *iev,
                                               const _mongocrypt_buffer_t *buf,
                                               mongocrypt_status_t *status);

#endif /* MONGOCRYPT_INDEXED_ENCRYPTED_VALUE_PRIVATE_V2_H */
