/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_Duration_h
#define builtin_temporal_Duration_h

#include "mozilla/Assertions.h"

#include <stdint.h>

#include "builtin/temporal/TemporalTypes.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

namespace js {
struct ClassSpec;
}

namespace js::temporal {

class DurationObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t YEARS_SLOT = 0;
  static constexpr uint32_t MONTHS_SLOT = 1;
  static constexpr uint32_t WEEKS_SLOT = 2;
  static constexpr uint32_t DAYS_SLOT = 3;
  static constexpr uint32_t HOURS_SLOT = 4;
  static constexpr uint32_t MINUTES_SLOT = 5;
  static constexpr uint32_t SECONDS_SLOT = 6;
  static constexpr uint32_t MILLISECONDS_SLOT = 7;
  static constexpr uint32_t MICROSECONDS_SLOT = 8;
  static constexpr uint32_t NANOSECONDS_SLOT = 9;
  static constexpr uint32_t SLOT_COUNT = 10;

  double years() const { return getFixedSlot(YEARS_SLOT).toNumber(); }
  double months() const { return getFixedSlot(MONTHS_SLOT).toNumber(); }
  double weeks() const { return getFixedSlot(WEEKS_SLOT).toNumber(); }
  double days() const { return getFixedSlot(DAYS_SLOT).toNumber(); }
  double hours() const { return getFixedSlot(HOURS_SLOT).toNumber(); }
  double minutes() const { return getFixedSlot(MINUTES_SLOT).toNumber(); }
  double seconds() const { return getFixedSlot(SECONDS_SLOT).toNumber(); }
  double milliseconds() const {
    return getFixedSlot(MILLISECONDS_SLOT).toNumber();
  }
  double microseconds() const {
    return getFixedSlot(MICROSECONDS_SLOT).toNumber();
  }
  double nanoseconds() const {
    return getFixedSlot(NANOSECONDS_SLOT).toNumber();
  }

 private:
  static const ClassSpec classSpec_;
};

/**
 * Extract the duration fields from the Duration object.
 */
inline Duration ToDuration(const DurationObject* duration) {
  return {
      duration->years(),        duration->months(),
      duration->weeks(),        duration->days(),
      duration->hours(),        duration->minutes(),
      duration->seconds(),      duration->milliseconds(),
      duration->microseconds(), duration->nanoseconds(),
  };
}

class Increment;
class CalendarValue;
class TimeZoneValue;
enum class TemporalRoundingMode;
enum class TemporalUnit;

/**
 * DurationSign ( duration )
 */
int32_t DurationSign(const Duration& duration);

/**
 * DateDurationSign ( dateDuration )
 */
int32_t DateDurationSign(const DateDuration& duration);

#ifdef DEBUG
/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
bool IsValidDuration(const Duration& duration);

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
bool IsValidDuration(const DateDuration& duration);

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
bool IsValidDuration(const InternalDuration& duration);
#endif

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
bool ThrowIfInvalidDuration(JSContext* cx, const Duration& duration);

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
inline bool IsValidTimeDuration(const TimeDuration& duration) {
  MOZ_ASSERT(0 <= duration.nanoseconds && duration.nanoseconds <= 999'999'999);

  // The absolute value of the seconds part of a time duration must be
  // less-or-equal to `2**53 - 1` and the nanoseconds part must be less or equal
  // to `999'999'999`.
  //
  // Add ±1 nanosecond to make the nanoseconds part zero, which enables faster
  // codegen.

  constexpr auto max = TimeDuration::max() + TimeDuration::fromNanoseconds(1);
  static_assert(max.nanoseconds == 0);

  constexpr auto min = TimeDuration::min() - TimeDuration::fromNanoseconds(1);
  static_assert(min.nanoseconds == 0);

  // Step 4.
  return min < duration && duration < max;
}

/**
 * TimeDurationFromComponents ( hours, minutes, seconds, milliseconds,
 * microseconds, nanoseconds )
 */
TimeDuration TimeDurationFromComponents(const Duration& duration);

/**
 * CompareTimeDuration ( one, two )
 */
inline int32_t CompareTimeDuration(const TimeDuration& one,
                                   const TimeDuration& two) {
  MOZ_ASSERT(IsValidTimeDuration(one));
  MOZ_ASSERT(IsValidTimeDuration(two));

  // Step 1.
  if (one > two) {
    return 1;
  }

  // Step 2.
  if (one < two) {
    return -1;
  }

  // Step 3.
  return 0;
}

/**
 * TimeDurationSign ( d )
 */
inline int32_t TimeDurationSign(const TimeDuration& d) {
  MOZ_ASSERT(IsValidTimeDuration(d));

  // Steps 1-3.
  return CompareTimeDuration(d, TimeDuration{});
}

/**
 * ToInternalDurationRecord ( duration )
 */
inline InternalDuration ToInternalDurationRecord(const Duration& duration) {
  MOZ_ASSERT(IsValidDuration(duration));

  // Steps 1-3.
  return {duration.toDateDuration(), TimeDurationFromComponents(duration)};
}

/**
 * ToInternalDurationRecordWith24HourDays ( duration )
 */
InternalDuration ToInternalDurationRecordWith24HourDays(
    const Duration& duration);

/**
 * ToDateDurationRecordWithoutTime ( duration )
 */
DateDuration ToDateDurationRecordWithoutTime(const Duration& duration);

/**
 * TemporalDurationFromInternal ( internalDuration, largestUnit )
 */
bool TemporalDurationFromInternal(JSContext* cx,
                                  const TimeDuration& timeDuration,
                                  TemporalUnit largestUnit, Duration* result);

/**
 * TemporalDurationFromInternal ( internalDuration, largestUnit )
 */
bool TemporalDurationFromInternal(JSContext* cx,
                                  const InternalDuration& internalDuration,
                                  TemporalUnit largestUnit, Duration* result);

/**
 * TimeDurationFromEpochNanosecondsDifference ( one, two )
 */
TimeDuration TimeDurationFromEpochNanosecondsDifference(
    const EpochNanoseconds& one, const EpochNanoseconds& two);

/**
 * CreateTemporalDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds [ , newTarget ] )
 */
DurationObject* CreateTemporalDuration(JSContext* cx, const Duration& duration);

/**
 * ToTemporalDuration ( item )
 */
bool ToTemporalDuration(JSContext* cx, JS::Handle<JS::Value> item,
                        Duration* result);

/**
 * RoundTimeDuration ( duration, increment, unit, roundingMode )
 */
TimeDuration RoundTimeDuration(const TimeDuration& duration,
                               Increment increment, TemporalUnit unit,
                               TemporalRoundingMode roundingMode);

/**
 * RoundRelativeDuration ( duration, destEpochNs, isoDateTime, timeZone,
 * calendar, largestUnit, increment, smallestUnit, roundingMode )
 */
bool RoundRelativeDuration(
    JSContext* cx, const InternalDuration& duration,
    const EpochNanoseconds& destEpochNs, const ISODateTime& isoDateTime,
    JS::Handle<TimeZoneValue> timeZone, JS::Handle<CalendarValue> calendar,
    TemporalUnit largestUnit, Increment increment, TemporalUnit smallestUnit,
    TemporalRoundingMode roundingMode, InternalDuration* result);

/**
 * TotalRelativeDuration ( duration, destEpochNs, isoDateTime, timeZone,
 * calendar, unit )
 */
bool TotalRelativeDuration(JSContext* cx, const InternalDuration& duration,
                           const EpochNanoseconds& destEpochNs,
                           const ISODateTime& isoDateTime,
                           JS::Handle<TimeZoneValue> timeZone,
                           JS::Handle<CalendarValue> calendar,
                           TemporalUnit unit, double* result);

/**
 * TotalTimeDuration ( timeDuration, unit )
 */
double TotalTimeDuration(const TimeDuration& duration, TemporalUnit unit);

} /* namespace js::temporal */

#endif /* builtin_temporal_Duration_h */
