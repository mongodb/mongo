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

#include "mongocrypt-status-private.h"

typedef struct {
   bson_t *bson;
   bson_iter_t min;
   bson_iter_t max;
   int64_t sparsity;
} mc_RangeOpts_t;

/* mc_RangeOpts_parse parses a BSON document into mc_RangeOpts_t.
 * The document is expected to have the form:
 * { "min": BSON value, "max": BSON value, "sparsity": Int64 }
 */
bool
mc_RangeOpts_parse (mc_RangeOpts_t *ro,
                    const bson_t *in,
                    mongocrypt_status_t *status);

/*
 * mc_RangeOpts_to_FLE2RangeInsertSpec creates a placeholder value to be
 * encrypted. It is only expected to be called when query_type is unset. The
 * output FLE2RangeInsertSpec is a BSON document of the form:
 * {
 *    "v": BSON value to encrypt,
 *    "min": BSON value,
 *    "max": BSON value
 * }
 *
 * v is expect to be a BSON document of the form:
 * { "v": BSON value to encrypt }.
 *
 * Preconditions: out must be initialized by caller.
 */
bool
mc_RangeOpts_to_FLE2RangeInsertSpec (const mc_RangeOpts_t *ro,
                                     const bson_t *v,
                                     bson_t *out,
                                     mongocrypt_status_t *status);

void
mc_RangeOpts_cleanup (mc_RangeOpts_t *ro);

#endif // MC_RANGEOPTS_PRIVATE_H
