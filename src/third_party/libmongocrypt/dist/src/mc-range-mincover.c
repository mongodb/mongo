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

#include <bson/bson.h>

#include "mc-check-conversions-private.h"

#include "mc-array-private.h"
#include "mc-range-edge-generation-private.h" // mc_count_leading_zeros_u32
#include "mc-range-encoding-private.h"        // mc_getTypeInfo32
#include "mc-range-mincover-private.h"
#include "mongocrypt-private.h"

struct _mc_mincover_t {
    /* mincover is an array of `char*` edge strings. */
    mc_array_t mincover;
};

static mc_mincover_t *mc_mincover_new(void) {
    mc_mincover_t *mincover = bson_malloc0(sizeof(mc_mincover_t));
    _mc_array_init(&mincover->mincover, sizeof(char *));
    return mincover;
}

const char *mc_mincover_get(mc_mincover_t *mincover, size_t index) {
    BSON_ASSERT_PARAM(mincover);
    if (mincover->mincover.len == 0 || index > mincover->mincover.len - 1u) {
        return NULL;
    }
    return _mc_array_index(&mincover->mincover, char *, index);
}

size_t mc_mincover_len(mc_mincover_t *mincover) {
    BSON_ASSERT_PARAM(mincover);
    return mincover->mincover.len;
}

void mc_mincover_destroy(mc_mincover_t *mincover) {
    if (NULL == mincover) {
        return;
    }
    for (size_t i = 0; i < mincover->mincover.len; i++) {
        char *val = _mc_array_index(&mincover->mincover, char *, i);
        bson_free(val);
    }
    _mc_array_destroy(&mincover->mincover);
    bson_free(mincover);
}

#define UINT_T uint32_t
#define UINT_C UINT32_C
#define UINT_FMT_S PRIu32
#define DECORATE_NAME(N) N##_u32
#include "mc-range-mincover-generator.template.h"

#define UINT_T uint64_t
#define UINT_C UINT64_C
#define UINT_FMT_S PRIu64
#define DECORATE_NAME(N) N##_u64
#include "mc-range-mincover-generator.template.h"

// The 128-bit version is only required for Decimal128, otherwise generates
// unused-fn warnings
#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
#define UINT_T mlib_int128
#define UINT_C MLIB_INT128
#define UINT_FMT_S "s"
#define UINT_FMT_ARG(X) (mlib_int128_format(X).str)
#define DECORATE_NAME(N) N##_u128
#define UINT_LESSTHAN(L, R) (mlib_int128_ucmp(L, R) < 0)
#define UINT_ADD mlib_int128_add
#define UINT_SUB mlib_int128_sub
#define UINT_LSHIFT mlib_int128_lshift
#define MC_UINT_MAX MLIB_INT128_UMAX
#define UINT_BITOR mlib_int128_bitor
#include "mc-range-mincover-generator.template.h"
#endif // MONGOCRYPT_HAVE_DECIMAL128_SUPPORT

// Check bounds and return an error message including the original inputs.
#define IDENTITY(X) X
#define LESSTHAN(L, R) ((L) < (R))
#define CHECK_BOUNDS(args, FMT, FormatArg, LessThan)                                                                   \
    if (1) {                                                                                                           \
        if ((args).min.set) {                                                                                          \
            if (LessThan((args).upperBound, (args).min.value)) {                                                       \
                CLIENT_ERR("Upper bound (%" FMT ") must be greater than or equal to the range minimum (%" FMT ")",     \
                           FormatArg((args).upperBound),                                                               \
                           FormatArg((args).min.value));                                                               \
                return false;                                                                                          \
            }                                                                                                          \
            if (!(args).includeUpperBound && !LessThan((args.min.value), (args.upperBound))) {                         \
                CLIENT_ERR("Upper bound (%" FMT ") must be greater than the range minimum (%" FMT                      \
                           ") if upper bound is excluded from range",                                                  \
                           FormatArg((args).upperBound),                                                               \
                           FormatArg((args).min.value));                                                               \
                return false;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
        if ((args).max.set) {                                                                                          \
            if (LessThan((args).max.value, (args).lowerBound)) {                                                       \
                CLIENT_ERR("Lower bound (%" FMT ") must be less than or equal to the range maximum (%" FMT ")",        \
                           FormatArg((args).lowerBound),                                                               \
                           FormatArg((args).max.value));                                                               \
                return false;                                                                                          \
            }                                                                                                          \
            if (!(args).includeLowerBound && !LessThan((args).lowerBound, (args).max.value)) {                         \
                CLIENT_ERR("Lower bound (%" FMT ") must be less than the range maximum (%" FMT                         \
                           ") if lower bound is excluded from range",                                                  \
                           FormatArg((args).lowerBound),                                                               \
                           FormatArg((args).max.value));                                                               \
                return false;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    } else                                                                                                             \
        (void)0

mc_mincover_t *mc_getMincoverInt32(mc_getMincoverInt32_args_t args, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(status);
    CHECK_BOUNDS(args, PRId32, IDENTITY, LESSTHAN);
    mc_OSTType_Int32 a, b;
    if (!mc_getTypeInfo32((mc_getTypeInfo32_args_t){.min = args.min, .max = args.max, .value = args.lowerBound},
                          &a,
                          status)) {
        return NULL;
    }
    if (!mc_getTypeInfo32((mc_getTypeInfo32_args_t){.min = args.min, .max = args.max, .value = args.upperBound},
                          &b,
                          status)) {
        return NULL;
    }

    BSON_ASSERT(a.min == b.min);
    BSON_ASSERT(a.max == b.max);

    if (!adjustBounds_u32(&a.value, args.includeLowerBound, a.min, &b.value, args.includeUpperBound, b.max, status)) {
        return NULL;
    }

    MinCoverGenerator_u32 *mcg = MinCoverGenerator_new_u32(a.value, b.value, a.max, args.sparsity, status);
    if (!mcg) {
        return NULL;
    }
    mc_mincover_t *mc = MinCoverGenerator_minCover_u32(mcg);
    MinCoverGenerator_destroy_u32(mcg);
    return mc;
}

mc_mincover_t *mc_getMincoverInt64(mc_getMincoverInt64_args_t args, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(status);
    CHECK_BOUNDS(args, PRId64, IDENTITY, LESSTHAN);
    mc_OSTType_Int64 a, b;
    if (!mc_getTypeInfo64((mc_getTypeInfo64_args_t){.min = args.min, .max = args.max, .value = args.lowerBound},
                          &a,
                          status)) {
        return NULL;
    }
    if (!mc_getTypeInfo64((mc_getTypeInfo64_args_t){.min = args.min, .max = args.max, .value = args.upperBound},
                          &b,
                          status)) {
        return NULL;
    }

    BSON_ASSERT(a.min == b.min);
    BSON_ASSERT(a.max == b.max);

    if (!adjustBounds_u64(&a.value, args.includeLowerBound, a.min, &b.value, args.includeUpperBound, b.max, status)) {
        return NULL;
    }

    MinCoverGenerator_u64 *mcg = MinCoverGenerator_new_u64(a.value, b.value, a.max, args.sparsity, status);
    if (!mcg) {
        return NULL;
    }
    mc_mincover_t *mc = MinCoverGenerator_minCover_u64(mcg);
    MinCoverGenerator_destroy_u64(mcg);
    return mc;
}

// mc_getMincoverDouble implements the Mincover Generation algorithm described
// in SERVER-68600 for double.
mc_mincover_t *mc_getMincoverDouble(mc_getMincoverDouble_args_t args, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(status);
    CHECK_BOUNDS(args, "g", IDENTITY, LESSTHAN);

    mc_OSTType_Double a, b;
    if (!mc_getTypeInfoDouble((mc_getTypeInfoDouble_args_t){.value = args.lowerBound,
                                                            .min = args.min,
                                                            .max = args.max,
                                                            .precision = args.precision},
                              &a,
                              status)) {
        return NULL;
    }
    if (!mc_getTypeInfoDouble((mc_getTypeInfoDouble_args_t){.value = args.upperBound,
                                                            .min = args.min,
                                                            .max = args.max,
                                                            .precision = args.precision},
                              &b,
                              status)) {
        return NULL;
    }

    BSON_ASSERT(a.min == b.min);
    BSON_ASSERT(a.max == b.max);

    if (!adjustBounds_u64(&a.value, args.includeLowerBound, a.min, &b.value, args.includeUpperBound, b.max, status)) {
        return NULL;
    }

    MinCoverGenerator_u64 *mcg = MinCoverGenerator_new_u64(a.value, b.value, a.max, args.sparsity, status);
    if (!mcg) {
        return NULL;
    }
    mc_mincover_t *mc = MinCoverGenerator_minCover_u64(mcg);
    MinCoverGenerator_destroy_u64(mcg);
    return mc;
}

#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
mc_mincover_t *mc_getMincoverDecimal128(mc_getMincoverDecimal128_args_t args, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(status);
#define ToString(Dec) (mc_dec128_to_string(Dec).str)
    CHECK_BOUNDS(args, "s", ToString, mc_dec128_less);

    mc_OSTType_Decimal128 a, b;
    if (!mc_getTypeInfoDecimal128((mc_getTypeInfoDecimal128_args_t){.value = args.lowerBound,
                                                                    .min = args.min,
                                                                    .max = args.max,
                                                                    .precision = args.precision},
                                  &a,
                                  status)) {
        return NULL;
    }
    if (!mc_getTypeInfoDecimal128((mc_getTypeInfoDecimal128_args_t){.value = args.upperBound,
                                                                    .min = args.min,
                                                                    .max = args.max,
                                                                    .precision = args.precision},
                                  &b,
                                  status)) {
        return NULL;
    }

    BSON_ASSERT(mlib_int128_eq(a.min, b.min));
    BSON_ASSERT(mlib_int128_eq(a.max, b.max));

    if (!adjustBounds_u128(&a.value, args.includeLowerBound, a.min, &b.value, args.includeUpperBound, b.max, status)) {
        return NULL;
    }

    MinCoverGenerator_u128 *mcg = MinCoverGenerator_new_u128(a.value, b.value, a.max, args.sparsity, status);
    if (!mcg) {
        return NULL;
    }
    mc_mincover_t *mc = MinCoverGenerator_minCover_u128(mcg);
    MinCoverGenerator_destroy_u128(mcg);
    return mc;
}
#endif // MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
