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

#ifndef MC_FLE2_INSERT_UPDATE_PAYLOAD_PRIVATE_V2_H
#define MC_FLE2_INSERT_UPDATE_PAYLOAD_PRIVATE_V2_H

#include <bson/bson.h>

#include "mc-array-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt.h"

/**
 * FLE2InsertUpdatePayloadV2 represents an FLE2 payload of an indexed field to
 * insert or update. It is created client side.
 *
 * FLE2InsertUpdatePayloadV2 has the following data layout:
 *
 * struct {
 *   uint8_t fle_blob_subtype = 11;
 *   uint8_t bson[];
 * } FLE2InsertUpdatePayloadV2;
 *
 * bson is a BSON document of this form:
 * d: <binary> // EDCDerivedFromDataTokenAndCounter
 * s: <binary> // ESCDerivedFromDataTokenAndCounter
 * p: <binary> // Encrypted Tokens
 * u: <UUID>   // Index KeyId
 * t: <int32>  // Encrypted type
 * v: <binary> // Encrypted value
 * e: <binary> // ServerDataEncryptionLevel1Token
 * l: <binary> // ServerDerivedFromDataToken
 * k: <int64> // Randomly sampled contention factor value
 * g: array<EdgeTokenSetV2> // Array of Edges
 *
 * p is the result of:
 * Encrypt(
 * key=ECOCToken,
 * plaintext=(
 *    ESCDerivedFromDataTokenAndCounter)
 * )
 *
 * v is the result of:
 * UserKeyId || EncryptAEAD(
 *    key=UserKey,
 *    plaintext=value
 *    associated_data=UserKeyId)
 */

typedef struct {
    _mongocrypt_buffer_t edcDerivedToken;            // d
    _mongocrypt_buffer_t escDerivedToken;            // s
    _mongocrypt_buffer_t encryptedTokens;            // p
    _mongocrypt_buffer_t indexKeyId;                 // u
    bson_type_t valueType;                           // t
    _mongocrypt_buffer_t value;                      // v
    _mongocrypt_buffer_t serverEncryptionToken;      // e
    _mongocrypt_buffer_t serverDerivedFromDataToken; // l
    int64_t contentionFactor;                        // k
    mc_array_t edgeTokenSetArray;                    // g
    _mongocrypt_buffer_t plaintext;
    _mongocrypt_buffer_t userKeyId;
} mc_FLE2InsertUpdatePayloadV2_t;

/**
 * EdgeTokenSetV2 is the following BSON document:
 * d: <binary> // EDCDerivedFromDataTokenAndCounter
 * s: <binary> // ESCDerivedFromDataTokenAndCounter
 * l: <binary> // ServerDerivedFromDataToken
 * p: <binary> // Encrypted Tokens
 *
 * Instances of mc_EdgeTokenSetV2_t are expected to be owned by
 * mc_FLE2InsertUpdatePayloadV2_t and are freed in
 * mc_FLE2InsertUpdatePayloadV2_cleanup.
 */
typedef struct {
    _mongocrypt_buffer_t edcDerivedToken;            // d
    _mongocrypt_buffer_t escDerivedToken;            // s
    _mongocrypt_buffer_t serverDerivedFromDataToken; // l
    _mongocrypt_buffer_t encryptedTokens;            // p
} mc_EdgeTokenSetV2_t;

void mc_FLE2InsertUpdatePayloadV2_init(mc_FLE2InsertUpdatePayloadV2_t *payload);

bool mc_FLE2InsertUpdatePayloadV2_parse(mc_FLE2InsertUpdatePayloadV2_t *out,
                                        const _mongocrypt_buffer_t *in,
                                        mongocrypt_status_t *status);

/* mc_FLE2InsertUpdatePayloadV2_decrypt decrypts ciphertext.
 * Returns NULL and sets @status on error. It is an error to call before
 * mc_FLE2InsertUpdatePayloadV2_parse. */
const _mongocrypt_buffer_t *mc_FLE2InsertUpdatePayloadV2_decrypt(_mongocrypt_crypto_t *crypto,
                                                                 mc_FLE2InsertUpdatePayloadV2_t *iup,
                                                                 const _mongocrypt_buffer_t *user_key,
                                                                 mongocrypt_status_t *status);

bool mc_FLE2InsertUpdatePayloadV2_serialize(const mc_FLE2InsertUpdatePayloadV2_t *payload, bson_t *out);

bool mc_FLE2InsertUpdatePayloadV2_serializeForRange(const mc_FLE2InsertUpdatePayloadV2_t *payload, bson_t *out);

void mc_FLE2InsertUpdatePayloadV2_cleanup(mc_FLE2InsertUpdatePayloadV2_t *payload);

#endif /* MC_FLE2_INSERT_UPDATE_PAYLOAD_PRIVATE_V2_H */
