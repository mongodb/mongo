/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_Int96_h
#define builtin_temporal_Int96_h

#include "mozilla/Assertions.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"

#include <array>
#include <climits>
#include <stddef.h>
#include <stdint.h>
#include <utility>

namespace js::temporal {

/**
 * 96-bit integer with explicit sign. Supports integers in the range
 * [-(2**96 - 1), 2**96 - 1].
 */
class Int96 final {
 public:
  using Digit = uint32_t;
  using TwoDigit = uint64_t;

  // The 96-bit integer is stored as three separate 32-bit integers.
  using Digits = std::array<Digit, 3>;

 private:
  // Unsigned number in the range [0, 0xffff'ffff'ffff'ffff'ffff'ffff].
  //
  // The least significant digit is stored at index 0. The most significant
  // digit is stored at index 2.
  Digits digits = {};

  // Explicit negative sign.
  bool negative = false;

 public:
  // Default constructor initializes to zero.
  constexpr Int96() = default;

  // Create from an 64-bit integer.
  constexpr explicit Int96(int64_t value) : negative(value < 0) {
    // NB: Not std::abs, because std::abs(INT64_MIN) is undefined behavior.
    uint64_t abs = mozilla::Abs(value);
    digits[1] = uint32_t(abs >> 32);
    digits[0] = uint32_t(abs);
  }

  constexpr Int96(Digits digits, bool negative)
      : digits(digits), negative(negative) {
    // Assert zero is non-negative.
    MOZ_ASSERT_IF((digits[0] | digits[1] | digits[2]) == 0, !negative);
  }

  constexpr bool operator==(const Int96& other) const {
    return digits[0] == other.digits[0] && digits[1] == other.digits[1] &&
           digits[2] == other.digits[2] && negative == other.negative;
  }

  constexpr bool operator<(const Int96& other) const {
    if (negative != other.negative) {
      return negative;
    }
    for (size_t i = digits.size(); i != 0; --i) {
      Digit x = digits[i - 1];
      Digit y = other.digits[i - 1];
      if (x != y) {
        return negative ? x > y : x < y;
      }
    }
    return false;
  }

  // Other operators are implemented in terms of operator== and operator<.
  constexpr bool operator!=(const Int96& other) const {
    return !(*this == other);
  }
  constexpr bool operator>(const Int96& other) const { return other < *this; }
  constexpr bool operator<=(const Int96& other) const {
    return !(other < *this);
  }
  constexpr bool operator>=(const Int96& other) const {
    return !(*this < other);
  }

  /**
   * Multiply this integer with an multiplier. Overflow is not supported.
   */
  constexpr Int96& operator*=(Digit multiplier) {
    Digit carry = 0;
    for (auto& digit : digits) {
      TwoDigit d = digit;
      d *= multiplier;
      d += carry;

      digit = Digit(d);
      carry = Digit(d >> 32);
    }
    MOZ_ASSERT(carry == 0, "unsupported overflow");

    return *this;
  }

  /**
   * Multiply this integer with an multiplier. Overflow is not supported.
   */
  constexpr Int96 operator*(Digit multiplier) const {
    auto result = *this;
    result *= multiplier;
    return result;
  }

  /**
   * Divide this integer by the divisor using Euclidean division. The divisor
   * must be smaller than the most significant digit of the integer. Returns the
   * quotient and the remainder.
   */
  constexpr std::pair<int64_t, int32_t> operator/(Digit divisor) const {
    MOZ_ASSERT(digits[2] < divisor, "unsupported divisor");

    Digit quotient[2] = {};
    Digit remainder = digits[2];
    for (int32_t i = 1; i >= 0; i--) {
      TwoDigit n = (TwoDigit(remainder) << 32) | digits[i];
      quotient[i] = n / divisor;
      remainder = n % divisor;
    }

    int64_t result = int64_t((TwoDigit(quotient[1]) << 32) | quotient[0]);
    if (negative) {
      result *= -1;
      if (remainder != 0) {
        result -= 1;
        remainder = divisor - remainder;
      }
    }
    return {result, int32_t(remainder)};
  }

  /**
   * Return the absolute value of this integer.
   */
  constexpr Int96 abs() const { return {digits, false}; }

  /**
   * Return Some(Int96) if the integer value fits into a 96-bit integer.
   * Otherwise returns Nothing().
   */
  static mozilla::Maybe<Int96> fromInteger(double value);
};

} /* namespace js::temporal */

#endif /* builtin_temporal_Int96_h */
