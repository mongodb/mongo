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

#ifndef MC_RANGEOPTS_PRIVATE_H
#define MC_RANGEOPTS_PRIVATE_H

#include <bson/bson.h>

#include "mc-optional-private.h"
#include "mongocrypt-status-private.h"

typedef struct {
    bson_t *bson;

    struct {
        bson_iter_t value;
        bool set;
    } min;

    struct {
        bson_iter_t value;
        bool set;
    } max;

    int64_t sparsity;
    mc_optional_int32_t precision;
    mc_optional_int32_t trimFactor;
} mc_RangeOpts_t;

// `mc_RangeOpts_t` inherits extended alignment from libbson. To dynamically allocate, use
// aligned allocation (e.g. BSON_ALIGNED_ALLOC)
BSON_STATIC_ASSERT2(alignof_mc_RangeOpts_t,
                    BSON_ALIGNOF(mc_RangeOpts_t) >= BSON_MAX(BSON_ALIGNOF(bson_t), BSON_ALIGNOF(bson_iter_t)));

/* mc_RangeOpts_parse parses a BSON document into mc_RangeOpts_t.
 * The document is expected to have the form:
 * {
 *    "min": BSON value,
 *    "max": BSON value,
 *    "sparsity": Optional<Int64>,
 *    "precision": Optional<Int32>,
 *    "trimFactor": Optional<Int32>,
 * }
 */
bool mc_RangeOpts_parse(mc_RangeOpts_t *ro, const bson_t *in, bool use_range_v2, mongocrypt_status_t *status);

/*
 * mc_RangeOpts_to_FLE2RangeInsertSpec creates a placeholder value to be
 * encrypted. It is only expected to be called when query_type is unset. The
 * output FLE2RangeInsertSpec is a BSON document of the form:
 * {
 *    "v": BSON value to encrypt,
 *    "min": BSON value,
 *    "max": BSON value,
 *    "precision": Optional<Int32>
 * }
 *
 * v is expect to be a BSON document of the form:
 * { "v": BSON value to encrypt }.
 *
 * Preconditions: out must be initialized by caller.
 */
bool mc_RangeOpts_to_FLE2RangeInsertSpec(const mc_RangeOpts_t *ro,
                                         const bson_t *v,
                                         bson_t *out,
                                         bool use_range_v2,
                                         mongocrypt_status_t *status);

/* mc_RangeOpts_appendMin appends the minimum value of the range for a given
 * type. If `ro->min` is unset, uses the lowest representable value of the value
 * type. Errors if `valueType` does not match the type of `ro->min`. */
bool mc_RangeOpts_appendMin(const mc_RangeOpts_t *ro,
                            bson_type_t valueType,
                            const char *fieldName,
                            bson_t *out,
                            mongocrypt_status_t *status);

/* mc_RangeOpts_appendMax appends the maximum value of the range for a given
 * type. If `ro->max` is unset, uses the highest representable value of the
 * value type. Errors if `valueType` does not match the type of `ro->max`. */
bool mc_RangeOpts_appendMax(const mc_RangeOpts_t *ro,
                            bson_type_t valueType,
                            const char *fieldName,
                            bson_t *out,
                            mongocrypt_status_t *status);

/* mc_RangeOpts_appendTrimFactor appends the trim factor of the field. If `ro->trimFactor` is unset,
 * defaults to 0. Errors if `ro->trimFactor` is out of bounds based on the size of the domain
 * computed from `valueType`, `ro->min` and `ro->max`. */
bool mc_RangeOpts_appendTrimFactor(const mc_RangeOpts_t *ro,
                                   bson_type_t valueType,
                                   const char *fieldName,
                                   bson_t *out,
                                   mongocrypt_status_t *status,
                                   bool use_range_v2);

void mc_RangeOpts_cleanup(mc_RangeOpts_t *ro);

#endif // MC_RANGEOPTS_PRIVATE_H
