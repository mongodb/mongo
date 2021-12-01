/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Helpers to manipulate integer types that don't fit in TypeTraits.h */

#ifndef mozilla_IntegerTypeTraits_h
#define mozilla_IntegerTypeTraits_h

#include "mozilla/TypeTraits.h"
#include <stdint.h>

namespace mozilla {

namespace detail {

/**
 * StdintTypeForSizeAndSignedness returns the stdint integer type
 * of given size (can be 1, 2, 4 or 8) and given signedness
 * (false means unsigned, true means signed).
 */
template<size_t Size, bool Signedness>
struct StdintTypeForSizeAndSignedness;

template<>
struct StdintTypeForSizeAndSignedness<1, true>
{
  typedef int8_t Type;
};

template<>
struct StdintTypeForSizeAndSignedness<1, false>
{
  typedef uint8_t Type;
};

template<>
struct StdintTypeForSizeAndSignedness<2, true>
{
  typedef int16_t Type;
};

template<>
struct StdintTypeForSizeAndSignedness<2, false>
{
  typedef uint16_t Type;
};

template<>
struct StdintTypeForSizeAndSignedness<4, true>
{
  typedef int32_t Type;
};

template<>
struct StdintTypeForSizeAndSignedness<4, false>
{
  typedef uint32_t Type;
};

template<>
struct StdintTypeForSizeAndSignedness<8, true>
{
  typedef int64_t Type;
};

template<>
struct StdintTypeForSizeAndSignedness<8, false>
{
  typedef uint64_t Type;
};

} // namespace detail

template<size_t Size>
struct UnsignedStdintTypeForSize
  : detail::StdintTypeForSizeAndSignedness<Size, false>
{};

template<size_t Size>
struct SignedStdintTypeForSize
  : detail::StdintTypeForSizeAndSignedness<Size, true>
{};

template<typename IntegerType>
struct PositionOfSignBit
{
  static_assert(IsIntegral<IntegerType>::value,
                "PositionOfSignBit is only for integral types");
  // 8 here should be CHAR_BIT from limits.h, but the world has moved on.
  static const size_t value = 8 * sizeof(IntegerType) - 1;
};

/**
 * MinValue returns the minimum value of the given integer type as a
 * compile-time constant, which std::numeric_limits<IntegerType>::min()
 * cannot do in c++98.
 */
template<typename IntegerType>
struct MinValue
{
private:
  static_assert(IsIntegral<IntegerType>::value,
                "MinValue is only for integral types");

  typedef typename MakeUnsigned<IntegerType>::Type UnsignedIntegerType;
  static const size_t PosOfSignBit = PositionOfSignBit<IntegerType>::value;

public:
  // Bitwise ops may return a larger type, that's why we cast explicitly.
  // In C++, left bit shifts on signed values is undefined by the standard
  // unless the shifted value is representable.
  // Notice that signed-to-unsigned conversions are always well-defined in
  // the standard as the value congruent to 2**n, as expected. By contrast,
  // unsigned-to-signed is only well-defined if the value is representable.
  static const IntegerType value =
      IsSigned<IntegerType>::value
      ? IntegerType(UnsignedIntegerType(1) << PosOfSignBit)
      : IntegerType(0);
};

/**
 * MaxValue returns the maximum value of the given integer type as a
 * compile-time constant, which std::numeric_limits<IntegerType>::max()
 * cannot do in c++98.
 */
template<typename IntegerType>
struct MaxValue
{
  static_assert(IsIntegral<IntegerType>::value,
                "MaxValue is only for integral types");

  // Tricksy, but covered by the CheckedInt unit test.
  // Relies on the type of MinValue<IntegerType>::value
  // being IntegerType.
  static const IntegerType value = ~MinValue<IntegerType>::value;
};

} // namespace mozilla

#endif // mozilla_IntegerTypeTraits_h
