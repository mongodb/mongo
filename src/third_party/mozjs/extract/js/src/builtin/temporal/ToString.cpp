/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/ToString.h"

#include "mozilla/Assertions.h"

#include <cstdlib>
#include <stddef.h>
#include <stdint.h>
#include <type_traits>
#include <utility>

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Crash.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainMonthDay.h"
#include "builtin/temporal/PlainYearMonth.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/TimeZone.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "gc/Policy.h"
#include "js/RootingAPI.h"
#include "util/StringBuffer.h"
#include "vm/StringType.h"

using namespace js;
using namespace js::temporal;

enum class TemporalStringFormat {
  None,
  Date,
  Time,
  DateTime,
  YearMonth,
  MonthDay,
  ZonedDateTime,
  Instant,
};

class TemporalStringBuilder {
  JSStringBuilder sb_;

  TemporalStringFormat kind_ = TemporalStringFormat::None;

#ifdef DEBUG
  bool reserved_ = false;
#endif

  static constexpr size_t reserveAmount(TemporalStringFormat format) {
    // Note: This doesn't reserve too much space, because the string builder
    // already internally reserves space for 64 characters.

    constexpr size_t datePart = 1 + 6 + 1 + 2 + 1 + 2;        // 13
    constexpr size_t timePart = 2 + 1 + 2 + 1 + 2 + 1 + 9;    // 18
    constexpr size_t dateTimePart = datePart + 1 + timePart;  // including 'T'
    constexpr size_t timeZoneOffsetPart = 1 + 2 + 1 + 2;      // 6

    switch (format) {
      case TemporalStringFormat::Date:
      case TemporalStringFormat::YearMonth:
      case TemporalStringFormat::MonthDay:
        return datePart;
      case TemporalStringFormat::Time:
        return timePart;
      case TemporalStringFormat::DateTime:
        return dateTimePart;
      case TemporalStringFormat::ZonedDateTime:
        return dateTimePart + timeZoneOffsetPart;
      case TemporalStringFormat::Instant:
        return dateTimePart + timeZoneOffsetPart;
      case TemporalStringFormat::None:
        break;
    }
    JS_CONSTEXPR_CRASH("invalid reserve amount");
  }

 public:
  TemporalStringBuilder(JSContext* cx, TemporalStringFormat kind)
      : sb_(cx), kind_(kind) {
    MOZ_ASSERT(kind != TemporalStringFormat::None);
  }

  bool reserve() {
    MOZ_ASSERT(!reserved_);

    if (!sb_.reserve(reserveAmount(kind_))) {
      return false;
    }

#ifdef DEBUG
    reserved_ = true;
#endif
    return true;
  }

  void append(char value) {
    MOZ_ASSERT(reserved_);
    sb_.infallibleAppend(value);
  }

  void appendTwoDigit(int32_t value) {
    MOZ_ASSERT(0 <= value && value <= 99);
    MOZ_ASSERT(reserved_);

    sb_.infallibleAppend(char('0' + (value / 10)));
    sb_.infallibleAppend(char('0' + (value % 10)));
  }

  void appendFourDigit(int32_t value) {
    MOZ_ASSERT(0 <= value && value <= 9999);
    MOZ_ASSERT(reserved_);

    sb_.infallibleAppend(char('0' + (value / 1000)));
    sb_.infallibleAppend(char('0' + (value % 1000) / 100));
    sb_.infallibleAppend(char('0' + (value % 100) / 10));
    sb_.infallibleAppend(char('0' + (value % 10)));
  }

  void appendSixDigit(int32_t value) {
    MOZ_ASSERT(0 <= value && value <= 999999);
    MOZ_ASSERT(reserved_);

    sb_.infallibleAppend(char('0' + (value / 100000)));
    sb_.infallibleAppend(char('0' + (value % 100000) / 10000));
    sb_.infallibleAppend(char('0' + (value % 10000) / 1000));
    sb_.infallibleAppend(char('0' + (value % 1000) / 100));
    sb_.infallibleAppend(char('0' + (value % 100) / 10));
    sb_.infallibleAppend(char('0' + (value % 10)));
  }

  void appendYear(int32_t year) {
    if (0 <= year && year <= 9999) {
      appendFourDigit(year);
    } else {
      append(year < 0 ? '-' : '+');
      appendSixDigit(std::abs(year));
    }
  }

  auto* finishString() { return sb_.finishString(); }

  auto& builder() { return sb_; }
};

/**
 * FormatFractionalSeconds ( subSecondNanoseconds, precision )
 */
static void FormatFractionalSeconds(TemporalStringBuilder& result,
                                    int32_t subSecondNanoseconds,
                                    Precision precision) {
  MOZ_ASSERT(0 <= subSecondNanoseconds && subSecondNanoseconds < 1'000'000'000);
  MOZ_ASSERT(precision != Precision::Minute());

  // Steps 1-2.
  if (precision == Precision::Auto()) {
    // Step 1.a.
    if (subSecondNanoseconds == 0) {
      return;
    }

    // Step 3. (Reordered)
    result.append('.');

    // Steps 1.b-c.
    int32_t k = 100'000'000;
    do {
      result.append(char('0' + (subSecondNanoseconds / k)));
      subSecondNanoseconds %= k;
      k /= 10;
    } while (subSecondNanoseconds);
  } else {
    // Step 2.a.
    uint8_t p = precision.value();
    if (p == 0) {
      return;
    }

    // Step 3. (Reordered)
    result.append('.');

    // Steps 2.b-c.
    int32_t k = 100'000'000;
    for (uint8_t i = 0; i < p; i++) {
      result.append(char('0' + (subSecondNanoseconds / k)));
      subSecondNanoseconds %= k;
      k /= 10;
    }
  }
}

/**
 * FormatTimeString ( hour, minute, second, subSecondNanoseconds, precision )
 */
static void FormatTimeString(TemporalStringBuilder& result,
                             const PlainTime& time, Precision precision) {
  // Step 1.
  result.appendTwoDigit(time.hour);

  // Step 2.
  result.append(':');
  result.appendTwoDigit(time.minute);

  // Steps 4-7.
  if (precision != Precision::Minute()) {
    result.append(':');
    result.appendTwoDigit(time.second);

    int32_t subSecondNanoseconds = time.millisecond * 1'000'000 +
                                   time.microsecond * 1'000 + time.nanosecond;
    FormatFractionalSeconds(result, subSecondNanoseconds, precision);
  }
}

static void FormatDateString(TemporalStringBuilder& result,
                             const PlainDate& date) {
  result.appendYear(date.year);
  result.append('-');
  result.appendTwoDigit(date.month);
  result.append('-');
  result.appendTwoDigit(date.day);
}

static void FormatDateTimeString(TemporalStringBuilder& result,
                                 const PlainDateTime& dateTime,
                                 Precision precision) {
  FormatDateString(result, dateTime.date);
  result.append('T');
  FormatTimeString(result, dateTime.time, precision);
}

/**
 * FormatOffsetTimeZoneIdentifier ( offsetMinutes [ , style ] )
 */
static void FormatOffsetTimeZoneIdentifier(TemporalStringBuilder& result,
                                           int32_t offsetMinutes) {
  MOZ_ASSERT(std::abs(offsetMinutes) < UnitsPerDay(TemporalUnit::Minute),
             "time zone offset mustn't exceed 24-hours");

  // Step 1.
  char sign = offsetMinutes >= 0 ? '+' : '-';

  // Step 2.
  int32_t absoluteMinutes = std::abs(offsetMinutes);

  // Step 3.
  int32_t hours = absoluteMinutes / 60;

  // Step 4.
  int32_t minutes = absoluteMinutes % 60;

  // Steps 5-6. (Inlined FormatTimeString)
  result.append(sign);
  result.appendTwoDigit(hours);
  result.append(':');
  result.appendTwoDigit(minutes);
}

// Returns |RoundNumberToIncrement(offsetNanoseconds, 60 × 10^9, "halfExpand")|
// divided by |60 × 10^9|.
static int32_t RoundNanosecondsToMinutes(int64_t offsetNanoseconds) {
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  constexpr int64_t increment = ToNanoseconds(TemporalUnit::Minute);

  int64_t quotient = offsetNanoseconds / increment;
  int64_t remainder = offsetNanoseconds % increment;
  if (std::abs(remainder * 2) >= increment) {
    quotient += (offsetNanoseconds > 0 ? 1 : -1);
  }
  return int32_t(quotient);
}

/**
 * FormatDateTimeUTCOffsetRounded ( offsetNanoseconds )
 */
static void FormatDateTimeUTCOffsetRounded(TemporalStringBuilder& result,
                                           int64_t offsetNanoseconds) {
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Steps 1-3.
  int32_t offsetMinutes = RoundNanosecondsToMinutes(offsetNanoseconds);

  // Step 4.
  FormatOffsetTimeZoneIdentifier(result, offsetMinutes);
}

/**
 * FormatCalendarAnnotation ( id, showCalendar )
 */
static bool FormatCalendarAnnotation(TemporalStringBuilder& result,
                                     JSLinearString* id,
                                     ShowCalendar showCalendar) {
  switch (showCalendar) {
    case ShowCalendar::Never:
      return true;

    case ShowCalendar::Auto: {
      if (StringEqualsLiteral(id, "iso8601")) {
        return true;
      }
      [[fallthrough]];
    }

    case ShowCalendar::Always: {
      auto& sb = result.builder();
      return sb.append("[u-ca=") && sb.append(id) && sb.append(']');
    }

    case ShowCalendar::Critical: {
      auto& sb = result.builder();
      return sb.append("[!u-ca=") && sb.append(id) && sb.append(']');
    }
  }
  MOZ_CRASH("bad calendar option");
}

/**
 * MaybeFormatCalendarAnnotation ( calendar, showCalendar )
 */
static bool MaybeFormatCalendarAnnotation(JSContext* cx,
                                          TemporalStringBuilder& result,
                                          Handle<CalendarValue> calendar,
                                          ShowCalendar showCalendar) {
  // Step 1.
  if (showCalendar == ShowCalendar::Never) {
    return true;
  }

  // Step 2.
  JSLinearString* calendarIdentifier =
      ToTemporalCalendarIdentifier(cx, calendar);
  if (!calendarIdentifier) {
    return false;
  }

  // Step 3.
  return FormatCalendarAnnotation(result, calendarIdentifier, showCalendar);
}

static bool FormatTimeZoneAnnotation(TemporalStringBuilder& result,
                                     JSLinearString* id,
                                     ShowTimeZoneName showTimeZone) {
  switch (showTimeZone) {
    case ShowTimeZoneName::Never:
      return true;

    case ShowTimeZoneName::Auto: {
      auto& sb = result.builder();
      return sb.append("[") && sb.append(id) && sb.append(']');
    }

    case ShowTimeZoneName::Critical: {
      auto& sb = result.builder();
      return sb.append("[!") && sb.append(id) && sb.append(']');
    }
  }
  MOZ_CRASH("bad time zone option");
}

static bool MaybeFormatTimeZoneAnnotation(JSContext* cx,
                                          TemporalStringBuilder& result,
                                          Handle<TimeZoneValue> timeZone,
                                          ShowTimeZoneName showTimeZone) {
  if (showTimeZone == ShowTimeZoneName::Never) {
    return true;
  }

  JSString* timeZoneIdentifier = ToTemporalTimeZoneIdentifier(cx, timeZone);
  if (!timeZoneIdentifier) {
    return false;
  }

  JSLinearString* linearTimeZoneId = timeZoneIdentifier->ensureLinear(cx);
  if (!linearTimeZoneId) {
    return false;
  }

  return FormatTimeZoneAnnotation(result, linearTimeZoneId, showTimeZone);
}

/**
 * TemporalInstantToString ( instant, timeZone, precision )
 */
JSString* js::temporal::TemporalInstantToString(JSContext* cx,
                                                Handle<InstantObject*> instant,
                                                Handle<TimeZoneValue> timeZone,
                                                Precision precision) {
  TemporalStringBuilder result(cx, TemporalStringFormat::Instant);
  if (!result.reserve()) {
    return nullptr;
  }

  // Steps 1-2. (Not applicable in our implementation.)

  // Steps 3-6.
  int64_t offsetNanoseconds = 0;
  if (timeZone) {
    // Steps 3-4. (Not applicable)

    // Steps 5-6.
    if (!GetOffsetNanosecondsFor(cx, timeZone, instant, &offsetNanoseconds)) {
      return nullptr;
    }
    MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));
  }

  // Step 7.
  auto dateTime = GetPlainDateTimeFor(ToInstant(instant), offsetNanoseconds);

  // Step 8. (Inlined TemporalDateTimeToString)
  FormatDateTimeString(result, dateTime, precision);

  // Steps 9-10.
  Rooted<JSString*> timeZoneString(cx);
  if (!timeZone) {
    // Step 9.a.
    result.append('Z');
  } else {
    // Step 10.a.
    FormatDateTimeUTCOffsetRounded(result, offsetNanoseconds);
  }

  // Step 11.
  return result.finishString();
}

/**
 * TemporalDateToString ( temporalDate, showCalendar )
 */
JSString* js::temporal::TemporalDateToString(
    JSContext* cx, Handle<PlainDateObject*> temporalDate,
    ShowCalendar showCalendar) {
  auto date = ToPlainDate(temporalDate);

  // Steps 1-2. (Not applicable in our implementation.)

  TemporalStringBuilder result(cx, TemporalStringFormat::Date);
  if (!result.reserve()) {
    return nullptr;
  }

  // Steps 3-5.
  FormatDateString(result, date);

  // Step 6.
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());
  if (!MaybeFormatCalendarAnnotation(cx, result, calendar, showCalendar)) {
    return nullptr;
  }

  // Step 7.
  return result.finishString();
}

/**
 * TemporalDateTimeToString ( isoYear, isoMonth, isoDay, hour, minute, second,
 * millisecond, microsecond, nanosecond, calendar, precision, showCalendar )
 */
JSString* js::temporal::TemporalDateTimeToString(JSContext* cx,
                                                 const PlainDateTime& dateTime,
                                                 Handle<CalendarValue> calendar,
                                                 Precision precision,
                                                 ShowCalendar showCalendar) {
  TemporalStringBuilder result(cx, TemporalStringFormat::DateTime);
  if (!result.reserve()) {
    return nullptr;
  }

  // Step 1. (Not applicable in our implementation.)

  // Steps 2-6.
  FormatDateTimeString(result, dateTime, precision);

  // Step 7.
  if (!MaybeFormatCalendarAnnotation(cx, result, calendar, showCalendar)) {
    return nullptr;
  }

  // Step 8.
  return result.finishString();
}

/**
 * TemporalTimeToString ( hour, minute, second, millisecond, microsecond,
 * nanosecond, precision )
 */
JSString* js::temporal::TemporalTimeToString(JSContext* cx,
                                             const PlainTime& time,
                                             Precision precision) {
  // Step 1. (Not applicable in our implementation.)

  TemporalStringBuilder result(cx, TemporalStringFormat::Time);
  if (!result.reserve()) {
    return nullptr;
  }

  // Steps 2-3.
  FormatTimeString(result, time, precision);

  return result.finishString();
}

/**
 * TemporalMonthDayToString ( monthDay, showCalendar )
 */
JSString* js::temporal::TemporalMonthDayToString(
    JSContext* cx, Handle<PlainMonthDayObject*> monthDay,
    ShowCalendar showCalendar) {
  // Steps 1-2. (Not applicable in our implementation.)

  TemporalStringBuilder result(cx, TemporalStringFormat::MonthDay);
  if (!result.reserve()) {
    return nullptr;
  }

  // Step 6. (Reordered)
  Rooted<CalendarValue> calendar(cx, monthDay->calendar());
  JSString* str = ToTemporalCalendarIdentifier(cx, calendar);
  if (!str) {
    return nullptr;
  }

  Rooted<JSLinearString*> calendarIdentifier(cx, str->ensureLinear(cx));
  if (!calendarIdentifier) {
    return nullptr;
  }

  // Steps 3-5 and 7.
  auto date = ToPlainDate(monthDay);
  if (showCalendar == ShowCalendar::Always ||
      showCalendar == ShowCalendar::Critical ||
      !StringEqualsLiteral(calendarIdentifier, "iso8601")) {
    // FIXME: spec issue - don't print "year" part when showCalendar is "never".
    //
    // ```js
    // let cal = new Proxy({id: "cal"}, {has(t, pk) { return true; }});
    // let pmd = new Temporal.PlainMonthDay(8, 1, cal);
    // pmd.toString({calendarName: "never"})
    // ```

    FormatDateString(result, date);
  } else {
    result.appendTwoDigit(date.month);
    result.append('-');
    result.appendTwoDigit(date.day);
  }

  // Steps 8-9.
  if (!FormatCalendarAnnotation(result, calendarIdentifier, showCalendar)) {
    return nullptr;
  }

  // Step 10.
  return result.finishString();
}

/**
 * TemporalYearMonthToString ( yearMonth, showCalendar )
 */
JSString* js::temporal::TemporalYearMonthToString(
    JSContext* cx, Handle<PlainYearMonthObject*> yearMonth,
    ShowCalendar showCalendar) {
  // Steps 1-2. (Not applicable in our implementation.)

  TemporalStringBuilder result(cx, TemporalStringFormat::YearMonth);
  if (!result.reserve()) {
    return nullptr;
  }

  // Step 6. (Reordered)
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());
  JSString* str = ToTemporalCalendarIdentifier(cx, calendar);
  if (!str) {
    return nullptr;
  }

  Rooted<JSLinearString*> calendarIdentifier(cx, str->ensureLinear(cx));
  if (!calendarIdentifier) {
    return nullptr;
  }

  // Steps 3-5 and 7.
  auto date = ToPlainDate(yearMonth);
  if (showCalendar == ShowCalendar::Always ||
      showCalendar == ShowCalendar::Critical ||
      !StringEqualsLiteral(calendarIdentifier, "iso8601")) {
    // FIXME: spec issue - don't print "day" part when showCalendar is "never".
    //
    // ```js
    // let cal = new Proxy({id: "cal"}, {has(t, pk) { return true; }});
    // let pym = new Temporal.PlainYearMonth(2023, 8, cal);
    // pym.toString({calendarName: "never"})
    // ```

    FormatDateString(result, date);
  } else {
    result.appendYear(date.year);
    result.append('-');
    result.appendTwoDigit(date.month);
  }

  // Steps 8-9.
  if (!FormatCalendarAnnotation(result, calendarIdentifier, showCalendar)) {
    return nullptr;
  }

  // Step 10.
  return result.finishString();
}

/**
 * TemporalZonedDateTimeToString ( zonedDateTime, precision, showCalendar,
 * showTimeZone, showOffset [ , increment, unit, roundingMode ] )
 */
JSString* js::temporal::TemporalZonedDateTimeToString(
    JSContext* cx, Handle<ZonedDateTime> zonedDateTime, Precision precision,
    ShowCalendar showCalendar, ShowTimeZoneName showTimeZone,
    ShowOffset showOffset, Increment increment, TemporalUnit unit,
    TemporalRoundingMode roundingMode) {
  TemporalStringBuilder result(cx, TemporalStringFormat::ZonedDateTime);
  if (!result.reserve()) {
    return nullptr;
  }

  // Steps 1-3. (Not applicable in our implementation.)

  // Step 4.
  auto ns = RoundTemporalInstant(zonedDateTime.instant(), increment, unit,
                                 roundingMode);

  // Step 5.
  auto timeZone = zonedDateTime.timeZone();

  // Steps 6-8.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, ns, &offsetNanoseconds)) {
    return nullptr;
  }
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Step 9.
  auto temporalDateTime = GetPlainDateTimeFor(ns, offsetNanoseconds);

  // Step 10. (Inlined TemporalDateTimeToString)
  FormatDateTimeString(result, temporalDateTime, precision);

  // Steps 11-12.
  if (showOffset != ShowOffset::Never) {
    FormatDateTimeUTCOffsetRounded(result, offsetNanoseconds);
  }

  // Steps 13-14.
  if (!MaybeFormatTimeZoneAnnotation(cx, result, timeZone, showTimeZone)) {
    return nullptr;
  }

  // Step 15.
  if (!MaybeFormatCalendarAnnotation(cx, result, zonedDateTime.calendar(),
                                     showCalendar)) {
    return nullptr;
  }

  // Step 16.
  return result.finishString();
}
