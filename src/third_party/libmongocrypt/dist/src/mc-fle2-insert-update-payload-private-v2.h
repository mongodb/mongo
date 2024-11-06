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
#include "mc-optional-private.h"
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
 * d: <binary> // EDCDerivedFromDataTokenAndContentionFactor
 * s: <binary> // ESCDerivedFromDataTokenAndContentionFactor
 * p: <binary> // Encrypted Tokens
 * u: <UUID>   // Index KeyId
 * t: <int32>  // Encrypted type
 * v: <binary> // Encrypted value
 * e: <binary> // ServerDataEncryptionLevel1Token
 * l: <binary> // ServerDerivedFromDataToken
 * k: <int64> // Randomly sampled contention factor value
 * g: array<EdgeTokenSetV2> // Array of Edges. Only included for range payloads.
 * sp: optional<int64> // Sparsity. Only included for range payloads.
 * pn: optional<int32> // Precision. Only included for range payloads.
 * tf: optional<int32> // Trim Factor. Only included for range payloads.
 * mn: optional<any> // Index Min. Only included for range payloads.
 * mx: optional<any> // Index Max. Only included for range payloads.
 *
 * p is the result of:
 * Encrypt(
 * key=ECOCToken,
 * plaintext=(
 *    ESCDerivedFromDataTokenAndContentionFactor)
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
    mc_optional_int64_t sparsity;                    // sp
    mc_optional_int32_t precision;                   // pn
    mc_optional_int32_t trimFactor;                  // tf
    bson_value_t indexMin;                           // mn
    bson_value_t indexMax;                           // mx
    _mongocrypt_buffer_t plaintext;
    _mongocrypt_buffer_t userKeyId;
} mc_FLE2InsertUpdatePayloadV2_t;

// `mc_FLE2InsertUpdatePayloadV2_t` inherits extended alignment from libbson. To dynamically allocate, use
// aligned allocation (e.g. BSON_ALIGNED_ALLOC)
BSON_STATIC_ASSERT2(alignof_mc_FLE2InsertUpdatePayloadV2_t,
                    BSON_ALIGNOF(mc_FLE2InsertUpdatePayloadV2_t) >= BSON_ALIGNOF(bson_value_t));

/**
 * EdgeTokenSetV2 is the following BSON document:
 * d: <binary> // EDCDerivedFromDataTokenAndContentionFactor
 * s: <binary> // ESCDerivedFromDataTokenAndContentionFactor
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

bool mc_FLE2InsertUpdatePayloadV2_serializeForRange(const mc_FLE2InsertUpdatePayloadV2_t *payload,
                                                    bson_t *out,
                                                    bool use_range_v2);

void mc_FLE2InsertUpdatePayloadV2_cleanup(mc_FLE2InsertUpdatePayloadV2_t *payload);

#endif /* MC_FLE2_INSERT_UPDATE_PAYLOAD_PRIVATE_V2_H */
