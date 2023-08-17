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

// mc-range-mincover-generator.template.h is meant to be included in another
// source file.

// TODO: replace `CONCAT` with `BSON_CONCAT` after libbson dependency is
// upgraded to 1.20.0 or higher.
#ifndef CONCAT
#define CONCAT_1(a, b) a##b
#define CONCAT(a, b) CONCAT_1(a, b)
#endif
// TODO: replace `CONCAT3` with `BSON_CONCAT3` after libbson dependency is
// upgraded to 1.20.0 or higher.
#ifndef CONCAT3
#define CONCAT3(a, b, c) CONCAT(a, CONCAT(b, c))
#endif

#if !(defined(UINT_T) && defined(UINT_C) && defined(UINT_FMT_S) && defined(DECORATE_NAME))
#ifdef __INTELLISENSE__
#define UINT_T uint32_t
#define UINT_C UINT32_C
#define UINT_FMT_S PRIu32
#define DECORATE_NAME(Name) Name##_u32
#else
#error All of UINT_T, UINT_C, UINT_FMT_S, UINT_FMT_ARG, and DECORATE_NAME must be defined before #including this file
#endif
#endif

#define BITS (sizeof(UINT_T) * CHAR_BIT)

#define ZERO UINT_C(0)

// Default for UINT_FMT_ARG
#ifndef UINT_FMT_ARG
#define UINT_FMT_ARG(X) X
#endif

// Default comparison
#ifndef UINT_LESSTHAN
#define UINT_LESSTHAN(A, B) ((A) < (B))
#endif

#ifndef MC_UINT_MAX
#define MC_UINT_MAX ~(UINT_C(0))
#endif

// Default addition
#ifndef UINT_ADD
#define UINT_ADD(A, B) ((A) + (B))
#endif
#ifndef UINT_SUB
#define UINT_SUB(A, B) ((A) - (B))
#endif

// Default lshift (also handles negatives as right-shift)
#ifndef UINT_LSHIFT
static inline UINT_T DECORATE_NAME(_mc_default_lshift)(UINT_T lhs, int off) {
    if (off < 0) {
        return lhs >> -off;
    } else {
        return lhs << off;
    }
}

#define UINT_LSHIFT DECORATE_NAME(_mc_default_lshift)
#endif

#ifndef UINT_BITOR
#define UINT_BITOR(A, B) ((A) | (B))
#endif

static inline int DECORATE_NAME(_mc_compare)(UINT_T lhs, UINT_T rhs) {
    if (UINT_LESSTHAN(lhs, rhs)) {
        return -1;
    } else if (UINT_LESSTHAN(rhs, lhs)) {
        return 1;
    } else {
        return 0;
    }
}

#define UINT_COMPARE DECORATE_NAME(_mc_compare)

// MinCoverGenerator models the MinCoverGenerator type added in
// SERVER-68600.
typedef struct {
    UINT_T _rangeMin;
    UINT_T _rangeMax;
    size_t _sparsity;
    // _maxlen is the maximum bit length of edges in the mincover.
    size_t _maxlen;
} DECORATE_NAME(MinCoverGenerator);

static inline DECORATE_NAME(MinCoverGenerator)
    * DECORATE_NAME(MinCoverGenerator_new)(UINT_T rangeMin,
                                           UINT_T rangeMax,
                                           UINT_T max,
                                           size_t sparsity,
                                           mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(status);

    if (UINT_COMPARE(rangeMin, rangeMax) > 0) {
        CLIENT_ERR("Range min (%" UINT_FMT_S ") must be less than or equal to range max (%" UINT_FMT_S
                   ") for range search",
                   UINT_FMT_ARG(rangeMin),
                   UINT_FMT_ARG(rangeMax));
        return NULL;
    }
    if (UINT_COMPARE(rangeMax, max) > 0) {
        CLIENT_ERR("Range max (%" UINT_FMT_S ") must be less than or equal to max (%" UINT_FMT_S ") for range search",
                   UINT_FMT_ARG(rangeMax),
                   UINT_FMT_ARG(max));
        return NULL;
    }

    if (sparsity == 0) {
        CLIENT_ERR("Sparsity must be > 0");
        return NULL;
    }
    DECORATE_NAME(MinCoverGenerator) *mcg = bson_malloc0(sizeof(DECORATE_NAME(MinCoverGenerator)));
    mcg->_rangeMin = rangeMin;
    mcg->_rangeMax = rangeMax;
    mcg->_maxlen = (size_t)BITS - DECORATE_NAME(mc_count_leading_zeros)(max);
    mcg->_sparsity = sparsity;
    return mcg;
}

static inline void DECORATE_NAME(MinCoverGenerator_destroy)(DECORATE_NAME(MinCoverGenerator) * mcg) {
    bson_free(mcg);
}

// applyMask applies a mask of 1 bits starting from the right.
// Bits 0 to bit-1 are replaced with 1. Other bits are left as-is.
static inline UINT_T DECORATE_NAME(applyMask)(UINT_T value, size_t maskedBits) {
    const UINT_T ones = MC_UINT_MAX;

    BSON_ASSERT(maskedBits <= (size_t)BITS);
    BSON_ASSERT(maskedBits >= 0);

    if (maskedBits == 0) {
        return value;
    }

    const size_t shift = ((size_t)BITS - maskedBits);
    const UINT_T mask = UINT_LSHIFT(ones, -(int)shift);
    return UINT_BITOR(value, mask);
}

static inline bool DECORATE_NAME(MinCoverGenerator_isLevelStored)(DECORATE_NAME(MinCoverGenerator) * mcg,
                                                                  size_t maskedBits) {
    BSON_ASSERT_PARAM(mcg);
    size_t level = mcg->_maxlen - maskedBits;
    return 0 == maskedBits || 0 == (level % mcg->_sparsity);
}

char *
DECORATE_NAME(MinCoverGenerator_toString)(DECORATE_NAME(MinCoverGenerator) * mcg, UINT_T start, size_t maskedBits) {
    BSON_ASSERT_PARAM(mcg);
    BSON_ASSERT(maskedBits <= mcg->_maxlen);
    BSON_ASSERT(maskedBits <= (size_t)BITS);
    BSON_ASSERT(maskedBits >= 0);

    if (maskedBits == mcg->_maxlen) {
        return bson_strdup("root");
    }

    UINT_T shifted = UINT_LSHIFT(start, -(int)maskedBits);
    mc_bitstring valueBin = DECORATE_NAME(mc_convert_to_bitstring)(shifted);
    char *ret = bson_strndup(valueBin.str + ((size_t)BITS - mcg->_maxlen + maskedBits), mcg->_maxlen + maskedBits);
    return ret;
}

static inline void DECORATE_NAME(MinCoverGenerator_minCoverRec)(DECORATE_NAME(MinCoverGenerator) * mcg,
                                                                mc_array_t *c,
                                                                UINT_T blockStart,
                                                                size_t maskedBits) {
    BSON_ASSERT_PARAM(mcg);
    BSON_ASSERT_PARAM(c);
    const UINT_T blockEnd = DECORATE_NAME(applyMask)(blockStart, maskedBits);

    if (UINT_COMPARE(blockEnd, mcg->_rangeMin) < 0 || UINT_COMPARE(blockStart, mcg->_rangeMax) > 0) {
        return;
    }

    if (UINT_COMPARE(blockStart, mcg->_rangeMin) >= 0 && UINT_COMPARE(blockEnd, mcg->_rangeMax) <= 0
        && DECORATE_NAME(MinCoverGenerator_isLevelStored)(mcg, maskedBits)) {
        char *edge = DECORATE_NAME(MinCoverGenerator_toString)(mcg, blockStart, maskedBits);
        _mc_array_append_val(c, edge);
        return;
    }

    BSON_ASSERT(maskedBits > 0);

    const size_t newBits = maskedBits - 1u;
    DECORATE_NAME(MinCoverGenerator_minCoverRec)(mcg, c, blockStart, newBits);
    DECORATE_NAME(MinCoverGenerator_minCoverRec)
    (mcg, c, UINT_BITOR(blockStart, UINT_LSHIFT(UINT_C(1), (int)newBits)), newBits);
}

static inline mc_mincover_t *DECORATE_NAME(MinCoverGenerator_minCover)(DECORATE_NAME(MinCoverGenerator) * mcg) {
    BSON_ASSERT_PARAM(mcg);
    mc_mincover_t *mc = mc_mincover_new();
    DECORATE_NAME(MinCoverGenerator_minCoverRec)
    (mcg, &mc->mincover, ZERO, mcg->_maxlen);
    return mc;
}

// adjustBounds increments *lowerBound if includeLowerBound is false and
// decrements *upperBound if includeUpperBound is false.
// lowerBound, min, upperBound, and max are expected to come from the result
// of mc_getTypeInfo.
static bool DECORATE_NAME(adjustBounds)(UINT_T *lowerBound,
                                        bool includeLowerBound,
                                        UINT_T min,
                                        UINT_T *upperBound,
                                        bool includeUpperBound,
                                        UINT_T max,
                                        mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(lowerBound);
    BSON_ASSERT_PARAM(upperBound);

    if (!includeLowerBound) {
        if (UINT_COMPARE(*lowerBound, max) >= 0) {
            CLIENT_ERR("Lower bound (%" UINT_FMT_S ") must be less than the range maximum (%" UINT_FMT_S
                       ") if lower bound is excluded from range.",
                       UINT_FMT_ARG(*lowerBound),
                       UINT_FMT_ARG(max));
            return false;
        }
        *lowerBound = UINT_ADD(*lowerBound, UINT_C(1));
    }
    if (!includeUpperBound) {
        if (UINT_COMPARE(*upperBound, min) <= 0) {
            CLIENT_ERR("Upper bound (%" UINT_FMT_S ") must be greater than the range minimum (%" UINT_FMT_S
                       ") if upper bound is excluded from range.",
                       UINT_FMT_ARG(*upperBound),
                       UINT_FMT_ARG(min));
            return false;
        }
        *upperBound = UINT_SUB(*upperBound, UINT_C(1));
    }
    return true;
}

#undef UINT_T
#undef UINT_C
#undef UINT_FMT_S
#undef UINT_FMT_ARG
#undef DECORATE_NAME
#undef BITS
#undef UINT_COMPARE
#undef UINT_ADD
#undef UINT_SUB
#undef UINT_LSHIFT
#undef UINT_BITOR
#undef MC_UINT_MAX
#undef ZERO
#undef UINT_LESSTHAN
