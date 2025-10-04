/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_TemporalRoundingMode_h
#define builtin_temporal_TemporalRoundingMode_h

#include "mozilla/Assertions.h"

#include <cmath>
#include <stdint.h>

#include "builtin/temporal/Crash.h"
#include "builtin/temporal/Int128.h"

namespace js::temporal {

// Overview of integer rounding modes is available at
// <https://en.wikipedia.org/wiki/Rounding#Rounding_to_integer>.
enum class TemporalRoundingMode {
  // 1. Directed rounding to an integer.

  // Round toward positive infinity.
  Ceil,

  // Round toward negative infinity.
  Floor,

  // Round toward infinity or round away from zero.
  Expand,

  // Round toward zero or round away from infinity.
  Trunc,

  // 2. Rounding to the nearest integer.

  // Round half toward positive infinity.
  HalfCeil,

  // Round half toward negative infinity.
  HalfFloor,

  // Round half toward infinity or round half away from zero.
  HalfExpand,

  // Round half toward zero or round half away from infinity.
  HalfTrunc,

  // Round half to even.
  HalfEven,
};

/**
 * NegateRoundingMode ( roundingMode )
 */
constexpr auto NegateRoundingMode(TemporalRoundingMode roundingMode) {
  // Steps 1-5.
  switch (roundingMode) {
    case TemporalRoundingMode::Ceil:
      return TemporalRoundingMode::Floor;

    case TemporalRoundingMode::Floor:
      return TemporalRoundingMode::Ceil;

    case TemporalRoundingMode::HalfCeil:
      return TemporalRoundingMode::HalfFloor;

    case TemporalRoundingMode::HalfFloor:
      return TemporalRoundingMode::HalfCeil;

    case TemporalRoundingMode::Expand:
    case TemporalRoundingMode::Trunc:
    case TemporalRoundingMode::HalfExpand:
    case TemporalRoundingMode::HalfTrunc:
    case TemporalRoundingMode::HalfEven:
      return roundingMode;
  }
  JS_CONSTEXPR_CRASH("invalid rounding mode");
}

/**
 * Adjust the rounding mode to round negative values in the same direction as
 * positive values.
 */
constexpr auto ToPositiveRoundingMode(TemporalRoundingMode roundingMode) {
  switch (roundingMode) {
    case TemporalRoundingMode::Ceil:
    case TemporalRoundingMode::Floor:
    case TemporalRoundingMode::HalfCeil:
    case TemporalRoundingMode::HalfFloor:
    case TemporalRoundingMode::HalfEven:
      // (Half-)Ceil/Floor round toward the same infinity for negative and
      // positive values, so the rounding mode doesn't need to be adjusted. The
      // same applies for half-even rounding.
      return roundingMode;

    case TemporalRoundingMode::Expand:
      // Expand rounds positive values toward +infinity, but negative values
      // toward -infinity. Adjust the rounding mode to Ceil to round negative
      // values in the same direction as positive values.
      return TemporalRoundingMode::Ceil;

    case TemporalRoundingMode::Trunc:
      // Truncation rounds positive values down toward zero, but negative values
      // up toward zero. Adjust the rounding mode to Floor to round negative
      // values in the same direction as positive values.
      return TemporalRoundingMode::Floor;

    case TemporalRoundingMode::HalfExpand:
      // Adjust the rounding mode to Half-Ceil, similar to the Expand case.
      return TemporalRoundingMode::HalfCeil;

    case TemporalRoundingMode::HalfTrunc:
      // Adjust the rounding mode to Half-Floor, similar to the Trunc case.
      return TemporalRoundingMode::HalfFloor;
  }
  JS_CONSTEXPR_CRASH("unexpected rounding mode");
}

// Temporal performs division on "mathematical values" [1] with implies using
// infinite precision. This rules out using IEE-754 floating point types like
// `double`. It also means we can't implement the algorithms from the
// specification verbatim, but instead have to translate them into equivalent
// operations.
//
// Throughout the following division functions, the divisor is required to be
// positive. This allows to simplify the implementation, because it ensures
// non-zero quotient and remainder values have the same sign as the dividend.
//
// [1] https://tc39.es/ecma262/#mathematical-value

/**
 * Compute ceiling division ⌈dividend / divisor⌉. The divisor must be a positive
 * number.
 */
constexpr int64_t CeilDiv(int64_t dividend, int64_t divisor) {
  MOZ_ASSERT(divisor > 0, "negative divisor not supported");

  // NB: Division and modulo operation are fused into a single machine code
  // instruction by the compiler.
  int64_t quotient = dividend / divisor;
  int64_t remainder = dividend % divisor;

  // Ceiling division rounds the quotient toward positive infinity. When the
  // quotient is negative, this is equivalent to rounding toward zero. See [1].
  //
  // int64_t division truncates, so rounding toward zero for negative quotients
  // is already covered. When there is a non-zero positive remainder, the
  // quotient is positive and we have to increment it by one to implement
  // rounding toward positive infinity.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (remainder > 0) {
    quotient += 1;
  }
  return quotient;
}

/**
 * Compute floor division ⌊dividend / divisor⌋. The divisor must be a positive
 * number.
 */
constexpr int64_t FloorDiv(int64_t dividend, int64_t divisor) {
  MOZ_ASSERT(divisor > 0, "negative divisor not supported");

  // NB: Division and modulo operation are fused into a single machine code
  // instruction by the compiler.
  int64_t quotient = dividend / divisor;
  int64_t remainder = dividend % divisor;

  // Floor division rounds the quotient toward negative infinity. When the
  // quotient is positive, this is equivalent to rounding toward zero. See [1].
  //
  // int64_t division truncates, so rounding toward zero for positive quotients
  // is already covered. When there is a non-zero negative remainder, the
  // quotient is negative and we have to decrement it by one to implement
  // rounding toward negative infinity.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (remainder < 0) {
    quotient -= 1;
  }
  return quotient;
}

/**
 * Compute "round toward infinity" division `dividend / divisor`. The divisor
 * must be a positive number.
 */
constexpr int64_t ExpandDiv(int64_t dividend, int64_t divisor) {
  MOZ_ASSERT(divisor > 0, "negative divisor not supported");

  // NB: Division and modulo operation are fused into a single machine code
  // instruction by the compiler.
  int64_t quotient = dividend / divisor;
  int64_t remainder = dividend % divisor;

  // "Round toward infinity" division rounds positive quotients toward positive
  // infinity and negative quotients toward negative infinity. See [1].
  //
  // When there is a non-zero positive remainder, the quotient is positive and
  // we have to increment it by one to implement rounding toward positive
  // infinity. When there is a non-zero negative remainder, the quotient is
  // negative and we have to decrement it by one to implement rounding toward
  // negative infinity.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (remainder > 0) {
    quotient += 1;
  }
  if (remainder < 0) {
    quotient -= 1;
  }
  return quotient;
}

/**
 * Compute truncating division `dividend / divisor`. The divisor must be a
 * positive number.
 */
constexpr int64_t TruncDiv(int64_t dividend, int64_t divisor) {
  MOZ_ASSERT(divisor > 0, "negative divisor not supported");

  // Truncating division rounds both positive and negative quotients toward
  // zero, cf. [1].
  //
  // int64_t division truncates, so rounding toward zero implicitly happens.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  return dividend / divisor;
}

/**
 * Compute "round half toward positive infinity" division `dividend / divisor`.
 * The divisor must be a positive number.
 */
inline int64_t HalfCeilDiv(int64_t dividend, int64_t divisor) {
  MOZ_ASSERT(divisor > 0, "negative divisor not supported");

  // NB: Division and modulo operation are fused into a single machine code
  // instruction by the compiler.
  int64_t quotient = dividend / divisor;
  int64_t remainder = dividend % divisor;

  // "Round half toward positive infinity" division rounds the quotient toward
  // positive infinity when the fractional part of the remainder is ≥0.5. When
  // the quotient is negative, this is equivalent to rounding toward zero
  // instead of toward positive infinity. See [1].
  //
  // When the remainder is a non-zero positive value, the quotient is positive,
  // too. When additionally the fractional part of the remainder is ≥0.5, we
  // have to increment the quotient by one to implement rounding toward positive
  // infinity.
  //
  // int64_t division truncates, so we implicitly round toward zero for negative
  // quotients. When the absolute value of the fractional part of the remainder
  // is >0.5, we should instead have rounded toward negative infinity, so we
  // need to decrement the quotient by one.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (remainder > 0 && uint64_t(std::abs(remainder)) * 2 >= uint64_t(divisor)) {
    quotient += 1;
  }
  if (remainder < 0 && uint64_t(std::abs(remainder)) * 2 > uint64_t(divisor)) {
    quotient -= 1;
  }
  return quotient;
}

/**
 * Compute "round half toward negative infinity" division `dividend / divisor`.
 * The divisor must be a positive number.
 */
inline int64_t HalfFloorDiv(int64_t dividend, int64_t divisor) {
  MOZ_ASSERT(divisor > 0, "negative divisor not supported");

  // NB: Division and modulo operation are fused into a single machine code
  // instruction by the compiler.
  int64_t quotient = dividend / divisor;
  int64_t remainder = dividend % divisor;

  // "Round half toward negative infinity" division rounds the quotient toward
  // negative infinity when the fractional part of the remainder is ≥0.5. When
  // the quotient is positive, this is equivalent to rounding toward zero
  // instead of toward negative infinity. See [1].
  //
  // When the remainder is a non-zero negative value, the quotient is negative,
  // too. When additionally the fractional part of the remainder is ≥0.5, we
  // have to decrement the quotient by one to implement rounding toward negative
  // infinity.
  //
  // int64_t division truncates, so we implicitly round toward zero for positive
  // quotients. When the absolute value of the fractional part of the remainder
  // is >0.5, we should instead have rounded toward positive infinity, so we
  // need to increment the quotient by one.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (remainder < 0 && uint64_t(std::abs(remainder)) * 2 >= uint64_t(divisor)) {
    quotient -= 1;
  }
  if (remainder > 0 && uint64_t(std::abs(remainder)) * 2 > uint64_t(divisor)) {
    quotient += 1;
  }
  return quotient;
}

/**
 * Compute "round half toward infinity" division `dividend / divisor`. The
 * divisor must be a positive number.
 */
inline int64_t HalfExpandDiv(int64_t dividend, int64_t divisor) {
  MOZ_ASSERT(divisor > 0, "negative divisor not supported");

  // NB: Division and modulo operation are fused into a single machine code
  // instruction by the compiler.
  int64_t quotient = dividend / divisor;
  int64_t remainder = dividend % divisor;

  // "Round half toward infinity" division rounds positive quotients whose
  // remainder has a fractional part ≥0.5 toward positive infinity. And negative
  // quotients whose remainder has a fractional part ≥0.5 toward negative
  // infinity. See [1].
  //
  // int64_t division truncates, which means it rounds toward zero, so we have
  // to increment resp. decrement the quotient when the fractional part of the
  // remainder is ≥0.5 to round toward ±infinity.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (uint64_t(std::abs(remainder)) * 2 >= uint64_t(divisor)) {
    quotient += (dividend > 0) ? 1 : -1;
  }
  return quotient;
}

/**
 * Compute "round half toward zero" division `dividend / divisor`. The divisor
 * must be a positive number.
 */
inline int64_t HalfTruncDiv(int64_t dividend, int64_t divisor) {
  MOZ_ASSERT(divisor > 0, "negative divisor not supported");

  // NB: Division and modulo operation are fused into a single machine code
  // instruction by the compiler.
  int64_t quotient = dividend / divisor;
  int64_t remainder = dividend % divisor;

  // "Round half toward zero" division rounds both positive and negative
  // quotients whose remainder has a fractional part ≤0.5 toward zero. See [1].
  //
  // int64_t division truncates, so we implicitly round toward zero. When the
  // fractional part of the remainder is >0.5, we should instead have rounded
  // toward ±infinity, so we need to increment resp. decrement the quotient by
  // one.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (uint64_t(std::abs(remainder)) * 2 > uint64_t(divisor)) {
    quotient += (dividend > 0) ? 1 : -1;
  }
  return quotient;
}

/**
 * Compute "round half to even" division `dividend / divisor`. The divisor must
 * be a positive number.
 */
inline int64_t HalfEvenDiv(int64_t dividend, int64_t divisor) {
  MOZ_ASSERT(divisor > 0, "negative divisor not supported");

  // NB: Division and modulo operation are fused into a single machine code
  // instruction by the compiler.
  int64_t quotient = dividend / divisor;
  int64_t remainder = dividend % divisor;

  // "Round half to even" division rounds both positive and negative quotients
  // to the nearest even integer. See [1].
  //
  // int64_t division truncates, so we implicitly round toward zero. When the
  // fractional part of the remainder is 0.5 and the quotient is odd or when the
  // fractional part of the remainder is >0.5, we should instead have rounded
  // toward ±infinity, so we need to increment resp. decrement the quotient by
  // one.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if ((quotient & 1) == 1 &&
      uint64_t(std::abs(remainder)) * 2 == uint64_t(divisor)) {
    quotient += (dividend > 0) ? 1 : -1;
  }
  if (uint64_t(std::abs(remainder)) * 2 > uint64_t(divisor)) {
    quotient += (dividend > 0) ? 1 : -1;
  }
  return quotient;
}

/**
 * Perform `dividend / divisor` and round the result according to the given
 * rounding mode.
 */
inline int64_t Divide(int64_t dividend, int64_t divisor,
                      TemporalRoundingMode roundingMode) {
  switch (roundingMode) {
    case TemporalRoundingMode::Ceil:
      return CeilDiv(dividend, divisor);
    case TemporalRoundingMode::Floor:
      return FloorDiv(dividend, divisor);
    case TemporalRoundingMode::Expand:
      return ExpandDiv(dividend, divisor);
    case TemporalRoundingMode::Trunc:
      return TruncDiv(dividend, divisor);
    case TemporalRoundingMode::HalfCeil:
      return HalfCeilDiv(dividend, divisor);
    case TemporalRoundingMode::HalfFloor:
      return HalfFloorDiv(dividend, divisor);
    case TemporalRoundingMode::HalfExpand:
      return HalfExpandDiv(dividend, divisor);
    case TemporalRoundingMode::HalfTrunc:
      return HalfTruncDiv(dividend, divisor);
    case TemporalRoundingMode::HalfEven:
      return HalfEvenDiv(dividend, divisor);
  }
  MOZ_CRASH("invalid rounding mode");
}

/**
 * Compute ceiling division ⌈dividend / divisor⌉. The divisor must be a positive
 * number.
 */
inline Int128 CeilDiv(const Int128& dividend, const Int128& divisor) {
  MOZ_ASSERT(divisor > Int128{0}, "negative divisor not supported");

  auto [quotient, remainder] = dividend.divrem(divisor);

  // Ceiling division rounds the quotient toward positive infinity. When the
  // quotient is negative, this is equivalent to rounding toward zero. See [1].
  //
  // Int128 division truncates, so rounding toward zero for negative quotients
  // is already covered. When there is a non-zero positive remainder, the
  // quotient is positive and we have to increment it by one to implement
  // rounding toward positive infinity.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (remainder > Int128{0}) {
    quotient += Int128{1};
  }
  return quotient;
}

/**
 * Compute floor division ⌊dividend / divisor⌋. The divisor must be a positive
 * number.
 */
inline Int128 FloorDiv(const Int128& dividend, const Int128& divisor) {
  MOZ_ASSERT(divisor > Int128{0}, "negative divisor not supported");

  auto [quotient, remainder] = dividend.divrem(divisor);

  // Floor division rounds the quotient toward negative infinity. When the
  // quotient is positive, this is equivalent to rounding toward zero. See [1].
  //
  // Int128 division truncates, so rounding toward zero for positive quotients
  // is already covered. When there is a non-zero negative remainder, the
  // quotient is negative and we have to decrement it by one to implement
  // rounding toward negative infinity.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (remainder < Int128{0}) {
    quotient -= Int128{1};
  }
  return quotient;
}

/**
 * Compute "round toward infinity" division `dividend / divisor`. The divisor
 * must be a positive number.
 */
inline Int128 ExpandDiv(const Int128& dividend, const Int128& divisor) {
  MOZ_ASSERT(divisor > Int128{0}, "negative divisor not supported");

  auto [quotient, remainder] = dividend.divrem(divisor);

  // "Round toward infinity" division rounds positive quotients toward positive
  // infinity and negative quotients toward negative infinity. See [1].
  //
  // When there is a non-zero positive remainder, the quotient is positive and
  // we have to increment it by one to implement rounding toward positive
  // infinity. When there is a non-zero negative remainder, the quotient is
  // negative and we have to decrement it by one to implement rounding toward
  // negative infinity.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (remainder > Int128{0}) {
    quotient += Int128{1};
  }
  if (remainder < Int128{0}) {
    quotient -= Int128{1};
  }
  return quotient;
}

/**
 * Compute truncating division `dividend / divisor`. The divisor must be a
 * positive number.
 */
inline Int128 TruncDiv(const Int128& dividend, const Int128& divisor) {
  MOZ_ASSERT(divisor > Int128{0}, "negative divisor not supported");

  // Truncating division rounds both positive and negative quotients toward
  // zero, cf. [1].
  //
  // Int128 division truncates, so rounding toward zero implicitly happens.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  return dividend / divisor;
}

/**
 * Compute "round half toward positive infinity" division `dividend / divisor`.
 * The divisor must be a positive number.
 */
inline Int128 HalfCeilDiv(const Int128& dividend, const Int128& divisor) {
  MOZ_ASSERT(divisor > Int128{0}, "negative divisor not supported");

  auto [quotient, remainder] = dividend.divrem(divisor);

  // "Round half toward positive infinity" division rounds the quotient toward
  // positive infinity when the fractional part of the remainder is ≥0.5. When
  // the quotient is negative, this is equivalent to rounding toward zero
  // instead of toward positive infinity. See [1].
  //
  // When the remainder is a non-zero positive value, the quotient is positive,
  // too. When additionally the fractional part of the remainder is ≥0.5, we
  // have to increment the quotient by one to implement rounding toward positive
  // infinity.
  //
  // Int128 division truncates, so we implicitly round toward zero for negative
  // quotients. When the absolute value of the fractional part of the remainder
  // is >0.5, we should instead have rounded toward negative infinity, so we
  // need to decrement the quotient by one.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (remainder > Int128{0} &&
      Uint128(remainder.abs()) * Uint128{2} >= static_cast<Uint128>(divisor)) {
    quotient += Int128{1};
  }
  if (remainder < Int128{0} &&
      Uint128(remainder.abs()) * Uint128{2} > static_cast<Uint128>(divisor)) {
    quotient -= Int128{1};
  }
  return quotient;
}

/**
 * Compute "round half toward negative infinity" division `dividend / divisor`.
 * The divisor must be a positive number.
 */
inline Int128 HalfFloorDiv(const Int128& dividend, const Int128& divisor) {
  MOZ_ASSERT(divisor > Int128{0}, "negative divisor not supported");

  auto [quotient, remainder] = dividend.divrem(divisor);

  // "Round half toward negative infinity" division rounds the quotient toward
  // negative infinity when the fractional part of the remainder is ≥0.5. When
  // the quotient is positive, this is equivalent to rounding toward zero
  // instead of toward negative infinity. See [1].
  //
  // When the remainder is a non-zero negative value, the quotient is negative,
  // too. When additionally the fractional part of the remainder is ≥0.5, we
  // have to decrement the quotient by one to implement rounding toward negative
  // infinity.
  //
  // Int128 division truncates, so we implicitly round toward zero for positive
  // quotients. When the absolute value of the fractional part of the remainder
  // is >0.5, we should instead have rounded toward positive infinity, so we
  // need to increment the quotient by one.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (remainder < Int128{0} &&
      Uint128(remainder.abs()) * Uint128{2} >= static_cast<Uint128>(divisor)) {
    quotient -= Int128{1};
  }
  if (remainder > Int128{0} &&
      Uint128(remainder.abs()) * Uint128{2} > static_cast<Uint128>(divisor)) {
    quotient += Int128{1};
  }
  return quotient;
}

/**
 * Compute "round half toward infinity" division `dividend / divisor`. The
 * divisor must be a positive number.
 */
inline Int128 HalfExpandDiv(const Int128& dividend, const Int128& divisor) {
  MOZ_ASSERT(divisor > Int128{0}, "negative divisor not supported");

  auto [quotient, remainder] = dividend.divrem(divisor);

  // "Round half toward infinity" division rounds positive quotients whose
  // remainder has a fractional part ≥0.5 toward positive infinity. And negative
  // quotients whose remainder has a fractional part ≥0.5 toward negative
  // infinity. See [1].
  //
  // Int128 division truncates, which means it rounds toward zero, so we have
  // to increment resp. decrement the quotient when the fractional part of the
  // remainder is ≥0.5 to round toward ±infinity.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (Uint128(remainder.abs()) * Uint128{2} >= static_cast<Uint128>(divisor)) {
    quotient += (dividend > Int128{0}) ? Int128{1} : Int128{-1};
  }
  return quotient;
}

/**
 * Compute "round half toward zero" division `dividend / divisor`. The divisor
 * must be a positive number.
 */
inline Int128 HalfTruncDiv(const Int128& dividend, const Int128& divisor) {
  MOZ_ASSERT(divisor > Int128{0}, "negative divisor not supported");

  auto [quotient, remainder] = dividend.divrem(divisor);

  // "Round half toward zero" division rounds both positive and negative
  // quotients whose remainder has a fractional part ≤0.5 toward zero. See [1].
  //
  // Int128 division truncates, so we implicitly round toward zero. When the
  // fractional part of the remainder is >0.5, we should instead have rounded
  // toward ±infinity, so we need to increment resp. decrement the quotient by
  // one.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if (Uint128(remainder.abs()) * Uint128{2} > static_cast<Uint128>(divisor)) {
    quotient += (dividend > Int128{0}) ? Int128{1} : Int128{-1};
  }
  return quotient;
}

/**
 * Compute "round half to even" division `dividend / divisor`. The divisor must
 * be a positive number.
 */
inline Int128 HalfEvenDiv(const Int128& dividend, const Int128& divisor) {
  MOZ_ASSERT(divisor > Int128{0}, "negative divisor not supported");

  auto [quotient, remainder] = dividend.divrem(divisor);

  // "Round half to even" division rounds both positive and negative quotients
  // to the nearest even integer. See [1].
  //
  // Int128 division truncates, so we implicitly round toward zero. When the
  // fractional part of the remainder is 0.5 and the quotient is odd or when the
  // fractional part of the remainder is >0.5, we should instead have rounded
  // toward ±infinity, so we need to increment resp. decrement the quotient by
  // one.
  //
  // [1]
  // https://tc39.es/proposal-temporal/#table-temporal-unsigned-rounding-modes
  if ((quotient & Int128{1}) == Int128{1} &&
      Uint128(remainder.abs()) * Uint128{2} == static_cast<Uint128>(divisor)) {
    quotient += (dividend > Int128{0}) ? Int128{1} : Int128{-1};
  }
  if (Uint128(remainder.abs()) * Uint128{2} > static_cast<Uint128>(divisor)) {
    quotient += (dividend > Int128{0}) ? Int128{1} : Int128{-1};
  }
  return quotient;
}

/**
 * Perform `dividend / divisor` and round the result according to the given
 * rounding mode.
 */
inline Int128 Divide(const Int128& dividend, const Int128& divisor,
                     TemporalRoundingMode roundingMode) {
  switch (roundingMode) {
    case TemporalRoundingMode::Ceil:
      return CeilDiv(dividend, divisor);
    case TemporalRoundingMode::Floor:
      return FloorDiv(dividend, divisor);
    case TemporalRoundingMode::Expand:
      return ExpandDiv(dividend, divisor);
    case TemporalRoundingMode::Trunc:
      return TruncDiv(dividend, divisor);
    case TemporalRoundingMode::HalfCeil:
      return HalfCeilDiv(dividend, divisor);
    case TemporalRoundingMode::HalfFloor:
      return HalfFloorDiv(dividend, divisor);
    case TemporalRoundingMode::HalfExpand:
      return HalfExpandDiv(dividend, divisor);
    case TemporalRoundingMode::HalfTrunc:
      return HalfTruncDiv(dividend, divisor);
    case TemporalRoundingMode::HalfEven:
      return HalfEvenDiv(dividend, divisor);
  }
  MOZ_CRASH("invalid rounding mode");
}

} /* namespace js::temporal */

#endif /* builtin_temporal_TemporalRoundingMode_h */
