/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// PowerOfTwo is a value type that always hold a power of 2.
// It has the same size as their underlying unsigned type, but offer the
// guarantee of being a power of 2, which permits some optimizations when
// involved in modulo operations (using masking instead of actual modulo).
//
// PowerOfTwoMask contains a mask corresponding to a power of 2.
// E.g., 2^8 is 256 or 0x100, the corresponding mask is 2^8-1 or 255 or 0xFF.
// It should be used instead of PowerOfTwo in situations where most operations
// would be modulo, this saves having to recompute the mask from the stored
// power of 2.
//
// One common use would be for ring-buffer containers with a power-of-2 size,
// where an index is usually converted to an in-buffer offset by `i % size`.
// Instead, the container could store a PowerOfTwo or PowerOfTwoMask, and do
// `i % p2` or `i & p2m`, which is more efficient than for arbitrary sizes.
//
// Shortcuts for common 32- and 64-bit values: PowerOfTwo32, etc.
//
// To create constexpr constants, use MakePowerOfTwo<Type, Value>(), etc.

#ifndef PowerOfTwo_h
#define PowerOfTwo_h

#include "mozilla/MathAlgorithms.h"

#include <limits>

namespace mozilla {

// Compute the smallest power of 2 greater than or equal to aInput, except if
// that would overflow in which case the highest possible power of 2 if chosen.
// 0->1, 1->1, 2->2, 3->4, ... 2^31->2^31, 2^31+1->2^31 (for uint32_t), etc.
template <typename T>
T FriendlyRoundUpPow2(T aInput) {
  // This is the same code as `RoundUpPow2()`, except we handle any type (that
  // CeilingLog2 supports) and allow the greater-than-max-power case.
  constexpr T max = T(1) << (sizeof(T) * CHAR_BIT - 1);
  if (aInput >= max) {
    return max;
  }
  return T(1) << CeilingLog2(aInput);
}

namespace detail {
// Same function name `CountLeadingZeroes` with uint32_t and uint64_t overloads.
inline uint_fast8_t CountLeadingZeroes(uint32_t aValue) {
  MOZ_ASSERT(aValue != 0);
  return detail::CountLeadingZeroes32(aValue);
}
inline uint_fast8_t CountLeadingZeroes(uint64_t aValue) {
  MOZ_ASSERT(aValue != 0);
  return detail::CountLeadingZeroes64(aValue);
}
// Refuse anything else.
template <typename T>
inline uint_fast8_t CountLeadingZeroes(T aValue) = delete;
}  // namespace detail

// Compute the smallest 2^N-1 mask where aInput can fit.
// I.e., `x & mask == x`, but `x & (mask >> 1) != x`.
// Or looking at binary, we want a mask with as many leading zeroes as the
// input, by right-shifting a full mask: (8-bit examples)
// input:          00000000    00000001   00000010  00010110  01111111 10000000
// N leading 0s:   ^^^^^^^^ 8  ^^^^^^^ 7  ^^^^^^ 6  ^^^ 3     ^ 1      0
// full mask:      11111111    11111111   11111111  11111111  11111111 11111111
// full mask >> N: 00000000    00000001   00000011  00011111  01111111 11111111
template <typename T>
T RoundUpPow2Mask(T aInput) {
  // Special case, as CountLeadingZeroes(0) is undefined. (And even if that was
  // defined, shifting by the full type size is also undefined!)
  if (aInput == 0) {
    return 0;
  }
  return T(-1) >> detail::CountLeadingZeroes(aInput);
}

template <typename T>
class PowerOfTwoMask;

template <typename T, T Mask>
constexpr PowerOfTwoMask<T> MakePowerOfTwoMask();

template <typename T>
class PowerOfTwo;

template <typename T, T Value>
constexpr PowerOfTwo<T> MakePowerOfTwo();

// PowerOfTwoMask will always contain a mask for a power of 2, which is useful
// for power-of-2 modulo operations (e.g., to keep an index inside a power-of-2
// container).
// Use this instead of PowerOfTwo if masking is the primary use of the value.
//
// Note that this class can store a "full" mask where all bits are set, so it
// works for mask corresponding to the power of 2 that would overflow `T`
// (e.g., 2^32 for uint32_t gives a mask of 2^32-1, which fits in a uint32_t).
// For this reason there is no API that computes the power of 2 corresponding to
// the mask; But this can be done explicitly with `MaskValue() + 1`, which may
// be useful for computing things like distance-to-the-end by doing
// `MaskValue() + 1 - offset`, which works fine with unsigned number types.
template <typename T>
class PowerOfTwoMask {
  static_assert(!std::numeric_limits<T>::is_signed,
                "PowerOfTwoMask must use an unsigned type");

 public:
  // Construct a power of 2 mask where the given value can fit.
  // Cannot be constexpr because of `RoundUpPow2Mask()`.
  explicit PowerOfTwoMask(T aInput) : mMask(RoundUpPow2Mask(aInput)) {}

  // Compute the mask corresponding to a PowerOfTwo.
  // This saves having to compute the nearest 2^N-1.
  // Not a conversion constructor, as that could be ambiguous whether we'd want
  // the mask corresponding to the power of 2 (2^N -> 2^N-1), or the mask that
  // can *contain* the PowerOfTwo value (2^N -> 2^(N+1)-1).
  // Note: Not offering reverse PowerOfTwoMark-to-PowerOfTwo conversion, because
  // that could result in an unexpected 0 result for the largest possible mask.
  template <typename U>
  static constexpr PowerOfTwoMask<U> MaskForPowerOfTwo(
      const PowerOfTwo<U>& aP2) {
    return PowerOfTwoMask(aP2);
  }

  // Allow smaller unsigned types as input.
  // Bigger or signed types must be explicitly converted by the caller.
  template <typename U>
  explicit constexpr PowerOfTwoMask(U aInput)
      : mMask(RoundUpPow2Mask(static_cast<T>(aInput))) {
    static_assert(!std::numeric_limits<T>::is_signed,
                  "PowerOfTwoMask does not accept signed types");
    static_assert(sizeof(U) <= sizeof(T),
                  "PowerOfTwoMask does not accept bigger types");
  }

  constexpr T MaskValue() const { return mMask; }

  // `x & aPowerOfTwoMask` just works.
  template <typename U>
  friend U operator&(U aNumber, PowerOfTwoMask aP2M) {
    return static_cast<U>(aNumber & aP2M.MaskValue());
  }

  // `aPowerOfTwoMask & x` just works.
  template <typename U>
  friend constexpr U operator&(PowerOfTwoMask aP2M, U aNumber) {
    return static_cast<U>(aP2M.MaskValue() & aNumber);
  }

  // `x % aPowerOfTwoMask(2^N-1)` is equivalent to `x % 2^N` but is more
  // optimal by doing `x & (2^N-1)`.
  // Useful for templated code doing modulo with a template argument type.
  template <typename U>
  friend constexpr U operator%(U aNumerator, PowerOfTwoMask aDenominator) {
    return aNumerator & aDenominator.MaskValue();
  }

  constexpr bool operator==(const PowerOfTwoMask& aRhs) const {
    return mMask == aRhs.mMask;
  }
  constexpr bool operator!=(const PowerOfTwoMask& aRhs) const {
    return mMask != aRhs.mMask;
  }

 private:
  // Trust `PowerOfTwo` to call the private Trusted constructor below.
  friend class PowerOfTwo<T>;

  // Trust `MakePowerOfTwoMask()` to call the private Trusted constructor below.
  template <typename U, U Mask>
  friend constexpr PowerOfTwoMask<U> MakePowerOfTwoMask();

  struct Trusted {
    T mMask;
  };
  // Construct the mask corresponding to a PowerOfTwo.
  // This saves having to compute the nearest 2^N-1.
  // Note: Not a public PowerOfTwo->PowerOfTwoMask conversion constructor, as
  // that could be ambiguous whether we'd want the mask corresponding to the
  // power of 2 (2^N -> 2^N-1), or the mask that can *contain* the PowerOfTwo
  // value (2^N -> 2^(N+1)-1).
  explicit constexpr PowerOfTwoMask(const Trusted& aP2) : mMask(aP2.mMask) {}

  T mMask = 0;
};

// Make a PowerOfTwoMask constant, statically-checked.
template <typename T, T Mask>
constexpr PowerOfTwoMask<T> MakePowerOfTwoMask() {
  static_assert(Mask == T(-1) || IsPowerOfTwo(Mask + 1),
                "MakePowerOfTwoMask<T, Mask>: Mask must be 2^N-1");
  using Trusted = typename PowerOfTwoMask<T>::Trusted;
  return PowerOfTwoMask<T>(Trusted{Mask});
}

// PowerOfTwo will always contain a power of 2.
template <typename T>
class PowerOfTwo {
  static_assert(!std::numeric_limits<T>::is_signed,
                "PowerOfTwo must use an unsigned type");

 public:
  // Construct a power of 2 that can fit the given value, or the highest power
  // of 2 possible.
  // Caller should explicitly check/assert `Value() <= aInput` if they want to.
  // Cannot be constexpr because of `FriendlyRoundUpPow2()`.
  explicit PowerOfTwo(T aInput) : mValue(FriendlyRoundUpPow2(aInput)) {}

  // Allow smaller unsigned types as input.
  // Bigger or signed types must be explicitly converted by the caller.
  template <typename U>
  explicit PowerOfTwo(U aInput)
      : mValue(FriendlyRoundUpPow2(static_cast<T>(aInput))) {
    static_assert(!std::numeric_limits<T>::is_signed,
                  "PowerOfTwo does not accept signed types");
    static_assert(sizeof(U) <= sizeof(T),
                  "PowerOfTwo does not accept bigger types");
  }

  constexpr T Value() const { return mValue; }

  // Binary mask corresponding to the power of 2, useful for modulo.
  // E.g., `x & powerOfTwo(y).Mask()` == `x % powerOfTwo(y)`.
  // Consider PowerOfTwoMask class instead of PowerOfTwo if masking is the
  // primary use case.
  constexpr T MaskValue() const { return mValue - 1; }

  // PowerOfTwoMask corresponding to this power of 2, useful for modulo.
  constexpr PowerOfTwoMask<T> Mask() const {
    using Trusted = typename PowerOfTwoMask<T>::Trusted;
    return PowerOfTwoMask<T>(Trusted{MaskValue()});
  }

  // `x % aPowerOfTwo` works optimally.
  // Useful for templated code doing modulo with a template argument type.
  // Use PowerOfTwoMask class instead if masking is the primary use case.
  template <typename U>
  friend constexpr U operator%(U aNumerator, PowerOfTwo aDenominator) {
    return aNumerator & aDenominator.MaskValue();
  }

  constexpr bool operator==(const PowerOfTwo& aRhs) const {
    return mValue == aRhs.mValue;
  }
  constexpr bool operator!=(const PowerOfTwo& aRhs) const {
    return mValue != aRhs.mValue;
  }
  constexpr bool operator<(const PowerOfTwo& aRhs) const {
    return mValue < aRhs.mValue;
  }
  constexpr bool operator<=(const PowerOfTwo& aRhs) const {
    return mValue <= aRhs.mValue;
  }
  constexpr bool operator>(const PowerOfTwo& aRhs) const {
    return mValue > aRhs.mValue;
  }
  constexpr bool operator>=(const PowerOfTwo& aRhs) const {
    return mValue >= aRhs.mValue;
  }

 private:
  // Trust `MakePowerOfTwo()` to call the private Trusted constructor below.
  template <typename U, U Value>
  friend constexpr PowerOfTwo<U> MakePowerOfTwo();

  struct Trusted {
    T mValue;
  };
  // Construct a PowerOfTwo with the given trusted value.
  // This saves having to compute the nearest 2^N.
  // Note: Not offering PowerOfTwoMark-to-PowerOfTwo conversion, because that
  // could result in an unexpected 0 result for the largest possible mask.
  explicit constexpr PowerOfTwo(const Trusted& aP2) : mValue(aP2.mValue) {}

  // The smallest power of 2 is 2^0 == 1.
  T mValue = 1;
};

// Make a PowerOfTwo constant, statically-checked.
template <typename T, T Value>
constexpr PowerOfTwo<T> MakePowerOfTwo() {
  static_assert(IsPowerOfTwo(Value),
                "MakePowerOfTwo<T, Value>: Value must be 2^N");
  using Trusted = typename PowerOfTwo<T>::Trusted;
  return PowerOfTwo<T>(Trusted{Value});
}

// Shortcuts for the most common types and functions.

using PowerOfTwoMask32 = PowerOfTwoMask<uint32_t>;
using PowerOfTwo32 = PowerOfTwo<uint32_t>;
using PowerOfTwoMask64 = PowerOfTwoMask<uint64_t>;
using PowerOfTwo64 = PowerOfTwo<uint64_t>;

template <uint32_t Mask>
constexpr PowerOfTwoMask32 MakePowerOfTwoMask32() {
  return MakePowerOfTwoMask<uint32_t, Mask>();
}

template <uint32_t Value>
constexpr PowerOfTwo32 MakePowerOfTwo32() {
  return MakePowerOfTwo<uint32_t, Value>();
}

template <uint64_t Mask>
constexpr PowerOfTwoMask64 MakePowerOfTwoMask64() {
  return MakePowerOfTwoMask<uint64_t, Mask>();
}

template <uint64_t Value>
constexpr PowerOfTwo64 MakePowerOfTwo64() {
  return MakePowerOfTwo<uint64_t, Value>();
}

}  // namespace mozilla

#endif  // PowerOfTwo_h
