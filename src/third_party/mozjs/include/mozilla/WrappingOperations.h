/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Math operations that implement wraparound semantics on overflow or underflow.
 *
 * While in some cases (but not all of them!) plain old C++ operators and casts
 * will behave just like these functions, there are three reasons you should use
 * these functions:
 *
 *   1) These functions make *explicit* the desire for and dependence upon
 *      wraparound semantics, just as Rust's i32::wrapping_add and similar
 *      functions explicitly produce wraparound in Rust.
 *   2) They implement this functionality *safely*, without invoking signed
 *      integer overflow that has undefined behavior in C++.
 *   3) They play nice with compiler-based integer-overflow sanitizers (see
 *      build/moz.configure/toolchain.configure), that in appropriately
 * configured builds verify at runtime that integral arithmetic doesn't
 * overflow.
 */

#ifndef mozilla_WrappingOperations_h
#define mozilla_WrappingOperations_h

#include "mozilla/Attributes.h"

#include <limits.h>
#include <type_traits>

namespace mozilla {

namespace detail {

template <typename UnsignedType>
struct WrapToSignedHelper {
  static_assert(std::is_unsigned_v<UnsignedType>,
                "WrapToSigned must be passed an unsigned type");

  using SignedType = std::make_signed_t<UnsignedType>;

  static constexpr SignedType MaxValue =
      (UnsignedType(1) << (CHAR_BIT * sizeof(SignedType) - 1)) - 1;
  static constexpr SignedType MinValue = -MaxValue - 1;

  static constexpr UnsignedType MinValueUnsigned =
      static_cast<UnsignedType>(MinValue);
  static constexpr UnsignedType MaxValueUnsigned =
      static_cast<UnsignedType>(MaxValue);

  // Overflow-correctness was proven in bug 1432646 and is explained in the
  // comment below.  This function is very hot, both at compile time and
  // runtime, so disable all overflow checking in it.
  MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW
  MOZ_NO_SANITIZE_SIGNED_OVERFLOW static constexpr SignedType compute(
      UnsignedType aValue) {
    // This algorithm was originally provided here:
    // https://stackoverflow.com/questions/13150449/efficient-unsigned-to-signed-cast-avoiding-implementation-defined-behavior
    //
    // If the value is in the non-negative signed range, just cast.
    //
    // If the value will be negative, compute its delta from the first number
    // past the max signed integer, then add that to the minimum signed value.
    //
    // At the low end: if |u| is the maximum signed value plus one, then it has
    // the same mathematical value as |MinValue| cast to unsigned form.  The
    // delta is zero, so the signed form of |u| is |MinValue| -- exactly the
    // result of adding zero delta to |MinValue|.
    //
    // At the high end: if |u| is the maximum *unsigned* value, then it has all
    // bits set.  |MinValue| cast to unsigned form is purely the high bit set.
    // So the delta is all bits but high set -- exactly |MaxValue|.  And as
    // |MinValue = -MaxValue - 1|, we have |MaxValue + (-MaxValue - 1)| to
    // equal -1.
    //
    // Thus the delta below is in signed range, the corresponding cast is safe,
    // and this computation produces values spanning [MinValue, 0): exactly the
    // desired range of all negative signed integers.
    return (aValue <= MaxValueUnsigned)
               ? static_cast<SignedType>(aValue)
               : static_cast<SignedType>(aValue - MinValueUnsigned) + MinValue;
  }
};

}  // namespace detail

/**
 * Convert an unsigned value to signed, if necessary wrapping around.
 *
 * This is the behavior normal C++ casting will perform in most implementations
 * these days -- but this function makes explicit that such conversion is
 * happening.
 */
template <typename UnsignedType>
constexpr typename detail::WrapToSignedHelper<UnsignedType>::SignedType
WrapToSigned(UnsignedType aValue) {
  return detail::WrapToSignedHelper<UnsignedType>::compute(aValue);
}

namespace detail {

template <typename T>
constexpr T ToResult(std::make_unsigned_t<T> aUnsigned) {
  // We could *always* return WrapToSigned and rely on unsigned conversion to
  // undo the wrapping when |T| is unsigned, but this seems clearer.
  return std::is_signed_v<T> ? WrapToSigned(aUnsigned) : aUnsigned;
}

template <typename T>
struct WrappingAddHelper {
 private:
  using UnsignedT = std::make_unsigned_t<T>;

 public:
  MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW
  static constexpr T compute(T aX, T aY) {
    return ToResult<T>(static_cast<UnsignedT>(aX) + static_cast<UnsignedT>(aY));
  }
};

}  // namespace detail

/**
 * Add two integers of the same type and return the result converted to that
 * type using wraparound semantics, without triggering overflow sanitizers.
 *
 * For N-bit unsigned integer types, this is equivalent to adding the two
 * numbers, then taking the result mod 2**N:
 *
 *   WrappingAdd(uint32_t(42), uint32_t(17)) is 59 (59 mod 2**32);
 *   WrappingAdd(uint8_t(240), uint8_t(20)) is 4 (260 mod 2**8).
 *
 * Unsigned WrappingAdd acts exactly like C++ unsigned addition.
 *
 * For N-bit signed integer types, this is equivalent to adding the two numbers
 * wrapped to unsigned, then wrapping the sum mod 2**N to the signed range:
 *
 *   WrappingAdd(int16_t(32767), int16_t(3)) is
 *     -32766 ((32770 mod 2**16) - 2**16);
 *   WrappingAdd(int8_t(-128), int8_t(-128)) is
 *     0 (256 mod 2**8);
 *   WrappingAdd(int32_t(-42), int32_t(-17)) is
 *     -59 ((8589934533 mod 2**32) - 2**32).
 *
 * There's no equivalent to this operation in C++, as C++ signed addition that
 * overflows has undefined behavior.  But it's how such addition *tends* to
 * behave with most compilers, unless an optimization or similar -- quite
 * permissibly -- triggers different behavior.
 */
template <typename T>
constexpr T WrappingAdd(T aX, T aY) {
  return detail::WrappingAddHelper<T>::compute(aX, aY);
}

namespace detail {

template <typename T>
struct WrappingSubtractHelper {
 private:
  using UnsignedT = std::make_unsigned_t<T>;

 public:
  MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW
  static constexpr T compute(T aX, T aY) {
    return ToResult<T>(static_cast<UnsignedT>(aX) - static_cast<UnsignedT>(aY));
  }
};

}  // namespace detail

/**
 * Subtract two integers of the same type and return the result converted to
 * that type using wraparound semantics, without triggering overflow sanitizers.
 *
 * For N-bit unsigned integer types, this is equivalent to subtracting the two
 * numbers, then taking the result mod 2**N:
 *
 *   WrappingSubtract(uint32_t(42), uint32_t(17)) is 29 (29 mod 2**32);
 *   WrappingSubtract(uint8_t(5), uint8_t(20)) is 241 (-15 mod 2**8).
 *
 * Unsigned WrappingSubtract acts exactly like C++ unsigned subtraction.
 *
 * For N-bit signed integer types, this is equivalent to subtracting the two
 * numbers wrapped to unsigned, then wrapping the difference mod 2**N to the
 * signed range:
 *
 *   WrappingSubtract(int16_t(32767), int16_t(-5)) is -32764 ((32772 mod 2**16)
 * - 2**16); WrappingSubtract(int8_t(-128), int8_t(127)) is 1 (-255 mod 2**8);
 *   WrappingSubtract(int32_t(-17), int32_t(-42)) is 25 (25 mod 2**32).
 *
 * There's no equivalent to this operation in C++, as C++ signed subtraction
 * that overflows has undefined behavior.  But it's how such subtraction *tends*
 * to behave with most compilers, unless an optimization or similar -- quite
 * permissibly -- triggers different behavior.
 */
template <typename T>
constexpr T WrappingSubtract(T aX, T aY) {
  return detail::WrappingSubtractHelper<T>::compute(aX, aY);
}

namespace detail {

template <typename T>
struct WrappingMultiplyHelper {
 private:
  using UnsignedT = std::make_unsigned_t<T>;

 public:
  MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW
  static constexpr T compute(T aX, T aY) {
    // Begin with |1U| to ensure the overall operation chain is never promoted
    // to signed integer operations that might have *signed* integer overflow.
    return ToResult<T>(static_cast<UnsignedT>(1U * static_cast<UnsignedT>(aX) *
                                              static_cast<UnsignedT>(aY)));
  }
};

}  // namespace detail

/**
 * Multiply two integers of the same type and return the result converted to
 * that type using wraparound semantics, without triggering overflow sanitizers.
 *
 * For N-bit unsigned integer types, this is equivalent to multiplying the two
 * numbers, then taking the result mod 2**N:
 *
 *   WrappingMultiply(uint32_t(42), uint32_t(17)) is 714 (714 mod 2**32);
 *   WrappingMultiply(uint8_t(16), uint8_t(24)) is 128 (384 mod 2**8);
 *   WrappingMultiply(uint16_t(3), uint16_t(32768)) is 32768 (98304 mod 2*16).
 *
 * Unsigned WrappingMultiply is *not* identical to C++ multiplication: with most
 * compilers, in rare cases uint16_t*uint16_t can invoke *signed* integer
 * overflow having undefined behavior!  http://kqueue.org/blog/2013/09/17/cltq/
 * has the grody details.  (Some compilers do this for uint32_t, not uint16_t.)
 * So it's especially important to use WrappingMultiply for wraparound math with
 * uint16_t.  That quirk aside, this function acts like you *thought* C++
 * unsigned multiplication always worked.
 *
 * For N-bit signed integer types, this is equivalent to multiplying the two
 * numbers wrapped to unsigned, then wrapping the product mod 2**N to the signed
 * range:
 *
 *   WrappingMultiply(int16_t(-456), int16_t(123)) is
 *     9448 ((-56088 mod 2**16) + 2**16);
 *   WrappingMultiply(int32_t(-7), int32_t(-9)) is 63 (63 mod 2**32);
 *   WrappingMultiply(int8_t(16), int8_t(24)) is -128 ((384 mod 2**8) - 2**8);
 *   WrappingMultiply(int8_t(16), int8_t(255)) is -16 ((4080 mod 2**8) - 2**8).
 *
 * There's no equivalent to this operation in C++, as C++ signed
 * multiplication that overflows has undefined behavior.  But it's how such
 * multiplication *tends* to behave with most compilers, unless an optimization
 * or similar -- quite permissibly -- triggers different behavior.
 */
template <typename T>
constexpr T WrappingMultiply(T aX, T aY) {
  return detail::WrappingMultiplyHelper<T>::compute(aX, aY);
}

} /* namespace mozilla */

#endif /* mozilla_WrappingOperations_h */
