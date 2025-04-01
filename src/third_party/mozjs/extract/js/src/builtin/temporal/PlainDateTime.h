/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_PlainDateTime_h
#define builtin_temporal_PlainDateTime_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stdint.h>

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/Wrapped.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

class JS_PUBLIC_API JSTracer;

namespace js {
struct ClassSpec;
class PlainObject;
}  // namespace js

namespace js::temporal {

class PlainDateTimeObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  // TODO: Consider compacting fields to reduce object size.
  //
  // See also PlainDateObject and PlainTimeObject.

  static constexpr uint32_t ISO_YEAR_SLOT = 0;
  static constexpr uint32_t ISO_MONTH_SLOT = 1;
  static constexpr uint32_t ISO_DAY_SLOT = 2;
  static constexpr uint32_t ISO_HOUR_SLOT = 3;
  static constexpr uint32_t ISO_MINUTE_SLOT = 4;
  static constexpr uint32_t ISO_SECOND_SLOT = 5;
  static constexpr uint32_t ISO_MILLISECOND_SLOT = 6;
  static constexpr uint32_t ISO_MICROSECOND_SLOT = 7;
  static constexpr uint32_t ISO_NANOSECOND_SLOT = 8;
  static constexpr uint32_t CALENDAR_SLOT = 9;
  static constexpr uint32_t SLOT_COUNT = 10;

  int32_t isoYear() const { return getFixedSlot(ISO_YEAR_SLOT).toInt32(); }

  int32_t isoMonth() const { return getFixedSlot(ISO_MONTH_SLOT).toInt32(); }

  int32_t isoDay() const { return getFixedSlot(ISO_DAY_SLOT).toInt32(); }

  int32_t isoHour() const { return getFixedSlot(ISO_HOUR_SLOT).toInt32(); }

  int32_t isoMinute() const { return getFixedSlot(ISO_MINUTE_SLOT).toInt32(); }

  int32_t isoSecond() const { return getFixedSlot(ISO_SECOND_SLOT).toInt32(); }

  int32_t isoMillisecond() const {
    return getFixedSlot(ISO_MILLISECOND_SLOT).toInt32();
  }

  int32_t isoMicrosecond() const {
    return getFixedSlot(ISO_MICROSECOND_SLOT).toInt32();
  }

  int32_t isoNanosecond() const {
    return getFixedSlot(ISO_NANOSECOND_SLOT).toInt32();
  }

  CalendarValue calendar() const {
    return CalendarValue(getFixedSlot(CALENDAR_SLOT));
  }

 private:
  static const ClassSpec classSpec_;
};

/**
 * Extract the date fields from the PlainDateTime object.
 */
inline PlainDate ToPlainDate(const PlainDateTimeObject* dateTime) {
  return {dateTime->isoYear(), dateTime->isoMonth(), dateTime->isoDay()};
}

/**
 * Extract the time fields from the PlainDateTime object.
 */
inline PlainTime ToPlainTime(const PlainDateTimeObject* dateTime) {
  return {dateTime->isoHour(),        dateTime->isoMinute(),
          dateTime->isoSecond(),      dateTime->isoMillisecond(),
          dateTime->isoMicrosecond(), dateTime->isoNanosecond()};
}

/**
 * Extract the date-time fields from the PlainDateTime object.
 */
inline PlainDateTime ToPlainDateTime(const PlainDateTimeObject* dateTime) {
  return {ToPlainDate(dateTime), ToPlainTime(dateTime)};
}

class Increment;
enum class TemporalRoundingMode;
enum class TemporalUnit;

#ifdef DEBUG
/**
 * IsValidISODateTime ( year, month, day, hour, minute, second, millisecond,
 * microsecond, nanosecond )
 */
bool IsValidISODateTime(const PlainDateTime& dateTime);
#endif

/**
 * ISODateTimeWithinLimits ( year, month, day, hour, minute, second,
 * millisecond, microsecond, nanosecond )
 */
bool ISODateTimeWithinLimits(const PlainDateTime& dateTime);

/**
 * ISODateTimeWithinLimits ( year, month, day, hour, minute, second,
 * millisecond, microsecond, nanosecond )
 */
bool ISODateTimeWithinLimits(const PlainDate& date);

/**
 * ISODateTimeWithinLimits ( year, month, day, hour, minute, second,
 * millisecond, microsecond, nanosecond )
 */
bool ISODateTimeWithinLimits(double year, double month, double day);

/**
 * CreateTemporalDateTime ( isoYear, isoMonth, isoDay, hour, minute, second,
 * millisecond, microsecond, nanosecond, calendar [ , newTarget ] )
 */
PlainDateTimeObject* CreateTemporalDateTime(JSContext* cx,
                                            const PlainDateTime& dateTime,
                                            JS::Handle<CalendarValue> calendar);

/**
 * ToTemporalDateTime ( item [ , options ] )
 */
Wrapped<PlainDateTimeObject*> ToTemporalDateTime(JSContext* cx,
                                                 JS::Handle<JS::Value> item);

/**
 * ToTemporalDateTime ( item [ , options ] )
 */
bool ToTemporalDateTime(JSContext* cx, JS::Handle<JS::Value> item,
                        PlainDateTime* result);

/**
 * InterpretTemporalDateTimeFields ( calendarRec, fields, options )
 */
bool InterpretTemporalDateTimeFields(JSContext* cx,
                                     JS::Handle<CalendarRecord> calendar,
                                     JS::Handle<PlainObject*> fields,
                                     JS::Handle<PlainObject*> options,
                                     PlainDateTime* result);

/**
 * InterpretTemporalDateTimeFields ( calendarRec, fields, options )
 */
bool InterpretTemporalDateTimeFields(JSContext* cx,
                                     JS::Handle<CalendarRecord> calendar,
                                     JS::Handle<PlainObject*> fields,
                                     PlainDateTime* result);

/**
 * RoundISODateTime ( year, month, day, hour, minute, second, millisecond,
 * microsecond, nanosecond, increment, unit, roundingMode )
 */
PlainDateTime RoundISODateTime(const PlainDateTime& dateTime,
                               Increment increment, TemporalUnit unit,
                               TemporalRoundingMode roundingMode);

class MOZ_STACK_CLASS PlainDateTimeWithCalendar final {
  PlainDateTime dateTime_;
  CalendarValue calendar_;

 public:
  PlainDateTimeWithCalendar() = default;

  PlainDateTimeWithCalendar(const PlainDateTime& dateTime,
                            const CalendarValue& calendar)
      : dateTime_(dateTime), calendar_(calendar) {
    MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));
  }

  explicit PlainDateTimeWithCalendar(const PlainDateTimeObject* dateTime)
      : PlainDateTimeWithCalendar(ToPlainDateTime(dateTime),
                                  dateTime->calendar()) {}

  const auto& dateTime() const { return dateTime_; }
  const auto& date() const { return dateTime_.date; }
  const auto& time() const { return dateTime_.time; }
  const auto& calendar() const { return calendar_; }

  // Allow implicit conversion to a calendar-less PlainDateTime.
  operator const PlainDateTime&() const { return dateTime(); }

  void trace(JSTracer* trc) { calendar_.trace(trc); }

  const auto* calendarDoNotUse() const { return &calendar_; }
};

/**
 * Extract the date-time fields from the PlainDateTimeWithCalendar object.
 */
inline const auto& ToPlainDateTime(const PlainDateTimeWithCalendar& dateTime) {
  return dateTime.dateTime();
}

/**
 * CreateTemporalDateTime ( isoYear, isoMonth, isoDay, hour, minute, second,
 * millisecond, microsecond, nanosecond, calendar [ , newTarget ] )
 */
bool CreateTemporalDateTime(
    JSContext* cx, const PlainDateTime& dateTime,
    JS::Handle<CalendarValue> calendar,
    JS::MutableHandle<PlainDateTimeWithCalendar> result);

} /* namespace js::temporal */

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::PlainDateTimeWithCalendar, Wrapper> {
  const auto& container() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  const auto& dateTime() const { return container().dateTime(); }
  const auto& date() const { return container().date(); }
  const auto& time() const { return container().time(); }

  auto calendar() const {
    return JS::Handle<temporal::CalendarValue>::fromMarkedLocation(
        container().calendarDoNotUse());
  }

  // Allow implicit conversion to a calendar-less PlainDateTime.
  operator const temporal::PlainDateTime&() const { return dateTime(); }
};

}  // namespace js

#endif /* builtin_temporal_PlainDateTime_h */
