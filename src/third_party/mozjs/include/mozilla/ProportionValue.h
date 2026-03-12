/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProportionValue_h
#define ProportionValue_h

#include "mozilla/Attributes.h"

#include <algorithm>
#include <limits>

namespace mozilla {

// Class storing a proportion value between 0 and 1, effectively 0% to 100%.
// The public interface deals with doubles, but internally the value is encoded
// in an integral type, so arithmetic operations are fast.
// It also supports an invalid value: Use MakeInvalid() to construct, it infects
// any operation, and gets converted to a signaling NaN.
class ProportionValue {
 public:
  using UnderlyingType = uint32_t;

  // Default-construct at 0%.
  constexpr ProportionValue()
      // This `noexcept` is necessary to avoid a build error when encapsulating
      // `ProportionValue` in `std::Atomic`:
      // "use of deleted function
      // 'constexpr std::atomic<mozilla::ProportionValue>::atomic()"
      // because the default `std::atomic<T>::atomic()` constructor is marked:
      // `noexcept(std::is_nothrow_default_constructible_v<T>)`
      // and therefore this default constructor here must be explicitly marked
      // `noexcept` as well.
      noexcept
      : mIntegralValue(0u) {}

  // Construct a ProportionValue with the given value, clamped to 0..1.
  // Note that it's constexpr, so construction from literal numbers should incur
  // no runtime costs.
  // If `aValue` is NaN, behavior is undefined! Use `MakeInvalid()` instead.
  constexpr explicit ProportionValue(double aValue)
      : mIntegralValue(UnderlyingType(std::clamp(aValue, 0.0, 1.0) * scMaxD)) {}

  [[nodiscard]] static constexpr ProportionValue MakeInvalid() {
    return ProportionValue(scInvalidU, Internal{});
  }

  [[nodiscard]] constexpr double ToDouble() const {
    return IsInvalid() ? std::numeric_limits<double>::signaling_NaN()
                       : (double(mIntegralValue) * scInvMaxD);
  }

  // Retrieve the underlying integral value, for storage or testing purposes.
  [[nodiscard]] constexpr UnderlyingType ToUnderlyingType() const {
    return mIntegralValue;
  };

  // Re-construct a ProportionValue from an underlying integral value.
  [[nodiscard]] static constexpr ProportionValue FromUnderlyingType(
      UnderlyingType aUnderlyingType) {
    return ProportionValue(
        (aUnderlyingType <= scMaxU) ? aUnderlyingType : scInvalidU, Internal{});
  }

  [[nodiscard]] constexpr bool IsExactlyZero() const {
    return mIntegralValue == 0u;
  }

  [[nodiscard]] constexpr bool IsExactlyOne() const {
    return mIntegralValue == scMaxU;
  }

  [[nodiscard]] constexpr bool IsValid() const {
    // Compare to the maximum value, not just exactly scInvalidU, to catch any
    // kind of invalid state.
    return mIntegralValue <= scMaxU;
  }
  [[nodiscard]] constexpr bool IsInvalid() const {
    // Compare to the maximum value, not just exactly scInvalidU, to catch any
    // kind of invalid state.
    return mIntegralValue > scMaxU;
  }

  // Strict comparisons based on the underlying integral value. Use
  // `CompareWithin` instead to make fuzzy comparisons.
  // `ProportionValue::MakeInvalid()`s are equal, and greater than anything
  // else; Best to avoid comparisons, and first use IsInvalid() instead.
#define OPERATOR_COMPARISON(CMP)                                  \
  [[nodiscard]] constexpr friend bool operator CMP(               \
      const ProportionValue& aLHS, const ProportionValue& aRHS) { \
    return aLHS.mIntegralValue CMP aRHS.mIntegralValue;           \
  }
  OPERATOR_COMPARISON(==)
  OPERATOR_COMPARISON(!=)
  OPERATOR_COMPARISON(<)
  OPERATOR_COMPARISON(<=)
  OPERATOR_COMPARISON(>)
  OPERATOR_COMPARISON(>=)
#undef OPERATOR_COMPARISON

  // Arithmetic operations + - *, all working on the underlying integral values
  // (i.e, no expensive floating-point operations are used), and always clamping
  // to 0..1 range. Invalid values are poisonous.

  [[nodiscard]] constexpr ProportionValue operator+(
      ProportionValue aRHS) const {
    return ProportionValue(
        (IsInvalid() || aRHS.IsInvalid())
            ? scInvalidU
            // Adding fixed-point values keep the same scale, so there is no
            // adjustment needed for that. [0,1]+[0,1]=[0,2], so we only need to
            // ensure that the result is capped at max 1, aka scMaxU:
            // a+b<=max <=> b<=max-a, so b is at maximum max-a.
            : (mIntegralValue +
               std::min(aRHS.mIntegralValue, scMaxU - mIntegralValue)),
        Internal{});
  }

  [[nodiscard]] constexpr ProportionValue operator-(
      ProportionValue aRHS) const {
    return ProportionValue(
        (IsInvalid() || aRHS.IsInvalid())
            ? scInvalidU
            // Subtracting fixed-point values keep the same scale, so there is
            // no adjustment needed for that. [0,1]-[0,1]=[-1,1], so we only
            // need to ensure that the value is positive:
            // a-b>=0 <=> b<=a, so b is at maximum a.
            : (mIntegralValue - std::min(aRHS.mIntegralValue, mIntegralValue)),
        Internal{});
  }

  [[nodiscard]] constexpr ProportionValue operator*(
      ProportionValue aRHS) const {
    // Type to hold the full result of multiplying two maximum numbers.
    using DoublePrecisionType = uint64_t;
    static_assert(sizeof(DoublePrecisionType) >= 2 * sizeof(UnderlyingType));
    return ProportionValue(
        (IsInvalid() || aRHS.IsInvalid())
            ? scInvalidU
            // Multiplying fixed-point values doubles the scale (2^31 -> 2^62),
            // so we need to adjust the result by dividing it by one scale
            // (which is optimized into a binary right-shift).
            : (UnderlyingType((DoublePrecisionType(mIntegralValue) *
                               DoublePrecisionType(aRHS.mIntegralValue)) /
                              DoublePrecisionType(scMaxU))),
        Internal{});
  }

  // Explicitly forbid divisions, they make little sense, and would almost
  // always return a clamped 100% (E.g.: 50% / 10% = 0.5 / 0.1 = 5 = 500%).
  [[nodiscard]] constexpr ProportionValue operator/(
      ProportionValue aRHS) const = delete;

  // Division by a positive integer value, useful to split an interval in equal
  // parts (with maybe some spare space at the end, because it is rounded down).
  // Division by 0 produces an invalid value.
  [[nodiscard]] constexpr ProportionValue operator/(uint32_t aDivisor) const {
    return ProportionValue((IsInvalid() || aDivisor == 0u)
                               ? scInvalidU
                               : (mIntegralValue / aDivisor),
                           Internal{});
  }

  // Multiplication by a positive integer value, useful as inverse of the
  // integer division above. But it may be lossy because the division is rounded
  // down, therefore: PV - u < (PV / u) * u <= PV.
  // Clamped to 100% max.
  [[nodiscard]] constexpr ProportionValue operator*(
      uint32_t aMultiplier) const {
    return ProportionValue(IsInvalid()
                               ? scInvalidU
                               : ((aMultiplier > scMaxU / mIntegralValue)
                                      ? scMaxU
                                      : (mIntegralValue * aMultiplier)),
                           Internal{});
  }

 private:
  // Tagged constructor for internal construction from the UnderlyingType, so
  // that it is never ambiguously considered in constructions from one number.
  struct Internal {};
  constexpr ProportionValue(UnderlyingType aIntegralValue, Internal)
      : mIntegralValue(aIntegralValue) {}

  // Use all but 1 bit for the fractional part.
  // Valid values can go from 0b0 (0%) up to 0b1000...00 (scMaxU aka 100%).
  static constexpr unsigned scFractionalBits = sizeof(UnderlyingType) * 8 - 1;
  // Maximum value corresponding to 1.0 or 100%.
  static constexpr UnderlyingType scMaxU = UnderlyingType(1u)
                                           << scFractionalBits;
  // This maximum value corresponding to 1.0 can also be seen as the scaling
  // factor from any [0,1] `double` value to the internal integral value.
  static constexpr double scMaxD = double(scMaxU);
  // The inverse can be used to convert the internal value back to [0,1].
  static constexpr double scInvMaxD = 1.0 / scMaxD;

  // Special value outside [0,max], used to construct invalid values.
  static constexpr UnderlyingType scInvalidU = ~UnderlyingType(0u);

  // Internal integral value, guaranteed to always be <= scMaxU, or scInvalidU.
  // This is effectively a fixed-point value using 1 bit for the integer part
  // and 31 bits for the fractional part.
  // It is roughly equal to the `double` value [0,1] multiplied by scMaxD.
  UnderlyingType mIntegralValue;
};

namespace literals {
inline namespace ProportionValue_literals {

// User-defined literal for integer percentages, e.g.: `10_pc`, `100_pc`
// (equivalent to `ProportionValue{0.1}` and `ProportionValue{1.0}`).
// Clamped to [0, 100]_pc.
[[nodiscard]] constexpr ProportionValue operator""_pc(
    unsigned long long int aPercentage) {
  return ProportionValue{
      double(std::clamp<unsigned long long int>(aPercentage, 0u, 100u)) /
      100.0};
}

// User-defined literal for non-integer percentages, e.g.: `12.3_pc`, `100.0_pc`
// (equivalent to `ProportionValue{0.123}` and `ProportionValue{1.0}`).
// Clamped to [0.0, 100.0]_pc.
[[nodiscard]] constexpr ProportionValue operator""_pc(long double aPercentage) {
  return ProportionValue{
      double(std::clamp<long double>(aPercentage, 0.0, 100.0)) / 100.0};
}

}  // namespace ProportionValue_literals
}  // namespace literals

}  // namespace mozilla

#endif  // ProportionValue_h
