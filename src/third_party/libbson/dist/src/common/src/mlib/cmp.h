/**
 * @file mlib/cmp.h
 * @brief Safe integer comparison and range checking
 * @date 2024-08-29
 *
 * This file provides safe and intuitive integer comparison macros that behave
 * appropriately, regardless of the sign or precision of the integer operands.
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
#ifndef MLIB_CMP_H_INCLUDED
#define MLIB_CMP_H_INCLUDED

#include <mlib/config.h>
#include <mlib/intutil.h>

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Result type of comparing two integral values with `mlib_cmp`
 *
 * The enumerator values are chosen such that they can be compared with zero
 */
enum mlib_cmp_result {
   // The two values are equivalent
   mlib_equal = 0,
   // The left-hand operand is less than the right-hand
   mlib_less = -1,
   // The left-hand operand is greater than the right-hand
   mlib_greater = 1,
};

/**
 * @brief Compare two integral values safely.
 *
 * This function can be called with two arguments or with three:
 *
 * - `mlib_cmp(a, b)` Returns a value of type `mlib_cmp_result`
 * - `mlib_cmp(a, Op, b)` where `Op` is a relational operator. Evaluates to a boolean value.
 */
#define mlib_cmp(...) MLIB_ARGC_PICK(_mlib_cmp, __VA_ARGS__)
// Compare two integers, and return the result of that comparison:
#define _mlib_cmp_argc_2(L, R) mlib_cmp(mlib_upsize_integer((L)), mlib_upsize_integer((R)), 0)
// Compare two integers, but with an infix operator:
#define _mlib_cmp_argc_3(L, Op, R) (mlib_cmp(mlib_upsize_integer((L)), mlib_upsize_integer((R)), 0) Op 0)
// Impl for mlib_cmp
mlib_always_inline static enum mlib_cmp_result(mlib_cmp)(struct mlib_upsized_integer x,
                                                         struct mlib_upsized_integer y,
                                                         int always_zero) mlib_noexcept
{
   (void)always_zero;
#if mlib_is_optimized_build() && !mlib_is_msvc() && \
   !(defined(MLIB_DISABLE_INLINING_ASSERTIONS) && MLIB_DISABLE_INLINING_ASSERTIONS)
   if (always_zero != 0) {
      // All calls must pass zero for `always_zero`. In optimized builds, we assert that this function
      // is always inlined, and that dead-code-elim triggers to delete this branch that calls
      // a never-defined function.
      extern void __assert_that_mlib_cmp_was_inlined(void);
      // If you see a call to this function ↑ produce a link error here ↓, it means
      // that inlining or DCE failed within the relevant translation unit.
      __assert_that_mlib_cmp_was_inlined();
   }
#endif
   if (x.is_signed) {
      if (y.is_signed) {
         // Both signed
         if (x.bits.as_signed < y.bits.as_signed) {
            return mlib_less;
         } else if (x.bits.as_signed > y.bits.as_signed) {
            return mlib_greater;
         }
      } else {
         // X signed, Y unsigned
         if (x.bits.as_signed < 0 || (uintmax_t)x.bits.as_signed < y.bits.as_unsigned) {
            return mlib_less;
         } else if ((uintmax_t)x.bits.as_signed > y.bits.as_unsigned) {
            return mlib_greater;
         }
      }
   } else {
      if (!y.is_signed) {
         // Both unsigned
         if (x.bits.as_unsigned < y.bits.as_unsigned) {
            return mlib_less;
         } else if (x.bits.as_unsigned > y.bits.as_unsigned) {
            return mlib_greater;
         }
      } else {
         // X unsigned, Y signed
         if (y.bits.as_signed < 0 || x.bits.as_unsigned > (uintmax_t)y.bits.as_signed) {
            return mlib_greater;
         } else if (x.bits.as_unsigned < (uintmax_t)y.bits.as_signed) {
            return mlib_less;
         }
      }
   }
   return mlib_equal;
}

/**
 * @brief Test whether the given operand is within the range of some other integral type
 *
 * @param T A type specifier of the target integral type
 * @param Operand the expression that is being inspected.
 *
 * @note This macro may evaluate the operand more than once
 */
#define mlib_in_range(T, Operand) \
   mlib_in_range((intmax_t)mlib_minof(T), (uintmax_t)mlib_maxof(T), mlib_upsize_integer(Operand))
static inline bool(mlib_in_range)(intmax_t min_, uintmax_t max_, struct mlib_upsized_integer val) mlib_noexcept
{
   if (val.is_signed) {
      return mlib_cmp(val.bits.as_signed, >=, min_) && mlib_cmp(val.bits.as_signed, <=, max_);
   } else {
      return mlib_cmp(val.bits.as_unsigned, >=, min_) && mlib_cmp(val.bits.as_unsigned, <=, max_);
   }
}

#endif // MLIB_CMP_H_INCLUDED
