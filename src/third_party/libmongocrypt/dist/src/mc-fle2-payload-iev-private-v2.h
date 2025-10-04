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

#include "mc-fle2-tag-and-encrypted-metadata-block-private.h"
#include "mc-tokens-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-status-private.h"

/*
 * FLE2IndexedEqualityEncryptedValueV2, FLE2IndexedRangeEncryptedValueV2, and FLEIndexedTextEncryptedValue
 * share a common internal implementation.
 *
 * Lifecycle:
 * 1. mc_FLE2IndexedEncryptedValueV2_init
 * 2. mc_FLE2IndexedEncryptedValueV2_parse
 * 3. mc_FLE2IndexedEncryptedValueV2_get_S_KeyId
 * 4. mc_FLE2IndexedEncryptedValueV2_add_S_Key
 * 5. mc_FLE2IndexedEncryptedValueV2_get_K_KeyId
 * 6. mc_FLE2IndexedEncryptedValueV2_add_K_Key
 * 7. mc_FLE2IndexedEncryptedValueV2_get_ClientValue
 * 8. mc_FLE2IndexedEncryptedValueV2_serialize
 * 9. mc_FLE2IndexedEncryptedValueV2_destroy
 *
 *
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
 *
 * struct FLE2TagAndEncryptedMetadataBlock {
 *   uint8_t encryptedCount[32]; // EncryptCTR(countEncryptionToken,
 *                               //            count || contentionFactor)
 *   uint8_t tag[32];            // HMAC-SHA256(count, edcTwiceDerived)
 *   uint8_t encryptedZeros[32]; // EncryptCTR(zerosEncryptionToken, 0*)
 * }
 *
 *
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
 * 1/ `edge_count` is introduced as a 8 bit int following `original_bson_type`.
 * 2/ Rather than a single metadata block, we have {edge_count} blocks.
 *
 * FLE2IndexedTextEncryptedValue has the following data layout:
 *
 * struct FLE2IndexedTextEncryptedValue {
 *   uint8_t fle_blob_subtype = 17;
 *   uint8_t S_KeyId[16];
 *   uint8_t original_bson_type;
 *   uint32_t edge_count;
 *   uint32_t substr_tag_count;
 *   uint32_t suffix_tag_count;
 *   uint8_t ServerEncryptedValue[ServerEncryptedValue.length];
 *   FLE2TagAndEncryptedMetadataBlock exact_metadata;
 *   FLE2TagAndEncryptedMetadataBlock substr_metadata[substr_tag_count];
 *   FLE2TagAndEncryptedMetadataBlock suffix_metadata[suffix_tag_count];
 *   FLE2TagAndEncryptedMetadataBlock prefix_metadata[edge_count - suffix_tag_count - substr_tag_count - 1];
 * }
 * The main difference in this format is that we split `metadata` into 4
 * sections, one for each text search index type. We expand edge_count
 * to be a 32 bit integer rather than 8 bit. We add two 32 bit ints,
 * `substr_tag_count` and `suffix_tag_count`, following `edge_count`
 * in order to track the delineation of the metadata. Similarly to
 * FLE2IndexedEqualityEncryptedValueV2, we have `edge_count` total
 * blocks.
 */

typedef enum {
    kFLE2IEVTypeInitV2,
    kFLE2IEVTypeEqualityV2,
    kFLE2IEVTypeRangeV2,
    kFLE2IEVTypeText,
} _mc_fle2_iev_v2_type;

typedef struct _mc_FLE2IndexedEncryptedValueV2_t {
    // Raw payload values
    uint8_t fle_blob_subtype;
    uint8_t bson_value_type;
    uint32_t edge_count;
    uint32_t substr_tag_count;
    uint32_t suffix_tag_count;
    _mongocrypt_buffer_t S_KeyId;
    _mongocrypt_buffer_t ServerEncryptedValue;

    // Decode State
    _mc_fle2_iev_v2_type type;
    bool ClientEncryptedValueDecoded;
    bool ClientValueDecoded;

    // Populated during _add_S_Key
    // DecryptedServerEncryptedValue := DecryptCTR(S_Key, ServerEncryptedValue)
    _mongocrypt_buffer_t DecryptedServerEncryptedValue;

    // Views on DecryptedServerEncryptedValue (DSEV)
    _mongocrypt_buffer_t K_KeyId;              // First 16 octets, UUID
    _mongocrypt_buffer_t ClientEncryptedValue; // Remainder of DSEV

    // Populated during _add_K_Key
    // ClientValue := DecryptCBCAEAD(K_Key, ClientEncryptedValue, AD=K_KeyId)
    _mongocrypt_buffer_t ClientValue;

    mc_FLE2TagAndEncryptedMetadataBlock_t *metadata;
} mc_FLE2IndexedEncryptedValueV2_t;

mc_FLE2IndexedEncryptedValueV2_t *mc_FLE2IndexedEncryptedValueV2_new(void);
bson_type_t mc_FLE2IndexedEncryptedValueV2_get_bson_value_type(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                               mongocrypt_status_t *status);

/*
 * Populates an mc_FLE2IndexedEncryptedValueV2_t from a buffer.
 *
 * Input buffer must take the form of:
 * fle_blob_subtype (8u)
 * S_KeyId (8u * 16u)
 * original_bson_type (8u)
 * if (range)
 *   edge_count(8u)
 * if (text)
 *   edge_count(32u)
 *   substr_tag_count(32u)
 *   suffix_tag_count(32u)
 * ServerEncryptedValue (8u * SEV_len)
 * metadata (96u * {range || text ? edge_count : 1u})
 *
 * Returns an error if the input buffer is not valid.
 */
bool mc_FLE2IndexedEncryptedValueV2_parse(mc_FLE2IndexedEncryptedValueV2_t *iev,
                                          const _mongocrypt_buffer_t *buf,
                                          mongocrypt_status_t *status);

/*
 * Serializes an mc_FLE2IndexedEncryptedValueV2_t into a buffer.
 *
 * The serialized output follows the same layout as the input `buf` to
 * mc_FLE2IndexedEncryptedValueV2_parse, allowing for round-trip
 * conversions between the serialized and parsed forms.
 *
 * Returns an error if the input structure is not valid, or if the buffer
 * provided is insufficient to hold the serialized data.
 */
bool mc_FLE2IndexedEncryptedValueV2_serialize(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                              _mongocrypt_buffer_t *buf,
                                              mongocrypt_status_t *status);

/**
 * Validates that a mc_FLE2IndexedEncryptedValueV2_t is well-formed, i.e. values are in their valid
 * ranges and buffers are correctly sized. Returns an error if the input structure is invalid.
 */
bool mc_FLE2IndexedEncryptedValueV2_validate(const mc_FLE2IndexedEncryptedValueV2_t *iev, mongocrypt_status_t *status);

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

uint32_t mc_FLE2IndexedEncryptedValueV2_get_edge_count(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                       mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValueV2_get_substr_tag_count(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                         uint32_t *count,
                                                         mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValueV2_get_suffix_tag_count(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                         uint32_t *count,
                                                         mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValueV2_get_prefix_tag_count(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                         uint32_t *count,
                                                         mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValueV2_get_edge(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                             mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                             const uint32_t edge_index,
                                             mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValueV2_get_metadata(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                 mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                                 mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValueV2_get_exact_metadata(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                       mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                                       mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValueV2_get_substr_metadata(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                        mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                                        const uint32_t block_index,
                                                        mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValueV2_get_suffix_metadata(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                        mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                                        const uint32_t block_index,
                                                        mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValueV2_get_prefix_metadata(const mc_FLE2IndexedEncryptedValueV2_t *iev,
                                                        mc_FLE2TagAndEncryptedMetadataBlock_t *out,
                                                        const uint32_t block_index,
                                                        mongocrypt_status_t *status);

void mc_FLE2IndexedEncryptedValueV2_destroy(mc_FLE2IndexedEncryptedValueV2_t *iev);

#endif /* MONGOCRYPT_INDEXED_ENCRYPTED_VALUE_PRIVATE_V2_H */
