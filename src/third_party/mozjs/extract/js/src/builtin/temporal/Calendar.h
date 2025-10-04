/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_Calendar_h
#define builtin_temporal_Calendar_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/EnumSet.h"

#include <initializer_list>
#include <stdint.h>

#include "builtin/temporal/Wrapped.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/NativeObject.h"
#include "vm/StringType.h"

class JS_PUBLIC_API JSTracer;

namespace js {
struct ClassSpec;
class PlainObject;
}  // namespace js

namespace js::temporal {

enum class CalendarId : int32_t {
  ISO8601,
};

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
 * Calendar value, which is either a string containing a canonical calendar
 * identifier or an object.
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
    MOZ_ASSERT(value.isInt32() || value.isObject());
  }

  /**
   * Initialize this CalendarValue with a canonical calendar identifier.
   */
  explicit CalendarValue(CalendarId calendarId)
      : value_(JS::Int32Value(static_cast<int32_t>(calendarId))) {}

  /**
   * Initialize this CalendarValue with a calendar object.
   */
  explicit CalendarValue(JSObject* calendar)
      : value_(JS::ObjectValue(*calendar)) {}

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
   * Return true if this CalendarValue is a string.
   */
  bool isString() const { return value_.isInt32(); }

  /**
   * Return true if this CalendarValue is an object.
   */
  bool isObject() const { return value_.isObject(); }

  /**
   * Return the calendar identifier.
   */
  CalendarId toString() const {
    return static_cast<CalendarId>(value_.toInt32());
  }

  /**
   * Return the calendar object.
   */
  JSObject* toObject() const { return &value_.toObject(); }

  void trace(JSTracer* trc);

  JS::Value* valueDoNotUse() { return &value_; }
  JS::Value const* valueDoNotUse() const { return &value_; }
};

enum class CalendarMethod {
  DateAdd,
  DateFromFields,
  DateUntil,
  Day,
  Fields,
  MergeFields,
  MonthDayFromFields,
  YearMonthFromFields,
};

class MOZ_STACK_CLASS CalendarRecord final {
  CalendarValue receiver_;

  // Null unless non-builtin calendar methods are used.
  JSObject* dateAdd_ = nullptr;
  JSObject* dateFromFields_ = nullptr;
  JSObject* dateUntil_ = nullptr;
  JSObject* day_ = nullptr;
  JSObject* fields_ = nullptr;
  JSObject* mergeFields_ = nullptr;
  JSObject* monthDayFromFields_ = nullptr;
  JSObject* yearMonthFromFields_ = nullptr;

#ifdef DEBUG
  mozilla::EnumSet<CalendarMethod> lookedUp_{};
#endif

 public:
  /**
   * Default initialize this CalendarRecord.
   */
  CalendarRecord() = default;

  explicit CalendarRecord(const CalendarValue& receiver)
      : receiver_(receiver) {}

  const auto& receiver() const { return receiver_; }
  auto* dateAdd() const { return dateAdd_; }
  auto* dateFromFields() const { return dateFromFields_; }
  auto* dateUntil() const { return dateUntil_; }
  auto* day() const { return day_; }
  auto* fields() const { return fields_; }
  auto* mergeFields() const { return mergeFields_; }
  auto* monthDayFromFields() const { return monthDayFromFields_; }
  auto* yearMonthFromFields() const { return yearMonthFromFields_; }

#ifdef DEBUG
  auto& lookedUp() const { return lookedUp_; }
  auto& lookedUp() { return lookedUp_; }
#endif

  // Helper methods for (Mutable)WrappedPtrOperations.
  auto* receiverDoNotUse() const { return &receiver_; }
  auto* dateAddDoNotUse() const { return &dateAdd_; }
  auto* dateAddDoNotUse() { return &dateAdd_; }
  auto* dateFromFieldsDoNotUse() const { return &dateFromFields_; }
  auto* dateFromFieldsDoNotUse() { return &dateFromFields_; }
  auto* dateUntilDoNotUse() const { return &dateUntil_; }
  auto* dateUntilDoNotUse() { return &dateUntil_; }
  auto* dayDoNotUse() const { return &day_; }
  auto* dayDoNotUse() { return &day_; }
  auto* fieldsDoNotUse() const { return &fields_; }
  auto* fieldsDoNotUse() { return &fields_; }
  auto* mergeFieldsDoNotUse() const { return &mergeFields_; }
  auto* mergeFieldsDoNotUse() { return &mergeFields_; }
  auto* monthDayFromFieldsDoNotUse() const { return &monthDayFromFields_; }
  auto* monthDayFromFieldsDoNotUse() { return &monthDayFromFields_; }
  auto* yearMonthFromFieldsDoNotUse() const { return &yearMonthFromFields_; }
  auto* yearMonthFromFieldsDoNotUse() { return &yearMonthFromFields_; }

  // Trace implementation.
  void trace(JSTracer* trc);
};

struct DateDuration;
struct Duration;
struct PlainDate;
struct PlainDateTime;
class DurationObject;
class PlainDateObject;
class PlainDateTimeObject;
class PlainMonthDayObject;
class PlainYearMonthObject;
enum class ShowCalendar;
enum class TemporalOverflow;
enum class TemporalUnit;

/**
 * ISODaysInYear ( year )
 */
int32_t ISODaysInYear(int32_t year);

/**
 * ISODaysInMonth ( year, month )
 */
int32_t ISODaysInMonth(int32_t year, int32_t month);

/**
 * ISODaysInMonth ( year, month )
 */
int32_t ISODaysInMonth(double year, int32_t month);

/**
 * ToISODayOfYear ( year, month, day )
 */
int32_t ToISODayOfYear(const PlainDate& date);

/**
 * 21.4.1.12 MakeDay ( year, month, date )
 */
int32_t MakeDay(const PlainDate& date);

/**
 * 21.4.1.13 MakeDate ( day, time )
 */
int64_t MakeDate(const PlainDateTime& dateTime);

/**
 * 21.4.1.13 MakeDate ( day, time )
 */
int64_t MakeDate(int32_t year, int32_t month, int32_t day);

/**
 * Return the case-normalized calendar identifier if |id| is a built-in calendar
 * identifier. Otherwise throws a RangeError.
 */
bool ToBuiltinCalendar(JSContext* cx, JS::Handle<JSString*> id,
                       JS::MutableHandle<CalendarValue> result);

/**
 * ToTemporalCalendarSlotValue ( temporalCalendarLike [ , default ] )
 */
bool ToTemporalCalendar(JSContext* cx,
                        JS::Handle<JS::Value> temporalCalendarLike,
                        JS::MutableHandle<CalendarValue> result);

/**
 * ToTemporalCalendarSlotValue ( temporalCalendarLike [ , default ] )
 */
bool ToTemporalCalendarWithISODefault(
    JSContext* cx, JS::Handle<JS::Value> temporalCalendarLike,
    JS::MutableHandle<CalendarValue> result);

/**
 * GetTemporalCalendarWithISODefault ( item )
 */
bool GetTemporalCalendarWithISODefault(JSContext* cx,
                                       JS::Handle<JSObject*> item,
                                       JS::MutableHandle<CalendarValue> result);

/**
 * ToTemporalCalendarIdentifier ( calendarSlotValue )
 */
JSLinearString* ToTemporalCalendarIdentifier(
    JSContext* cx, JS::Handle<CalendarValue> calendar);

/**
 * ToTemporalCalendarObject ( calendarSlotValue )
 */
JSObject* ToTemporalCalendarObject(JSContext* cx,
                                   JS::Handle<CalendarValue> calendar);

bool ToTemporalCalendar(JSContext* cx, const CalendarValue& calendar,
                        JS::MutableHandle<JS::Value> result);

enum class CalendarField {
  Year,
  Month,
  MonthCode,
  Day,
};

using CalendarFieldNames = JS::StackGCVector<JS::PropertyKey>;

/**
 * CalendarFields ( calendarRec, fieldNames )
 */
bool CalendarFields(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                    mozilla::EnumSet<CalendarField> fieldNames,
                    JS::MutableHandle<CalendarFieldNames> result);

/**
 * CalendarMergeFields ( calendarRec, fields, additionalFields )
 */
JSObject* CalendarMergeFields(JSContext* cx,
                              JS::Handle<CalendarRecord> calendar,
                              JS::Handle<PlainObject*> fields,
                              JS::Handle<PlainObject*> additionalFields);

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
Wrapped<PlainDateObject*> CalendarDateAdd(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<Wrapped<PlainDateObject*>> date, const DateDuration& duration);

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
Wrapped<PlainDateObject*> CalendarDateAdd(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<Wrapped<PlainDateObject*>> date, const Duration& duration,
    JS::Handle<JSObject*> options);

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
Wrapped<PlainDateObject*> CalendarDateAdd(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<Wrapped<PlainDateObject*>> date,
    JS::Handle<Wrapped<DurationObject*>> duration);

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
Wrapped<PlainDateObject*> CalendarDateAdd(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<Wrapped<PlainDateObject*>> date,
    JS::Handle<Wrapped<DurationObject*>> duration,
    JS::Handle<JSObject*> options);

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
bool CalendarDateAdd(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                     const PlainDate& date, const DateDuration& duration,
                     PlainDate* result);

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
bool CalendarDateAdd(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                     const PlainDate& date, const DateDuration& duration,
                     JS::Handle<JSObject*> options, PlainDate* result);

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
bool CalendarDateAdd(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                     JS::Handle<Wrapped<PlainDateObject*>> date,
                     const DateDuration& duration, PlainDate* result);

/**
 * CalendarDateUntil ( calendarRec, one, two, options )
 */
bool CalendarDateUntil(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                       const PlainDate& one, const PlainDate& two,
                       TemporalUnit largestUnit, DateDuration* result);

/**
 * CalendarDateUntil ( calendarRec, one, two, options )
 */
bool CalendarDateUntil(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                       const PlainDate& one, const PlainDate& two,
                       TemporalUnit largestUnit,
                       JS::Handle<PlainObject*> options, DateDuration* result);

/**
 * CalendarDateUntil ( calendarRec, one, two, options )
 */
bool CalendarDateUntil(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                       JS::Handle<Wrapped<PlainDateObject*>> one,
                       JS::Handle<Wrapped<PlainDateObject*>> two,
                       TemporalUnit largestUnit, DateDuration* result);

/**
 * CalendarDateUntil ( calendarRec, one, two, options )
 */
bool CalendarDateUntil(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                       JS::Handle<Wrapped<PlainDateObject*>> one,
                       JS::Handle<Wrapped<PlainDateObject*>> two,
                       TemporalUnit largestUnit,
                       JS::Handle<PlainObject*> options, DateDuration* result);

/**
 * CalendarEra ( calendar, dateLike )
 */
bool CalendarEra(JSContext* cx, JS::Handle<CalendarValue> calendar,
                 JS::Handle<PlainDateObject*> dateLike,
                 JS::MutableHandle<JS::Value> result);

/**
 * CalendarEra ( calendar, dateLike )
 */
bool CalendarEra(JSContext* cx, JS::Handle<CalendarValue> calendar,
                 JS::Handle<PlainDateTimeObject*> dateLike,
                 JS::MutableHandle<JS::Value> result);

/**
 * CalendarEra ( calendar, dateLike )
 */
bool CalendarEra(JSContext* cx, JS::Handle<CalendarValue> calendar,
                 JS::Handle<PlainYearMonthObject*> dateLike,
                 JS::MutableHandle<JS::Value> result);

/**
 * CalendarEra ( calendar, dateLike )
 */
bool CalendarEra(JSContext* cx, JS::Handle<CalendarValue> calendar,
                 const PlainDateTime& dateTime,
                 JS::MutableHandle<JS::Value> result);

/**
 * CalendarEraYear ( calendar, dateLike )
 */
bool CalendarEraYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                     JS::Handle<PlainDateObject*> dateLike,
                     JS::MutableHandle<JS::Value> result);

/**
 * CalendarEraYear ( calendar, dateLike )
 */
bool CalendarEraYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                     JS::Handle<PlainDateTimeObject*> dateLike,
                     JS::MutableHandle<JS::Value> result);

/**
 * CalendarEraYear ( calendar, dateLike )
 */
bool CalendarEraYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                     JS::Handle<PlainYearMonthObject*> dateLike,
                     JS::MutableHandle<JS::Value> result);

/**
 * CalendarEraYear ( calendar, dateLike )
 */
bool CalendarEraYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                     const PlainDateTime& dateTime,
                     JS::MutableHandle<JS::Value> result);

/**
 * CalendarYear ( calendar, dateLike )
 */
bool CalendarYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                  JS::Handle<PlainDateObject*> dateLike,
                  JS::MutableHandle<JS::Value> result);

/**
 * CalendarYear ( calendar, dateLike )
 */
bool CalendarYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                  JS::Handle<PlainDateTimeObject*> dateLike,
                  JS::MutableHandle<JS::Value> result);

/**
 * CalendarYear ( calendar, dateLike )
 */
bool CalendarYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                  JS::Handle<PlainYearMonthObject*> dateLike,
                  JS::MutableHandle<JS::Value> result);

/**
 * CalendarYear ( calendar, dateLike )
 */
bool CalendarYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                  const PlainDateTime& dateTime,
                  JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonth ( calendar, dateLike )
 */
bool CalendarMonth(JSContext* cx, JS::Handle<CalendarValue> calendar,
                   JS::Handle<PlainDateObject*> dateLike,
                   JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonth ( calendar, dateLike )
 */
bool CalendarMonth(JSContext* cx, JS::Handle<CalendarValue> calendar,
                   JS::Handle<PlainDateTimeObject*> dateLike,
                   JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonth ( calendar, dateLike )
 */
bool CalendarMonth(JSContext* cx, JS::Handle<CalendarValue> calendar,
                   JS::Handle<PlainYearMonthObject*> dateLike,
                   JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonth ( calendar, dateLike )
 */
bool CalendarMonth(JSContext* cx, JS::Handle<CalendarValue> calendar,
                   const PlainDateTime& dateTime,
                   JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonthCode ( calendar, dateLike )
 */
bool CalendarMonthCode(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       JS::Handle<PlainDateObject*> dateLike,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonthCode ( calendar, dateLike )
 */
bool CalendarMonthCode(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       JS::Handle<PlainDateTimeObject*> dateLike,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonthCode ( calendar, dateLike )
 */
bool CalendarMonthCode(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       JS::Handle<PlainMonthDayObject*> dateLike,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonthCode ( calendar, dateLike )
 */
bool CalendarMonthCode(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       JS::Handle<PlainYearMonthObject*> dateLike,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonthCode ( calendar, dateLike )
 */
bool CalendarMonthCode(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       const PlainDateTime& dateTime,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarDay ( calendarRec, dateLike )
 */
bool CalendarDay(JSContext* cx, JS::Handle<CalendarValue> calendar,
                 JS::Handle<PlainDateObject*> dateLike,
                 JS::MutableHandle<JS::Value> result);

/**
 * CalendarDay ( calendarRec, dateLike )
 */
bool CalendarDay(JSContext* cx, JS::Handle<CalendarValue> calendar,
                 JS::Handle<PlainDateTimeObject*> dateLike,
                 JS::MutableHandle<JS::Value> result);

/**
 * CalendarDay ( calendarRec, dateLike )
 */
bool CalendarDay(JSContext* cx, JS::Handle<CalendarValue> calendar,
                 JS::Handle<PlainMonthDayObject*> dateLike,
                 JS::MutableHandle<JS::Value> result);

/**
 * CalendarDay ( calendarRec, dateLike )
 */
bool CalendarDay(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                 const PlainDate& date, JS::MutableHandle<JS::Value> result);

/**
 * CalendarDay ( calendarRec, dateLike )
 */
bool CalendarDay(JSContext* cx, JS::Handle<CalendarRecord> calendar,
                 const PlainDateTime& dateTime,
                 JS::MutableHandle<JS::Value> result);

/**
 * CalendarDayOfWeek ( calendar, dateLike )
 */
bool CalendarDayOfWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       JS::Handle<PlainDateObject*> dateLike,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarDayOfWeek ( calendar, dateLike )
 */
bool CalendarDayOfWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       JS::Handle<PlainDateTimeObject*> dateLike,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarDayOfWeek ( calendar, dateLike )
 */
bool CalendarDayOfWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       const PlainDateTime& dateTime,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarDayOfYear ( calendar, dateLike )
 */
bool CalendarDayOfYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       JS::Handle<PlainDateObject*> dateLike,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarDayOfYear ( calendar, dateLike )
 */
bool CalendarDayOfYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       JS::Handle<PlainDateTimeObject*> dateLike,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarDayOfYear ( calendar, dateLike )
 */
bool CalendarDayOfYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                       const PlainDateTime& dateTime,
                       JS::MutableHandle<JS::Value> result);

/**
 * CalendarWeekOfYear ( calendar, dateLike )
 */
bool CalendarWeekOfYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainDateObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarWeekOfYear ( calendar, dateLike )
 */
bool CalendarWeekOfYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainDateTimeObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarWeekOfYear ( calendar, dateLike )
 */
bool CalendarWeekOfYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        const PlainDateTime& dateTime,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarYearOfWeek ( calendar, dateLike )
 */
bool CalendarYearOfWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainDateObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarYearOfWeek ( calendar, dateLike )
 */
bool CalendarYearOfWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainDateTimeObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarYearOfWeek ( calendar, dateLike )
 */
bool CalendarYearOfWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        const PlainDateTime& dateTime,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarDaysInWeek ( calendar, dateLike )
 */
bool CalendarDaysInWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainDateObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarDaysInWeek ( calendar, dateLike )
 */
bool CalendarDaysInWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainDateTimeObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarDaysInWeek ( calendar, dateLike )
 */
bool CalendarDaysInWeek(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        const PlainDateTime& dateTime,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarDaysInMonth ( calendar, dateLike )
 */
bool CalendarDaysInMonth(JSContext* cx, JS::Handle<CalendarValue> calendar,
                         JS::Handle<PlainDateObject*> dateLike,
                         JS::MutableHandle<JS::Value> result);

/**
 * CalendarDaysInMonth ( calendar, dateLike )
 */
bool CalendarDaysInMonth(JSContext* cx, JS::Handle<CalendarValue> calendar,
                         JS::Handle<PlainDateTimeObject*> dateLike,
                         JS::MutableHandle<JS::Value> result);

/**
 * CalendarDaysInMonth ( calendar, dateLike )
 */
bool CalendarDaysInMonth(JSContext* cx, JS::Handle<CalendarValue> calendar,
                         JS::Handle<PlainYearMonthObject*> dateLike,
                         JS::MutableHandle<JS::Value> result);

/**
 * CalendarDaysInMonth ( calendar, dateLike )
 */
bool CalendarDaysInMonth(JSContext* cx, JS::Handle<CalendarValue> calendar,
                         const PlainDateTime& dateTime,
                         JS::MutableHandle<JS::Value> result);

/**
 * CalendarDaysInYear ( calendar, dateLike )
 */
bool CalendarDaysInYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainDateObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarDaysInYear ( calendar, dateLike )
 */
bool CalendarDaysInYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainDateTimeObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarDaysInYear ( calendar, dateLike )
 */
bool CalendarDaysInYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainYearMonthObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarDaysInYear ( calendar, dateLike )
 */
bool CalendarDaysInYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        const PlainDateTime& dateTime,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonthsInYear ( calendar, dateLike )
 */
bool CalendarMonthsInYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                          JS::Handle<PlainDateObject*> dateLike,
                          JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonthsInYear ( calendar, dateLike )
 */
bool CalendarMonthsInYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                          JS::Handle<PlainDateTimeObject*> dateLike,
                          JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonthsInYear ( calendar, dateLike )
 */
bool CalendarMonthsInYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                          JS::Handle<PlainYearMonthObject*> dateLike,
                          JS::MutableHandle<JS::Value> result);

/**
 * CalendarMonthsInYear ( calendar, dateLike )
 */
bool CalendarMonthsInYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                          const PlainDateTime& dateTime,
                          JS::MutableHandle<JS::Value> result);

/**
 * CalendarInLeapYear ( calendar, dateLike )
 */
bool CalendarInLeapYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainDateObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarInLeapYear ( calendar, dateLike )
 */
bool CalendarInLeapYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainDateTimeObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarInLeapYear ( calendar, dateLike )
 */
bool CalendarInLeapYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        JS::Handle<PlainYearMonthObject*> dateLike,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarInLeapYear ( calendar, dateLike )
 */
bool CalendarInLeapYear(JSContext* cx, JS::Handle<CalendarValue> calendar,
                        const PlainDateTime& dateTime,
                        JS::MutableHandle<JS::Value> result);

/**
 * CalendarDateFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainDateObject*> CalendarDateFromFields(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<PlainObject*> fields);

/**
 * CalendarDateFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainDateObject*> CalendarDateFromFields(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<PlainObject*> fields, JS::Handle<PlainObject*> options);

/**
 * CalendarYearMonthFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainYearMonthObject*> CalendarYearMonthFromFields(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<PlainObject*> fields);

/**
 * CalendarYearMonthFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainYearMonthObject*> CalendarYearMonthFromFields(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<PlainYearMonthObject*> fields);

/**
 * CalendarYearMonthFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainYearMonthObject*> CalendarYearMonthFromFields(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<PlainObject*> fields, JS::Handle<PlainObject*> options);

/**
 * CalendarMonthDayFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainMonthDayObject*> CalendarMonthDayFromFields(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<PlainObject*> fields);

/**
 * CalendarMonthDayFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainMonthDayObject*> CalendarMonthDayFromFields(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<PlainMonthDayObject*> fields);

/**
 * CalendarMonthDayFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainMonthDayObject*> CalendarMonthDayFromFields(
    JSContext* cx, JS::Handle<CalendarRecord> calendar,
    JS::Handle<PlainObject*> fields, JS::Handle<PlainObject*> options);

/**
 * CalendarEquals ( one, two )
 */
bool CalendarEquals(JSContext* cx, JS::Handle<CalendarValue> one,
                    JS::Handle<CalendarValue> two, bool* equals);

/**
 * CalendarEquals ( one, two )
 */
bool CalendarEqualsOrThrow(JSContext* cx, JS::Handle<CalendarValue> one,
                           JS::Handle<CalendarValue> two);

/**
 * ConsolidateCalendars ( one, two )
 */
bool ConsolidateCalendars(JSContext* cx, JS::Handle<CalendarValue> one,
                          JS::Handle<CalendarValue> two,
                          JS::MutableHandle<CalendarValue> result);

/**
 * CreateCalendarMethodsRecord ( calendar, methods )
 */
bool CreateCalendarMethodsRecord(JSContext* cx,
                                 JS::Handle<CalendarValue> calendar,
                                 mozilla::EnumSet<CalendarMethod> methods,
                                 JS::MutableHandle<CalendarRecord> result);

#ifdef DEBUG
/**
 * CalendarMethodsRecordHasLookedUp ( calendarRec, methodName )
 */
inline bool CalendarMethodsRecordHasLookedUp(const CalendarRecord& calendar,
                                             CalendarMethod methodName) {
  // Steps 1-10.
  return calendar.lookedUp().contains(methodName);
}
#endif

/**
 * CalendarMethodsRecordIsBuiltin ( calendarRec )
 */
inline bool CalendarMethodsRecordIsBuiltin(const CalendarRecord& calendar) {
  // Steps 1-2.
  return calendar.receiver().isString();
}

/**
 * Return true when accessing the calendar fields |fieldNames| can be optimized.
 * Otherwise returns false.
 */
bool IsBuiltinAccess(JSContext* cx, JS::Handle<CalendarObject*> calendar,
                     std::initializer_list<CalendarField> fieldNames);

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

  JS::Handle<JS::Value> toObjectValue() const {
    MOZ_ASSERT(isObject());
    return JS::Handle<JS::Value>::fromMarkedLocation(
        container().valueDoNotUse());
  }

  bool isString() const { return container().isString(); }

  bool isObject() const { return container().isObject(); }

  temporal::CalendarId toString() const { return container().toString(); }

  JSObject* toObject() const { return container().toObject(); }
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

template <typename Wrapper>
class WrappedPtrOperations<temporal::CalendarRecord, Wrapper> {
  const auto& container() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  JS::Handle<temporal::CalendarValue> receiver() const {
    return JS::Handle<temporal::CalendarValue>::fromMarkedLocation(
        container().receiverDoNotUse());
  }

  JS::Handle<JSObject*> dateAdd() const {
    return JS::Handle<JSObject*>::fromMarkedLocation(
        container().dateAddDoNotUse());
  }
  JS::Handle<JSObject*> dateFromFields() const {
    return JS::Handle<JSObject*>::fromMarkedLocation(
        container().dateFromFieldsDoNotUse());
  }
  JS::Handle<JSObject*> dateUntil() const {
    return JS::Handle<JSObject*>::fromMarkedLocation(
        container().dateUntilDoNotUse());
  }
  JS::Handle<JSObject*> day() const {
    return JS::Handle<JSObject*>::fromMarkedLocation(container().dayDoNotUse());
  }
  JS::Handle<JSObject*> fields() const {
    return JS::Handle<JSObject*>::fromMarkedLocation(
        container().fieldsDoNotUse());
  }
  JS::Handle<JSObject*> mergeFields() const {
    return JS::Handle<JSObject*>::fromMarkedLocation(
        container().mergeFieldsDoNotUse());
  }
  JS::Handle<JSObject*> monthDayFromFields() const {
    return JS::Handle<JSObject*>::fromMarkedLocation(
        container().monthDayFromFieldsDoNotUse());
  }
  JS::Handle<JSObject*> yearMonthFromFields() const {
    return JS::Handle<JSObject*>::fromMarkedLocation(
        container().yearMonthFromFieldsDoNotUse());
  }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<temporal::CalendarRecord, Wrapper>
    : public WrappedPtrOperations<temporal::CalendarRecord, Wrapper> {
  auto& container() { return static_cast<Wrapper*>(this)->get(); }

 public:
  JS::MutableHandle<JSObject*> dateAdd() {
    return JS::MutableHandle<JSObject*>::fromMarkedLocation(
        container().dateAddDoNotUse());
  }
  JS::MutableHandle<JSObject*> dateFromFields() {
    return JS::MutableHandle<JSObject*>::fromMarkedLocation(
        container().dateFromFieldsDoNotUse());
  }
  JS::MutableHandle<JSObject*> dateUntil() {
    return JS::MutableHandle<JSObject*>::fromMarkedLocation(
        container().dateUntilDoNotUse());
  }
  JS::MutableHandle<JSObject*> day() {
    return JS::MutableHandle<JSObject*>::fromMarkedLocation(
        container().dayDoNotUse());
  }
  JS::MutableHandle<JSObject*> fields() {
    return JS::MutableHandle<JSObject*>::fromMarkedLocation(
        container().fieldsDoNotUse());
  }
  JS::MutableHandle<JSObject*> mergeFields() {
    return JS::MutableHandle<JSObject*>::fromMarkedLocation(
        container().mergeFieldsDoNotUse());
  }
  JS::MutableHandle<JSObject*> monthDayFromFields() {
    return JS::MutableHandle<JSObject*>::fromMarkedLocation(
        container().monthDayFromFieldsDoNotUse());
  }
  JS::MutableHandle<JSObject*> yearMonthFromFields() {
    return JS::MutableHandle<JSObject*>::fromMarkedLocation(
        container().yearMonthFromFieldsDoNotUse());
  }
};

} /* namespace js */

#endif /* builtin_temporal_Calendar_h */
