/**
 * @file mlib/ckdint.h
 * @brief Checked integer arithmetic
 * @date 2025-02-04
 *
 * This file implements the C23 checked-integer-arithmetic functions as macros.
 *
 * The function-like macros are defined:
 *
 * - `mlib_add(Dst, L, R)` / `mlib_add(Dst, A)`
 * - `mlib_sub(Dst, L, R)` / `mlib_sub(Dst, A)`
 * - `mlib_mul(Dst, L, R)` / `mlib_mul(Dst, A)`
 * - `mlib_narrow(Dst, V)` (not from stdckdint, but defined as `mlib_add(Dst, V, 0)`)
 *
 * Where `Dst` is a pointer to integral storage, and `L` and `R` are arbitrary
 * integral expressions. The two-argument variants treat `Dst` as the the left-hand
 * operand for in-place arithmetic.
 *
 * Each macro accepts arguments of arbitrary type at any position, and will "do
 * the right thing", regardless of the parameter types. No funny integer promotion,
 * sign extension, sign conversion, nor implicit narrowing. The macros return `false`
 * if-and-only-if the result was lossless. They return `true` if-and-only-if the
 * value written to `Dst` does not represent the true arithmetic result.
 *
 * The following additional macros are defined:
 *
 * - `mlib_assert_add(T, L, R)`
 * - `mlib_assert_sub(T, L, R)`
 * - `mlib_assert_mul(T, L, R)`
 *
 * Where `T` is an integer type. The macro will yield a value of that type, asserting
 * that the operation on `L` and `R` does not overflow. If the operation overflows,
 * the program will be terminated with a diagnostic to `stderr` pointing to the call site.
 *
 * For implementation details and a usage guide, see `ckdint.md`
 *
 * @copyright Copyright (c) 2025
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
#pragma once

#include <mlib/config.h>
#include <mlib/intutil.h>
#include <mlib/test.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

mlib_extern_c_begin();

/**
 * @brief Perform integer addition
 *
 * @param Out A non-null pointer to a modifiable integer.
 * @param A The left-hand addend of addition
 * @param B The right-hand addend of addition
 * @retval `true` if the value written to `Out` does not represent the true arithmetic sum.
 * @retval `false` Otherwise.
 *
 * The sum `A + B` is written to `Out`. The operation is commutative.
 *
 * If the argument `B` is omitted, computes `*Out + A` (performs in-place addition).
 */
#define mlib_add(...) MLIB_ARGC_PICK(_mlib_ckdint, mlib_add, __VA_ARGS__)
/**
 * @brief Perform integer subtraction
 *
 * @param Out A non-null pointer to a modifiable integer.
 * @param A The left-hand operand of the subtraction (minuend)
 * @param B The right-hand operand of subtraction (subtrahend)
 * @retval `true` if the value written to `Out` does not represent the true arithmetic difference.
 * @retval `false` Otherwise.
 *
 * The difference `A - B` will be written to `Out`.
 *
 * If the argument `B` is omitted, computes `*Out - A` (performs in-place subtraction)
 */
#define mlib_sub(...) MLIB_ARGC_PICK(_mlib_ckdint, mlib_sub, __VA_ARGS__)
/**
 * @brief Perform integer multiplication
 *
 * @param Out A non-null pointer to a modifiable integer.
 * @param A The left-hand factor of multiplication
 * @param B The right-hand factor of multiplication
 * @retval `true` if the value written to `Out` does not represent the true arithmetic product.
 * @retval `false` Otherwise.
 *
 * The product `A × B` will be written to `Out`. The operation is commutative.
 *
 * If the argument `B` is omitted, computes `Out × A` (performs in-place multiplication)
 */
#define mlib_mul(...) MLIB_ARGC_PICK(_mlib_ckdint, mlib_mul, __VA_ARGS__)
/**
 * @brief Perform narrowing assignment from one integer value to another.
 *
 * @param Out A non-null pointer to a modifiable integer.
 * @param A The integer value to be narrowed.
 * @retval `true` if the value written to `Out` is not equivalent to the value of `A`
 * @retval `false` otherwise
 */
#define mlib_narrow(O, A) mlib_add((O), (A), 0)

/**
 * @brief Perform an asserting addition, yielding the result
 *
 * @param T The target type of the operation
 * @param A The left-hand addend for the addition
 * @param B The right-hand addend for the addition
 * @return The sum `A + B` as type `T`
 *
 * If the true arithmetic sum is not representable in `T`, the program terminates.
 */
#define mlib_assert_add(T, A, B) \
   _mlib_assert_ckdint(T, A, B, &mlib_add, "mlib_assert_add", #T, #A, #B, mlib_this_source_location())
/**
 * @brief Perform an asserting subtraction, yielding the result
 *
 * @param T The target type of the operation
 * @param A The left-hand minuend for the subtraction
 * @param B The right-hand subtrahend for the subtraction
 * @return The difference `A - B` as type `T`
 *
 * If the true arithmetic difference is not representable in `T`, the program terminates.
 */
#define mlib_assert_sub(T, A, B) \
   _mlib_assert_ckdint(T, A, B, &mlib_sub, "mlib_assert_sub", #T, #A, #B, mlib_this_source_location())
/**
 * @brief Perform an asserting multiplication, yielding the result
 *
 * @param T The target type of the operation
 * @param A The left-hand factor for the multiplication
 * @param B The right-hand factor for the multiplication
 * @return The product `A × B` as type `T`
 *
 * If the true arithmetic product is not representable in `T`, the program terminates.
 */
#define mlib_assert_mul(T, A, B) \
   _mlib_assert_ckdint(T, A, B, &mlib_mul, "mlib_assert_mul", #T, #A, #B, mlib_this_source_location())

/**
 * @brief Perform a runtime-checked cast of an integral value to another type.
 *
 * @param T A type specifier for a target integral type for the cast.
 * @param Operand The integral value to be converted.
 *
 * If the cast would result in the operand value changing, the program will be
 * terminated with a diagnostic.
 */
#define mlib_assert_narrow(T, Operand) \
   (T) _mlib_checked_cast(             \
      mlib_minof(T), mlib_maxof(T), mlib_upsize_integer(Operand), #T, #Operand, mlib_this_source_location())

#define _mlib_ckdint_argc_3(Fn, Out, Arg) _mlib_ckdint_argc_4(Fn, Out, *(Out), Arg)
#define _mlib_ckdint_argc_4(Fn, O, A, B)                                                                     \
   _mlib_ckdint(O,                                                                                           \
                sizeof(*(O)),                                                                                \
                _mlibClobberIsSigned(*(O), 0) ? (intmax_t)_mlibMinofSigned(*(O)) : _mlibMinofUnsigned(*(O)), \
                _mlibClobberIsSigned(*(O), 1) ? _mlibMaxofSigned(*(O)) : _mlibMaxofUnsigned(*(O)),           \
                mlib_upsize_integer(A),                                                                      \
                mlib_upsize_integer(B),                                                                      \
                &Fn)

// Impl macro for the asserting checked arithmetic functions
#define _mlib_assert_ckdint(T, A, B, Fn, F_str, T_str, A_str, B_str, Here) \
   ((T)_mlib_assert_ckdint(sizeof(T),                                      \
                           mlib_minof(T),                                  \
                           mlib_maxof(T),                                  \
                           mlib_upsize_integer(A),                         \
                           mlib_upsize_integer(B),                         \
                           Fn,                                             \
                           F_str,                                          \
                           T_str,                                          \
                           A_str,                                          \
                           B_str,                                          \
                           Here))

// clang-format off
// Generates an 0b11111 bit pattern for appropriate size:
#define _mlibMaxofUnsigned(V) \
   /* NOLINTNEXTLINE(bugprone-sizeof-expression) */ \
   mlib_bits(mlib_bitsizeof((V)), 0)

// Generates an 0b01111 bit pattern for the two's complement max value:
#define _mlibMaxofSigned(V) \
   /* NOLINTNEXTLINE(bugprone-sizeof-expression) */ \
   mlib_bits(mlib_bitsizeof(V) - 1u, 0)
// Generates an 0b10000... bit pattern for the two's complement min value:
#define _mlibMinofSigned(V) \
   /* NOLINTNEXTLINE(bugprone-sizeof-expression) */ \
   (0 - mlib_bits(1, mlib_bitsizeof(V) - 1u))
// For completeness:
#define _mlibMinofUnsigned(V) 0
// Yields true iff the operand expression has a signed type, but requires that
// the operand is a modifiable l-value. The `N` must be 0 or 1, arbitrarily (see below).
#define _mlibClobberIsSigned(V, N) \
   MLIB_IF_ELSE(mlib_have_typeof()) \
      /* Prefer using typeof(), if we can. */ \
      (mlib_is_signed(mlib_typeof(V))) \
      /* Otherwise, do a dance: */ \
      (( \
         /* Save the value of V */ \
         _mlibSignCheckTmp[N] = 0ull | (uintmax_t) (V), \
         /* Set V to zero, and check whether decrementing results in a negative value */ \
         (V) = 0, \
         _mlibSignCheckResult[N] = (--(V) < 0), \
         /* Restore the value of V (bit hacks to prevent conversion warnings) */ \
         (V) = 0, \
         (V) |= _mlibSignCheckTmp[N], \
         /* Yield the sign-check result */ \
         _mlibSignCheckResult[N] \
      ))
// Storage for `_mlibClobberIsSigned`. We use more than one storage space to prevent
// unsequenced-operation warnings when we use `_mlibClobberIsSigned` multiple times
// in a function argument list. GCC and Clang are able to easily elide these from
// an optimized TU. MSVC has trouble, but is still able to constant-fold where it matters.
static mlib_maybe_unused mlib_thread_local uintmax_t _mlibSignCheckTmp[2];
static mlib_maybe_unused mlib_thread_local bool _mlibSignCheckResult[2];
// clang-format on

// Compile-time assert that the compiler's integer conversions obey two's complement encoding
mlib_static_assert((intmax_t)UINTMAX_MAX == -1 //
                      && (intmax_t)(UINTMAX_MAX - 5) == -6,
                   "This file requires two's complement signed integers");

/**
 * @brief Function signature for checked arithmetic support functions
 *
 * The function operates on max-precision integers of either sign, and should
 * return true iff the arithmetic operation overflows for the given sign configuration.
 *
 * @param dst The output parameter for the operation. Never a null pointer.
 * @param dst_signed Whether to treat the result as a signed integer
 * @param a_signed Whether to treat the `a` operand as signed
 * @param a The left-hand operand of the operation
 * @param b_signed Whether to treat the `b` operand as signed
 * @param b The right-hand operand of the operation
 *
 * @note This was original written to use `mlib_upscaled_integer` for `a/b/dst`, but
 * this defeats MSVC's ability to inline an indirect call through a constant-folded function
 * pointer with this signature. GCC and Clang handle this fine, but for MSVC performance
 * the more verbose signature is used.
 */
typedef bool (*_mlib_ckdint_arith_fn)(
   uintmax_t *dst, bool dst_signed, bool a_signed, uintmax_t a, bool b_signed, uintmax_t b);

// Support function for the `mlib_add` macro
static inline bool(mlib_add)(uintmax_t *dst, bool dst_signed, bool a_signed, uintmax_t a, bool b_signed, uintmax_t b)
   mlib_noexcept
{
   // Perform regular wrapping arithmetic on the unsigned value. The bit pattern
   // is equivalent if there is two's complement signed arithmetic.
   const uintmax_t sum = *dst = a + b;
   const uintmax_t signbit = mlib_bits(1, mlib_bitsizeof(uintmax_t) - 1u);
   // Now we check whether that overflowed according to the sign configuration.
   // We use some bit fiddling magic that treat the signbit as a boolean for
   // "is this number negative?" or "is this number “large” (i.e. bigger than signed-max)?"
   // The expanded verbose form of each bit-magic is written below the more esoteric cases
   if (dst_signed) {
      if (a_signed) {
         if (b_signed) { // S = S + S
            return signbit & (sum ^ a) & (sum ^ b);
            // Expanded:
            // Test whether the product sign is unequal to both input signs
            // X ^ Y yields a negative value if the signs are unequal
            //     const bool a_signflipped = (intmax_t) (sum ^ a) < 0;
            //     const bool b_signflipped = (intmax_t) (sum ^ b) < 0;
            //     return a_signflipped && b_signflipped;
         } else { // S = S + U
            // Flip the sign bit of a, test whether that sum overflows
            a ^= signbit;
            return a + b < a;
         }
      } else {
         if (b_signed) { // S = U + S
            // Flip the sign bit of `b`, test whether that sum overflows
            b ^= signbit;
            return a + b < b;
         } else { // S = U + U
            // The signed sum must not be less than the addend, and not negative
            return sum < a || (signbit & sum);
         }
      }
   } else {
      if (a_signed) {
         if (b_signed) { // U = S + S
            return signbit & (((sum | a) & b) | ((sum & a) & ~b));
            // Expanded:
            //     const bool a_is_negative = (intmax_t) a < 0;
            //     const bool b_is_negative = (intmax_t) b < 0;
            //     const bool sum_is_large = sum > INTMAX_MAX;
            //     if (b_is_negative) {
            //        if (a_is_negative) {
            //           // The sum must be negative, and therefore cannot be stored in an unsigned
            //           return true;
            //        } else if (sum_is_large) {
            //           // We added a negative value B to a positive value A, but the sum
            //           // ended up larger than the max signed value, so we wrapped
            //           return true;
            //        }
            //     } else if (a_is_negative) {
            //        if (sum_is_large) {
            //           // Same as above case with sum_is_large
            //           return true;
            //        }
            //     }
            //     return false;
         } else { // U = S + U
            return signbit & (sum ^ a ^ signbit) & (sum ^ b);
            // Expanded:
            //     const bool sum_is_large = sum > INTMAX_MAX;
            //     const bool b_is_large = b > INTMAX_MAX;
            //     const bool a_is_negative = (intmax_t) a < 0;
            //     if (!a_is_negative && b_is_large) {
            //        // We are adding a non-negative value to a large number, so the
            //        // sum must also be large
            //        if (!sum_is_large) {
            //           // We ended up with a smaller value, meaning that we must have wrapped
            //           return true;
            //        }
            //     }
            //     if (a_is_negative && !b_is_large) {
            //        // We subtracted a non-negative value from a non-large number, so
            //        // the result should not be large
            //        if (sum_is_large) {
            //           // We ended up with a large value, so we must have wrapped
            //           return true;
            //        }
            //     }
            //     return false;
         }
      } else {
         if (b_signed) { // U = U + S  --- (See [U = S + U] for an explanation)
            return signbit & (sum ^ a) & (sum ^ b ^ signbit);
         } else { // U = U + U (simple case)
            return sum < a;
         }
      }
   }
}

// Support for the `mlib_sub` macro
static inline bool(mlib_sub)(uintmax_t *dst, bool dst_signed, bool a_signed, uintmax_t a, bool b_signed, uintmax_t b)
   mlib_noexcept
{
   // Perform the subtraction using regular wrapping arithmetic
   const uintmax_t diff = *dst = a - b;
   const uintmax_t signbit = mlib_bits(1, mlib_bitsizeof(uintmax_t) - 1u);
   // Test whether the operation overflowed for the given sign configuration
   // (See mlib_add for more details on why we do this bit fiddling)
   if (dst_signed) {
      const bool diff_is_negative = signbit & diff;
      if (a_signed) {
         if (b_signed) { // S = S - S
            return signbit & (a ^ b) & (diff ^ a);
            // Explain:
            //     const bool a_is_negative = (intmax_t) a < 0;
            //     const bool b_is_negative = (intmax_t) b < 0;
            //     if (a_is_negative != b_is_negative) {
            //        // Given: Pos - Neg = Pos
            //        //      ∧ Neg - Pos = Neg
            //        // We expect that the difference preserves the sign of the minuend
            //        if (diff_is_negative != a_is_negative) {
            //           return true;
            //        }
            //     }
            //     // Otherwise, `Pos - Pos` and `Neg - Neg` cannot possibly overflow
            //     return false;
         } else { // S = S - U
            // The diff overflows if the sign-bit-flipped minuend is smaller than the subtrahend
            return (a ^ signbit) < b;
         }
      } else {
         if (b_signed) { // S = U - S
            // The diff overflows if the sign-bit-flipped subtrahend is greater than or equal to the minuend
            return a >= (b ^ signbit);
         } else { // S = U - U
            const bool expect_negative = a < b;
            return expect_negative != diff_is_negative;
         }
      }
   } else {
      if (a_signed) {
         if (b_signed) { // U = S - S
            return signbit & (((diff & a) & b) | ((diff | a) & ~b));
            // Expanded:
            //     const bool a_is_negative = (intmax_t) a < 0;
            //     const bool b_is_negative = (intmax_t) b < 0;
            //     const bool diff_is_large = diff > INTMAX_MAX;
            //     if (!b_is_negative) {
            //        if (a_is_negative) {
            //           // We subtracted a non-negative from a negative value, so the difference
            //           // must be negative and cannot be stored as unsigned
            //           return true;
            //        }
            //        if (diff_is_large) {
            //           // We subtracted a positive value from a signed value, so we must not
            //           // end up with a large value
            //           return true;
            //        }
            //     }
            //     if (a_is_negative) {
            //        if (diff_is_large) {
            //           // A is negative, and there is no possible value that we can subtract
            //           // from it to obtain this large integer, so we must have overflowed
            //           return true;
            //        }
            //     }
            //     return false;
         } else { //
            return (b > a) || (signbit & a);
         }
      } else {
         if (b_signed) { // U = U - S
            return signbit & (a ^ b ^ signbit) & (diff ^ a);
            // Explain:
            //     const bool a_is_large = a > INTMAX_MAX;
            //     const bool b_is_negative = (intmax_t) b < 0;
            //     const bool diff_is_large = diff > INTMAX_MAX;
            //     if (a_is_large && b_is_negative) {
            //        // The difference between a large value and a negative
            //        // value must also be a large value
            //        if (!diff_is_large) {
            //           // We expected another large value to appear.
            //           return true;
            //        }
            //     }
            //     if (!a_is_large && !b_is_negative) {
            //        // The difference between a non-large positive value and a non-negative value
            //        // must not be a large value
            //        if (diff_is_large) {
            //           // We did not expect a large difference
            //           return true;
            //        }
            //     }
            //     return false;
         } else {
            return a < b;
         }
      }
   }
}

// Support for the `mlib_mul` macro
static inline bool(mlib_mul)(uintmax_t *dst, bool dst_signed, bool a_signed, uintmax_t a, bool b_signed, uintmax_t b)
   mlib_noexcept
{
   // Multiplication is a lot more subtle
   const uintmax_t signbit = mlib_bits(1, mlib_bitsizeof(uintmax_t) - 1u);
   if (dst_signed) {
      if (a_signed) {
         if (b_signed) {
            // S = S × S
            *dst = a * b;
            if (((intmax_t)b == -1 && (intmax_t)a == INTMAX_MIN) || ((intmax_t)a == -1 && (intmax_t)b == INTMAX_MIN)) {
               // MIN × -1 is undefined
               return true;
            }
            if (a && (intmax_t)*dst / (intmax_t)a != (intmax_t)b) {
               // Mult did not preserve the arithmetic identity
               return true;
            }
            return false;
         } else {
            // S = S × U
            *dst = a * b;
            const bool a_is_negative = signbit & a;
            const uintmax_t positive_a = a_is_negative ? (0 - a) : a;
            const uintmax_t positive_prod = positive_a * b;
            const bool did_overflow = positive_a && positive_prod / positive_a != b;
            if (did_overflow) {
               return true;
            }
            if (positive_prod > (uintmax_t)INTMAX_MAX + (unsigned)a_is_negative) {
               return true;
            }
            return false;
         }
      } else {
         if (b_signed) {
            // S = U × S
            // Swap args: [S = S × U]
            return (mlib_mul)(dst, dst_signed, b_signed, b, a_signed, a);
         } else {
            // S = U × U
            *dst = a * b;
            const bool did_overflow = a && *dst / a != b;
            if (did_overflow) {
               return true;
            }
            if (signbit & *dst) {
               // A negative product indicates wrapping
               return true;
            }
            return false;
         }
      }
   } else {
      if (a_signed) {
         if (b_signed) {
            // U = S × S
            // Is either operand the min?
            bool either_min = false;
            if (signbit & a & b) {
               // Both negative: Flip the signs
               a = 0 - a;
               b = 0 - b;
               // MIN is pathological: 0 - MIN = MIN, so we need to check that:
               either_min = (intmax_t)a == INTMAX_MIN || (intmax_t)b == INTMAX_MIN;
            }
            // Check if the product would be a negative number
            const bool neg_prod = (signbit & (a ^ b)) && a && b && !either_min;
            *dst = a * b;
            return neg_prod || (a && *dst / a != b);
         } else {
            // U = S × U
            *dst = a * b;
            const bool did_ovr = a && *dst / a != b;
            const bool a_is_negative = signbit & a;
            if (did_ovr || (a_is_negative && b)) {
               return true;
            }
            return false;
         }
      } else {
         if (b_signed) {
            // U = U × S
            // Swap to [U = S × U]
            return (mlib_mul)(dst, dst_signed, b_signed, b, a_signed, a);
         } else {
            // U = U × U: Simple:
            *dst = a * b;
            return a && *dst / a != b;
         }
      }
   }
}

/**
 * @private
 * @brief This function performs the narrowing checks around a ckdint funciton
 *
 * @param dst Pointer to the target interger
 * @param dst_sz The size of the target integer, in bytes
 * @param minval The minimum value for the result. If negative, the target is treated as signed
 * @param maxval The maximum value for the result
 * @param a The left-hand operand for the operation
 * @param b The right-hand operand for the operation
 * @param fn The arithmetic function that performs arithmetic on the max-precision integer
 * @return true If the resulting value DOES NOT equal the true arithmetic result
 * @return false If the resulting value represents the true arithmetic results
 */
static inline bool
_mlib_ckdint(void *dst,
             int dst_sz,
             intmax_t minval,
             uintmax_t maxval,
             struct mlib_upsized_integer a,
             struct mlib_upsized_integer b,
             _mlib_ckdint_arith_fn fn) mlib_noexcept
{
   // Perform the arithmetic on uintmax_t, for wrapping behavior
   uintmax_t tmp;
   bool ovr = fn(&tmp, minval < 0, a.is_signed, a.bits.as_unsigned, b.is_signed, b.bits.as_unsigned);
   // Endian-adjusting for writing the result
   const char *copy_from = (const char *)&tmp;
   if (!mlib_is_little_endian()) {
      // We need to adjust the copy src in order to truncate the integer for big-endian encoding.
      // Number of high bytes that we need to drop:
      const int n_drop = (int)sizeof(tmp) - dst_sz;
      // Adjust the copy pointer to so that we copy from the most significant byte that
      // we wish to keep
      copy_from += n_drop;
   } else {
      // For little-endian native, we don't need to adjust the bytes, since we can just
      // truncate using the memcpy()
   }
   // Send the result to the destination
   memcpy(dst, copy_from, (size_t)dst_sz);
   // Final range check:
   if (minval < 0) {
      // Treat the target as signed:
      intmax_t idst = (intmax_t)tmp;
      return ovr || idst < minval || (idst > 0 && (uintmax_t)idst > maxval);
   } else {
      return ovr || tmp > maxval;
   }
}

/**
 * @internal
 * @brief Implementation function for the asserting arithmetic functions
 */
static inline uintmax_t(_mlib_assert_ckdint)(size_t dst_sz,
                                             intmax_t minval,
                                             uintmax_t maxval,
                                             struct mlib_upsized_integer a,
                                             struct mlib_upsized_integer b,
                                             _mlib_ckdint_arith_fn arith,
                                             const char *fn_str,
                                             const char *type_str,
                                             const char *a_str,
                                             const char *b_str,
                                             struct mlib_source_location here) mlib_noexcept
{
   uintmax_t tmp;
   bool did_overflow = _mlib_ckdint(&tmp, dst_sz, minval, maxval, a, b, arith);
   if (did_overflow) {
      fprintf(stderr,
              "%s:%d: [in %s]: Call of %s(%s, %s, %s) resulted in arithmetic overflow\n",
              here.file,
              here.lineno,
              here.func,
              fn_str,
              type_str,
              a_str,
              b_str);
      abort();
   }
   if (!mlib_is_little_endian()) {
      // We unconditionally set the leading bytes of `tmp`, but big-endian expects
      // the lower place values to be in the later bytes. If the target int is
      // smaller than intmax, we must shift all the bits over to their proper
      // position. This expression is trivially constant-folded by an optimizer.
      tmp >>= ((size_t)CHAR_BIT * ((sizeof tmp) - dst_sz));
   }
   return tmp;
}

static inline uintmax_t
_mlib_checked_cast(intmax_t min_,
                   uintmax_t max_,
                   struct mlib_upsized_integer val,
                   const char *typename_,
                   const char *expr,
                   struct mlib_source_location here) mlib_noexcept
{
   if (!(mlib_in_range)(min_, max_, val)) {
      if (val.is_signed) {
         fprintf(stderr,
                 "%s:%d: in [%s]: Checked integer cast of “%s” (value = %lld) to “%s” loses information\n",
                 here.file,
                 here.lineno,
                 here.func,
                 expr,
                 (long long)val.bits.as_signed,
                 typename_);
      } else {
         fprintf(stderr,
                 "%s:%d: in [%s]: Checked integer cast of “%s” (value = %llu) to “%s” loses information\n",
                 here.file,
                 here.lineno,
                 here.func,
                 expr,
                 (unsigned long long)val.bits.as_unsigned,
                 typename_);
      }
      fflush(stderr);
      abort();
   }
   if (val.is_signed) {
      return (uintmax_t)val.bits.as_signed;
   }
   return val.bits.as_unsigned;
}

mlib_extern_c_end();
