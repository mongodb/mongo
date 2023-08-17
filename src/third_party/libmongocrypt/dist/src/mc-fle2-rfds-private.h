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

#include "mc-fle2-range-operator-private.h"
#include "mc-rangeopts-private.h"
#include "mongocrypt-buffer-private.h"
#include <bson/bson.h>
#include <mongocrypt.h>

typedef struct {
    const char *field;

    struct {
        bool set;
        bson_iter_t value;
        bool included;
    } lower;

    struct {
        bool set;
        bson_iter_t value;
        bool included;
    } upper;

    bool isAggregateExpression;
    int nOps;
    mc_FLE2RangeOperator_t firstOp;
    mc_FLE2RangeOperator_t secondOp;
} mc_FLE2RangeFindDriverSpec_t;

// mc_FLE2RangeFindDriverSpec_parse parses a FLE2RangeFindDriverSpec document.
bool mc_FLE2RangeFindDriverSpec_parse(mc_FLE2RangeFindDriverSpec_t *spec,
                                      const bson_t *in,
                                      mongocrypt_status_t *status);

// mc_FLE2RangeFindDriverSpec_to_placeholders creates a new document with
// placeholders to encrypt.
//
// `out` must be initialized by caller.
bool mc_FLE2RangeFindDriverSpec_to_placeholders(mc_FLE2RangeFindDriverSpec_t *spec,
                                                const mc_RangeOpts_t *range_opts,
                                                int64_t maxContentionCounter,
                                                const _mongocrypt_buffer_t *user_key_id,
                                                const _mongocrypt_buffer_t *index_key_id,
                                                int32_t payloadId,
                                                bson_t *out,
                                                mongocrypt_status_t *status);

typedef struct {
    // isStub is true when edgesInfo is not appended.
    bool isStub;
    const _mongocrypt_buffer_t *user_key_id;
    const _mongocrypt_buffer_t *index_key_id;
    bson_iter_t lowerBound;
    bool lbIncluded;
    bson_iter_t upperBound;
    bool ubIncluded;
    int32_t payloadId;
    mc_FLE2RangeOperator_t firstOp;
    mc_FLE2RangeOperator_t secondOp;
    bson_iter_t indexMin;
    bson_iter_t indexMax;
    int64_t maxContentionCounter;
    int64_t sparsity;
    mc_optional_uint32_t precision;
} mc_makeRangeFindPlaceholder_args_t;

// mc_makeRangeFindPlaceholder creates a placeholder to be consumed by
// libmongocrypt to encrypt a range find query. It is included in the header to
// be used by tests.
bool mc_makeRangeFindPlaceholder(mc_makeRangeFindPlaceholder_args_t *args,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status);

// mc_getNextPayloadId returns a payload ID. It is thread safe. It resets to 0
// after reaching INT32_MAX.
int32_t mc_getNextPayloadId(void);
