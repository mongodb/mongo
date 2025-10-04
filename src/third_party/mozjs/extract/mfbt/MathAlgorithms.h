/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* mfbt maths algorithms. */

#ifndef mozilla_MathAlgorithms_h
#define mozilla_MathAlgorithms_h

#include "mozilla/Assertions.h"

#include <cmath>
#include <algorithm>
#include <limits.h>
#include <stdint.h>
#include <type_traits>

namespace mozilla {

namespace detail {

template <typename T>
struct AllowDeprecatedAbsFixed : std::false_type {};

template <>
struct AllowDeprecatedAbsFixed<int32_t> : std::true_type {};
template <>
struct AllowDeprecatedAbsFixed<int64_t> : std::true_type {};

template <typename T>
struct AllowDeprecatedAbs : AllowDeprecatedAbsFixed<T> {};

template <>
struct AllowDeprecatedAbs<int> : std::true_type {};
template <>
struct AllowDeprecatedAbs<long> : std::true_type {};

}  // namespace detail

// DO NOT USE DeprecatedAbs.  It exists only until its callers can be converted
// to Abs below, and it will be removed when all callers have been changed.
template <typename T>
inline std::enable_if_t<detail::AllowDeprecatedAbs<T>::value, T> DeprecatedAbs(
    const T aValue) {
  // The absolute value of the smallest possible value of a signed-integer type
  // won't fit in that type (on twos-complement systems -- and we're blithely
  // assuming we're on such systems, for the non-<stdint.h> types listed above),
  // so assert that the input isn't that value.
  //
  // This is the case if: the value is non-negative; or if adding one (giving a
  // value in the range [-maxvalue, 0]), then negating (giving a value in the
  // range [0, maxvalue]), doesn't produce maxvalue (because in twos-complement,
  // (minvalue + 1) == -maxvalue).
  MOZ_ASSERT(aValue >= 0 ||
                 -(aValue + 1) != T((1ULL << (CHAR_BIT * sizeof(T) - 1)) - 1),
             "You can't negate the smallest possible negative integer!");
  return aValue >= 0 ? aValue : -aValue;
}

namespace detail {

template <typename T, typename = void>
struct AbsReturnType;

template <typename T>
struct AbsReturnType<
    T, std::enable_if_t<std::is_integral_v<T> && std::is_signed_v<T>>> {
  using Type = std::make_unsigned_t<T>;
};

template <typename T>
struct AbsReturnType<T, std::enable_if_t<std::is_floating_point_v<T>>> {
  using Type = T;
};

}  // namespace detail

template <typename T>
inline constexpr typename detail::AbsReturnType<T>::Type Abs(const T aValue) {
  using ReturnType = typename detail::AbsReturnType<T>::Type;
  return aValue >= 0 ? ReturnType(aValue) : ~ReturnType(aValue) + 1;
}

template <>
inline float Abs<float>(const float aFloat) {
  return std::fabs(aFloat);
}

template <>
inline double Abs<double>(const double aDouble) {
  return std::fabs(aDouble);
}

template <>
inline long double Abs<long double>(const long double aLongDouble) {
  return std::fabs(aLongDouble);
}

}  // namespace mozilla

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_AMD64) || \
                          defined(_M_X64) || defined(_M_ARM64))
#  define MOZ_BITSCAN_WINDOWS

#  include <intrin.h>
#  pragma intrinsic(_BitScanForward, _BitScanReverse)

#  if defined(_M_AMD64) || defined(_M_X64) || defined(_M_ARM64)
#    define MOZ_BITSCAN_WINDOWS64
#    pragma intrinsic(_BitScanForward64, _BitScanReverse64)
#  endif

#endif

namespace mozilla {

namespace detail {

#if defined(MOZ_BITSCAN_WINDOWS)

inline uint_fast8_t CountLeadingZeroes32(uint32_t aValue) {
  unsigned long index;
  if (!_BitScanReverse(&index, static_cast<unsigned long>(aValue))) return 32;
  return uint_fast8_t(31 - index);
}

inline uint_fast8_t CountTrailingZeroes32(uint32_t aValue) {
  unsigned long index;
  if (!_BitScanForward(&index, static_cast<unsigned long>(aValue))) return 32;
  return uint_fast8_t(index);
}

inline uint_fast8_t CountPopulation32(uint32_t aValue) {
  uint32_t x = aValue - ((aValue >> 1) & 0x55555555);
  x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
  return (((x + (x >> 4)) & 0xf0f0f0f) * 0x1010101) >> 24;
}
inline uint_fast8_t CountPopulation64(uint64_t aValue) {
  return uint_fast8_t(CountPopulation32(aValue & 0xffffffff) +
                      CountPopulation32(aValue >> 32));
}

inline uint_fast8_t CountLeadingZeroes64(uint64_t aValue) {
#  if defined(MOZ_BITSCAN_WINDOWS64)
  unsigned long index;
  if (!_BitScanReverse64(&index, static_cast<unsigned __int64>(aValue)))
    return 64;
  return uint_fast8_t(63 - index);
#  else
  uint32_t hi = uint32_t(aValue >> 32);
  if (hi != 0) {
    return CountLeadingZeroes32(hi);
  }
  return 32u + CountLeadingZeroes32(uint32_t(aValue));
#  endif
}

inline uint_fast8_t CountTrailingZeroes64(uint64_t aValue) {
#  if defined(MOZ_BITSCAN_WINDOWS64)
  unsigned long index;
  if (!_BitScanForward64(&index, static_cast<unsigned __int64>(aValue)))
    return 64;
  return uint_fast8_t(index);
#  else
  uint32_t lo = uint32_t(aValue);
  if (lo != 0) {
    return CountTrailingZeroes32(lo);
  }
  return 32u + CountTrailingZeroes32(uint32_t(aValue >> 32));
#  endif
}

#elif defined(__clang__) || defined(__GNUC__)

inline uint_fast8_t CountLeadingZeroes32(uint32_t aValue) {
  return static_cast<uint_fast8_t>(__builtin_clz(aValue));
}

inline uint_fast8_t CountTrailingZeroes32(uint32_t aValue) {
  return static_cast<uint_fast8_t>(__builtin_ctz(aValue));
}

inline uint_fast8_t CountPopulation32(uint32_t aValue) {
  return static_cast<uint_fast8_t>(__builtin_popcount(aValue));
}

inline uint_fast8_t CountPopulation64(uint64_t aValue) {
  return static_cast<uint_fast8_t>(__builtin_popcountll(aValue));
}

inline uint_fast8_t CountLeadingZeroes64(uint64_t aValue) {
  return static_cast<uint_fast8_t>(__builtin_clzll(aValue));
}

inline uint_fast8_t CountTrailingZeroes64(uint64_t aValue) {
  return static_cast<uint_fast8_t>(__builtin_ctzll(aValue));
}

#else
#  error "Implement these!"
inline uint_fast8_t CountLeadingZeroes32(uint32_t aValue) = delete;
inline uint_fast8_t CountTrailingZeroes32(uint32_t aValue) = delete;
inline uint_fast8_t CountPopulation32(uint32_t aValue) = delete;
inline uint_fast8_t CountPopulation64(uint64_t aValue) = delete;
inline uint_fast8_t CountLeadingZeroes64(uint64_t aValue) = delete;
inline uint_fast8_t CountTrailingZeroes64(uint64_t aValue) = delete;
#endif

}  // namespace detail

/**
 * Compute the number of high-order zero bits in the NON-ZERO number |aValue|.
 * That is, looking at the bitwise representation of the number, with the
 * highest- valued bits at the start, return the number of zeroes before the
 * first one is observed.
 *
 * CountLeadingZeroes32(0xF0FF1000) is 0;
 * CountLeadingZeroes32(0x7F8F0001) is 1;
 * CountLeadingZeroes32(0x3FFF0100) is 2;
 * CountLeadingZeroes32(0x1FF50010) is 3; and so on.
 */
inline uint_fast8_t CountLeadingZeroes32(uint32_t aValue) {
  MOZ_ASSERT(aValue != 0);
  return detail::CountLeadingZeroes32(aValue);
}

/**
 * Compute the number of low-order zero bits in the NON-ZERO number |aValue|.
 * That is, looking at the bitwise representation of the number, with the
 * lowest- valued bits at the start, return the number of zeroes before the
 * first one is observed.
 *
 * CountTrailingZeroes32(0x0100FFFF) is 0;
 * CountTrailingZeroes32(0x7000FFFE) is 1;
 * CountTrailingZeroes32(0x0080FFFC) is 2;
 * CountTrailingZeroes32(0x0080FFF8) is 3; and so on.
 */
inline uint_fast8_t CountTrailingZeroes32(uint32_t aValue) {
  MOZ_ASSERT(aValue != 0);
  return detail::CountTrailingZeroes32(aValue);
}

/**
 * Compute the number of one bits in the number |aValue|,
 */
inline uint_fast8_t CountPopulation32(uint32_t aValue) {
  return detail::CountPopulation32(aValue);
}

/** Analogous to CountPopulation32, but for 64-bit numbers */
inline uint_fast8_t CountPopulation64(uint64_t aValue) {
  return detail::CountPopulation64(aValue);
}

/** Analogous to CountLeadingZeroes32, but for 64-bit numbers. */
inline uint_fast8_t CountLeadingZeroes64(uint64_t aValue) {
  MOZ_ASSERT(aValue != 0);
  return detail::CountLeadingZeroes64(aValue);
}

/** Analogous to CountTrailingZeroes32, but for 64-bit numbers. */
inline uint_fast8_t CountTrailingZeroes64(uint64_t aValue) {
  MOZ_ASSERT(aValue != 0);
  return detail::CountTrailingZeroes64(aValue);
}

namespace detail {

template <typename T, size_t Size = sizeof(T)>
class CeilingLog2;

template <typename T>
class CeilingLog2<T, 4> {
 public:
  static uint_fast8_t compute(const T aValue) {
    // Check for <= 1 to avoid the == 0 undefined case.
    return aValue <= 1 ? 0u : 32u - CountLeadingZeroes32(aValue - 1);
  }
};

template <typename T>
class CeilingLog2<T, 8> {
 public:
  static uint_fast8_t compute(const T aValue) {
    // Check for <= 1 to avoid the == 0 undefined case.
    return aValue <= 1 ? 0u : 64u - CountLeadingZeroes64(aValue - 1);
  }
};

}  // namespace detail

/**
 * Compute the log of the least power of 2 greater than or equal to |aValue|.
 *
 * CeilingLog2(0..1) is 0;
 * CeilingLog2(2) is 1;
 * CeilingLog2(3..4) is 2;
 * CeilingLog2(5..8) is 3;
 * CeilingLog2(9..16) is 4; and so on.
 */
template <typename T>
inline uint_fast8_t CeilingLog2(const T aValue) {
  return detail::CeilingLog2<T>::compute(aValue);
}

/** A CeilingLog2 variant that accepts only size_t. */
inline uint_fast8_t CeilingLog2Size(size_t aValue) {
  return CeilingLog2(aValue);
}

namespace detail {

template <typename T, size_t Size = sizeof(T)>
class FloorLog2;

template <typename T>
class FloorLog2<T, 4> {
 public:
  static uint_fast8_t compute(const T aValue) {
    return 31u - CountLeadingZeroes32(aValue | 1);
  }
};

template <typename T>
class FloorLog2<T, 8> {
 public:
  static uint_fast8_t compute(const T aValue) {
    return 63u - CountLeadingZeroes64(aValue | 1);
  }
};

}  // namespace detail

/**
 * Compute the log of the greatest power of 2 less than or equal to |aValue|.
 *
 * FloorLog2(0..1) is 0;
 * FloorLog2(2..3) is 1;
 * FloorLog2(4..7) is 2;
 * FloorLog2(8..15) is 3; and so on.
 */
template <typename T>
inline constexpr uint_fast8_t FloorLog2(const T aValue) {
  return detail::FloorLog2<T>::compute(aValue);
}

/** A FloorLog2 variant that accepts only size_t. */
inline uint_fast8_t FloorLog2Size(size_t aValue) { return FloorLog2(aValue); }

/*
 * Compute the smallest power of 2 greater than or equal to |x|.  |x| must not
 * be so great that the computed value would overflow |size_t|.
 */
inline size_t RoundUpPow2(size_t aValue) {
  MOZ_ASSERT(aValue <= (size_t(1) << (sizeof(size_t) * CHAR_BIT - 1)),
             "can't round up -- will overflow!");
  return size_t(1) << CeilingLog2(aValue);
}

/**
 * Rotates the bits of the given value left by the amount of the shift width.
 */
template <typename T>
MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW inline T RotateLeft(const T aValue,
                                                      uint_fast8_t aShift) {
  static_assert(std::is_unsigned_v<T>, "Rotates require unsigned values");

  MOZ_ASSERT(aShift < sizeof(T) * CHAR_BIT, "Shift value is too large!");
  MOZ_ASSERT(aShift > 0,
             "Rotation by value length is undefined behavior, but compilers "
             "do not currently fold a test into the rotate instruction. "
             "Please remove this restriction when compilers optimize the "
             "zero case (http://blog.regehr.org/archives/1063).");

  return (aValue << aShift) | (aValue >> (sizeof(T) * CHAR_BIT - aShift));
}

/**
 * Rotates the bits of the given value right by the amount of the shift width.
 */
template <typename T>
MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW inline T RotateRight(const T aValue,
                                                       uint_fast8_t aShift) {
  static_assert(std::is_unsigned_v<T>, "Rotates require unsigned values");

  MOZ_ASSERT(aShift < sizeof(T) * CHAR_BIT, "Shift value is too large!");
  MOZ_ASSERT(aShift > 0,
             "Rotation by value length is undefined behavior, but compilers "
             "do not currently fold a test into the rotate instruction. "
             "Please remove this restriction when compilers optimize the "
             "zero case (http://blog.regehr.org/archives/1063).");

  return (aValue >> aShift) | (aValue << (sizeof(T) * CHAR_BIT - aShift));
}

/**
 * Returns true if |x| is a power of two.
 * Zero is not an integer power of two. (-Inf is not an integer)
 */
template <typename T>
constexpr bool IsPowerOfTwo(T x) {
  static_assert(std::is_unsigned_v<T>, "IsPowerOfTwo requires unsigned values");
  return x && (x & (x - 1)) == 0;
}

template <typename T>
inline T Clamp(const T aValue, const T aMin, const T aMax) {
  static_assert(std::is_integral_v<T>,
                "Clamp accepts only integral types, so that it doesn't have"
                " to distinguish differently-signed zeroes (which users may"
                " or may not care to distinguish, likely at a perf cost) or"
                " to decide how to clamp NaN or a range with a NaN"
                " endpoint.");
  MOZ_ASSERT(aMin <= aMax);

  if (aValue <= aMin) return aMin;
  if (aValue >= aMax) return aMax;
  return aValue;
}

template <typename T>
inline uint_fast8_t CountTrailingZeroes(T aValue) {
  static_assert(sizeof(T) <= 8);
  static_assert(std::is_integral_v<T>);
  // This casts to 32-bits
  if constexpr (sizeof(T) <= 4) {
    return CountTrailingZeroes32(aValue);
  }
  // This doesn't
  if constexpr (sizeof(T) == 8) {
    return CountTrailingZeroes64(aValue);
  }
}

// Greatest Common Divisor, from
// https://en.wikipedia.org/wiki/Binary_GCD_algorithm#Implementation
template <typename T>
MOZ_ALWAYS_INLINE T GCD(T aA, T aB) {
  static_assert(std::is_integral_v<T>);

  MOZ_ASSERT(aA >= 0);
  MOZ_ASSERT(aB >= 0);

  if (aA == 0) {
    return aB;
  }
  if (aB == 0) {
    return aA;
  }

  T az = CountTrailingZeroes(aA);
  T bz = CountTrailingZeroes(aB);
  T shift = std::min<T>(az, bz);
  aA >>= az;
  aB >>= bz;

  while (aA != 0) {
    if constexpr (!std::is_signed_v<T>) {
      if (aA < aB) {
        std::swap(aA, aB);
      }
    }
    T diff = aA - aB;
    if constexpr (std::is_signed_v<T>) {
      aB = std::min<T>(aA, aB);
    }
    if constexpr (std::is_signed_v<T>) {
      aA = std::abs(diff);
    } else {
      aA = diff;
    }
    if (aA) {
      aA >>= CountTrailingZeroes(aA);
    }
  }

  return aB << shift;
}

} /* namespace mozilla */

#endif /* mozilla_MathAlgorithms_h */
