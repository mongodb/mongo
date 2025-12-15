/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_ZonedDateTime_h
#define builtin_temporal_ZonedDateTime_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stdint.h>

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TimeZone.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

class JS_PUBLIC_API JSTracer;

namespace js {
struct ClassSpec;
}

namespace js::temporal {

class ZonedDateTimeObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t SECONDS_SLOT = 0;
  static constexpr uint32_t NANOSECONDS_SLOT = 1;
  static constexpr uint32_t TIMEZONE_SLOT = 2;
  static constexpr uint32_t CALENDAR_SLOT = 3;
  static constexpr uint32_t SLOT_COUNT = 4;

  /**
   * Extract the epoch nanoseconds fields from this ZonedDateTime object.
   */
  EpochNanoseconds epochNanoseconds() const {
    double seconds = getFixedSlot(SECONDS_SLOT).toNumber();
    MOZ_ASSERT(-8'640'000'000'000 <= seconds && seconds <= 8'640'000'000'000);

    int32_t nanoseconds = getFixedSlot(NANOSECONDS_SLOT).toInt32();
    MOZ_ASSERT(0 <= nanoseconds && nanoseconds <= 999'999'999);

    return {{int64_t(seconds), nanoseconds}};
  }

  TimeZoneValue timeZone() const {
    return TimeZoneValue(getFixedSlot(TIMEZONE_SLOT));
  }

  CalendarValue calendar() const {
    return CalendarValue(getFixedSlot(CALENDAR_SLOT));
  }

 private:
  static const ClassSpec classSpec_;
};

class MOZ_STACK_CLASS ZonedDateTime final {
  EpochNanoseconds epochNanoseconds_;
  TimeZoneValue timeZone_;
  CalendarValue calendar_;

 public:
  ZonedDateTime() = default;

  ZonedDateTime(const EpochNanoseconds& epochNanoseconds,
                const TimeZoneValue& timeZone, const CalendarValue& calendar)
      : epochNanoseconds_(epochNanoseconds),
        timeZone_(timeZone),
        calendar_(calendar) {
    MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));
    MOZ_ASSERT(timeZone);
    MOZ_ASSERT(calendar);
  }

  explicit ZonedDateTime(const ZonedDateTimeObject* obj)
      : ZonedDateTime(obj->epochNanoseconds(), obj->timeZone(),
                      obj->calendar()) {}

  const auto& epochNanoseconds() const { return epochNanoseconds_; }

  const auto& timeZone() const { return timeZone_; }

  const auto& calendar() const { return calendar_; }

  explicit operator bool() const { return !!timeZone_ && !!calendar_; }

  void trace(JSTracer* trc) {
    timeZone_.trace(trc);
    calendar_.trace(trc);
  }

  const auto* timeZoneDoNotUse() const { return &timeZone_; }
  const auto* calendarDoNotUse() const { return &calendar_; }
};

struct DifferenceSettings;
enum class TemporalDisambiguation;
enum class TemporalOffset;
enum class TemporalUnit;

/**
 * CreateTemporalZonedDateTime ( epochNanoseconds, timeZone, calendar [ ,
 * newTarget ] )
 */
ZonedDateTimeObject* CreateTemporalZonedDateTime(
    JSContext* cx, const EpochNanoseconds& epochNanoseconds,
    JS::Handle<TimeZoneValue> timeZone, JS::Handle<CalendarValue> calendar);

/**
 * AddZonedDateTime ( epochNanoseconds, timeZone, calendar, duration, overflow )
 */
bool AddZonedDateTime(JSContext* cx, JS::Handle<ZonedDateTime> zonedDateTime,
                      const InternalDuration& duration,
                      EpochNanoseconds* result);

/**
 * DifferenceZonedDateTimeWithRounding ( ns1, ns2, timeZone, calendar,
 * largestUnit, roundingIncrement, smallestUnit, roundingMode )
 */
bool DifferenceZonedDateTimeWithRounding(
    JSContext* cx, JS::Handle<ZonedDateTime> zonedDateTime,
    const EpochNanoseconds& ns2, const DifferenceSettings& settings,
    InternalDuration* result);

/**
 * DifferenceZonedDateTimeWithTotal ( ns1, ns2, timeZone, calendar, unit )
 */
bool DifferenceZonedDateTimeWithTotal(JSContext* cx,
                                      JS::Handle<ZonedDateTime> zonedDateTime,
                                      const EpochNanoseconds& ns2,
                                      TemporalUnit unit, double* result);

enum class OffsetBehaviour { Option, Exact, Wall };

enum class MatchBehaviour { MatchExactly, MatchMinutes };

/**
 * InterpretISODateTimeOffset ( isoDate, time, offsetBehaviour,
 * offsetNanoseconds, timeZone, disambiguation, offsetOption, matchBehaviour )
 */
bool InterpretISODateTimeOffset(
    JSContext* cx, const ISODateTime& dateTime, OffsetBehaviour offsetBehaviour,
    int64_t offsetNanoseconds, JS::Handle<TimeZoneValue> timeZone,
    TemporalDisambiguation disambiguation, TemporalOffset offsetOption,
    MatchBehaviour matchBehaviour, EpochNanoseconds* result);

/**
 * InterpretISODateTimeOffset ( isoDate, time, offsetBehaviour,
 * offsetNanoseconds, timeZone, disambiguation, offsetOption, matchBehaviour )
 */
bool InterpretISODateTimeOffset(
    JSContext* cx, const ISODate& isoDate, OffsetBehaviour offsetBehaviour,
    int64_t offsetNanoseconds, JS::Handle<TimeZoneValue> timeZone,
    TemporalDisambiguation disambiguation, TemporalOffset offsetOption,
    MatchBehaviour matchBehaviour, EpochNanoseconds* result);

} /* namespace js::temporal */

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::ZonedDateTime, Wrapper> {
  const auto& container() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  explicit operator bool() const { return bool(container()); }

  const auto& epochNanoseconds() const {
    return container().epochNanoseconds();
  }

  JS::Handle<temporal::TimeZoneValue> timeZone() const {
    return JS::Handle<temporal::TimeZoneValue>::fromMarkedLocation(
        container().timeZoneDoNotUse());
  }

  JS::Handle<temporal::CalendarValue> calendar() const {
    return JS::Handle<temporal::CalendarValue>::fromMarkedLocation(
        container().calendarDoNotUse());
  }
};

} /* namespace js */

#endif /* builtin_temporal_ZonedDateTime_h */
