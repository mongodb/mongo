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
#ifndef BITS
#error "must be included with BITS defined"
#endif

// TODO: replace `CONCAT` with `BSON_CONCAT` after libbson dependency is
// upgraded to 1.20.0 or higher.
#ifndef CONCAT
#define CONCAT_1(a, b) a##b
#define CONCAT(a, b) CONCAT_1 (a, b)
#endif
// TODO: replace `CONCAT3` with `BSON_CONCAT3` after libbson dependency is
// upgraded to 1.20.0 or higher.
#ifndef CONCAT3
#define CONCAT3(a, b, c) CONCAT (a, CONCAT (b, c))
#endif

#define UINT_T CONCAT3 (uint, BITS, _t)
#define UINT_C CONCAT3 (UINT, BITS, _C)
#define FMT_UINT_T CONCAT (PRId, BITS)
#define WITH_BITS(X) CONCAT3 (X, _u, BITS)


// MinCoverGenerator models the MinCoverGenerator type added in
// SERVER-68600.
typedef struct {
   UINT_T _rangeMin;
   UINT_T _rangeMax;
   size_t _sparsity;
   // _maxlen is the maximum bit length of edges in the mincover.
   size_t _maxlen;
} WITH_BITS (MinCoverGenerator);

static inline WITH_BITS (MinCoverGenerator) *
   WITH_BITS (MinCoverGenerator_new) (UINT_T rangeMin,
                                      UINT_T rangeMax,
                                      UINT_T max,
                                      size_t sparsity,
                                      mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (status);

   if (rangeMin > rangeMax) {
      CLIENT_ERR ("Range min (%" FMT_UINT_T
                  ") must be less than or equal to range max (%" FMT_UINT_T
                  ") for range search",
                  rangeMin,
                  rangeMax);
      return NULL;
   }
   if (rangeMax > max) {
      CLIENT_ERR ("Range max (%" FMT_UINT_T
                  ") must be less than or equal to max (%" FMT_UINT_T
                  ") for range search",
                  rangeMax,
                  max);
      return NULL;
   }

   if (sparsity == 0) {
      CLIENT_ERR ("Sparsity must be > 0");
      return NULL;
   }
   WITH_BITS (MinCoverGenerator) *mcg =
      bson_malloc0 (sizeof (WITH_BITS (MinCoverGenerator)));
   mcg->_rangeMin = rangeMin;
   mcg->_rangeMax = rangeMax;
   mcg->_maxlen = (size_t) BITS - WITH_BITS (mc_count_leading_zeros) (max);
   mcg->_sparsity = sparsity;
   return mcg;
}

static inline void
WITH_BITS (MinCoverGenerator_destroy) (WITH_BITS (MinCoverGenerator) * mcg)
{
   bson_free (mcg);
}

// applyMask applies a mask of 1 bits starting from the right.
// Bits 0 to bit-1 are replaced with 1. Other bits are left as-is.
static inline UINT_T
WITH_BITS (applyMask) (UINT_T value, size_t maskedBits)
{
   const UINT_T ones = ~UINT_C (0);

   BSON_ASSERT (maskedBits <= (size_t) BITS);
   BSON_ASSERT (maskedBits >= 0);

   if (maskedBits == 0) {
      return value;
   }

   const size_t shift = ((size_t) BITS - maskedBits);
   const UINT_T mask = ones >> shift;
   return value | mask;
}

static inline bool
WITH_BITS (MinCoverGenerator_isLevelStored) (WITH_BITS (MinCoverGenerator) *
                                                mcg,
                                             size_t maskedBits)
{
   BSON_ASSERT_PARAM (mcg);
   size_t level = mcg->_maxlen - maskedBits;
   return 0 == maskedBits || 0 == (level % mcg->_sparsity);
}

char *
WITH_BITS (MinCoverGenerator_toString) (WITH_BITS (MinCoverGenerator) * mcg,
                                        UINT_T start,
                                        size_t maskedBits)
{
   BSON_ASSERT_PARAM (mcg);
   BSON_ASSERT (maskedBits <= mcg->_maxlen);
   BSON_ASSERT (maskedBits <= (size_t) BITS);
   BSON_ASSERT (maskedBits >= 0);

   if (maskedBits == mcg->_maxlen) {
      return bson_strdup ("root");
   }

   UINT_T shifted = start >> maskedBits;
   char *valueBin = WITH_BITS (mc_convert_to_bitstring) (shifted);
   char *ret =
      bson_strndup (valueBin + ((size_t) BITS - mcg->_maxlen + maskedBits),
                    mcg->_maxlen + maskedBits);
   bson_free (valueBin);
   return ret;
}

static inline void
WITH_BITS (MinCoverGenerator_minCoverRec) (WITH_BITS (MinCoverGenerator) * mcg,
                                           mc_array_t *c,
                                           UINT_T blockStart,
                                           size_t maskedBits)
{
   BSON_ASSERT_PARAM (mcg);
   BSON_ASSERT_PARAM (c);
   const UINT_T blockEnd = WITH_BITS (applyMask) (blockStart, maskedBits);

   if (blockEnd < mcg->_rangeMin || blockStart > mcg->_rangeMax) {
      return;
   }

   if (blockStart >= mcg->_rangeMin && blockEnd <= mcg->_rangeMax &&
       WITH_BITS (MinCoverGenerator_isLevelStored) (mcg, maskedBits)) {
      char *edge =
         WITH_BITS (MinCoverGenerator_toString) (mcg, blockStart, maskedBits);
      _mc_array_append_val (c, edge);
      return;
   }

   BSON_ASSERT (maskedBits > 0);

   const size_t newBits = maskedBits - 1u;
   WITH_BITS (MinCoverGenerator_minCoverRec) (mcg, c, blockStart, newBits);
   WITH_BITS (MinCoverGenerator_minCoverRec)
   (mcg, c, blockStart | UINT_C (1) << newBits, newBits);
}

static inline mc_mincover_t *
WITH_BITS (MinCoverGenerator_minCover) (WITH_BITS (MinCoverGenerator) * mcg)
{
   BSON_ASSERT_PARAM (mcg);
   mc_mincover_t *mc = mc_mincover_new ();
   WITH_BITS (MinCoverGenerator_minCoverRec)
   (mcg, &mc->mincover, 0, mcg->_maxlen);
   return mc;
}

// adjustBounds increments *lowerBound if includeLowerBound is false and
// decrements *upperBound if includeUpperBound is false.
// lowerBound, min, upperBound, and max are expected to come from the result
// of mc_getTypeInfo.
static bool
WITH_BITS (adjustBounds) (UINT_T *lowerBound,
                          bool includeLowerBound,
                          UINT_T min,
                          UINT_T *upperBound,
                          bool includeUpperBound,
                          UINT_T max,
                          mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (lowerBound);
   BSON_ASSERT_PARAM (upperBound);

   if (!includeLowerBound) {
      if (*lowerBound >= max) {
         CLIENT_ERR ("Lower bound (%" FMT_UINT_T
                     ") must be less than the range maximum (%" FMT_UINT_T
                     ") if lower bound is excluded from range.",
                     *lowerBound,
                     max);
         return false;
      }
      *lowerBound += 1u;
   }
   if (!includeUpperBound) {
      if (*upperBound <= min) {
         CLIENT_ERR ("Upper bound (%" FMT_UINT_T
                     ") must be greater than the range minimum (%" FMT_UINT_T
                     ") if upper bound is excluded from range.",
                     *upperBound,
                     max);
         return false;
      }
      *upperBound -= 1u;
   }
   return true;
}

#undef UINT_T
#undef UINT_C
#undef FMT_UINT_T
#undef WITH_BITS
