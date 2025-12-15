/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_Calendar_h
#define builtin_temporal_Calendar_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stdint.h>
#include <string_view>

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

class JS_PUBLIC_API JSTracer;

namespace js {
struct ClassSpec;
}  // namespace js

namespace js::temporal {

enum class CalendarId : int32_t {
  ISO8601,

  // Thai Buddhist solar calendar.
  Buddhist,

  // Chinese lunisolar calendar.
  Chinese,

  // Coptic calendar.
  Coptic,

  // Korean lunisolar calendar.
  Dangi,

  // Ethiopian Amete Mihret calendar.
  Ethiopian,

  // Ethiopian Amete Alem calendar.
  EthiopianAmeteAlem,

  // Gregorian calendar.
  Gregorian,

  // Hebrew lunisolar calendar.
  Hebrew,

  // Indian national calendar.
  Indian,

  // Islamic lunar calendars.
  Islamic,
  IslamicCivil,
  IslamicRGSA,
  IslamicTabular,
  IslamicUmmAlQura,

  // Japanese calendar.
  Japanese,

  // Persian solar Hijri calendar.
  Persian,

  // Republic of China (ROC) calendar.
  ROC,
};

inline constexpr auto availableCalendars = {
    CalendarId::ISO8601,
    CalendarId::Buddhist,
    CalendarId::Chinese,
    CalendarId::Coptic,
    CalendarId::Dangi,
    CalendarId::Ethiopian,
    CalendarId::EthiopianAmeteAlem,
    CalendarId::Gregorian,
    CalendarId::Hebrew,
    CalendarId::Indian,
// See Bug 1950425, this calendar is only available on Nightly due to
// inconsistencies between ICU4X and ICU4C.
#ifdef NIGHTLY_BUILD
    CalendarId::Islamic,
#endif
    CalendarId::IslamicCivil,
// See Bug 1950425, this calendar is only available on Nightly due to
// inconsistencies between ICU4X and ICU4C.
#ifdef NIGHTLY_BUILD
    CalendarId::IslamicRGSA,
#endif
    CalendarId::IslamicTabular,
// See Bug 1950425, this calendar is only available on Nightly due to
// inconsistencies between ICU4X and ICU4C.
#ifdef NIGHTLY_BUILD
    CalendarId::IslamicUmmAlQura,
#endif
    CalendarId::Japanese,
    CalendarId::Persian,
    CalendarId::ROC,
};

/**
 * AvailableCalendars ( )
 */
constexpr auto& AvailableCalendars() { return availableCalendars; }

class CalendarObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t IDENTIFIER_SLOT = 0;
  static constexpr uint32_t SLOT_COUNT = 1;

  CalendarId identifier() const {
    return static_cast<CalendarId>(getFixedSlot(IDENTIFIER_SLOT).toInt32());
  }

 private:
  static const ClassSpec classSpec_;
};

/**
 * Calendar value, which is a string containing a canonical calendar identifier.
 */
class MOZ_STACK_CLASS CalendarValue final {
  JS::Value value_{};

 public:
  /**
   * Default initialize this CalendarValue.
   */
  CalendarValue() = default;

  /**
   * Default initialize this CalendarValue.
   */
  explicit CalendarValue(const JS::Value& value) : value_(value) {
    MOZ_ASSERT(value.isInt32());
  }

  /**
   * Initialize this CalendarValue with a canonical calendar identifier.
   */
  explicit CalendarValue(CalendarId calendarId)
      : value_(JS::Int32Value(static_cast<int32_t>(calendarId))) {}

  /**
   * Return true iff this CalendarValue is initialized with either a canonical
   * calendar identifier or a calendar object.
   */
  explicit operator bool() const { return !value_.isUndefined(); }

  /**
   * Return the slot Value representation of this CalendarValue.
   */
  JS::Value toSlotValue() const { return value_; }

  /**
   * Return the calendar identifier.
   */
  CalendarId identifier() const {
    return static_cast<CalendarId>(value_.toInt32());
  }

  void trace(JSTracer* trc);

  JS::Value* valueDoNotUse() { return &value_; }
  JS::Value const* valueDoNotUse() const { return &value_; }
};

struct DateDuration;
struct ISODate;
struct ISODateTime;
class PlainDate;
class PlainMonthDayObject;
class PlainMonthDay;
class PlainYearMonthObject;
class PlainYearMonth;
class CalendarFields;
enum class TemporalOverflow;
enum class TemporalUnit;

/**
 * ISODaysInMonth ( year, month )
 */
int32_t ISODaysInMonth(int32_t year, int32_t month);

/**
 * 21.4.1.12 MakeDay ( year, month, date )
 */
int32_t MakeDay(const ISODate& date);

/**
 * 21.4.1.13 MakeDate ( day, time )
 */
int64_t MakeDate(const ISODateTime& dateTime);

/**
 * Return the BCP 47 identifier of the calendar.
 */
std::string_view CalendarIdentifier(CalendarId calendarId);

/**
 * Return the BCP 47 identifier of the calendar.
 */
inline std::string_view CalendarIdentifier(const CalendarValue& calendar) {
  return CalendarIdentifier(calendar.identifier());
}

/**
 * CanonicalizeCalendar ( id )
 *
 * Return the case-normalized calendar identifier if |id| is a built-in calendar
 * identifier. Otherwise throws a RangeError.
 */
bool CanonicalizeCalendar(JSContext* cx, JS::Handle<JSString*> id,
                          JS::MutableHandle<CalendarValue> result);

/**
 * ToTemporalCalendarSlotValue ( temporalCalendarLike )
 */
bool ToTemporalCalendar(JSContext* cx,
                        JS::Handle<JS::Value> temporalCalendarLike,
                        JS::MutableHandle<CalendarValue> result);

/**
 * GetTemporalCalendarWithISODefault ( item )
 */
bool GetTemporalCalendarWithISODefault(JSContext* cx,
                                       JS::Handle<JSObject*> item,
                                       JS::MutableHandle<CalendarValue> result);

/**
 * CalendarDateAdd ( calendar, isoDate, duration, overflow )
 */
bool CalendarDateAdd(JSContext* cx, JS::Handle<CalendarValue> calendar,
                     const ISODate& isoDate, const DateDuration& duration,
                     TemporalOverflow overflow, ISODate* result);

/**
 * CalendarDateUntil ( calendar, one, two, largestUnit )
 */
bool CalendarDateUntil(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       const ISODate& one, const ISODate& two,
                       TemporalUnit largestUnit, DateDuration* result);

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[Era]] of the returned Calendar Date Record.
 */
bool CalendarEra(JSContext* cx, JS::Handle<CalendarValue> calendar,
                 const ISODate& date, JS::MutableHandle<JS::Value> result);

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[EraYear]] of the returned Calendar Date Record.
 */
bool CalendarEraYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                     const ISODate& date, JS::MutableHandle<JS::Value> result);
/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[Year]] of the returned Calendar Date Record.
 */
bool CalendarYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                  const ISODate& date, JS::MutableHandle<JS::Value> result);

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[Month]] of the returned Calendar Date Record.
 */
bool CalendarMonth(JSContext* cx, JS::Handle<CalendarValue> calendar,
                   const ISODate& date, JS::MutableHandle<JS::Value> result);

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[MonthCode]] of the returned Calendar Date Record.
 */
bool CalendarMonthCode(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       const ISODate& date,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[Day]] of the returned Calendar Date Record.
 */
bool CalendarDay(JSContext* cx, JS::Handle<CalendarValue> calendar,
                 const ISODate& date, JS::MutableHandle<JS::Value> result);

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[DayOfWeek]] of the returned Calendar Date Record.
 */
bool CalendarDayOfWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       const ISODate& date,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[DayOfYear]] of the returned Calendar Date Record.
 */
bool CalendarDayOfYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       const ISODate& date,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[Week]] field of the [[WeekOfYear]] of the returned
 * Calendar Date Record.
 */
bool CalendarWeekOfYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        const ISODate& date,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[Year]] field of the [[WeekOfYear]] of the returned
 * Calendar Date Record.
 */
bool CalendarYearOfWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        const ISODate& date,
                        JS::MutableHandle<JS::Value> result);

/**
 * * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[DaysInWeek]] of the returned Calendar Date Record.
 */
bool CalendarDaysInWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        const ISODate& date,
                        JS::MutableHandle<JS::Value> result);

/**
 * * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[DaysInMonth]] of the returned Calendar Date Record.
 */
bool CalendarDaysInMonth(JSContext* cx, JS::Handle<CalendarValue> calendar,
                         const ISODate& date,
                         JS::MutableHandle<JS::Value> result);

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[DaysInYear]] of the returned Calendar Date Record.
 */
bool CalendarDaysInYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        const ISODate& date,
                        JS::MutableHandle<JS::Value> result);

/**
 * * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[MonthsInYear]] of the returned Calendar Date Record.
 */
bool CalendarMonthsInYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                          const ISODate& date,
                          JS::MutableHandle<JS::Value> result);

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * When accessing the [[InLeapYear]] of the returned Calendar Date Record.
 */
bool CalendarInLeapYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        const ISODate& date,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarDateFromFields ( calendar, fields, overflow )
 */
bool CalendarDateFromFields(JSContext* cx, JS::Handle<CalendarValue> calendar,
                            JS::Handle<CalendarFields> fields,
                            TemporalOverflow overflow,
                            MutableHandle<PlainDate> result);

/**
 * CalendarYearMonthFromFields ( calendar, fields, overflow )
 */
bool CalendarYearMonthFromFields(JSContext* cx,
                                 JS::Handle<CalendarValue> calendar,
                                 JS::Handle<CalendarFields> fields,
                                 TemporalOverflow overflow,
                                 JS::MutableHandle<PlainYearMonth> result);

/**
 * CalendarMonthDayFromFields ( calendar, fields, overflow )
 */
bool CalendarMonthDayFromFields(JSContext* cx,
                                JS::Handle<CalendarValue> calendar,
                                JS::Handle<CalendarFields> fields,
                                TemporalOverflow overflow,
                                JS::MutableHandle<PlainMonthDay> result);

/**
 * CalendarEquals ( one, two )
 */
inline bool CalendarEquals(const CalendarValue& one, const CalendarValue& two) {
  // Steps 1-2.
  return one.identifier() == two.identifier();
}

// Helper for MutableWrappedPtrOperations.
bool WrapCalendarValue(JSContext* cx, JS::MutableHandle<JS::Value> calendar);

} /* namespace js::temporal */

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::CalendarValue, Wrapper> {
  const auto& container() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  explicit operator bool() const { return bool(container()); }

  JS::Handle<JS::Value> toSlotValue() const {
    return JS::Handle<JS::Value>::fromMarkedLocation(
        container().valueDoNotUse());
  }

  temporal::CalendarId identifier() const { return container().identifier(); }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<temporal::CalendarValue, Wrapper>
    : public WrappedPtrOperations<temporal::CalendarValue, Wrapper> {
  auto& container() { return static_cast<Wrapper*>(this)->get(); }

  JS::MutableHandle<JS::Value> toMutableValue() {
    return JS::MutableHandle<JS::Value>::fromMarkedLocation(
        container().valueDoNotUse());
  }

 public:
  bool wrap(JSContext* cx) {
    return temporal::WrapCalendarValue(cx, toMutableValue());
  }
};

} /* namespace js */

#endif /* builtin_temporal_Calendar_h */
