/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_PlainDate_h
#define builtin_temporal_PlainDate_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <initializer_list>
#include <stdint.h>

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/PlainDateTime.h"
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

class PlainDateObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  // TODO: Consider compacting fields to reduce object size.
  //
  // ceil(log2(271821)) + ceil(log2(12)) + ceil(log2(31)) = 28 bits are
  // needed to store a date value in a single int32.

  static constexpr uint32_t ISO_YEAR_SLOT = 0;
  static constexpr uint32_t ISO_MONTH_SLOT = 1;
  static constexpr uint32_t ISO_DAY_SLOT = 2;
  static constexpr uint32_t CALENDAR_SLOT = 3;
  static constexpr uint32_t SLOT_COUNT = 4;

  int32_t isoYear() const { return getFixedSlot(ISO_YEAR_SLOT).toInt32(); }

  int32_t isoMonth() const { return getFixedSlot(ISO_MONTH_SLOT).toInt32(); }

  int32_t isoDay() const { return getFixedSlot(ISO_DAY_SLOT).toInt32(); }

  CalendarValue calendar() const {
    return CalendarValue(getFixedSlot(CALENDAR_SLOT));
  }

 private:
  static const ClassSpec classSpec_;
};

class MOZ_STACK_CLASS PlainDateWithCalendar final {
  PlainDate date_;
  CalendarValue calendar_;

 public:
  PlainDateWithCalendar() = default;

  PlainDateWithCalendar(const PlainDate& date, const CalendarValue& calendar)
      : date_(date), calendar_(calendar) {
    MOZ_ASSERT(ISODateTimeWithinLimits(date));
  }

  const auto& date() const { return date_; }
  const auto& calendar() const { return calendar_; }

  // Allow implicit conversion to a calendar-less PlainDate.
  operator const PlainDate&() const { return date(); }

  void trace(JSTracer* trc) { calendar_.trace(trc); }

  const auto* calendarDoNotUse() const { return &calendar_; }
};

/**
 * Extract the date fields from the PlainDate object.
 */
inline PlainDate ToPlainDate(const PlainDateObject* date) {
  return {date->isoYear(), date->isoMonth(), date->isoDay()};
}

enum class TemporalOverflow;
enum class TemporalUnit;
class DurationObject;
class ZonedDateTimeObject;

#ifdef DEBUG
/**
 * IsValidISODate ( year, month, day )
 */
bool IsValidISODate(const PlainDate& date);

/**
 * IsValidISODate ( year, month, day )
 */
bool IsValidISODate(double year, double month, double day);
#endif

/**
 * IsValidISODate ( year, month, day )
 */
bool ThrowIfInvalidISODate(JSContext* cx, const PlainDate& date);

/**
 * IsValidISODate ( year, month, day )
 */
bool ThrowIfInvalidISODate(JSContext* cx, double year, double month,
                           double day);

/**
 * ToTemporalDate ( item [ , options ] )
 */
bool ToTemporalDate(JSContext* cx, JS::Handle<JS::Value> item,
                    PlainDate* result);

/**
 * ToTemporalDate ( item [ , options ] )
 */
bool ToTemporalDate(JSContext* cx, JS::Handle<JS::Value> item,
                    JS::MutableHandle<PlainDateWithCalendar> result);

/**
 * CreateTemporalDate ( isoYear, isoMonth, isoDay, calendar [ , newTarget ] )
 */
PlainDateObject* CreateTemporalDate(JSContext* cx, const PlainDate& date,
                                    JS::Handle<CalendarValue> calendar);

/**
 * CreateTemporalDate ( isoYear, isoMonth, isoDay, calendar [ , newTarget ] )
 */
bool CreateTemporalDate(JSContext* cx, const PlainDate& date,
                        JS::Handle<CalendarValue> calendar,
                        JS::MutableHandle<PlainDateWithCalendar> result);

/**
 * RegulateISODate ( year, month, day, overflow )
 */
bool RegulateISODate(JSContext* cx, const PlainDate& date,
                     TemporalOverflow overflow, PlainDate* result);

struct RegulatedISODate final {
  double year = 0;
  int32_t month = 0;
  int32_t day = 0;
};

/**
 * RegulateISODate ( year, month, day, overflow )
 */
bool RegulateISODate(JSContext* cx, double year, double month, double day,
                     TemporalOverflow overflow, RegulatedISODate* result);

/**
 * AddISODate ( year, month, day, years, months, weeks, days, overflow )
 */
bool AddISODate(JSContext* cx, const PlainDate& date,
                const DateDuration& duration, TemporalOverflow overflow,
                PlainDate* result);

/**
 * AddDate ( calendarRec, plainDate, duration [ , options ] )
 */
Wrapped<PlainDateObject*> AddDate(JSContext* cx,
                                  JS::Handle<CalendarRecord> calendar,
                                  JS::Handle<Wrapped<PlainDateObject*>> date,
                                  const DateDuration& duration);

/**
 * AddDate ( calendarRec, plainDate, duration [ , options ] )
 */
Wrapped<PlainDateObject*> AddDate(JSContext* cx,
                                  JS::Handle<CalendarRecord> calendar,
                                  JS::Handle<Wrapped<PlainDateObject*>> date,
                                  const DateDuration& duration,
                                  JS::Handle<JSObject*> options);

/**
 * AddDate ( calendarRec, plainDate, duration [ , options ] )
 */
bool AddDate(JSContext* cx, JS::Handle<CalendarRecord> calendar,
             const PlainDate& date, const DateDuration& duration,
             JS::Handle<JSObject*> options, PlainDate* result);

/**
 * AddDate ( calendarRec, plainDate, duration [ , options ] )
 */
bool AddDate(JSContext* cx, JS::Handle<CalendarRecord> calendar,
             JS::Handle<Wrapped<PlainDateObject*>> date,
             const DateDuration& duration, PlainDate* result);

/**
 * DifferenceISODate ( y1, m1, d1, y2, m2, d2, largestUnit )
 */
DateDuration DifferenceISODate(const PlainDate& start, const PlainDate& end,
                               TemporalUnit largestUnit);

/**
 * DifferenceDate ( calendarRec, one, two, options )
 */
bool DifferenceDate(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                    JS::Handle<Wrapped<PlainDateObject*>> one,
                    JS::Handle<Wrapped<PlainDateObject*>> two,
                    TemporalUnit largestUnit, JS::Handle<PlainObject*> options,
                    DateDuration* result);

/**
 * DifferenceDate ( calendarRec, one, two, options )
 */
bool DifferenceDate(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                    JS::Handle<Wrapped<PlainDateObject*>> one,
                    JS::Handle<Wrapped<PlainDateObject*>> two,
                    TemporalUnit largestUnit, DateDuration* result);

/**
 * DifferenceDate ( calendarRec, one, two, options )
 */
bool DifferenceDate(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                    const PlainDate& one, const PlainDate& two,
                    TemporalUnit largestUnit, JS::Handle<PlainObject*> options,
                    DateDuration* result);

/**
 * DifferenceDate ( calendarRec, one, two, options )
 */
bool DifferenceDate(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                    const PlainDate& one, const PlainDate& two,
                    TemporalUnit largestUnit, DateDuration* result);

/**
 * CompareISODate ( y1, m1, d1, y2, m2, d2 )
 */
int32_t CompareISODate(const PlainDate& one, const PlainDate& two);

/**
 * BalanceISODate ( year, month, day )
 */
bool BalanceISODate(JSContext* cx, const PlainDate& date, int64_t days,
                    PlainDate* result);

/**
 * BalanceISODate ( year, month, day )
 */
PlainDate BalanceISODate(int32_t year, int32_t month, int32_t day);

/**
 * BalanceISODate ( year, month, day )
 */
PlainDate BalanceISODateNew(int32_t year, int32_t month, int32_t day);

/**
 * Return true when accessing the calendar fields |fieldNames| can be optimized.
 * Otherwise returns false.
 */
bool IsBuiltinAccess(JSContext* cx, JS::Handle<PlainDateObject*> date,
                     std::initializer_list<CalendarField> fieldNames);

} /* namespace js::temporal */

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::PlainDateWithCalendar, Wrapper> {
  const auto& container() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  const auto& date() const { return container().date(); }

  JS::Handle<temporal::CalendarValue> calendar() const {
    return JS::Handle<temporal::CalendarValue>::fromMarkedLocation(
        container().calendarDoNotUse());
  }

  // Allow implicit conversion to a calendar-less PlainDate.
  operator const temporal::PlainDate&() const { return date(); }
};

}  // namespace js

#endif /* builtin_temporal_PlainDate_h */
