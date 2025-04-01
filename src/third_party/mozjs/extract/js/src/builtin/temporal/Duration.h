/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_Duration_h
#define builtin_temporal_Duration_h

#include <stdint.h>

#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/Wrapped.h"
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
class CalendarRecord;
class PlainDateObject;
class TimeZoneRecord;
class ZonedDateTime;
class ZonedDateTimeObject;
enum class TemporalRoundingMode;
enum class TemporalUnit;

/**
 * DurationSign ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
int32_t DurationSign(const Duration& duration);

/**
 * DurationSign ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
int32_t DurationSign(const DateDuration& duration);

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
bool IsValidDuration(const Duration& duration);

#ifdef DEBUG
/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
bool IsValidDuration(const DateDuration& duration);

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
bool IsValidDuration(const NormalizedDuration& duration);
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
bool ThrowIfInvalidDuration(JSContext* cx, const DateDuration& duration);

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
inline bool IsValidNormalizedTimeDuration(
    const NormalizedTimeDuration& duration) {
  MOZ_ASSERT(0 <= duration.nanoseconds && duration.nanoseconds <= 999'999'999);

  // Step 4.
  //
  // The absolute value of the seconds part of normalized time duration must be
  // less-or-equal to `2**53 - 1` and the nanoseconds part must be less or equal
  // to `999'999'999`.
  return NormalizedTimeDuration::min() <= duration &&
         duration <= NormalizedTimeDuration::max();
}

/**
 * NormalizeTimeDuration ( hours, minutes, seconds, milliseconds, microseconds,
 * nanoseconds )
 */
NormalizedTimeDuration NormalizeTimeDuration(int32_t hours, int32_t minutes,
                                             int32_t seconds,
                                             int32_t milliseconds,
                                             int32_t microseconds,
                                             int32_t nanoseconds);

/**
 * NormalizeTimeDuration ( hours, minutes, seconds, milliseconds, microseconds,
 * nanoseconds )
 */
NormalizedTimeDuration NormalizeTimeDuration(const Duration& duration);

/**
 * CompareNormalizedTimeDuration ( one, two )
 */
inline int32_t CompareNormalizedTimeDuration(
    const NormalizedTimeDuration& one, const NormalizedTimeDuration& two) {
  MOZ_ASSERT(IsValidNormalizedTimeDuration(one));
  MOZ_ASSERT(IsValidNormalizedTimeDuration(two));

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
 * NormalizedTimeDurationSign ( d )
 */
inline int32_t NormalizedTimeDurationSign(const NormalizedTimeDuration& d) {
  MOZ_ASSERT(IsValidNormalizedTimeDuration(d));

  // Steps 1-3.
  return CompareNormalizedTimeDuration(d, NormalizedTimeDuration{});
}

/**
 * Add24HourDaysToNormalizedTimeDuration ( d, days )
 */
bool Add24HourDaysToNormalizedTimeDuration(JSContext* cx,
                                           const NormalizedTimeDuration& d,
                                           int64_t days,
                                           NormalizedTimeDuration* result);

/**
 * CreateNormalizedDurationRecord ( years, months, weeks, days, norm )
 */
inline NormalizedDuration CreateNormalizedDurationRecord(
    const DateDuration& date, const NormalizedTimeDuration& time) {
  MOZ_ASSERT(IsValidDuration(date));
  MOZ_ASSERT(IsValidNormalizedTimeDuration(time));
#ifdef DEBUG
  int64_t dateValues = date.years | date.months | date.weeks | date.days;
  int32_t dateSign = dateValues ? dateValues < 0 ? -1 : 1 : 0;
  int32_t timeSign = NormalizedTimeDurationSign(time);
  MOZ_ASSERT((dateSign * timeSign) >= 0);
#endif

  return {date, time};
}

/**
 * CreateNormalizedDurationRecord ( years, months, weeks, days, norm )
 */
inline NormalizedDuration CreateNormalizedDurationRecord(
    const Duration& duration) {
  return CreateNormalizedDurationRecord(duration.toDateDuration(),
                                        NormalizeTimeDuration(duration));
}

/**
 * CombineDateAndNormalizedTimeDuration ( dateDurationRecord, norm )
 */
bool CombineDateAndNormalizedTimeDuration(JSContext* cx,
                                          const DateDuration& date,
                                          const NormalizedTimeDuration& time,
                                          NormalizedDuration* result);

/**
 * CreateNormalizedDurationRecord ( years, months, weeks, days, norm )
 */
inline bool CreateNormalizedDurationRecord(JSContext* cx,
                                           const DateDuration& date,
                                           const NormalizedTimeDuration& time,
                                           NormalizedDuration* result) {
  return CombineDateAndNormalizedTimeDuration(cx, date, time, result);
}

/**
 * NormalizedTimeDurationFromEpochNanosecondsDifference ( one, two )
 */
NormalizedTimeDuration NormalizedTimeDurationFromEpochNanosecondsDifference(
    const Instant& one, const Instant& two);

/**
 * CreateTemporalDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds [ , newTarget ] )
 */
DurationObject* CreateTemporalDuration(JSContext* cx, const Duration& duration);

/**
 * ToTemporalDuration ( item )
 */
Wrapped<DurationObject*> ToTemporalDuration(JSContext* cx,
                                            JS::Handle<JS::Value> item);

/**
 * ToTemporalDuration ( item )
 */
bool ToTemporalDuration(JSContext* cx, JS::Handle<JS::Value> item,
                        Duration* result);

/**
 * ToTemporalDurationRecord ( temporalDurationLike )
 */
bool ToTemporalDurationRecord(JSContext* cx,
                              JS::Handle<JS::Value> temporalDurationLike,
                              Duration* result);

/**
 * BalanceTimeDuration ( norm, largestUnit )
 */
TimeDuration BalanceTimeDuration(const NormalizedTimeDuration& duration,
                                 TemporalUnit largestUnit);

/**
 * BalanceTimeDuration ( norm, largestUnit )
 */
bool BalanceTimeDuration(JSContext* cx, const NormalizedTimeDuration& duration,
                         TemporalUnit largestUnit, TimeDuration* result);

/**
 * BalanceDateDurationRelative ( years, months, weeks, days, largestUnit,
 * smallestUnit, plainRelativeTo, calendarRec )
 */
bool BalanceDateDurationRelative(
    JSContext* cx, const DateDuration& duration, TemporalUnit largestUnit,
    TemporalUnit smallestUnit,
    JS::Handle<Wrapped<PlainDateObject*>> plainRelativeTo,
    JS::Handle<CalendarRecord> calendar, DateDuration* result);

/**
 * AdjustRoundedDurationDays ( years, months, weeks, days, norm, increment,
 * unit, roundingMode, zonedRelativeTo, calendarRec, timeZoneRec,
 * precalculatedPlainDateTime )
 */
bool AdjustRoundedDurationDays(JSContext* cx,
                               const NormalizedDuration& duration,
                               Increment increment, TemporalUnit unit,
                               TemporalRoundingMode roundingMode,
                               JS::Handle<ZonedDateTime> relativeTo,
                               JS::Handle<CalendarRecord> calendar,
                               JS::Handle<TimeZoneRecord> timeZone,
                               const PlainDateTime& precalculatedPlainDateTime,
                               NormalizedDuration* result);

/**
 * RoundDuration ( years, months, weeks, days, norm, increment, unit,
 * roundingMode [ , plainRelativeTo [ , calendarRec [ , zonedRelativeTo [ ,
 * timeZoneRec [ , precalculatedPlainDateTime ] ] ] ] ] )
 */
NormalizedTimeDuration RoundDuration(const NormalizedTimeDuration& duration,
                                     Increment increment, TemporalUnit unit,
                                     TemporalRoundingMode roundingMode);

/**
 * RoundDuration ( years, months, weeks, days, norm, increment, unit,
 * roundingMode [ , plainRelativeTo [ , calendarRec [ , zonedRelativeTo [ ,
 * timeZoneRec [ , precalculatedPlainDateTime ] ] ] ] ] )
 */
bool RoundDuration(JSContext* cx, const NormalizedTimeDuration& duration,
                   Increment increment, TemporalUnit unit,
                   TemporalRoundingMode roundingMode,
                   NormalizedTimeDuration* result);

/**
 * RoundDuration ( years, months, weeks, days, norm, increment, unit,
 * roundingMode [ , plainRelativeTo [ , calendarRec [ , zonedRelativeTo [ ,
 * timeZoneRec [ , precalculatedPlainDateTime ] ] ] ] ] )
 */
bool RoundDuration(JSContext* cx, const NormalizedDuration& duration,
                   Increment increment, TemporalUnit unit,
                   TemporalRoundingMode roundingMode,
                   JS::Handle<Wrapped<PlainDateObject*>> plainRelativeTo,
                   JS::Handle<CalendarRecord> calendar,
                   NormalizedDuration* result);

/**
 * RoundDuration ( years, months, weeks, days, norm, increment, unit,
 * roundingMode [ , plainRelativeTo [ , calendarRec [ , zonedRelativeTo [ ,
 * timeZoneRec [ , precalculatedPlainDateTime ] ] ] ] ] )
 */
bool RoundDuration(JSContext* cx, const NormalizedDuration& duration,
                   Increment increment, TemporalUnit unit,
                   TemporalRoundingMode roundingMode,
                   JS::Handle<PlainDateObject*> plainRelativeTo,
                   JS::Handle<CalendarRecord> calendar,
                   JS::Handle<ZonedDateTime> zonedRelativeTo,
                   JS::Handle<TimeZoneRecord> timeZone,
                   const PlainDateTime& precalculatedPlainDateTime,
                   NormalizedDuration* result);

/**
 * DaysUntil ( earlier, later )
 */
int32_t DaysUntil(const PlainDate& earlier, const PlainDate& later);

} /* namespace js::temporal */

#endif /* builtin_temporal_Duration_h */
