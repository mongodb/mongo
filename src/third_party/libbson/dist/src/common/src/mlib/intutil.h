/**
 * @file mlib/intutil.h
 * @brief Integer utilities
 * @date 2025-01-28
 *
 * @copyright Copyright 2009-present MongoDB, Inc.
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
#ifndef MLIB_INTUTIL_H_INCLUDED
#define MLIB_INTUTIL_H_INCLUDED

#include <mlib/config.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Given an integral type, evaluates to `true` if that type is signed,
 * otherwise `false`
 */
#define mlib_is_signed(T) (!((T)(-1) > 0))

/**
 * @brief Like `sizeof`, but returns the number of bits in the object representation
 */
#define mlib_bitsizeof(T) ((sizeof(T)) * ((size_t)CHAR_BIT))

// clang-format off
/**
 * @brief Generate a mask of contiguous bits.
 *
 * @param NumOnes The non-negative number of contiguous 1 bits
 * @param NumZeros The non-negative number of contiguous 0 bits to set in the low position
 *
 * The generated mask is of the form:
 *
 *             NumZeros
 *                 │
 *                ┌┴─┐
 *                │  │
 *     `0..0 1..1 0..0`
 *           │  │
 *           └┬─┘
 *            │
 *        NumOnes
 *
 * Explain the arithmetic below:
 *
 * 1. `ones = 0b1111...` : All high bits
 * 2. `tmp  = ones >> (NumOnes - num_bits_of(ones))` : Truncate to the number of 1s we want
 * 3. `res  = tmp  << NumZeros` : Add the 0s in the low position
 */
#define mlib_bits(NumOnes, NumZeros) ( \
   ((NumOnes) \
      ? (~UINTMAX_C(0) >> ((mlib_bitsizeof(uintmax_t) - (uintmax_t)(NumOnes)))) \
      : 0) \
   << ((uintmax_t)(NumZeros)))

/**
 * @brief Given an integral type, yield an integral constant value representing
 * the maximal value of that type.
 */
#define mlib_maxof(T) \
   ((T) (mlib_is_signed (T) \
        ? ((T) mlib_bits(mlib_bitsizeof(T) - 1u, 0)) \
        : ((T) mlib_bits(mlib_bitsizeof(T),      0))))

/**
 * @brief Given an integral type, yield an integral constant value for the
 * minimal value of that type.
 */
#define mlib_minof(T) \
   ((T) (!mlib_is_signed (T) \
        ? (T) 0 \
        : (T) mlib_bits(1, mlib_bitsizeof(T) - 1u)))
// clang-format on

/**
 * @brief A container for an integer that has been "scaled up" to maximum precision
 *
 * Don't create this manually. Instead, use `mlib_upsize_integer` to do it automatically
 */
typedef struct mlib_upsized_integer {
   union {
      // The signed value of the integer
      intmax_t as_signed;
      // The unsigned value of the integer
      uintmax_t as_unsigned;
   } bits;
   // Whether the upscaled integer bits should be treated as a two's complement signed integer
   bool is_signed;
} mlib_upsized_integer;

// clang-format off
/**
 * @brief Create an "upsized" version of an integer, normalizing all integral
 * values into a single type so that we can deduplicate functions that operate
 * on disparate integer types.
 *
 * Details: The integer is upcast into the maximum precision integer type (intmax_t). If
 * the operand is smaller than `intmax_t`, we assume that casting to the signed `intmax_t`
 * is always safe, even if the operand is unsigned, since e.g. a u32 can always be cast to
 * an i64 losslessly.
 *
 * If the integer to upcast is the same size as `intmax_t`, we need to decide whether to store
 * it as unsigned. The expression `(_mlibGetOne(Value)) - 2 < 1` will be `true` iff the operand is signed,
 * otherwise false. If the operand is signed, we can safely cast to `intmax_t` (it probably already
 * is of that type), otherwise, we cast to `uintmax_t` and the returned `mlib_upsized_integer` will
 * indicate that the stored value is unsigned. The expression `1 - 2 < 1` is chosen
 * to avoid `-Wtype-limits` warnings from some compilers about unsigned comparison.
 */
#define mlib_upsize_integer(Value) \
   mlib_upsize_integer((uintmax_t)(intmax_t)((Value)), _mlibShouldTreatBitsAsSigned(Value))
#define _mlibShouldTreatBitsAsSigned(Value) \
   /* NOLINTNEXTLINE(bugprone-sizeof-expression) */ \
   (sizeof ((Value)) < sizeof (intmax_t) || (_mlibGetOne(Value) - 2) < _mlibGetOne(Value))
// Yield a 1 value of similar-ish type to the given expression. The ternary
// forces an integer promotion of literal 1 match the type of `V`, while leaving
// `V` unevaluated. Note that this will also promote `V` to be at least `(unsigned) int`,
// so the 1 value is only "similar" to `V`, and may be of a larger type
#define _mlibGetOne(V) (1 ? 1 : (V))
// Function impl for upsize_integer
static inline mlib_upsized_integer
(mlib_upsize_integer) (uintmax_t bits, bool treat_as_signed)
{
   mlib_upsized_integer ret;
   ret.bits.as_unsigned = bits;
   ret.is_signed = treat_as_signed;
   return ret;
}
// clang-format on

#endif // MLIB_INTUTIL_H_INCLUDED
