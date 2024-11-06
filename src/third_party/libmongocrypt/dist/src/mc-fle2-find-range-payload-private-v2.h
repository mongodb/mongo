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

#ifndef MC_FLE2_FIND_RANGE_PAYLOAD_PRIVATE_V2_H
#define MC_FLE2_FIND_RANGE_PAYLOAD_PRIVATE_V2_H

#include "mongocrypt-buffer-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt.h"

#include "mc-array-private.h"
#include "mc-fle2-range-operator-private.h"
#include "mc-optional-private.h"

/** FLE2FindRangePayloadEdgesInfoV2 represents the token information for a range
 * find query. It is encoded inside an FLE2FindRangePayloadV2.
 */
typedef struct {
    mc_array_t edgeFindTokenSetArray; // g
    int64_t maxContentionFactor;      // cm
} mc_FLE2FindRangePayloadEdgesInfoV2_t;

/**
 * FLE2FindRangePayloadV2 represents an FLE2 payload of a range indexed field to
 * query. It is created client side.
 *
 * FLE2FindRangePayloadV2 has the following data layout:
 *
 * struct {
 *   uint8_t fle_blob_subtype = 13;
 *   uint8_t bson[];
 * } FLE2FindRangePayloadV2;
 *
 * bson is a BSON document of this form:
 * payload: <document>
 *  g: array<EdgeFindTokenSetV2> // Array of Edges
 *  cm: <int64> // Queryable Encryption max counter
 * payloadId: <int32> // Payload ID.
 * firstOperator: <int32>
 * secondOperator: <int32>
 * sp: optional<int64> // Sparsity.
 * pn: optional<int32> // Precision.
 * tf: optional<int32> // Trim Factor.
 * mn: optional<any> // Index Min.
 * mx: optional<any> // Index Max.
 */
typedef struct {
    struct {
        mc_FLE2FindRangePayloadEdgesInfoV2_t value;
        bool set;
    } payload;

    // payloadId Id of payload - must be paired with another payload.
    int32_t payloadId;
    // firstOperator represents the first query operator for which this payload
    // was generated.
    mc_FLE2RangeOperator_t firstOperator;
    // secondOperator represents the second query operator for which this payload
    // was generated. Only populated for two-sided ranges. It is 0 if unset.
    mc_FLE2RangeOperator_t secondOperator;
    mc_optional_int64_t sparsity;   // sp
    mc_optional_int32_t precision;  // pn
    mc_optional_int32_t trimFactor; // tf
    bson_value_t indexMin;          // mn
    bson_value_t indexMax;          // mx
} mc_FLE2FindRangePayloadV2_t;

// `mc_FLE2FindRangePayloadV2_t` inherits extended alignment from libbson. To dynamically allocate, use aligned
// allocation (e.g. BSON_ALIGNED_ALLOC)
BSON_STATIC_ASSERT2(alignof_mc_FLE2FindRangePayloadV2_t,
                    BSON_ALIGNOF(mc_FLE2FindRangePayloadV2_t) >= BSON_ALIGNOF(bson_value_t));

/**
 * EdgeFindTokenSetV2 is the following BSON document:
 * d: <binary> // EDCDerivedFromDataTokenAndContentionFactor
 * s: <binary> // ESCDerivedFromDataTokenAndContentionFactor
 * l: <binary> // ServerDerivedFromDataToken
 *
 * Instances of mc_EdgeFindTokenSetV2_t are expected to be owned by
 * mc_FLE2FindRangePayloadV2_t and are freed in
 * mc_FLE2FindRangePayloadV2_cleanup.
 */
typedef struct {
    _mongocrypt_buffer_t edcDerivedToken;            // d
    _mongocrypt_buffer_t escDerivedToken;            // s
    _mongocrypt_buffer_t serverDerivedFromDataToken; // l
} mc_EdgeFindTokenSetV2_t;

void mc_FLE2FindRangePayloadV2_init(mc_FLE2FindRangePayloadV2_t *payload);

bool mc_FLE2FindRangePayloadV2_serialize(const mc_FLE2FindRangePayloadV2_t *payload, bson_t *out, bool use_range_v2);

void mc_FLE2FindRangePayloadV2_cleanup(mc_FLE2FindRangePayloadV2_t *payload);

#endif /* MC_FLE2_FIND_RANGE_PAYLOAD_PRIVATE_V2_H */
