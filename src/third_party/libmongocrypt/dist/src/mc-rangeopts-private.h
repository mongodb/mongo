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
    mc_optional_uint32_t precision;
} mc_RangeOpts_t;

/* mc_RangeOpts_parse parses a BSON document into mc_RangeOpts_t.
 * The document is expected to have the form:
 * {
 *    "min": BSON value,
 *    "max": BSON value,
 *    "sparsity": Int64,
 *    "precision": Optional<Int32>
 * }
 */
bool mc_RangeOpts_parse(mc_RangeOpts_t *ro, const bson_t *in, mongocrypt_status_t *status);

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

void mc_RangeOpts_cleanup(mc_RangeOpts_t *ro);

#endif // MC_RANGEOPTS_PRIVATE_H
