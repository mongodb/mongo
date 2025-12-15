/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/CalendarFields.h"

#include "mozilla/Assertions.h"
#include "mozilla/EnumTypeTraits.h"
#include "mozilla/Maybe.h"
#include "mozilla/Range.h"
#include "mozilla/TextUtils.h"

#include <stdint.h>
#include <string_view>

#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Crash.h"
#include "builtin/temporal/Era.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TimeZone.h"
#include "gc/Barrier.h"
#include "gc/Tracer.h"
#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/GCAPI.h"
#include "js/Printer.h"
#include "js/RootingAPI.h"
#include "js/Value.h"
#include "util/Text.h"
#include "vm/BytecodeUtil.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;
using namespace js::temporal;

void CalendarFields::trace(JSTracer* trc) {
  TraceNullableRoot(trc, &era_, "CalendarFields::era");
  timeZone_.trace(trc);
}

void CalendarFields::setFrom(CalendarField field,
                             const CalendarFields& source) {
  MOZ_ASSERT(source.has(field));

  switch (field) {
    case CalendarField::Era:
      setEra(source.era());
      return;
    case CalendarField::EraYear:
      setEraYear(source.eraYear());
      return;
    case CalendarField::Year:
      setYear(source.year());
      return;
    case CalendarField::Month:
      setMonth(source.month());
      return;
    case CalendarField::MonthCode:
      setMonthCode(source.monthCode());
      return;
    case CalendarField::Day:
      setDay(source.day());
      return;
    case CalendarField::Hour:
      setHour(source.hour());
      return;
    case CalendarField::Minute:
      setMinute(source.minute());
      return;
    case CalendarField::Second:
      setSecond(source.second());
      return;
    case CalendarField::Millisecond:
      setMillisecond(source.millisecond());
      return;
    case CalendarField::Microsecond:
      setMicrosecond(source.microsecond());
      return;
    case CalendarField::Nanosecond:
      setNanosecond(source.nanosecond());
      return;
    case CalendarField::Offset:
      setOffset(source.offset());
      return;
    case CalendarField::TimeZone:
      setTimeZone(source.timeZone());
      return;
  }
  MOZ_CRASH("invalid temporal field");
}

static PropertyName* ToPropertyName(JSContext* cx, CalendarField field) {
  switch (field) {
    case CalendarField::Era:
      return cx->names().era;
    case CalendarField::EraYear:
      return cx->names().eraYear;
    case CalendarField::Year:
      return cx->names().year;
    case CalendarField::Month:
      return cx->names().month;
    case CalendarField::MonthCode:
      return cx->names().monthCode;
    case CalendarField::Day:
      return cx->names().day;
    case CalendarField::Hour:
      return cx->names().hour;
    case CalendarField::Minute:
      return cx->names().minute;
    case CalendarField::Second:
      return cx->names().second;
    case CalendarField::Millisecond:
      return cx->names().millisecond;
    case CalendarField::Microsecond:
      return cx->names().microsecond;
    case CalendarField::Nanosecond:
      return cx->names().nanosecond;
    case CalendarField::Offset:
      return cx->names().offset;
    case CalendarField::TimeZone:
      return cx->names().timeZone;
  }
  MOZ_CRASH("invalid temporal field name");
}

static constexpr const char* ToCString(CalendarField field) {
  switch (field) {
    case CalendarField::Era:
      return "era";
    case CalendarField::EraYear:
      return "eraYear";
    case CalendarField::Year:
      return "year";
    case CalendarField::Month:
      return "month";
    case CalendarField::MonthCode:
      return "monthCode";
    case CalendarField::Day:
      return "day";
    case CalendarField::Hour:
      return "hour";
    case CalendarField::Minute:
      return "minute";
    case CalendarField::Second:
      return "second";
    case CalendarField::Millisecond:
      return "millisecond";
    case CalendarField::Microsecond:
      return "microsecond";
    case CalendarField::Nanosecond:
      return "nanosecond";
    case CalendarField::Offset:
      return "offset";
    case CalendarField::TimeZone:
      return "timeZone";
  }
  JS_CONSTEXPR_CRASH("invalid temporal field name");
}

static constexpr bool CalendarFieldsAreSorted() {
  constexpr auto min = mozilla::ContiguousEnumValues<CalendarField>::min;
  constexpr auto max = mozilla::ContiguousEnumValues<CalendarField>::max;

  auto field = min;
  while (field != max) {
    auto next = static_cast<CalendarField>(mozilla::UnderlyingValue(field) + 1);

    auto a = std::string_view{ToCString(field)};
    auto b = std::string_view{ToCString(next)};
    if (a.compare(b) >= 0) {
      return false;
    }
    field = next;
  }
  return true;
}

/**
 * CalendarExtraFields ( calendar, fields )
 */
static mozilla::EnumSet<CalendarField> CalendarExtraFields(
    CalendarId calendar, mozilla::EnumSet<CalendarField> fields) {
  // Step 1.
  if (calendar == CalendarId::ISO8601) {
    return {};
  }

  // Step 2.

  // "era" and "eraYear" are relevant for calendars with multiple eras when
  // "year" is present.
  if (fields.contains(CalendarField::Year) && CalendarEraRelevant(calendar)) {
    return {CalendarField::Era, CalendarField::EraYear};
  }
  return {};
}

/**
 * ToMonthCode ( argument )
 */
template <typename CharT>
static mozilla::Maybe<MonthCodeField> ToMonthCode(
    mozilla::Range<const CharT> chars) {
  // Steps 1-2. (Not applicable)

  // Step 3.
  //
  // Caller is responsible to ensure the string has the correct length.
  MOZ_ASSERT(chars.length() >= 3 && chars.length() <= 4);

  // Steps 4 and 7.
  //
  // Starts with capital letter 'M'. Leap months end with capital letter 'L'.
  bool isLeapMonth = chars.length() == 4;
  if (chars[0] != 'M' || (isLeapMonth && chars[3] != 'L')) {
    return mozilla::Nothing();
  }

  // Steps 5-6.
  //
  // Month numbers are ASCII digits.
  if (!mozilla::IsAsciiDigit(chars[1]) || !mozilla::IsAsciiDigit(chars[2])) {
    return mozilla::Nothing();
  }

  // Steps 8-9.
  int32_t ordinal =
      AsciiDigitToNumber(chars[1]) * 10 + AsciiDigitToNumber(chars[2]);

  // Step 10.
  if (ordinal == 0 && !isLeapMonth) {
    return mozilla::Nothing();
  }

  // Step 11.
  return mozilla::Some(MonthCodeField{ordinal, isLeapMonth});
}

/**
 * ToMonthCode ( argument )
 */
static auto ToMonthCode(const JSLinearString* linear) {
  JS::AutoCheckCannotGC nogc;

  if (linear->hasLatin1Chars()) {
    return ToMonthCode(linear->latin1Range(nogc));
  }
  return ToMonthCode(linear->twoByteRange(nogc));
}

/**
 * ToMonthCode ( argument )
 */
static bool ToMonthCode(JSContext* cx, Handle<Value> value,
                        MonthCodeField* result) {
  auto reportInvalidMonthCode = [&](JSLinearString* monthCode) {
    if (auto code = QuoteString(cx, monthCode)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_CALENDAR_INVALID_MONTHCODE,
                               code.get());
    }
    return false;
  };

  // Step 1.
  Rooted<Value> monthCode(cx, value);
  if (!ToPrimitive(cx, JSTYPE_STRING, &monthCode)) {
    return false;
  }

  // Step 2.
  if (!monthCode.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, monthCode,
                     nullptr, "not a string");
    return false;
  }

  JSLinearString* monthCodeStr = monthCode.toString()->ensureLinear(cx);
  if (!monthCodeStr) {
    return false;
  }

  // Step 3.
  if (monthCodeStr->length() < 3 || monthCodeStr->length() > 4) {
    return reportInvalidMonthCode(monthCodeStr);
  }

  // Steps 4-11.
  auto parsed = ToMonthCode(monthCodeStr);
  if (!parsed) {
    return reportInvalidMonthCode(monthCodeStr);
  }

  *result = *parsed;
  return true;
}

/**
 * ToOffsetString ( argument )
 */
static bool ToOffsetString(JSContext* cx, Handle<Value> value,
                           int64_t* result) {
  // Step 1.
  Rooted<Value> offset(cx, value);
  if (!ToPrimitive(cx, JSTYPE_STRING, &offset)) {
    return false;
  }

  // Step 2.
  if (!offset.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, offset,
                     nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> offsetStr(cx, offset.toString());

  // Steps 3-4.
  return ParseDateTimeUTCOffset(cx, offsetStr, result);
}

enum class Partial : bool { No, Yes };

/**
 * PrepareCalendarFields ( calendar, fields, calendarFieldNames,
 * nonCalendarFieldNames, requiredFieldNames )
 */
static bool PrepareCalendarFields(
    JSContext* cx, Handle<CalendarValue> calendar, Handle<JSObject*> fields,
    mozilla::EnumSet<CalendarField> fieldNames,
    mozilla::EnumSet<CalendarField> requiredFields, Partial partial,
    MutableHandle<CalendarFields> result) {
  MOZ_ASSERT_IF(partial == Partial::Yes, requiredFields.isEmpty());

  // Steps 1-2. (Not applicable in our implementation.)

  // Step 3.
  auto extraFieldNames = CalendarExtraFields(calendar.identifier(), fieldNames);

  // Step 4.
  fieldNames += extraFieldNames;

  // Step 5. (Not applicable in our implementation.)

  // Step 6.
  //
  // Default initialize the result.
  result.set(CalendarFields{});

  // Step 7. (Not applicable in our implementation.)

  // Step 8.
  static_assert(CalendarFieldsAreSorted(),
                "EnumSet<CalendarField> iteration is sorted");

  // Step 9.
  Rooted<Value> value(cx);
  for (auto fieldName : fieldNames) {
    auto* propertyName = ToPropertyName(cx, fieldName);
    const auto* cstr = ToCString(fieldName);

    // Step 9.a. (Not applicable in our implementation.)

    // Step 9.b.
    if (!GetProperty(cx, fields, fields, propertyName, &value)) {
      return false;
    }

    // Steps 9.c-d.
    if (!value.isUndefined()) {
      // Steps 9.c.i-ii. (Not applicable in our implementation.)

      // Steps 9.c.iii-ix.
      switch (fieldName) {
        case CalendarField::Era: {
          JSString* era = ToString(cx, value);
          if (!era) {
            return false;
          }
          result.setEra(era);
          break;
        }
        case CalendarField::EraYear: {
          double eraYear;
          if (!ToIntegerWithTruncation(cx, value, cstr, &eraYear)) {
            return false;
          }
          result.setEraYear(eraYear);
          break;
        }
        case CalendarField::Year: {
          double year;
          if (!ToIntegerWithTruncation(cx, value, cstr, &year)) {
            return false;
          }
          result.setYear(year);
          break;
        }
        case CalendarField::Month: {
          double month;
          if (!ToPositiveIntegerWithTruncation(cx, value, cstr, &month)) {
            return false;
          }
          result.setMonth(month);
          break;
        }
        case CalendarField::MonthCode: {
          MonthCodeField monthCode;
          if (!ToMonthCode(cx, value, &monthCode)) {
            return false;
          }
          result.setMonthCode(monthCode);
          break;
        }
        case CalendarField::Day: {
          double day;
          if (!ToPositiveIntegerWithTruncation(cx, value, cstr, &day)) {
            return false;
          }
          result.setDay(day);
          break;
        }
        case CalendarField::Hour: {
          double hour;
          if (!ToIntegerWithTruncation(cx, value, cstr, &hour)) {
            return false;
          }
          result.setHour(hour);
          break;
        }
        case CalendarField::Minute: {
          double minute;
          if (!ToIntegerWithTruncation(cx, value, cstr, &minute)) {
            return false;
          }
          result.setMinute(minute);
          break;
        }
        case CalendarField::Second: {
          double second;
          if (!ToIntegerWithTruncation(cx, value, cstr, &second)) {
            return false;
          }
          result.setSecond(second);
          break;
        }
        case CalendarField::Millisecond: {
          double millisecond;
          if (!ToIntegerWithTruncation(cx, value, cstr, &millisecond)) {
            return false;
          }
          result.setMillisecond(millisecond);
          break;
        }
        case CalendarField::Microsecond: {
          double microsecond;
          if (!ToIntegerWithTruncation(cx, value, cstr, &microsecond)) {
            return false;
          }
          result.setMicrosecond(microsecond);
          break;
        }
        case CalendarField::Nanosecond: {
          double nanosecond;
          if (!ToIntegerWithTruncation(cx, value, cstr, &nanosecond)) {
            return false;
          }
          result.setNanosecond(nanosecond);
          break;
        }
        case CalendarField::Offset: {
          int64_t offset;
          if (!ToOffsetString(cx, value, &offset)) {
            return false;
          }
          result.setOffset(OffsetField{offset});
          break;
        }
        case CalendarField::TimeZone:
          Rooted<TimeZoneValue> timeZone(cx);
          if (!ToTemporalTimeZone(cx, value, &timeZone)) {
            return false;
          }
          result.setTimeZone(timeZone);
          break;
      }
    } else if (partial == Partial::No) {
      // Step 9.d.i.
      if (requiredFields.contains(fieldName)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_TEMPORAL_MISSING_PROPERTY, cstr);
        return false;
      }

      // Step 9.d.ii.
      result.setDefault(fieldName);
    }
  }

  // Step 10.
  if (partial == Partial::Yes && result.keys().isEmpty()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_MISSING_TEMPORAL_FIELDS);
    return false;
  }

  // Step 11.
  return true;
}

/**
 * PrepareCalendarFields ( calendar, fields, calendarFieldNames,
 * nonCalendarFieldNames, requiredFieldNames )
 */
bool js::temporal::PrepareCalendarFields(
    JSContext* cx, Handle<CalendarValue> calendar, Handle<JSObject*> fields,
    mozilla::EnumSet<CalendarField> fieldNames,
    mozilla::EnumSet<CalendarField> requiredFields,
    MutableHandle<CalendarFields> result) {
  return PrepareCalendarFields(cx, calendar, fields, fieldNames, requiredFields,
                               Partial::No, result);
}

/**
 * PrepareCalendarFields ( calendar, fields, calendarFieldNames,
 * nonCalendarFieldNames, requiredFieldNames )
 */
bool js::temporal::PreparePartialCalendarFields(
    JSContext* cx, Handle<CalendarValue> calendar, Handle<JSObject*> fields,
    mozilla::EnumSet<CalendarField> fieldNames,
    JS::MutableHandle<CalendarFields> result) {
  return PrepareCalendarFields(cx, calendar, fields, fieldNames, {},
                               Partial::Yes, result);
}

/**
 * CalendarFieldKeysToIgnore ( calendar, keys )
 */
static auto CalendarFieldKeysToIgnore(CalendarId calendar,
                                      mozilla::EnumSet<CalendarField> keys) {
  // Step 1.
  if (calendar == CalendarId::ISO8601) {
    // Steps 1.a and 1.b.i.
    auto ignoredKeys = keys;

    // Step 1.b.ii.
    if (keys.contains(CalendarField::Month)) {
      ignoredKeys += CalendarField::MonthCode;
    }

    // Step 1.b.iii.
    else if (keys.contains(CalendarField::MonthCode)) {
      ignoredKeys += CalendarField::Month;
    }

    // Steps 1.c-d.
    return ignoredKeys;
  }

  // Step 2.

  static constexpr auto eraOrEraYear = mozilla::EnumSet{
      CalendarField::Era,
      CalendarField::EraYear,
  };

  static constexpr auto eraOrAnyYear = mozilla::EnumSet{
      CalendarField::Era,
      CalendarField::EraYear,
      CalendarField::Year,
  };

  static constexpr auto monthOrMonthCode = mozilla::EnumSet{
      CalendarField::Month,
      CalendarField::MonthCode,
  };

  static constexpr auto dayOrAnyMonth = mozilla::EnumSet{
      CalendarField::Day,
      CalendarField::Month,
      CalendarField::MonthCode,
  };

  // A field always invalidates at least itself, so start with ignoring all
  // input fields.
  auto result = keys;

  // "month" and "monthCode" are mutually exclusive.
  if (!(keys & monthOrMonthCode).isEmpty()) {
    result += monthOrMonthCode;
  }

  // "era", "eraYear", and "year" are mutually exclusive in non-single era
  // calendar systems.
  if (CalendarEraRelevant(calendar) && !(keys & eraOrAnyYear).isEmpty()) {
    result += eraOrAnyYear;
  }

  // If eras don't start at year boundaries, we have to ignore "era" and
  // "eraYear" if any of "day", "month", or "monthCode" is present.
  if (!CalendarEraStartsAtYearBoundary(calendar) &&
      !(keys & dayOrAnyMonth).isEmpty()) {
    result += eraOrEraYear;
  }

  return result;
}

/**
 * CalendarMergeFields ( calendar, fields, additionalFields )
 */
CalendarFields js::temporal::CalendarMergeFields(
    const CalendarValue& calendar, const CalendarFields& fields,
    const CalendarFields& additionalFields) {
  auto calendarId = calendar.identifier();

  // Steps 1.
  auto additionalKeys = additionalFields.keys();

  // Step 2.
  auto overriddenKeys = CalendarFieldKeysToIgnore(calendarId, additionalKeys);
  MOZ_ASSERT(overriddenKeys.contains(additionalKeys));

  // Step 3.
  auto merged = CalendarFields{};

  // Step 4.
  auto fieldsKeys = fields.keys();

  // Step 5.b.
  for (auto key : (fieldsKeys - overriddenKeys)) {
    merged.setFrom(key, fields);
  }

  // Step 5.c.
  for (auto key : additionalKeys) {
    merged.setFrom(key, additionalFields);
  }

  // Step 6.
  return merged;
}
