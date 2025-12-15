/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_CalendarFields_h
#define builtin_temporal_CalendarFields_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/EnumSet.h"
#include "mozilla/EnumTypeTraits.h"
#include "mozilla/FloatingPoint.h"

#include <cmath>
#include <stdint.h>

#include "jstypes.h"

#include "builtin/temporal/MonthCode.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/TimeZone.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

class JS_PUBLIC_API JSTracer;

namespace js::temporal {

// NB: The fields must be sorted alphabetically!
enum class CalendarField {
  Day,
  Era,
  EraYear,
  Hour,
  Microsecond,
  Millisecond,
  Minute,
  Month,
  MonthCode,
  Nanosecond,
  Offset,
  Second,
  TimeZone,
  Year,
};

class MonthCodeField final {
  // Packed representation for ordinal month (31 bits) and leap month (1 bit).
  uint32_t code_ = 0;

 public:
  MonthCodeField() = default;

  MonthCodeField(int32_t ordinal, bool isLeapMonth)
      : code_((ordinal << 1) | isLeapMonth) {
    MOZ_ASSERT(ordinal >= 0);
    MOZ_ASSERT_IF(ordinal == 0, isLeapMonth);
  }

  MOZ_IMPLICIT MonthCodeField(MonthCode monthCode)
      : MonthCodeField(monthCode.ordinal(), monthCode.isLeapMonth()) {}

  int32_t ordinal() const { return (code_ >> 1); }

  bool isLeapMonth() const { return bool(code_ & 1); }
};

class OffsetField final {
  int64_t offset_ = INT64_MIN;

 public:
  OffsetField() = default;

  explicit OffsetField(int64_t offset) : offset_(offset) {
    MOZ_ASSERT(std::abs(offset) < ToNanoseconds(TemporalUnit::Day));
  }

  explicit operator int64_t() const {
    MOZ_ASSERT(offset_ != INT64_MIN);
    return offset_;
  }
};

// Default values are specified in [1]. `UNSET` is replaced with an appropriate
// value based on the type, for example `double` fields use NaN whereas pointer
// fields use nullptr.
//
// [1]
// <https://tc39.es/proposal-temporal/#table-temporal-calendar-fields-record-fields>
class MOZ_STACK_CLASS CalendarFields final {
  mozilla::EnumSet<CalendarField> fields_ = {};

  JSString* era_ = nullptr;
  double eraYear_ = mozilla::UnspecifiedNaN<double>();
  double year_ = mozilla::UnspecifiedNaN<double>();
  double month_ = mozilla::UnspecifiedNaN<double>();
  MonthCodeField monthCode_ = {};
  double day_ = mozilla::UnspecifiedNaN<double>();
  double hour_ = 0;
  double minute_ = 0;
  double second_ = 0;
  double millisecond_ = 0;
  double microsecond_ = 0;
  double nanosecond_ = 0;
  OffsetField offset_ = {};
  TimeZoneValue timeZone_ = {};

 public:
  CalendarFields() = default;
  CalendarFields(const CalendarFields&) = default;

  auto* era() const { return era_; }
  auto eraYear() const { return eraYear_; }
  auto year() const { return year_; }
  auto month() const { return month_; }
  auto monthCode() const { return monthCode_; }
  auto day() const { return day_; }
  auto hour() const { return hour_; }
  auto minute() const { return minute_; }
  auto second() const { return second_; }
  auto millisecond() const { return millisecond_; }
  auto microsecond() const { return microsecond_; }
  auto nanosecond() const { return nanosecond_; }
  auto offset() const { return offset_; }
  auto& timeZone() const { return timeZone_; }

  void setEra(JSString* era) {
    fields_ += CalendarField::Era;
    era_ = era;
  }
  void setEraYear(double eraYear) {
    fields_ += CalendarField::EraYear;
    eraYear_ = eraYear;
  }
  void setYear(double year) {
    fields_ += CalendarField::Year;
    year_ = year;
  }
  void setMonth(double month) {
    fields_ += CalendarField::Month;
    month_ = month;
  }
  void setMonthCode(MonthCodeField monthCode) {
    fields_ += CalendarField::MonthCode;
    monthCode_ = monthCode;
  }
  void setDay(double day) {
    fields_ += CalendarField::Day;
    day_ = day;
  }
  void setHour(double hour) {
    fields_ += CalendarField::Hour;
    hour_ = hour;
  }
  void setMinute(double minute) {
    fields_ += CalendarField::Minute;
    minute_ = minute;
  }
  void setSecond(double second) {
    fields_ += CalendarField::Second;
    second_ = second;
  }
  void setMillisecond(double millisecond) {
    fields_ += CalendarField::Millisecond;
    millisecond_ = millisecond;
  }
  void setMicrosecond(double microsecond) {
    fields_ += CalendarField::Microsecond;
    microsecond_ = microsecond;
  }
  void setNanosecond(double nanosecond) {
    fields_ += CalendarField::Nanosecond;
    nanosecond_ = nanosecond;
  }
  void setOffset(OffsetField offset) {
    fields_ += CalendarField::Offset;
    offset_ = offset;
  }
  void setTimeZone(const TimeZoneValue& timeZone) {
    fields_ += CalendarField::TimeZone;
    timeZone_ = timeZone;
  }

  /**
   * Return `true` if the field is present.
   */
  bool has(CalendarField field) const { return fields_.contains(field); }

  /**
   * Return the set of all present fields.
   */
  mozilla::EnumSet<CalendarField> keys() const { return fields_; }

  /**
   * Mark that `field` is present, but uses its default value. The field must
   * not already be present in `this`.
   */
  void setDefault(CalendarField field) {
    MOZ_ASSERT(!fields_.contains(field));

    // Field whose default value is not UNSET.
    static constexpr mozilla::EnumSet<CalendarField> notUnsetDefault = {
        CalendarField::Hour,        CalendarField::Minute,
        CalendarField::Second,      CalendarField::Millisecond,
        CalendarField::Microsecond, CalendarField::Nanosecond,
    };

    // Fields whose default value is UNSET are ignored.
    if (notUnsetDefault.contains(field)) {
      fields_ += field;
    }
  }

  /**
   * Set `field` from `source`. The field must be present in `source`.
   */
  void setFrom(CalendarField field, const CalendarFields& source);

  // Helper methods for WrappedPtrOperations.
  auto eraDoNotUse() const { return &era_; }
  auto timeZoneDoNotUse() const { return &timeZone_; }

  // Trace implementation.
  void trace(JSTracer* trc);
};
}  // namespace js::temporal

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::CalendarFields, Wrapper> {
  const temporal::CalendarFields& container() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  JS::Handle<JSString*> era() const {
    return JS::Handle<JSString*>::fromMarkedLocation(container().eraDoNotUse());
  }
  double eraYear() const { return container().eraYear(); }
  double year() const { return container().year(); }
  double month() const { return container().month(); }
  temporal::MonthCodeField monthCode() const { return container().monthCode(); }
  double day() const { return container().day(); }
  double hour() const { return container().hour(); }
  double minute() const { return container().minute(); }
  double second() const { return container().second(); }
  double millisecond() const { return container().millisecond(); }
  double microsecond() const { return container().microsecond(); }
  double nanosecond() const { return container().nanosecond(); }
  temporal::OffsetField offset() const { return container().offset(); }
  JS::Handle<temporal::TimeZoneValue> timeZone() const {
    return JS::Handle<temporal::TimeZoneValue>::fromMarkedLocation(
        container().timeZoneDoNotUse());
  }

  bool has(temporal::CalendarField field) const {
    return container().has(field);
  }
  auto keys() const { return container().keys(); }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<temporal::CalendarFields, Wrapper>
    : public WrappedPtrOperations<temporal::CalendarFields, Wrapper> {
  temporal::CalendarFields& container() {
    return static_cast<Wrapper*>(this)->get();
  }

 public:
  void setEra(JSString* era) { container().setEra(era); }
  void setEraYear(double eraYear) { container().setEraYear(eraYear); }
  void setYear(double year) { container().setYear(year); }
  void setMonth(double month) { container().setMonth(month); }
  void setMonthCode(temporal::MonthCodeField monthCode) {
    container().setMonthCode(monthCode);
  }
  void setDay(double day) { container().setDay(day); }
  void setHour(double hour) { container().setHour(hour); }
  void setMinute(double minute) { container().setMinute(minute); }
  void setSecond(double second) { container().setSecond(second); }
  void setMillisecond(double millisecond) {
    container().setMillisecond(millisecond);
  }
  void setMicrosecond(double microsecond) {
    container().setMicrosecond(microsecond);
  }
  void setNanosecond(double nanosecond) {
    container().setNanosecond(nanosecond);
  }
  void setOffset(temporal::OffsetField offset) {
    container().setOffset(offset);
  }
  void setTimeZone(const temporal::TimeZoneValue& timeZone) {
    container().setTimeZone(timeZone);
  }

  void setDefault(temporal::CalendarField field) {
    container().setDefault(field);
  }
};

}  // namespace js

namespace js::temporal {

class CalendarValue;
class PlainDate;
class PlainDateTime;
class PlainMonthDay;
class PlainYearMonth;

/**
 * PrepareCalendarFields ( calendar, fields, calendarFieldNames,
 * nonCalendarFieldNames, requiredFieldNames )
 */
bool PrepareCalendarFields(JSContext* cx, JS::Handle<CalendarValue> calendar,
                           JS::Handle<JSObject*> fields,
                           mozilla::EnumSet<CalendarField> fieldNames,
                           mozilla::EnumSet<CalendarField> requiredFields,
                           JS::MutableHandle<CalendarFields> result);

/**
 * PrepareCalendarFields ( calendar, fields, calendarFieldNames,
 * nonCalendarFieldNames, requiredFieldNames )
 */
inline bool PrepareCalendarFields(JSContext* cx,
                                  JS::Handle<CalendarValue> calendar,
                                  JS::Handle<JSObject*> fields,
                                  mozilla::EnumSet<CalendarField> fieldNames,
                                  JS::MutableHandle<CalendarFields> result) {
  return PrepareCalendarFields(cx, calendar, fields, fieldNames, {}, result);
}

/**
 * PrepareCalendarFields ( calendar, fields, calendarFieldNames,
 * nonCalendarFieldNames, requiredFieldNames )
 */
bool PreparePartialCalendarFields(JSContext* cx,
                                  JS::Handle<CalendarValue> calendar,
                                  JS::Handle<JSObject*> fields,
                                  mozilla::EnumSet<CalendarField> fieldNames,
                                  JS::MutableHandle<CalendarFields> result);

/**
 * CalendarMergeFields ( calendar, fields, additionalFields )
 */
CalendarFields CalendarMergeFields(const CalendarValue& calendar,
                                   const CalendarFields& fields,
                                   const CalendarFields& additionalFields);

/**
 * ISODateToFields ( calendar, isoDate, type )
 */
bool ISODateToFields(JSContext* cx, Handle<PlainDate> date,
                     MutableHandle<CalendarFields> result);

/**
 * ISODateToFields ( calendar, isoDate, type )
 */
bool ISODateToFields(JSContext* cx, Handle<PlainDateTime> dateTime,
                     MutableHandle<CalendarFields> result);

/**
 * ISODateToFields ( calendar, isoDate, type )
 */
bool ISODateToFields(JSContext* cx, Handle<PlainMonthDay> monthDay,
                     MutableHandle<CalendarFields> result);

/**
 * ISODateToFields ( calendar, isoDate, type )
 */
bool ISODateToFields(JSContext* cx, Handle<PlainYearMonth> yearMonth,
                     MutableHandle<CalendarFields> result);

} /* namespace js::temporal */

namespace mozilla {
template <>
struct MaxContiguousEnumValue<js::temporal::CalendarField> {
  static constexpr auto value = js::temporal::CalendarField::Year;
};
}  // namespace mozilla

#endif /* builtin_temporal_CalendarFields_h */
