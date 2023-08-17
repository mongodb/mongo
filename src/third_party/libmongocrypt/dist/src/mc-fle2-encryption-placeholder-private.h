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

#ifndef MC_FLE2_ENCRYPTION_PLACEHOLDER_PRIVATE_H
#define MC_FLE2_ENCRYPTION_PLACEHOLDER_PRIVATE_H

#include <bson/bson.h>

#include "mc-fle2-find-range-payload-private.h"
#include "mc-optional-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt.h"

/** FLE2RangeFindSpecEdgesInfo represents the information needed to generate
 * edges for a range find query. It is encoded inside an FLE2RangeFindSpec. See
 * https://github.com/mongodb/mongo/blob/master/src/mongo/crypto/fle_field_schema.idl
 * for the representation in the MongoDB server.
 *
 * Bounds on range queries are referred to as lowerBound or lb, and upperBound
 * or ub. Bounds on an underlying range index are referred to as min and max.
 */
typedef struct {
    // lowerBound is the lower bound for an encrypted range query.
    bson_iter_t lowerBound;
    // lbIncluded indicates if the lower bound should be included in the range.
    bool lbIncluded;
    // upperBound is the upperBound for an encrypted range query.
    bson_iter_t upperBound;
    // ubIncluded indicates if the upper bound should be included in the range.
    bool ubIncluded;
    // indexMin is the minimum value for the encrypted index that this query is
    // using.
    bson_iter_t indexMin;
    // indexMax is the maximum value for the encrypted index that this query is
    // using.
    bson_iter_t indexMax;
    // precision determines the number of digits after the decimal point for
    // floating point values.
    mc_optional_uint32_t precision;
} mc_FLE2RangeFindSpecEdgesInfo_t;

/** FLE2RangeFindSpec represents the range find specification that is encoded
 * inside of a FLE2EncryptionPlaceholder. See
 * https://github.com/mongodb/mongo/blob/master/src/mongo/crypto/fle_field_schema.idl
 * for the representation in the MongoDB server.
 */
typedef struct {
    // edgesInfo is the information about the edges in an FLE2 find payload.
    struct {
        mc_FLE2RangeFindSpecEdgesInfo_t value;
        bool set;
    } edgesInfo;

    // payloadId Id of payload - must be paired with another payload.
    int32_t payloadId;
    // firstOperator represents the first query operator for which this payload
    // was generated.
    mc_FLE2RangeOperator_t firstOperator;
    // secondOperator represents the second query operator for which this payload
    // was generated. Only populated for two-sided ranges. It is 0 if unset.
    mc_FLE2RangeOperator_t secondOperator;
} mc_FLE2RangeFindSpec_t;

bool mc_FLE2RangeFindSpec_parse(mc_FLE2RangeFindSpec_t *out, const bson_iter_t *in, mongocrypt_status_t *status);

/** mc_FLE2RangeInsertSpec_t represents the range insert specification that is
 * encoded inside of a FLE2EncryptionPlaceholder. See
 * https://github.com/mongodb/mongo/blob/master/src/mongo/crypto/fle_field_schema.idl#L364
 * for the representation in the MongoDB server. */
typedef struct {
    // v is the value to encrypt.
    bson_iter_t v;
    // min is the Queryable Encryption min bound for range.
    bson_iter_t min;
    // max is the Queryable Encryption max bound for range.
    bson_iter_t max;
    // precision determines the number of digits after the decimal point for
    // floating point values.
    mc_optional_uint32_t precision;
} mc_FLE2RangeInsertSpec_t;

bool mc_FLE2RangeInsertSpec_parse(mc_FLE2RangeInsertSpec_t *out, const bson_iter_t *in, mongocrypt_status_t *status);

/** FLE2EncryptionPlaceholder implements Encryption BinData (subtype 6)
 * sub-subtype 0, the intent-to-encrypt mapping. Contains a value to encrypt and
 * a description of how it should be encrypted.
 *
 * For automatic encryption, FLE2EncryptionPlaceholder is created by query
 * analysis (mongocryptd or mongo_crypt shared library). For explicit
 * encryption, FLE2EncryptionPlaceholder is created by libmongocrypt.
 *
 * FLE2EncryptionPlaceholder is processed by libmongocrypt into a payload
 * suitable to send to the MongoDB server (mongod/mongos).
 *
 * See
 * https://github.com/mongodb/mongo/blob/d870dda33fb75983f628636ff8f849c7f1c90b09/src/mongo/crypto/fle_field_schema.idl#L133
 * for the representation in the MongoDB server.
 */

typedef struct {
    mongocrypt_fle2_placeholder_type_t type;
    mongocrypt_fle2_encryption_algorithm_t algorithm;
    bson_iter_t v_iter;
    _mongocrypt_buffer_t index_key_id;
    _mongocrypt_buffer_t user_key_id;
    int64_t maxContentionCounter;
    // sparsity is the Queryable Encryption range hypergraph sparsity factor
    int64_t sparsity;
} mc_FLE2EncryptionPlaceholder_t;

void mc_FLE2EncryptionPlaceholder_init(mc_FLE2EncryptionPlaceholder_t *placeholder);

bool mc_FLE2EncryptionPlaceholder_parse(mc_FLE2EncryptionPlaceholder_t *out,
                                        const bson_t *in,
                                        mongocrypt_status_t *status);

void mc_FLE2EncryptionPlaceholder_cleanup(mc_FLE2EncryptionPlaceholder_t *placeholder);

/* mc_validate_contention is used to check that contention is a valid
 * value. contention may come from the 'cm' field in FLE2EncryptionPlaceholder
 * or from mongocrypt_ctx_setopt_contention_factor. */
bool mc_validate_contention(int64_t contention, mongocrypt_status_t *status);

/* mc_validate_sparsity is used to check that sparsity is a valid
 * value. */
bool mc_validate_sparsity(int64_t sparsity, mongocrypt_status_t *status);

#endif /* MC_FLE2_ENCRYPTION_PLACEHOLDER_PRIVATE_H */
