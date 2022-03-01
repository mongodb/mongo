/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Helpers to manipulate integer types that don't fit in TypeTraits.h */

#ifndef mozilla_IntegerTypeTraits_h
#define mozilla_IntegerTypeTraits_h

#include <stddef.h>
#include <stdint.h>
#include <type_traits>

namespace mozilla {

namespace detail {

/**
 * StdintTypeForSizeAndSignedness returns the stdint integer type
 * of given size (can be 1, 2, 4 or 8) and given signedness
 * (false means unsigned, true means signed).
 */
template <size_t Size, bool Signedness>
struct StdintTypeForSizeAndSignedness;

template <>
struct StdintTypeForSizeAndSignedness<1, true> {
  typedef int8_t Type;
};

template <>
struct StdintTypeForSizeAndSignedness<1, false> {
  typedef uint8_t Type;
};

template <>
struct StdintTypeForSizeAndSignedness<2, true> {
  typedef int16_t Type;
};

template <>
struct StdintTypeForSizeAndSignedness<2, false> {
  typedef uint16_t Type;
};

template <>
struct StdintTypeForSizeAndSignedness<4, true> {
  typedef int32_t Type;
};

template <>
struct StdintTypeForSizeAndSignedness<4, false> {
  typedef uint32_t Type;
};

template <>
struct StdintTypeForSizeAndSignedness<8, true> {
  typedef int64_t Type;
};

template <>
struct StdintTypeForSizeAndSignedness<8, false> {
  typedef uint64_t Type;
};

}  // namespace detail

template <size_t Size>
struct UnsignedStdintTypeForSize
    : detail::StdintTypeForSizeAndSignedness<Size, false> {};

template <size_t Size>
struct SignedStdintTypeForSize
    : detail::StdintTypeForSizeAndSignedness<Size, true> {};

template <typename IntegerType>
struct PositionOfSignBit {
  static_assert(std::is_integral_v<IntegerType>,
                "PositionOfSignBit is only for integral types");
  // 8 here should be CHAR_BIT from limits.h, but the world has moved on.
  static const size_t value = 8 * sizeof(IntegerType) - 1;
};

}  // namespace mozilla

#endif  // mozilla_IntegerTypeTraits_h
