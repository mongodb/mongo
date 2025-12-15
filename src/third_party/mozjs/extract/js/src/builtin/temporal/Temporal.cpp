/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/Temporal.h"

#include "mozilla/Casting.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Likely.h"
#include "mozilla/MathAlgorithms.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <stdint.h>
#include <string_view>
#include <utility>

#include "jsnum.h"
#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/temporal/Int128.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainMonthDay.h"
#include "builtin/temporal/PlainTime.h"
#include "builtin/temporal/PlainYearMonth.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "gc/Barrier.h"
#include "js/Class.h"
#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/Id.h"
#include "js/Printer.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/String.h"
#include "js/Value.h"
#include "vm/BytecodeUtil.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtomState.h"
#include "vm/JSAtomUtils.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/ObjectOperations.h"
#include "vm/Realm.h"
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;
using namespace js::temporal;

/**
 * GetOption ( options, property, type, values, default )
 *
 * GetOption specialization when `type=string`. Default value handling must
 * happen in the caller, so we don't provide the `default` parameter here.
 */
static bool GetStringOption(JSContext* cx, Handle<JSObject*> options,
                            Handle<PropertyName*> property,
                            MutableHandle<JSString*> string) {
  // Step 1.
  Rooted<Value> value(cx);
  if (!GetProperty(cx, options, options, property, &value)) {
    return false;
  }

  // Step 2. (Caller should fill in the fallback.)
  if (value.isUndefined()) {
    return true;
  }

  // Steps 3-4. (Not applicable when type=string)

  // Step 5.
  string.set(JS::ToString(cx, value));
  if (!string) {
    return false;
  }

  // Step 6. (Not applicable in our implementation)

  // Step 7.
  return true;
}

/**
 * GetRoundingIncrementOption ( normalizedOptions, dividend, inclusive )
 */
bool js::temporal::GetRoundingIncrementOption(JSContext* cx,
                                              Handle<JSObject*> options,
                                              Increment* increment) {
  // Step 1.
  Rooted<Value> value(cx);
  if (!GetProperty(cx, options, options, cx->names().roundingIncrement,
                   &value)) {
    return false;
  }

  // Step 2.
  if (value.isUndefined()) {
    *increment = Increment{1};
    return true;
  }

  // Step 3.
  double number;
  if (!ToIntegerWithTruncation(cx, value, "roundingIncrement", &number)) {
    return false;
  }

  // Step 4.
  if (number < 1 || number > 1'000'000'000) {
    ToCStringBuf cbuf;
    const char* numStr = NumberToCString(&cbuf, number);

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INVALID_OPTION_VALUE, "roundingIncrement",
                              numStr);
    return false;
  }

  // Step 5.
  *increment = Increment{uint32_t(number)};
  return true;
}

/**
 * ValidateTemporalRoundingIncrement ( increment, dividend, inclusive )
 */
bool js::temporal::ValidateTemporalRoundingIncrement(JSContext* cx,
                                                     Increment increment,
                                                     int64_t dividend,
                                                     bool inclusive) {
  MOZ_ASSERT(dividend > 0);
  MOZ_ASSERT_IF(!inclusive, dividend > 1);

  // Steps 1-2.
  int64_t maximum = inclusive ? dividend : dividend - 1;

  // Steps 3-4.
  if (increment.value() > maximum || dividend % increment.value() != 0) {
    Int32ToCStringBuf cbuf;
    const char* numStr = Int32ToCString(&cbuf, int32_t(increment.value()));

    // TODO: Better error message could be helpful.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INVALID_OPTION_VALUE, "roundingIncrement",
                              numStr);
    return false;
  }

  // Step 5.
  return true;
}

static Handle<PropertyName*> ToPropertyName(JSContext* cx,
                                            TemporalUnitKey key) {
  switch (key) {
    case TemporalUnitKey::SmallestUnit:
      return cx->names().smallestUnit;
    case TemporalUnitKey::LargestUnit:
      return cx->names().largestUnit;
    case TemporalUnitKey::Unit:
      return cx->names().unit;
  }
  MOZ_CRASH("invalid temporal unit group");
}

static const char* ToCString(TemporalUnitKey key) {
  switch (key) {
    case TemporalUnitKey::SmallestUnit:
      return "smallestUnit";
    case TemporalUnitKey::LargestUnit:
      return "largestUnit";
    case TemporalUnitKey::Unit:
      return "unit";
  }
  MOZ_CRASH("invalid temporal unit group");
}

static bool ToTemporalUnit(JSContext* cx, JSLinearString* str,
                           TemporalUnitKey key, TemporalUnit* unit) {
  struct UnitMap {
    std::string_view name;
    TemporalUnit unit;
  };

  static constexpr UnitMap mapping[] = {
      {"year", TemporalUnit::Year},
      {"years", TemporalUnit::Year},
      {"month", TemporalUnit::Month},
      {"months", TemporalUnit::Month},
      {"week", TemporalUnit::Week},
      {"weeks", TemporalUnit::Week},
      {"day", TemporalUnit::Day},
      {"days", TemporalUnit::Day},
      {"hour", TemporalUnit::Hour},
      {"hours", TemporalUnit::Hour},
      {"minute", TemporalUnit::Minute},
      {"minutes", TemporalUnit::Minute},
      {"second", TemporalUnit::Second},
      {"seconds", TemporalUnit::Second},
      {"millisecond", TemporalUnit::Millisecond},
      {"milliseconds", TemporalUnit::Millisecond},
      {"microsecond", TemporalUnit::Microsecond},
      {"microseconds", TemporalUnit::Microsecond},
      {"nanosecond", TemporalUnit::Nanosecond},
      {"nanoseconds", TemporalUnit::Nanosecond},
  };

  // Compute the length of the longest name.
  constexpr size_t maxNameLength =
      std::max_element(std::begin(mapping), std::end(mapping),
                       [](const auto& x, const auto& y) {
                         return x.name.length() < y.name.length();
                       })
          ->name.length();

  // Twenty StringEqualsLiteral calls for each possible combination seems a bit
  // expensive, so let's instead copy the input name into a char array and rely
  // on the compiler to generate optimized code for the comparisons.

  size_t length = str->length();
  if (length <= maxNameLength && StringIsAscii(str)) {
    char chars[maxNameLength] = {};
    JS::LossyCopyLinearStringChars(chars, str, length);

    for (const auto& m : mapping) {
      if (m.name == std::string_view(chars, length)) {
        *unit = m.unit;
        return true;
      }
    }
  }

  if (auto chars = QuoteString(cx, str, '"')) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_INVALID_OPTION_VALUE, ToCString(key),
                             chars.get());
  }
  return false;
}

static std::pair<TemporalUnit, TemporalUnit> AllowedValues(
    TemporalUnitGroup unitGroup) {
  switch (unitGroup) {
    case TemporalUnitGroup::Date:
      return {TemporalUnit::Year, TemporalUnit::Day};
    case TemporalUnitGroup::Time:
      return {TemporalUnit::Hour, TemporalUnit::Nanosecond};
    case TemporalUnitGroup::DateTime:
      return {TemporalUnit::Year, TemporalUnit::Nanosecond};
    case TemporalUnitGroup::DayTime:
      return {TemporalUnit::Day, TemporalUnit::Nanosecond};
  }
  MOZ_CRASH("invalid temporal unit group");
}

/**
 * GetTemporalUnitValuedOption ( normalizedOptions, key, unitGroup, default [ ,
 * extraValues ] )
 */
bool js::temporal::GetTemporalUnitValuedOption(JSContext* cx,
                                               Handle<JSObject*> options,
                                               TemporalUnitKey key,
                                               TemporalUnitGroup unitGroup,
                                               TemporalUnit* unit) {
  // Steps 1-8. (Not applicable in our implementation.)

  // Step 9.
  Rooted<JSString*> value(cx);
  if (!GetStringOption(cx, options, ToPropertyName(cx, key), &value)) {
    return false;
  }

  // Caller should fill in the fallback.
  if (!value) {
    return true;
  }

  return GetTemporalUnitValuedOption(cx, value, key, unitGroup, unit);
}

/**
 * GetTemporalUnitValuedOption ( normalizedOptions, key, unitGroup, default [ ,
 * extraValues ] )
 */
bool js::temporal::GetTemporalUnitValuedOption(JSContext* cx,
                                               Handle<JSString*> value,
                                               TemporalUnitKey key,
                                               TemporalUnitGroup unitGroup,
                                               TemporalUnit* unit) {
  // Steps 1-9. (Not applicable in our implementation.)

  // Step 10. (Handled in caller.)

  Rooted<JSLinearString*> linear(cx, value->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Caller should fill in the fallback.
  if (key == TemporalUnitKey::LargestUnit) {
    if (StringEqualsLiteral(linear, "auto")) {
      return true;
    }
  }

  // Step 11.
  if (!ToTemporalUnit(cx, linear, key, unit)) {
    return false;
  }

  auto allowedValues = AllowedValues(unitGroup);
  if (*unit < allowedValues.first || *unit > allowedValues.second) {
    if (auto chars = QuoteString(cx, linear, '"')) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_INVALID_OPTION_VALUE, ToCString(key),
                                chars.get());
    }
    return false;
  }

  return true;
}

/**
 * GetRoundingModeOption ( normalizedOptions, fallback )
 */
bool js::temporal::GetRoundingModeOption(JSContext* cx,
                                         Handle<JSObject*> options,
                                         TemporalRoundingMode* mode) {
  // Steps 1-2.
  Rooted<JSString*> string(cx);
  if (!GetStringOption(cx, options, cx->names().roundingMode, &string)) {
    return false;
  }

  // Caller should fill in the fallback.
  if (!string) {
    return true;
  }

  JSLinearString* linear = string->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  if (StringEqualsLiteral(linear, "ceil")) {
    *mode = TemporalRoundingMode::Ceil;
  } else if (StringEqualsLiteral(linear, "floor")) {
    *mode = TemporalRoundingMode::Floor;
  } else if (StringEqualsLiteral(linear, "expand")) {
    *mode = TemporalRoundingMode::Expand;
  } else if (StringEqualsLiteral(linear, "trunc")) {
    *mode = TemporalRoundingMode::Trunc;
  } else if (StringEqualsLiteral(linear, "halfCeil")) {
    *mode = TemporalRoundingMode::HalfCeil;
  } else if (StringEqualsLiteral(linear, "halfFloor")) {
    *mode = TemporalRoundingMode::HalfFloor;
  } else if (StringEqualsLiteral(linear, "halfExpand")) {
    *mode = TemporalRoundingMode::HalfExpand;
  } else if (StringEqualsLiteral(linear, "halfTrunc")) {
    *mode = TemporalRoundingMode::HalfTrunc;
  } else if (StringEqualsLiteral(linear, "halfEven")) {
    *mode = TemporalRoundingMode::HalfEven;
  } else {
    if (auto chars = QuoteString(cx, linear, '"')) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_INVALID_OPTION_VALUE, "roundingMode",
                               chars.get());
    }
    return false;
  }
  return true;
}

#ifdef DEBUG
template <typename T>
static bool IsValidMul(const T& x, const T& y) {
  return (mozilla::CheckedInt<T>(x) * y).isValid();
}

// Copied from mozilla::CheckedInt.
template <>
bool IsValidMul<Int128>(const Int128& x, const Int128& y) {
  static constexpr auto min = Int128{1} << 127;
  static constexpr auto max = ~min;

  if (x == Int128{0} || y == Int128{0}) {
    return true;
  }
  if (x > Int128{0}) {
    return y > Int128{0} ? x <= max / y : y >= min / x;
  }
  return y > Int128{0} ? x >= min / y : y >= max / x;
}
#endif

/**
 * RoundNumberToIncrement ( x, increment, roundingMode )
 */
Int128 js::temporal::RoundNumberToIncrement(const Int128& numerator,
                                            int64_t denominator,
                                            Increment increment,
                                            TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(denominator > 0);
  MOZ_ASSERT(Increment::min() <= increment && increment <= Increment::max());

  auto inc = Int128{increment.value()};
  MOZ_ASSERT(IsValidMul(Int128{denominator}, inc), "unsupported overflow");

  auto divisor = Int128{denominator} * inc;
  MOZ_ASSERT(divisor > Int128{0});

  // Steps 1-8.
  auto rounded = Divide(numerator, divisor, roundingMode);

  // Step 9.
  MOZ_ASSERT(IsValidMul(rounded, inc), "unsupported overflow");
  return rounded * inc;
}

/**
 * RoundNumberToIncrement ( x, increment, roundingMode )
 */
int64_t js::temporal::RoundNumberToIncrement(
    int64_t x, int64_t increment, TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(increment > 0);

  // Steps 1-8.
  int64_t rounded = Divide(x, increment, roundingMode);

  // Step 9.
  MOZ_ASSERT(IsValidMul(rounded, increment), "unsupported overflow");
  return rounded * increment;
}

/**
 * RoundNumberToIncrement ( x, increment, roundingMode )
 */
Int128 js::temporal::RoundNumberToIncrement(const Int128& x,
                                            const Int128& increment,
                                            TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(increment > Int128{0});

  // Steps 1-8.
  auto rounded = Divide(x, increment, roundingMode);

  // Step 9.
  MOZ_ASSERT(IsValidMul(rounded, increment), "unsupported overflow");
  return rounded * increment;
}

template <typename IntT>
static inline constexpr bool IsSafeInteger(const IntT& x) {
  constexpr IntT MaxSafeInteger = IntT{int64_t(1) << 53};
  constexpr IntT MinSafeInteger = -MaxSafeInteger;
  return MinSafeInteger < x && x < MaxSafeInteger;
}

/**
 * Return the real number value of the fraction |numerator / denominator|.
 *
 * As an optimization we multiply the remainder by 16 when computing the number
 * of digits after the decimal point, i.e. we compute four instead of one bit of
 * the fractional digits. The denominator is therefore required to not exceed
 * 2**(N - log2(16)), where N is the number of non-sign bits in the mantissa.
 */
template <typename T>
static double FractionToDoubleSlow(const T& numerator, const T& denominator) {
  MOZ_ASSERT(denominator > T{0}, "expected positive denominator");
  MOZ_ASSERT(denominator <= (T{1} << (std::numeric_limits<T>::digits - 4)),
             "denominator too large");

  auto absValue = [](const T& value) {
    if constexpr (std::is_same_v<T, Int128>) {
      return value.abs();
    } else {
      // NB: Not std::abs, because std::abs(INT64_MIN) is undefined behavior.
      return mozilla::Abs(value);
    }
  };

  using UnsignedT = decltype(absValue(T{0}));
  static_assert(!std::numeric_limits<UnsignedT>::is_signed);

  auto divrem = [](const UnsignedT& x, const UnsignedT& y) {
    if constexpr (std::is_same_v<T, Int128>) {
      return x.divrem(y);
    } else {
      return std::pair{x / y, x % y};
    }
  };

  auto [quot, rem] =
      divrem(absValue(numerator), static_cast<UnsignedT>(denominator));

  // Simple case when no remainder is present.
  if (rem == UnsignedT{0}) {
    double sign = numerator < T{0} ? -1 : 1;
    return sign * double(quot);
  }

  using Double = mozilla::FloatingPoint<double>;

  // Significand including the implicit one of IEEE-754 floating point numbers.
  static constexpr uint32_t SignificandWidthWithImplicitOne =
      Double::kSignificandWidth + 1;

  // Number of leading zeros for a correctly adjusted significand.
  static constexpr uint32_t SignificandLeadingZeros =
      64 - SignificandWidthWithImplicitOne;

  // Exponent bias for an integral significand. (`Double::kExponentBias` is the
  // bias for the binary fraction `1.xyz * 2**exp`. For an integral significand
  // the significand width has to be added to the bias.)
  static constexpr int32_t ExponentBias =
      Double::kExponentBias + Double::kSignificandWidth;

  // Significand, possibly unnormalized.
  uint64_t significand = 0;

  // Significand ignored msd bits.
  uint32_t ignoredBits = 0;

  // Read quotient, from most to least significant digit. Stop when the
  // significand got too large for double precision.
  int32_t shift = std::numeric_limits<UnsignedT>::digits;
  for (; shift != 0 && ignoredBits == 0; shift -= 4) {
    uint64_t digit = uint64_t(quot >> (shift - 4)) & 0xf;

    significand = significand * 16 + digit;
    ignoredBits = significand >> SignificandWidthWithImplicitOne;
  }

  // Read remainder, from most to least significant digit. Stop when the
  // remainder is zero or the significand got too large.
  int32_t fractionDigit = 0;
  for (; rem != UnsignedT{0} && ignoredBits == 0; fractionDigit++) {
    auto [digit, next] =
        divrem(rem * UnsignedT{16}, static_cast<UnsignedT>(denominator));
    rem = next;

    significand = significand * 16 + uint64_t(digit);
    ignoredBits = significand >> SignificandWidthWithImplicitOne;
  }

  // Unbiased exponent. (`shift` remaining bits in the quotient, minus the
  // fractional digits.)
  int32_t exponent = shift - (fractionDigit * 4);

  // Significand got too large and some bits are now ignored. Adjust the
  // significand and exponent.
  if (ignoredBits != 0) {
    //        significand
    //  ___________|__________
    // /                      |
    // [xxx················yyy|
    //  \_/                \_/
    //   |                  |
    // ignoredBits       extraBits
    //
    // `ignoredBits` have to be shifted back into the 53 bits of the significand
    // and `extraBits` has to be checked if the result has to be rounded up.

    // Number of ignored/extra bits in the significand.
    uint32_t extraBitsCount = 32 - mozilla::CountLeadingZeroes32(ignoredBits);
    MOZ_ASSERT(extraBitsCount > 0);

    // Extra bits in the significand.
    uint32_t extraBits = uint32_t(significand) & ((1 << extraBitsCount) - 1);

    // Move the ignored bits into the proper significand position and adjust the
    // exponent to reflect the now moved out extra bits.
    significand >>= extraBitsCount;
    exponent += extraBitsCount;

    MOZ_ASSERT((significand >> SignificandWidthWithImplicitOne) == 0,
               "no excess bits in the significand");

    // When the most significant digit in the extra bits is set, we may need to
    // round the result.
    uint32_t msdExtraBit = extraBits >> (extraBitsCount - 1);
    if (msdExtraBit != 0) {
      // Extra bits, excluding the most significant digit.
      uint32_t extraBitExcludingMsdMask = (1 << (extraBitsCount - 1)) - 1;

      // Unprocessed bits in the quotient.
      auto bitsBelowExtraBits = quot & ((UnsignedT{1} << shift) - UnsignedT{1});

      // Round up if the extra bit's msd is set and either the significand is
      // odd or any other bits below the extra bit's msd are non-zero.
      //
      // Bits below the extra bit's msd are:
      // 1. The remaining bits of the extra bits.
      // 2. Any bits below the extra bits.
      // 3. Any rest of the remainder.
      bool shouldRoundUp = (significand & 1) != 0 ||
                           (extraBits & extraBitExcludingMsdMask) != 0 ||
                           bitsBelowExtraBits != UnsignedT{0} ||
                           rem != UnsignedT{0};
      if (shouldRoundUp) {
        // Add one to the significand bits.
        significand += 1;

        // If they overflow, the exponent must also be increased.
        if ((significand >> SignificandWidthWithImplicitOne) != 0) {
          exponent++;
          significand >>= 1;
        }
      }
    }
  }

  MOZ_ASSERT(significand > 0, "significand is non-zero");
  MOZ_ASSERT((significand >> SignificandWidthWithImplicitOne) == 0,
             "no excess bits in the significand");

  // Move the significand into the correct position and adjust the exponent
  // accordingly.
  uint32_t significandZeros = mozilla::CountLeadingZeroes64(significand);
  if (significandZeros < SignificandLeadingZeros) {
    uint32_t shift = SignificandLeadingZeros - significandZeros;
    significand >>= shift;
    exponent += shift;
  } else if (significandZeros > SignificandLeadingZeros) {
    uint32_t shift = significandZeros - SignificandLeadingZeros;
    significand <<= shift;
    exponent -= shift;
  }

  // Combine the individual bits of the double value and return it.
  uint64_t signBit = uint64_t(numerator < T{0} ? 1 : 0)
                     << (Double::kExponentWidth + Double::kSignificandWidth);
  uint64_t exponentBits = static_cast<uint64_t>(exponent + ExponentBias)
                          << Double::kExponentShift;
  uint64_t significandBits = significand & Double::kSignificandBits;
  return mozilla::BitwiseCast<double>(signBit | exponentBits | significandBits);
}

double js::temporal::FractionToDouble(int64_t numerator, int64_t denominator) {
  MOZ_ASSERT(denominator > 0);

  // Zero divided by any divisor is still zero.
  if (numerator == 0) {
    return 0;
  }

  // When both values can be represented as doubles, use double division to
  // compute the exact result. The result is exact, because double division is
  // guaranteed to return the exact result.
  if (MOZ_LIKELY(::IsSafeInteger(numerator) && ::IsSafeInteger(denominator))) {
    return double(numerator) / double(denominator);
  }

  // Otherwise call into |FractionToDoubleSlow| to compute the exact result.
  if (denominator <=
      (int64_t(1) << (std::numeric_limits<int64_t>::digits - 4))) {
    // Slightly faster, but still slow approach when |denominator| is small
    // enough to allow computing on int64 values.
    return FractionToDoubleSlow(numerator, denominator);
  }
  return FractionToDoubleSlow(Int128{numerator}, Int128{denominator});
}

double js::temporal::FractionToDouble(const Int128& numerator,
                                      const Int128& denominator) {
  MOZ_ASSERT(denominator > Int128{0});

  // Zero divided by any divisor is still zero.
  if (numerator == Int128{0}) {
    return 0;
  }

  // When both values can be represented as doubles, use double division to
  // compute the exact result. The result is exact, because double division is
  // guaranteed to return the exact result.
  if (MOZ_LIKELY(::IsSafeInteger(numerator) && ::IsSafeInteger(denominator))) {
    return double(numerator) / double(denominator);
  }

  // Otherwise call into |FractionToDoubleSlow| to compute the exact result.
  return FractionToDoubleSlow(numerator, denominator);
}

/**
 * GetTemporalShowCalendarNameOption ( normalizedOptions )
 */
bool js::temporal::GetTemporalShowCalendarNameOption(JSContext* cx,
                                                     Handle<JSObject*> options,
                                                     ShowCalendar* result) {
  // Step 1.
  Rooted<JSString*> calendarName(cx);
  if (!GetStringOption(cx, options, cx->names().calendarName, &calendarName)) {
    return false;
  }

  // Caller should fill in the fallback.
  if (!calendarName) {
    return true;
  }

  JSLinearString* linear = calendarName->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  if (StringEqualsLiteral(linear, "auto")) {
    *result = ShowCalendar::Auto;
  } else if (StringEqualsLiteral(linear, "always")) {
    *result = ShowCalendar::Always;
  } else if (StringEqualsLiteral(linear, "never")) {
    *result = ShowCalendar::Never;
  } else if (StringEqualsLiteral(linear, "critical")) {
    *result = ShowCalendar::Critical;
  } else {
    if (auto chars = QuoteString(cx, linear, '"')) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_INVALID_OPTION_VALUE, "calendarName",
                               chars.get());
    }
    return false;
  }
  return true;
}

/**
 * GetTemporalFractionalSecondDigitsOption ( normalizedOptions )
 */
bool js::temporal::GetTemporalFractionalSecondDigitsOption(
    JSContext* cx, Handle<JSObject*> options, Precision* precision) {
  // Step 1.
  Rooted<Value> digitsValue(cx);
  if (!GetProperty(cx, options, options, cx->names().fractionalSecondDigits,
                   &digitsValue)) {
    return false;
  }

  // Step 2.
  if (digitsValue.isUndefined()) {
    *precision = Precision::Auto();
    return true;
  }

  // Step 3.
  if (!digitsValue.isNumber()) {
    // Step 3.a.
    JSString* string = JS::ToString(cx, digitsValue);
    if (!string) {
      return false;
    }

    JSLinearString* linear = string->ensureLinear(cx);
    if (!linear) {
      return false;
    }

    if (!StringEqualsLiteral(linear, "auto")) {
      if (auto chars = QuoteString(cx, linear, '"')) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_INVALID_OPTION_VALUE,
                                 "fractionalSecondDigits", chars.get());
      }
      return false;
    }

    // Step 3.b.
    *precision = Precision::Auto();
    return true;
  }

  // Step 4.
  double digitCount = digitsValue.toNumber();
  if (!std::isfinite(digitCount)) {
    ToCStringBuf cbuf;
    const char* numStr = NumberToCString(&cbuf, digitCount);

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INVALID_OPTION_VALUE,
                              "fractionalSecondDigits", numStr);
    return false;
  }

  // Step 5.
  digitCount = std::floor(digitCount);

  // Step 6.
  if (digitCount < 0 || digitCount > 9) {
    ToCStringBuf cbuf;
    const char* numStr = NumberToCString(&cbuf, digitCount);

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INVALID_OPTION_VALUE,
                              "fractionalSecondDigits", numStr);
    return false;
  }

  // Step 7.
  *precision = Precision{uint8_t(digitCount)};
  return true;
}

/**
 * ToSecondsStringPrecisionRecord ( smallestUnit, fractionalDigitCount )
 */
SecondsStringPrecision js::temporal::ToSecondsStringPrecision(
    TemporalUnit smallestUnit, Precision fractionalDigitCount) {
  MOZ_ASSERT(smallestUnit == TemporalUnit::Auto ||
             smallestUnit >= TemporalUnit::Minute);
  MOZ_ASSERT(fractionalDigitCount == Precision::Auto() ||
             fractionalDigitCount.value() <= 9);

  // Steps 1-5.
  switch (smallestUnit) {
    // Step 1.
    case TemporalUnit::Minute:
      return {Precision::Minute(), TemporalUnit::Minute, Increment{1}};

    // Step 2.
    case TemporalUnit::Second:
      return {Precision{0}, TemporalUnit::Second, Increment{1}};

    // Step 3.
    case TemporalUnit::Millisecond:
      return {Precision{3}, TemporalUnit::Millisecond, Increment{1}};

    // Step 4.
    case TemporalUnit::Microsecond:
      return {Precision{6}, TemporalUnit::Microsecond, Increment{1}};

    // Step 5.
    case TemporalUnit::Nanosecond:
      return {Precision{9}, TemporalUnit::Nanosecond, Increment{1}};

    case TemporalUnit::Auto:
      break;

    case TemporalUnit::Year:
    case TemporalUnit::Month:
    case TemporalUnit::Week:
    case TemporalUnit::Day:
    case TemporalUnit::Hour:
      MOZ_CRASH("Unexpected temporal unit");
  }

  // Step 6. (Not applicable in our implementation.)

  // Step 7.
  if (fractionalDigitCount == Precision::Auto()) {
    return {Precision::Auto(), TemporalUnit::Nanosecond, Increment{1}};
  }

  static constexpr Increment increments[] = {
      Increment{1},
      Increment{10},
      Increment{100},
  };

  uint8_t digitCount = fractionalDigitCount.value();

  // Step 8.
  if (digitCount == 0) {
    return {Precision{0}, TemporalUnit::Second, Increment{1}};
  }

  // Step 9.
  if (digitCount <= 3) {
    return {fractionalDigitCount, TemporalUnit::Millisecond,
            increments[3 - digitCount]};
  }

  // Step 10.
  if (digitCount <= 6) {
    return {fractionalDigitCount, TemporalUnit::Microsecond,
            increments[6 - digitCount]};
  }

  // Step 11.
  MOZ_ASSERT(digitCount <= 9);

  // Step 12.
  return {fractionalDigitCount, TemporalUnit::Nanosecond,
          increments[9 - digitCount]};
}

/**
 * GetTemporalOverflowOption ( normalizedOptions )
 */
bool js::temporal::GetTemporalOverflowOption(JSContext* cx,
                                             Handle<JSObject*> options,
                                             TemporalOverflow* result) {
  // Step 1.
  Rooted<JSString*> overflow(cx);
  if (!GetStringOption(cx, options, cx->names().overflow, &overflow)) {
    return false;
  }

  // Caller should fill in the fallback.
  if (!overflow) {
    return true;
  }

  JSLinearString* linear = overflow->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  if (StringEqualsLiteral(linear, "constrain")) {
    *result = TemporalOverflow::Constrain;
  } else if (StringEqualsLiteral(linear, "reject")) {
    *result = TemporalOverflow::Reject;
  } else {
    if (auto chars = QuoteString(cx, linear, '"')) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_INVALID_OPTION_VALUE, "overflow",
                               chars.get());
    }
    return false;
  }
  return true;
}

/**
 * GetTemporalDisambiguationOption ( options )
 */
bool js::temporal::GetTemporalDisambiguationOption(
    JSContext* cx, Handle<JSObject*> options,
    TemporalDisambiguation* disambiguation) {
  // Step 1. (Not applicable)

  // Step 2.
  Rooted<JSString*> string(cx);
  if (!GetStringOption(cx, options, cx->names().disambiguation, &string)) {
    return false;
  }

  // Caller should fill in the fallback.
  if (!string) {
    return true;
  }

  JSLinearString* linear = string->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  if (StringEqualsLiteral(linear, "compatible")) {
    *disambiguation = TemporalDisambiguation::Compatible;
  } else if (StringEqualsLiteral(linear, "earlier")) {
    *disambiguation = TemporalDisambiguation::Earlier;
  } else if (StringEqualsLiteral(linear, "later")) {
    *disambiguation = TemporalDisambiguation::Later;
  } else if (StringEqualsLiteral(linear, "reject")) {
    *disambiguation = TemporalDisambiguation::Reject;
  } else {
    if (auto chars = QuoteString(cx, linear, '"')) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_INVALID_OPTION_VALUE, "disambiguation",
                               chars.get());
    }
    return false;
  }
  return true;
}

/**
 * GetTemporalOffsetOption ( options, fallback )
 */
bool js::temporal::GetTemporalOffsetOption(JSContext* cx,
                                           Handle<JSObject*> options,
                                           TemporalOffset* offset) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Rooted<JSString*> string(cx);
  if (!GetStringOption(cx, options, cx->names().offset, &string)) {
    return false;
  }

  // Caller should fill in the fallback.
  if (!string) {
    return true;
  }

  JSLinearString* linear = string->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  if (StringEqualsLiteral(linear, "prefer")) {
    *offset = TemporalOffset::Prefer;
  } else if (StringEqualsLiteral(linear, "use")) {
    *offset = TemporalOffset::Use;
  } else if (StringEqualsLiteral(linear, "ignore")) {
    *offset = TemporalOffset::Ignore;
  } else if (StringEqualsLiteral(linear, "reject")) {
    *offset = TemporalOffset::Reject;
  } else {
    if (auto chars = QuoteString(cx, linear, '"')) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_INVALID_OPTION_VALUE, "offset",
                               chars.get());
    }
    return false;
  }
  return true;
}

/**
 * GetTemporalShowTimeZoneNameOption ( normalizedOptions )
 */
bool js::temporal::GetTemporalShowTimeZoneNameOption(JSContext* cx,
                                                     Handle<JSObject*> options,
                                                     ShowTimeZoneName* result) {
  // Step 1.
  Rooted<JSString*> timeZoneName(cx);
  if (!GetStringOption(cx, options, cx->names().timeZoneName, &timeZoneName)) {
    return false;
  }

  // Caller should fill in the fallback.
  if (!timeZoneName) {
    return true;
  }

  JSLinearString* linear = timeZoneName->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  if (StringEqualsLiteral(linear, "auto")) {
    *result = ShowTimeZoneName::Auto;
  } else if (StringEqualsLiteral(linear, "never")) {
    *result = ShowTimeZoneName::Never;
  } else if (StringEqualsLiteral(linear, "critical")) {
    *result = ShowTimeZoneName::Critical;
  } else {
    if (auto chars = QuoteString(cx, linear, '"')) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_INVALID_OPTION_VALUE, "timeZoneName",
                               chars.get());
    }
    return false;
  }
  return true;
}

/**
 * GetTemporalShowOffsetOption ( normalizedOptions )
 */
bool js::temporal::GetTemporalShowOffsetOption(JSContext* cx,
                                               Handle<JSObject*> options,
                                               ShowOffset* result) {
  // Step 1.
  Rooted<JSString*> offset(cx);
  if (!GetStringOption(cx, options, cx->names().offset, &offset)) {
    return false;
  }

  // Caller should fill in the fallback.
  if (!offset) {
    return true;
  }

  JSLinearString* linear = offset->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  if (StringEqualsLiteral(linear, "auto")) {
    *result = ShowOffset::Auto;
  } else if (StringEqualsLiteral(linear, "never")) {
    *result = ShowOffset::Never;
  } else {
    if (auto chars = QuoteString(cx, linear, '"')) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_INVALID_OPTION_VALUE, "offset",
                               chars.get());
    }
    return false;
  }
  return true;
}

/**
 * GetDirectionOption ( options )
 */
bool js::temporal::GetDirectionOption(JSContext* cx,
                                      Handle<JSString*> direction,
                                      Direction* result) {
  JSLinearString* linear = direction->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  if (StringEqualsLiteral(linear, "next")) {
    *result = Direction::Next;
  } else if (StringEqualsLiteral(linear, "previous")) {
    *result = Direction::Previous;
  } else {
    if (auto chars = QuoteString(cx, linear, '"')) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_INVALID_OPTION_VALUE, "direction",
                               chars.get());
    }
    return false;
  }
  return true;
}

/**
 * GetDirectionOption ( options )
 */
bool js::temporal::GetDirectionOption(JSContext* cx, Handle<JSObject*> options,
                                      Direction* result) {
  // Step 1.
  Rooted<JSString*> direction(cx);
  if (!GetStringOption(cx, options, cx->names().direction, &direction)) {
    return false;
  }

  if (!direction) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_MISSING_OPTION, "direction");
    return false;
  }

  return GetDirectionOption(cx, direction, result);
}

template <typename T, typename... Ts>
static JSObject* MaybeUnwrapIf(JSObject* object) {
  if (auto* unwrapped = object->maybeUnwrapIf<T>()) {
    return unwrapped;
  }
  if constexpr (sizeof...(Ts) > 0) {
    return MaybeUnwrapIf<Ts...>(object);
  }
  return nullptr;
}

/**
 * IsPartialTemporalObject ( object )
 */
bool js::temporal::ThrowIfTemporalLikeObject(JSContext* cx,
                                             Handle<JSObject*> object) {
  // Step 1. (Handled in caller)

  // Step 2.
  if (auto* unwrapped =
          MaybeUnwrapIf<PlainDateObject, PlainDateTimeObject,
                        PlainMonthDayObject, PlainTimeObject,
                        PlainYearMonthObject, ZonedDateTimeObject>(object)) {
    Rooted<Value> value(cx, ObjectValue(*object));
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, value,
                     nullptr, unwrapped->getClass()->name);
    return false;
  }

  Rooted<Value> property(cx);

  // Step 3.
  if (!GetProperty(cx, object, object, cx->names().calendar, &property)) {
    return false;
  }

  // Step 4.
  if (!property.isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_UNEXPECTED_PROPERTY, "calendar");
    return false;
  }

  // Step 5.
  if (!GetProperty(cx, object, object, cx->names().timeZone, &property)) {
    return false;
  }

  // Step 6.
  if (!property.isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_UNEXPECTED_PROPERTY, "timeZone");
    return false;
  }

  // Step 7.
  return true;
}

/**
 * ToPositiveIntegerWithTruncation ( argument )
 */
bool js::temporal::ToPositiveIntegerWithTruncation(JSContext* cx,
                                                   Handle<Value> value,
                                                   const char* name,
                                                   double* result) {
  // Step 1.
  double number;
  if (!ToIntegerWithTruncation(cx, value, name, &number)) {
    return false;
  }

  // Step 2.
  if (number <= 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INVALID_NUMBER, name);
    return false;
  }

  // Step 3.
  *result = number;
  return true;
}

/**
 * ToIntegerWithTruncation ( argument )
 */
bool js::temporal::ToIntegerWithTruncation(JSContext* cx, Handle<Value> value,
                                           const char* name, double* result) {
  // Step 1.
  double number;
  if (!JS::ToNumber(cx, value, &number)) {
    return false;
  }

  // Step 2.
  if (!std::isfinite(number)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INVALID_INTEGER, name);
    return false;
  }

  // Step 3.
  *result = std::trunc(number) + (+0.0);  // Add zero to convert -0 to +0.
  return true;
}

/**
 * GetDifferenceSettings ( operation, options, unitGroup, disallowedUnits,
 * fallbackSmallestUnit, smallestLargestDefaultUnit )
 */
bool js::temporal::GetDifferenceSettings(
    JSContext* cx, TemporalDifference operation, Handle<JSObject*> options,
    TemporalUnitGroup unitGroup, TemporalUnit smallestAllowedUnit,
    TemporalUnit fallbackSmallestUnit, TemporalUnit smallestLargestDefaultUnit,
    DifferenceSettings* result) {
  // Steps 1-2.
  auto largestUnit = TemporalUnit::Auto;
  if (!GetTemporalUnitValuedOption(cx, options, TemporalUnitKey::LargestUnit,
                                   unitGroup, &largestUnit)) {
    return false;
  }

  // Step 3.
  if (largestUnit > smallestAllowedUnit) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INVALID_UNIT_OPTION,
                              TemporalUnitToString(largestUnit), "largestUnit");
    return false;
  }

  // Step 4.
  auto roundingIncrement = Increment{1};
  if (!GetRoundingIncrementOption(cx, options, &roundingIncrement)) {
    return false;
  }

  // Step 5.
  auto roundingMode = TemporalRoundingMode::Trunc;
  if (!GetRoundingModeOption(cx, options, &roundingMode)) {
    return false;
  }

  // Step 6.
  if (operation == TemporalDifference::Since) {
    roundingMode = NegateRoundingMode(roundingMode);
  }

  // Step 7.
  auto smallestUnit = fallbackSmallestUnit;
  if (!GetTemporalUnitValuedOption(cx, options, TemporalUnitKey::SmallestUnit,
                                   unitGroup, &smallestUnit)) {
    return false;
  }

  // Step 8.
  if (smallestUnit > smallestAllowedUnit) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_TEMPORAL_INVALID_UNIT_OPTION,
        TemporalUnitToString(smallestUnit), "smallestUnit");
    return false;
  }

  // Step 9. (Inlined call to LargerOfTwoTemporalUnits)
  auto defaultLargestUnit = std::min(smallestLargestDefaultUnit, smallestUnit);

  // Step 10.
  if (largestUnit == TemporalUnit::Auto) {
    largestUnit = defaultLargestUnit;
  }

  // Step 11.
  if (largestUnit > smallestUnit) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INVALID_UNIT_RANGE);
    return false;
  }

  // Steps 12-13.
  if (smallestUnit > TemporalUnit::Day) {
    // Step 12.
    auto maximum = MaximumTemporalDurationRoundingIncrement(smallestUnit);

    // Step 13.
    if (!ValidateTemporalRoundingIncrement(cx, roundingIncrement, maximum,
                                           false)) {
      return false;
    }
  }

  // Step 14.
  *result = {smallestUnit, largestUnit, roundingMode, roundingIncrement};
  return true;
}

static JSObject* CreateTemporalObject(JSContext* cx, JSProtoKey key) {
  Rooted<JSObject*> proto(cx, &cx->global()->getObjectPrototype());

  // The |Temporal| object is just a plain object with some "static" data
  // properties and some constructor properties.
  return NewTenuredObjectWithGivenProto<TemporalObject>(cx, proto);
}

/**
 * Initializes the Temporal Object and its standard built-in properties.
 */
static bool TemporalClassFinish(JSContext* cx, Handle<JSObject*> temporal,
                                Handle<JSObject*> proto) {
  Rooted<PropertyKey> ctorId(cx);
  Rooted<Value> ctorValue(cx);
  auto defineProperty = [&](JSProtoKey protoKey, Handle<PropertyName*> name) {
    JSObject* ctor = GlobalObject::getOrCreateConstructor(cx, protoKey);
    if (!ctor) {
      return false;
    }

    ctorId = NameToId(name);
    ctorValue.setObject(*ctor);
    return DefineDataProperty(cx, temporal, ctorId, ctorValue, 0);
  };

  // Add the constructor properties.
  for (const auto& protoKey : {
           JSProto_Duration,
           JSProto_Instant,
           JSProto_PlainDate,
           JSProto_PlainDateTime,
           JSProto_PlainMonthDay,
           JSProto_PlainTime,
           JSProto_PlainYearMonth,
           JSProto_ZonedDateTime,
       }) {
    if (!defineProperty(protoKey, ClassName(protoKey, cx))) {
      return false;
    }
  }

  // ClassName(JSProto_TemporalNow) returns "TemporalNow", so we need to handle
  // it separately.
  if (!defineProperty(JSProto_TemporalNow, cx->names().Now)) {
    return false;
  }

  return true;
}

const JSClass TemporalObject::class_ = {
    "Temporal",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Temporal),
    JS_NULL_CLASS_OPS,
    &TemporalObject::classSpec_,
};

static const JSPropertySpec Temporal_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Temporal", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec TemporalObject::classSpec_ = {
    CreateTemporalObject, nullptr, nullptr,
    Temporal_properties,  nullptr, nullptr,
    TemporalClassFinish,
};
