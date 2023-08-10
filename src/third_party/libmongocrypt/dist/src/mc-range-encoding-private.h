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

#ifndef MC_RANGE_ENCODING_PRIVATE_H
#define MC_RANGE_ENCODING_PRIVATE_H

#include "mc-dec128.h"
#include "mc-optional-private.h"
#include "mongocrypt-status-private.h"

#include <mlib/int128.h>

#include <stdint.h>

/* mc-range-encoding-private.h has functions to encode numeric types for
 * Queryable Encryption Range queries. It is a translation from server code:
 * https://github.com/mongodb/mongo/blob/1364f5c5004ac5503837ac5b315c189625f97269/src/mongo/crypto/fle_crypto.h#L1194-L1196
 * "OST" is an abbreviation taken from server code. It stands for "Outsourced
 * STate".
 */

/* mc_OSTType_Int32 describes the encoding of a BSON int32. */
typedef struct {
    uint32_t value;
    uint32_t min;
    uint32_t max;
} mc_OSTType_Int32;

typedef struct {
    int32_t value;
    mc_optional_int32_t min;
    mc_optional_int32_t max;
} mc_getTypeInfo32_args_t;

/* mc_getTypeInfo32 encodes the int32_t `args.value` into an OSTType_Int32
 * `out`. `args.min` and `args.max` may be unset. Returns false and sets
 * `status` on error. */
bool mc_getTypeInfo32(mc_getTypeInfo32_args_t args,
                      mc_OSTType_Int32 *out,
                      mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

/* mc_OSTType_Int64 describes the encoding of a BSON int64. */
typedef struct {
    uint64_t value;
    uint64_t min;
    uint64_t max;
} mc_OSTType_Int64;

typedef struct {
    int64_t value;
    mc_optional_int64_t min;
    mc_optional_int64_t max;
} mc_getTypeInfo64_args_t;

/* mc_getTypeInfo64 encodes the int64_t `args.value` into an OSTType_Int64
 * `out`. `args.min` and `args.max` may be unset. Returns false and sets
 * `status` on error. */
bool mc_getTypeInfo64(mc_getTypeInfo64_args_t args,
                      mc_OSTType_Int64 *out,
                      mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

/* mc_OSTType_Double describes the encoding of a BSON double. */
typedef struct {
    uint64_t value;
    uint64_t min;
    uint64_t max;
} mc_OSTType_Double;

typedef struct {
    double value;
    mc_optional_double_t min;
    mc_optional_double_t max;
    mc_optional_uint32_t precision;
} mc_getTypeInfoDouble_args_t;

/* mc_getTypeInfoDouble encodes the double `args.value` into an OSTType_Double
 * `out`. Returns false and sets `status` on error. */
bool mc_getTypeInfoDouble(mc_getTypeInfoDouble_args_t args,
                          mc_OSTType_Double *out,
                          mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
/**
 * @brief OST-encoding of a Decimal128
 */
typedef struct {
    mlib_int128 value, min, max;
} mc_OSTType_Decimal128;

typedef struct {
    mc_dec128 value;
    mc_optional_dec128_t min, max;
    mc_optional_uint32_t precision;
} mc_getTypeInfoDecimal128_args_t;

/**
 * @brief Obtain the OST encoding of a finite Decimal128 value.
 *
 * @param out Output for the result
 * @param status Output for status on error
 * @retval true On success
 * @retval false Otherwise
 */
bool mc_getTypeInfoDecimal128(mc_getTypeInfoDecimal128_args_t args,
                              mc_OSTType_Decimal128 *out,
                              mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;
#endif // MONGOCRYPT_HAVE_DECIMAL128_SUPPORT

#endif /* MC_RANGE_ENCODING_PRIVATE_H */
