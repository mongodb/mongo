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

#ifndef MC_RANGE_MINCOVER_PRIVATE_H
#define MC_RANGE_MINCOVER_PRIVATE_H

#include "mc-dec128.h"
#include "mc-optional-private.h"
#include "mongocrypt-status-private.h"
#include <stddef.h> // size_t
#include <stdint.h>

// mc_mincover_t represents the results of the mincover algorithm.
typedef struct _mc_mincover_t mc_mincover_t;

// mc_mincover_get returns edge at an index.
// Returns NULL if `index` is out of range.
const char *mc_mincover_get(mc_mincover_t *mincover, size_t index);

// mc_mincover_len returns the number of represented mincover.
size_t mc_mincover_len(mc_mincover_t *mincover);

// mc_mincover_destroys frees `mincover`.
void mc_mincover_destroy(mc_mincover_t *mincover);

typedef struct {
    int32_t lowerBound;
    bool includeLowerBound;
    int32_t upperBound;
    bool includeUpperBound;
    mc_optional_int32_t min;
    mc_optional_int32_t max;
    size_t sparsity;
} mc_getMincoverInt32_args_t;

// mc_getMincoverInt32 implements the Mincover Generation algorithm described in
// SERVER-68600 for int32_t.
mc_mincover_t *mc_getMincoverInt32(mc_getMincoverInt32_args_t args,
                                   mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

typedef struct {
    int64_t lowerBound;
    bool includeLowerBound;
    int64_t upperBound;
    bool includeUpperBound;
    mc_optional_int64_t min;
    mc_optional_int64_t max;
    size_t sparsity;
} mc_getMincoverInt64_args_t;

// mc_getMincoverInt64 implements the Mincover Generation algorithm described in
// SERVER-68600 for int64_t.
mc_mincover_t *mc_getMincoverInt64(mc_getMincoverInt64_args_t args,
                                   mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

typedef struct {
    double lowerBound;
    bool includeLowerBound;
    double upperBound;
    bool includeUpperBound;
    size_t sparsity;
    mc_optional_double_t min;
    mc_optional_double_t max;
    mc_optional_uint32_t precision;
} mc_getMincoverDouble_args_t;

// mc_getMincoverDouble implements the Mincover Generation algorithm described
// in SERVER-68600 for double.
mc_mincover_t *mc_getMincoverDouble(mc_getMincoverDouble_args_t args,
                                    mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
typedef struct {
    mc_dec128 lowerBound;
    bool includeLowerBound;
    mc_dec128 upperBound;
    bool includeUpperBound;
    size_t sparsity;
    mc_optional_dec128_t min, max;
    mc_optional_uint32_t precision;
} mc_getMincoverDecimal128_args_t;

// mc_getMincoverDecimal128 implements the Mincover Generation algorithm
// described in SERVER-68600 for Decimal128 (as mc_dec128).
mc_mincover_t *mc_getMincoverDecimal128(mc_getMincoverDecimal128_args_t args,
                                        mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;
#endif // MONGOCRYPT_HAVE_DECIMAL128_SUPPORT

#endif /* MC_RANGE_MINCOVER_PRIVATE_H */
