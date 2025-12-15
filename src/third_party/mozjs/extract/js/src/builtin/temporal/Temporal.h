/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_Temporal_h
#define builtin_temporal_Temporal_h

#include "mozilla/Assertions.h"

#include <stdint.h>

#include "jstypes.h"

#include "builtin/temporal/Int128.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalUnit.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "vm/NativeObject.h"

namespace js {
struct ClassSpec;
}  // namespace js

namespace js::temporal {

class TemporalObject : public NativeObject {
 public:
  static const JSClass class_;

 private:
  static const ClassSpec classSpec_;
};

/**
 * Rounding increment, which is an integer in the range [1, 1'000'000'000].
 *
 * Temporal units are rounded to a multiple of the specified increment value.
 */
class Increment final {
  uint32_t value_;

 public:
  constexpr explicit Increment(uint32_t value) : value_(value) {
    MOZ_ASSERT(1 <= value && value <= 1'000'000'000);
  }

  /**
   * Minimum allowed rounding increment.
   */
  static constexpr auto min() { return Increment{1}; }

  /**
   * Maximum allowed rounding increment.
   */
  static constexpr auto max() { return Increment{1'000'000'000}; }

  /**
   * The rounding increment's value.
   */
  uint32_t value() const { return value_; }

  bool operator==(const Increment& other) const {
    return value_ == other.value_;
  }

  bool operator<(const Increment& other) const { return value_ < other.value_; }

  // Other operators are implemented in terms of operator== and operator<.
  bool operator!=(const Increment& other) const { return !(*this == other); }
  bool operator>(const Increment& other) const { return other < *this; }
  bool operator<=(const Increment& other) const { return !(other < *this); }
  bool operator>=(const Increment& other) const { return !(*this < other); }
};

/**
 * GetRoundingIncrementOption ( normalizedOptions, dividend, inclusive )
 */
bool GetRoundingIncrementOption(JSContext* cx, JS::Handle<JSObject*> options,
                                Increment* increment);

/**
 * ValidateTemporalRoundingIncrement ( increment, dividend, inclusive )
 */
bool ValidateTemporalRoundingIncrement(JSContext* cx, Increment increment,
                                       int64_t dividend, bool inclusive);

/**
 * ValidateTemporalRoundingIncrement ( increment, dividend, inclusive )
 */
inline bool ValidateTemporalRoundingIncrement(JSContext* cx,
                                              Increment increment,
                                              Increment dividend,
                                              bool inclusive) {
  return ValidateTemporalRoundingIncrement(cx, increment, dividend.value(),
                                           inclusive);
}

/**
 * MaximumTemporalDurationRoundingIncrement ( unit )
 */
constexpr Increment MaximumTemporalDurationRoundingIncrement(
    TemporalUnit unit) {
  // Step 1. (Not applicable in our implementation.)
  MOZ_ASSERT(unit > TemporalUnit::Day);

  // Step 2.
  if (unit == TemporalUnit::Hour) {
    return Increment{24};
  }

  // Step 3.
  if (unit <= TemporalUnit::Second) {
    return Increment{60};
  }

  // Steps 4-5.
  return Increment{1000};
}

enum class TemporalUnitGroup {
  // Allow date units: "year", "month", "week", "day".
  Date,

  // Allow time units: "hour", "minute", "second", "milli-/micro-/nanoseconds".
  Time,

  // Allow date and time units.
  DateTime,

  // Allow "day" and time units.
  DayTime,
};

enum class TemporalUnitKey {
  SmallestUnit,
  LargestUnit,
  Unit,
};

/**
 * GetTemporalUnitValuedOption ( normalizedOptions, key, unitGroup, default [ ,
 * extraValues ] )
 */
bool GetTemporalUnitValuedOption(JSContext* cx, JS::Handle<JSObject*> options,
                                 TemporalUnitKey key,
                                 TemporalUnitGroup unitGroup,
                                 TemporalUnit* unit);

/**
 * GetTemporalUnitValuedOption ( normalizedOptions, key, unitGroup, default [ ,
 * extraValues ] )
 */
bool GetTemporalUnitValuedOption(JSContext* cx, JS::Handle<JSString*> value,
                                 TemporalUnitKey key,
                                 TemporalUnitGroup unitGroup,
                                 TemporalUnit* unit);

/**
 * GetRoundingModeOption ( normalizedOptions, fallback )
 */
bool GetRoundingModeOption(JSContext* cx, JS::Handle<JSObject*> options,
                           TemporalRoundingMode* mode);

/**
 * RoundNumberToIncrement ( x, increment, roundingMode )
 */
Int128 RoundNumberToIncrement(const Int128& numerator, int64_t denominator,
                              Increment increment,
                              TemporalRoundingMode roundingMode);

/**
 * RoundNumberToIncrement ( x, increment, roundingMode )
 */
int64_t RoundNumberToIncrement(int64_t x, int64_t increment,
                               TemporalRoundingMode roundingMode);

/**
 * RoundNumberToIncrement ( x, increment, roundingMode )
 */
inline int64_t RoundNumberToIncrement(int64_t x, Increment increment,
                                      TemporalRoundingMode roundingMode) {
  return RoundNumberToIncrement(x, int64_t(increment.value()), roundingMode);
}

/**
 * RoundNumberToIncrement ( x, increment, roundingMode )
 */
Int128 RoundNumberToIncrement(const Int128& x, const Int128& increment,
                              TemporalRoundingMode roundingMode);

/**
 * Return the double value of the fractional number `numerator / denominator`.
 */
double FractionToDouble(int64_t numerator, int64_t denominator);

/**
 * Return the double value of the fractional number `numerator / denominator`.
 */
double FractionToDouble(const Int128& numerator, const Int128& denominator);

enum class ShowCalendar { Auto, Always, Never, Critical };

/**
 * GetTemporalShowCalendarNameOption ( normalizedOptions )
 */
bool GetTemporalShowCalendarNameOption(JSContext* cx,
                                       JS::Handle<JSObject*> options,
                                       ShowCalendar* result);

/**
 * Precision when displaying fractional seconds.
 */
class Precision final {
  int8_t value_;

  enum class Tag {};
  constexpr Precision(int8_t value, Tag) : value_(value) {}

 public:
  constexpr explicit Precision(uint8_t value) : value_(int8_t(value)) {
    MOZ_ASSERT(value < 10);
  }

  bool operator==(const Precision& other) const {
    return value_ == other.value_;
  }

  bool operator!=(const Precision& other) const { return !(*this == other); }

  /**
   * Return the number of fractional second digits.
   */
  uint8_t value() const {
    MOZ_ASSERT(value_ >= 0, "auto and minute precision don't have a value");
    return uint8_t(value_);
  }

  /**
   * Limit the precision to trim off any trailing zeros.
   */
  static constexpr Precision Auto() { return {-1, Tag{}}; }

  /**
   * Limit the precision to minutes, i.e. don't display seconds and sub-seconds.
   */
  static constexpr Precision Minute() { return {-2, Tag{}}; }
};

/**
 * GetTemporalFractionalSecondDigitsOption ( normalizedOptions )
 */
bool GetTemporalFractionalSecondDigitsOption(JSContext* cx,
                                             JS::Handle<JSObject*> options,
                                             Precision* precision);

struct SecondsStringPrecision final {
  Precision precision = Precision{0};
  TemporalUnit unit = TemporalUnit::Auto;
  Increment increment = Increment{1};
};

/**
 * ToSecondsStringPrecisionRecord ( smallestUnit, fractionalDigitCount )
 */
SecondsStringPrecision ToSecondsStringPrecision(TemporalUnit smallestUnit,
                                                Precision fractionalDigitCount);

enum class TemporalOverflow { Constrain, Reject };

/**
 * GetTemporalOverflowOption ( normalizedOptions )
 */
bool GetTemporalOverflowOption(JSContext* cx, JS::Handle<JSObject*> options,
                               TemporalOverflow* result);

enum class TemporalDisambiguation { Compatible, Earlier, Later, Reject };

/**
 * GetTemporalDisambiguationOption ( options )
 */
bool GetTemporalDisambiguationOption(JSContext* cx,
                                     JS::Handle<JSObject*> options,
                                     TemporalDisambiguation* disambiguation);

enum class TemporalOffset { Prefer, Use, Ignore, Reject };

/**
 * GetTemporalOffsetOption ( options, fallback )
 */
bool GetTemporalOffsetOption(JSContext* cx, JS::Handle<JSObject*> options,
                             TemporalOffset* offset);

enum class ShowTimeZoneName { Auto, Never, Critical };

bool GetTemporalShowTimeZoneNameOption(JSContext* cx,
                                       JS::Handle<JSObject*> options,
                                       ShowTimeZoneName* result);

enum class ShowOffset { Auto, Never };

/**
 * GetTemporalShowOffsetOption ( normalizedOptions )
 */
bool GetTemporalShowOffsetOption(JSContext* cx, JS::Handle<JSObject*> options,
                                 ShowOffset* result);

enum class Direction { Next, Previous };

/**
 * GetDirectionOption ( options )
 */
bool GetDirectionOption(JSContext* cx, JS::Handle<JSObject*> options,
                        Direction* result);

/**
 * GetDirectionOption ( options )
 */
bool GetDirectionOption(JSContext* cx, JS::Handle<JSString*> direction,
                        Direction* result);

/**
 * IsPartialTemporalObject ( object )
 *
 * Our implementation performs error reporting in this function instead of in
 * the caller to provide better error messages.
 */
bool ThrowIfTemporalLikeObject(JSContext* cx, JS::Handle<JSObject*> object);

/**
 * ToPositiveIntegerWithTruncation ( argument )
 */
bool ToPositiveIntegerWithTruncation(JSContext* cx, JS::Handle<JS::Value> value,
                                     const char* name, double* result);

/**
 * ToIntegerWithTruncation ( argument )
 */
bool ToIntegerWithTruncation(JSContext* cx, JS::Handle<JS::Value> value,
                             const char* name, double* result);

enum class TemporalDifference { Since, Until };

inline const char* ToName(TemporalDifference difference) {
  return difference == TemporalDifference::Since ? "since" : "until";
}

enum class TemporalAddDuration { Add, Subtract };

inline const char* ToName(TemporalAddDuration addDuration) {
  return addDuration == TemporalAddDuration::Add ? "add" : "subtract";
}

struct DifferenceSettings final {
  TemporalUnit smallestUnit = TemporalUnit::Auto;
  TemporalUnit largestUnit = TemporalUnit::Auto;
  TemporalRoundingMode roundingMode = TemporalRoundingMode::Trunc;
  Increment roundingIncrement = Increment{1};
};

/**
 * GetDifferenceSettings ( operation, options, unitGroup, disallowedUnits,
 * fallbackSmallestUnit, smallestLargestDefaultUnit )
 */
bool GetDifferenceSettings(JSContext* cx, TemporalDifference operation,
                           JS::Handle<JSObject*> options,
                           TemporalUnitGroup unitGroup,
                           TemporalUnit smallestAllowedUnit,
                           TemporalUnit fallbackSmallestUnit,
                           TemporalUnit smallestLargestDefaultUnit,
                           DifferenceSettings* result);

/**
 * GetDifferenceSettings ( operation, options, unitGroup, disallowedUnits,
 * fallbackSmallestUnit, smallestLargestDefaultUnit )
 */
inline bool GetDifferenceSettings(JSContext* cx, TemporalDifference operation,
                                  JS::Handle<JSObject*> options,
                                  TemporalUnitGroup unitGroup,
                                  TemporalUnit fallbackSmallestUnit,
                                  TemporalUnit smallestLargestDefaultUnit,
                                  DifferenceSettings* result) {
  return GetDifferenceSettings(cx, operation, options, unitGroup,
                               TemporalUnit::Nanosecond, fallbackSmallestUnit,
                               smallestLargestDefaultUnit, result);
}

} /* namespace js::temporal */

#endif /* builtin_temporal_Temporal_h */
