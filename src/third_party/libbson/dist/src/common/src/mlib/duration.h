/**
 * @file mlib/duration.h
 * @brief Duration types and functions
 * @date 2025-04-17
 *
 * This file contains types and functions for working with a "duration" type,
 * which represents an elapsed amount of time, possibly negative.
 *
 * The type `mlib_duration_rep_t` is a typedef of the intregral type that is
 * used to represent duration units.
 *
 * The `mlib_duration` is a trivial object that represents a duration of time.
 * The internal representation should not be inspected outside of this file.
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
#ifndef MLIB_DURATION_H_INCLUDED
#define MLIB_DURATION_H_INCLUDED

#include <mlib/ckdint.h>
#include <mlib/cmp.h>
#include <mlib/config.h>
#include <mlib/intutil.h>

#include <stdint.h>
#include <time.h>

mlib_extern_c_begin();

/**
 * @brief The integral type used to represent a count of units of time.
 */
typedef int64_t mlib_duration_rep_t;

/**
 * @brief Represents a duration of time, either positive, negative, or zero.
 *
 * @note A zero-initialized (static initialized) duration represents the zero
 * duration (no elapsed time)
 *
 * @note The time representation is intended to be abstract, and should be
 * converted to concrete units of time by calling the `_count` functions.
 */
typedef struct mlib_duration {
   /**
    * @brief The integral representation of the duration.
    *
    * Do not read or modify this field except to zero-initialize it.
    */
   mlib_duration_rep_t _rep;
} mlib_duration;

/**
 * @brief A macro that expands to the maximum positive duration
 */
#define mlib_duration_max() (mlib_init(mlib_duration){mlib_maxof(mlib_duration_rep_t)})
/**
 * @brief A macro that expands to the minimum duration (a negative duration)
 */
#define mlib_duration_min() (mlib_init(mlib_duration){mlib_minof(mlib_duration_rep_t)})

/**
 * @brief Obtain the count of microseconds represented by the duration (round
 * toward zero)
 */
static inline mlib_duration_rep_t
mlib_microseconds_count(const mlib_duration dur) mlib_noexcept
{
   return dur._rep;
}

/**
 * @brief Obtain the count of milliseconds represented by the duration (round
 * toward zero)
 */
static inline mlib_duration_rep_t
mlib_milliseconds_count(const mlib_duration dur) mlib_noexcept
{
   return mlib_microseconds_count(dur) / 1000;
}

/**
 * @brief Obtain the count of seconds represented by the duration (rounded
 * toward zero)
 */
static inline mlib_duration_rep_t
mlib_seconds_count(const mlib_duration dur) mlib_noexcept
{
   return mlib_milliseconds_count(dur) / 1000;
}

/**
 * @brief Duration creation and manipulation shorthands
 *
 * This function-like macro is used to create and manipulate durations on-the-fly.
 * It can be called with the following syntaxes:
 *
 * - `mlib_duration()` (no arguments)
 *       creates a zero-valued duration
 * - `mlib_duration(<dur>)`
 *       copies the duration object `<dur>`
 * - `mlib_duration(<count>, <unit>)`
 *       Creates a duration of `<count>` instances of `<unit>`.
 * - `mlib_duration(<dur>, <op>, <operand>)`
 *       Manipulates a duration according to `<op>`.
 *
 * In the above, `<dur>` may be a parenthesized `mlib_duration` argument list or a
 * duration object; `<count>` must be an integral expression and `<unit>` is a
 * unit suffix identifer (see: `mlib_duration_with_unit`) to create a duration
 * of `<count>` instances of `<unit>`, and `<op>` is one of:
 *
 * - `plus`/`minus` to add/subtract two durations.
 * - `mul`/`div` to multiply/divide a duration by a scalar factor.
 * - `min`/`max` to get the minimum/maximum between two durations.
 *
 * All duration arithmetic/conversion operations use well-defined saturating
 * arithmetic, and never wrap or trap.
 */
#define mlib_duration(...) MLIB_EVAL_16(_mlibDurationMagic(__VA_ARGS__))
#define _mlibDurationMagic(...)                                \
   MLIB_DEFERRED(MLIB_ARGC_PASTE(_mlib_duration, __VA_ARGS__)) \
   (__VA_ARGS__)
// Wraps a `<dur>` argument, and expands to the magic only if it is parenthesized
#define _mlibDurationArgument(X)                                                       \
   /* If given a parenthesized expression, act as an invocation of `mlib_duration() */ \
   MLIB_IF_ELSE(MLIB_IS_PARENTHESIZED(X))                                              \
   /* then: */ (_mlibDurationMagic X) /* else: */ (X)

// Wrap a macro argument that should support the duration DSL
#define mlib_duration_arg(X) MLIB_EVAL_16(_mlibDurationArgument(X))

// Zero arguments, just return a zero duration:
#define _mlib_duration_argc_0() (mlib_init(mlib_duration){0})
// One argument, just copy the duration. Passing through a function forces the type to be correct
#define _mlib_duration_argc_1(D) _mlibDurationCopy(D)
// Two arguments, the second arg is a unit suffix:
#define _mlib_duration_argc_2(Count, Unit) mlib_duration_with_unit(Count, Unit)
// Three arguments, an infix operation:
#define _mlib_duration_argc_3(Duration, Operator, Operand)          \
   MLIB_DEFERRED(MLIB_PASTE(_mlibDurationInfixOperator_, Operator)) \
   (Duration, Operand)

// By-value copy a duration
static inline mlib_duration
_mlibDurationCopy(mlib_duration d)
{
   return d;
}

// Duration scalar multiply
#define _mlibDurationInfixOperator_mul(LHS, Fac) \
   _mlibDurationMultiply(_mlibDurationArgument(LHS), mlib_upsize_integer(Fac))
static inline mlib_duration
_mlibDurationMultiply(const mlib_duration dur, mlib_upsized_integer fac) mlib_noexcept
{
   mlib_duration ret = {0};
   const bool overflowed = fac.is_signed ? mlib_mul(&ret._rep, dur._rep, fac.bits.as_signed)
                                         : mlib_mul(&ret._rep, dur._rep, fac.bits.as_unsigned);
   if (overflowed) {
      if ((dur._rep < 0) != (fac.is_signed && fac.bits.as_signed < 0)) {
         // Different signs:  Neg × Pos = Neg
         ret = mlib_duration_min();
      } else {
         // Same signs: Pos × Pos = Pos
         //             Neg × Neg = Pos
         ret = mlib_duration_max();
      }
   }
   return ret;
}

// Duration scalar divide
#define _mlibDurationInfixOperator_div(LHS, Div) \
   _mlibDurationDivide(_mlibDurationArgument(LHS), mlib_upsize_integer(Div))
static inline mlib_duration
_mlibDurationDivide(mlib_duration a, mlib_upsized_integer div) mlib_noexcept
{
   mlib_check(div.bits.as_unsigned, neq, 0);
   if ((div.is_signed && div.bits.as_signed == -1) //
       && a._rep == mlib_minof(mlib_duration_rep_t)) {
      // MIN / -1 is UB, but the saturating result is the max
      a = mlib_duration_max();
   } else {
      if (div.is_signed) {
         a._rep /= div.bits.as_signed;
      } else {
         a._rep = (mlib_duration_rep_t)((uintmax_t)a._rep / div.bits.as_unsigned);
      }
   }
   return a;
}

// Duration addition
#define _mlibDurationInfixOperator_plus(LHS, RHS) \
   _mlibDurationAdd(_mlibDurationArgument(LHS), _mlibDurationArgument(RHS))
static inline mlib_duration
_mlibDurationAdd(const mlib_duration a, const mlib_duration b) mlib_noexcept
{
   mlib_duration ret = {0};
   if (mlib_add(&ret._rep, a._rep, b._rep)) {
      if (a._rep > 0) {
         ret = mlib_duration_max();
      } else {
         ret = mlib_duration_min();
      }
   }
   return ret;
}

// Duration subtraction
#define _mlibDurationInfixOperator_minus(LHS, RHS) \
   _mlibDurationSubtract(_mlibDurationArgument(LHS), _mlibDurationArgument(RHS))
static inline mlib_duration
_mlibDurationSubtract(const mlib_duration a, const mlib_duration b) mlib_noexcept
{
   mlib_duration ret = {0};
   if (mlib_sub(&ret._rep, a._rep, b._rep)) {
      if (a._rep < 0) {
         ret = mlib_duration_min();
      } else {
         ret = mlib_duration_max();
      }
   }
   return ret;
}

#define _mlibDurationInfixOperator_min(Duration, RHS) \
   _mlibDurationMinBetween(_mlibDurationArgument(Duration), _mlibDurationArgument(RHS))
static inline mlib_duration
_mlibDurationMinBetween(mlib_duration lhs, mlib_duration rhs)
{
   if (lhs._rep < rhs._rep) {
      return lhs;
   }
   return rhs;
}

#define _mlibDurationInfixOperator_max(Duration, RHS) \
   _mlibDurationMaxBetween(_mlibDurationArgument(Duration), _mlibDurationArgument(RHS))
static inline mlib_duration
_mlibDurationMaxBetween(mlib_duration lhs, mlib_duration rhs)
{
   if (lhs._rep > rhs._rep) {
      return lhs;
   }
   return rhs;
}

/**
 * @brief Create a duration object from a count of some unit of time
 *
 * @param Count An integral expression
 * @param Unit A unit suffix identifier, must be one of:
 *
 * - `ns` (nanoseconds)
 * - `us` (microseconds)
 * - `ms` (milliseconds)
 * - `s` (seconds)
 * - `mn` (minutes)
 * - `h` (hours)
 *
 * Other unit suffixes will generate a compile-time error
 */
#define mlib_duration_with_unit(Count, Unit) \
   MLIB_PASTE(_mlibCreateDurationFromUnitCount_, Unit)(mlib_upsize_integer(Count))

static inline mlib_duration
_mlibCreateDurationFromUnitCount_us(const mlib_upsized_integer n) mlib_noexcept
{
   mlib_duration ret = mlib_duration();
   if (n.is_signed) {
      // The duration rep is the same as the signed max type, so we don't need to do any
      // special arithmetic to encode it
      mlib_static_assert(sizeof(mlib_duration_rep_t) == sizeof(n.bits.as_signed));
      ret._rep = mlib_assert_narrow(mlib_duration_rep_t, n.bits.as_signed);
   } else {
      if (mlib_narrow(&ret._rep, n.bits.as_unsigned)) {
         // Unsigned value is too large to fit in our signed repr, so just use the max repr
         ret = mlib_duration_max();
      }
   }
   return ret;
}

static inline mlib_duration
_mlibCreateDurationFromUnitCount_ns(mlib_upsized_integer n) mlib_noexcept
{
   // We encode as a count of microseconds, so we lose precision here.
   if (n.is_signed) {
      n.bits.as_signed /= 1000;
   } else {
      n.bits.as_unsigned /= 1000;
   }
   return _mlibCreateDurationFromUnitCount_us(n);
}

static inline mlib_duration
_mlibCreateDurationFromUnitCount_ms(const mlib_upsized_integer n) mlib_noexcept
{
   return mlib_duration(_mlibCreateDurationFromUnitCount_us(n), mul, 1000);
}

static inline mlib_duration
_mlibCreateDurationFromUnitCount_s(const mlib_upsized_integer n)
{
   return mlib_duration(_mlibCreateDurationFromUnitCount_us(n), mul, 1000 * 1000);
}

static inline mlib_duration
_mlibCreateDurationFromUnitCount_mn(const mlib_upsized_integer n)
{
   return mlib_duration(_mlibCreateDurationFromUnitCount_us(n), mul, 60 * 1000 * 1000);
}

static inline mlib_duration
_mlibCreateDurationFromUnitCount_h(const mlib_upsized_integer n)
{
   return mlib_duration(_mlibCreateDurationFromUnitCount_mn(n), mul, 60);
}

/**
 * @brief Compare two durations
 *
 * @retval <0 If `a` is less-than `b`
 * @retval >0 If `b` is less-than `a`
 * @retval  0 If `a` and `b` are equal durations
 *
 * @note This is a function-like macro that can be called with an infix operator
 * as the second argument to do natural duration comparisons:
 *
 * ```
 *    mlib_duration_cmp(<dur>, <operator>, <dur>)
 * ```
 *
 * Where each `<dur>` should be an arglist for @see mlib_duration
 */
static inline enum mlib_cmp_result
mlib_duration_cmp(const mlib_duration a, const mlib_duration b) mlib_noexcept
{
   return mlib_cmp(a._rep, b._rep);
}

#define mlib_duration_cmp(...) MLIB_ARGC_PICK(_mlibDurationCmp, __VA_ARGS__)
#define _mlibDurationCmp_argc_2 mlib_duration_cmp
#define _mlibDurationCmp_argc_3(Left, Op, Right) \
   (mlib_duration_cmp(mlib_duration_arg(Left), mlib_duration_arg(Right)) Op 0)

/**
 * @brief Obtain an mlib_duration that corresponds to a `timespec` value
 *
 * @note The `timespec` type may represent times outside of the range of, or
 * more precise than, what is representable in `mlib_duration`. In such case,
 * the returned duration will be the nearest representable duration, rounded
 * toward zero.
 */
static inline mlib_duration
mlib_duration_from_timespec(const struct timespec ts) mlib_noexcept
{
   return mlib_duration((ts.tv_sec, s), plus, (ts.tv_nsec, ns));
}

/**
 * @brief Create a C `struct timespec` that corresponds to the given duration
 *
 * @param d The duration to be converted
 * @return struct timespec A timespec that represents the same durations
 */
static inline struct timespec
mlib_duration_to_timespec(const mlib_duration d) mlib_noexcept
{
   // Number of full seconds in the duration
   const mlib_duration_rep_t n_full_seconds = mlib_seconds_count(d);
   // Duration with full seconds removed
   const mlib_duration usec_part = mlib_duration(d, minus, (n_full_seconds, s));
   // Number of microseconds in the duration, minus all full seconds
   const mlib_duration_rep_t n_remaining_microseconds = mlib_microseconds_count(usec_part);
   // Compute the number of nanoseconds:
   const int32_t n_nsec = mlib_assert_mul(int32_t, n_remaining_microseconds, 1000);
   struct timespec ret;
   ret.tv_sec = n_full_seconds;
   ret.tv_nsec = n_nsec;
   return ret;
}

mlib_extern_c_end();

#endif // MLIB_DURATION_H_INCLUDED
