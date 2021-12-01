/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Math operations that implement wraparound semantics on overflow or underflow
 * without performing C++ undefined behavior or tripping up compiler-based
 * integer-overflow sanitizers.
 */

#ifndef mozilla_WrappingOperations_h
#define mozilla_WrappingOperations_h

#include "mozilla/Attributes.h"
#include "mozilla/TypeTraits.h"

#include <limits.h>

namespace mozilla {

namespace detail {

template<typename UnsignedType>
struct WrapToSignedHelper
{
  static_assert(mozilla::IsUnsigned<UnsignedType>::value,
                "WrapToSigned must be passed an unsigned type");

  using SignedType = typename mozilla::MakeSigned<UnsignedType>::Type;

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
  MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW MOZ_NO_SANITIZE_SIGNED_OVERFLOW
  static constexpr SignedType compute(UnsignedType aValue)
  {
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

} // namespace detail

/**
 * Convert an unsigned value to signed, if necessary wrapping around.
 *
 * This is the behavior normal C++ casting will perform in most implementations
 * these days -- but this function makes explicit that such conversion is
 * happening.
 */
template<typename UnsignedType>
inline constexpr typename detail::WrapToSignedHelper<UnsignedType>::SignedType
WrapToSigned(UnsignedType aValue)
{
  return detail::WrapToSignedHelper<UnsignedType>::compute(aValue);
}

namespace detail {

template<typename T>
struct WrappingMultiplyHelper
{
private:
  using UnsignedT = typename MakeUnsigned<T>::Type;

  MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW
  static UnsignedT
  multiply(UnsignedT aX, UnsignedT aY)
  {
  // |mozilla::WrappingMultiply| isn't constexpr because MSVC warns about well-
  // defined unsigned integer overflows that may happen here.
  // https://msdn.microsoft.com/en-us/library/4kze989h.aspx  And constexpr
  // seems to cause the warning to be emitted at |WrappingMultiply| call *sites*
  // instead of here, so these #pragmas are ineffective.
  //
  // https://stackoverflow.com/questions/37658794/integer-constant-overflow-warning-in-constexpr
  //
  // If/when MSVC fix this bug, we should make these functions constexpr.

    // Begin with |1U| to ensure the overall operation chain is never promoted
    // to signed integer operations that might have *signed* integer overflow.
    return static_cast<UnsignedT>(1U * aX * aY);
  }

  static T
  toResult(UnsignedT aX, UnsignedT aY)
  {
    // We could always return WrapToSigned and rely on unsigned conversion
    // undoing the wrapping when |T| is unsigned, but this seems clearer.
    return IsSigned<T>::value
           ? WrapToSigned(multiply(aX, aY))
           : multiply(aX, aY);
  }

public:
  MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW
  static T compute(T aX, T aY)
  {
    return toResult(static_cast<UnsignedT>(aX), static_cast<UnsignedT>(aY));
  }
};

} // namespace detail

/**
 * Multiply two integers of the same type, and return the result converted to
 * that type using wraparound semantics.  This function:
 *
 *   1) makes explicit the desire for and dependence upon wraparound semantics,
 *   2) provides wraparound semantics *safely* with no signed integer overflow
 *      that would have undefined behavior, and
 *   3) won't trip up {,un}signed-integer overflow sanitizers (see
 *      build/autoconf/sanitize.m4) at runtime.
 *
 * For N-bit unsigned integer types, this is equivalent to multiplying the two
 * numbers, then taking the result mod 2**N:
 *
 *   WrappingMultiply(uint32_t(42), uint32_t(17)) is 714 (714 mod 2**32);
 *   WrappingMultiply(uint8_t(16), uint8_t(24)) is 128 (384 mod 2**8);
 *   WrappingMultiply(uint16_t(3), uint16_t(32768)) is 32768 (98304 mod 2*16).
 *
 * Use this function for any unsigned multiplication that can wrap (instead of
 * normal C++ multiplication) to play nice with the sanitizers.  But it's
 * especially important to use it for uint16_t multiplication: in most compilers
 * for uint16_t*uint16_t some operand values will trigger signed integer
 * overflow with undefined behavior!  http://kqueue.org/blog/2013/09/17/cltq/
 * has the grody details.  Other than that one weird case, WrappingMultiply on
 * unsigned types is the same as C++ multiplication.
 *
 * For N-bit signed integer types, this is equivalent to multiplying the two
 * numbers wrapped to unsigned, taking the product mod 2**N, then wrapping that
 * number to the signed range:
 *
 *   WrappingMultiply(int16_t(-456), int16_t(123)) is 9448 ((-56088 mod 2**16) + 2**16);
 *   WrappingMultiply(int32_t(-7), int32_t(-9)) is 63 (63 mod 2**32);
 *   WrappingMultiply(int8_t(16), int8_t(24)) is -128 ((384 mod 2**8) - 2**8);
 *   WrappingMultiply(int8_t(16), int8_t(255)) is -16 ((4080 mod 2**8) - 2**8).
 *
 * There is no ready equivalent to this operation in C++, as applying C++
 * multiplication to signed integer types in ways that trigger overflow has
 * undefined behavior.  However, it's how multiplication *tends* to behave with
 * most compilers in most situations, even though it's emphatically not required
 * to do so.
 */
template<typename T>
inline T
WrappingMultiply(T aX, T aY)
{
  return detail::WrappingMultiplyHelper<T>::compute(aX, aY);
}

} /* namespace mozilla */

#endif /* mozilla_WrappingOperations_h */
