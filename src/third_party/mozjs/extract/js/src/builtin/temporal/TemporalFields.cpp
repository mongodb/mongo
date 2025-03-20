/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/TemporalFields.h"

#include "mozilla/Assertions.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"
#include "mozilla/Range.h"
#include "mozilla/RangedPtr.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdint.h>
#include <type_traits>
#include <utility>

#include "jsnum.h"
#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/temporal/Crash.h"
#include "builtin/temporal/Temporal.h"
#include "ds/Sort.h"
#include "gc/Barrier.h"
#include "gc/Tracer.h"
#include "js/AllocPolicy.h"
#include "js/ComparisonOperators.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/GCVector.h"
#include "js/Id.h"
#include "js/Printer.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"
#include "js/Value.h"
#include "util/Text.h"
#include "vm/BytecodeUtil.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"

#include "vm/JSAtomUtils-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;
using namespace js::temporal;

void TemporalFields::trace(JSTracer* trc) {
  TraceNullableRoot(trc, &monthCode, "TemporalFields::monthCode");
  TraceNullableRoot(trc, &offset, "TemporalFields::offset");
  TraceNullableRoot(trc, &era, "TemporalFields::era");
  TraceRoot(trc, &timeZone, "TemporalFields::timeZone");
}

PropertyName* js::temporal::ToPropertyName(JSContext* cx, TemporalField field) {
  switch (field) {
    case TemporalField::Year:
      return cx->names().year;
    case TemporalField::Month:
      return cx->names().month;
    case TemporalField::MonthCode:
      return cx->names().monthCode;
    case TemporalField::Day:
      return cx->names().day;
    case TemporalField::Hour:
      return cx->names().hour;
    case TemporalField::Minute:
      return cx->names().minute;
    case TemporalField::Second:
      return cx->names().second;
    case TemporalField::Millisecond:
      return cx->names().millisecond;
    case TemporalField::Microsecond:
      return cx->names().microsecond;
    case TemporalField::Nanosecond:
      return cx->names().nanosecond;
    case TemporalField::Offset:
      return cx->names().offset;
    case TemporalField::Era:
      return cx->names().era;
    case TemporalField::EraYear:
      return cx->names().eraYear;
    case TemporalField::TimeZone:
      return cx->names().timeZone;
  }
  MOZ_CRASH("invalid temporal field name");
}

static constexpr const char* ToCString(TemporalField field) {
  switch (field) {
    case TemporalField::Year:
      return "year";
    case TemporalField::Month:
      return "month";
    case TemporalField::MonthCode:
      return "monthCode";
    case TemporalField::Day:
      return "day";
    case TemporalField::Hour:
      return "hour";
    case TemporalField::Minute:
      return "minute";
    case TemporalField::Second:
      return "second";
    case TemporalField::Millisecond:
      return "millisecond";
    case TemporalField::Microsecond:
      return "microsecond";
    case TemporalField::Nanosecond:
      return "nanosecond";
    case TemporalField::Offset:
      return "offset";
    case TemporalField::Era:
      return "era";
    case TemporalField::EraYear:
      return "eraYear";
    case TemporalField::TimeZone:
      return "timeZone";
  }
  JS_CONSTEXPR_CRASH("invalid temporal field name");
}

static JS::UniqueChars QuoteString(JSContext* cx, const char* str) {
  Sprinter sprinter(cx);
  if (!sprinter.init()) {
    return nullptr;
  }
  mozilla::Range range(reinterpret_cast<const Latin1Char*>(str),
                       std::strlen(str));
  QuoteString<QuoteTarget::String>(&sprinter, range);
  return sprinter.release();
}

static JS::UniqueChars QuoteString(JSContext* cx, PropertyKey key) {
  if (key.isString()) {
    return QuoteString(cx, key.toString());
  }

  if (key.isInt()) {
    Int32ToCStringBuf buf;
    size_t length;
    const char* str = Int32ToCString(&buf, key.toInt(), &length);
    return DuplicateString(cx, str, length);
  }

  MOZ_ASSERT(key.isSymbol());
  return QuoteString(cx, key.toSymbol()->description());
}

mozilla::Maybe<TemporalField> js::temporal::ToTemporalField(
    JSContext* cx, PropertyKey property) {
  static constexpr TemporalField fieldNames[] = {
      TemporalField::Year,        TemporalField::Month,
      TemporalField::MonthCode,   TemporalField::Day,
      TemporalField::Hour,        TemporalField::Minute,
      TemporalField::Second,      TemporalField::Millisecond,
      TemporalField::Microsecond, TemporalField::Nanosecond,
      TemporalField::Offset,      TemporalField::Era,
      TemporalField::EraYear,     TemporalField::TimeZone,
  };

  for (const auto& fieldName : fieldNames) {
    auto* name = ToPropertyName(cx, fieldName);
    if (property.isAtom(name)) {
      return mozilla::Some(fieldName);
    }
  }
  return mozilla::Nothing();
}

static JSString* ToPrimitiveAndRequireString(JSContext* cx,
                                             Handle<Value> value) {
  Rooted<Value> primitive(cx, value);
  if (!ToPrimitive(cx, JSTYPE_STRING, &primitive)) {
    return nullptr;
  }
  if (!primitive.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, primitive,
                     nullptr, "not a string");
    return nullptr;
  }
  return primitive.toString();
}

static Value TemporalFieldDefaultValue(TemporalField field) {
  switch (field) {
    case TemporalField::Year:
    case TemporalField::Month:
    case TemporalField::MonthCode:
    case TemporalField::Day:
    case TemporalField::Offset:
    case TemporalField::Era:
    case TemporalField::EraYear:
    case TemporalField::TimeZone:
      return UndefinedValue();
    case TemporalField::Hour:
    case TemporalField::Minute:
    case TemporalField::Second:
    case TemporalField::Millisecond:
    case TemporalField::Microsecond:
    case TemporalField::Nanosecond:
      return Int32Value(0);
  }
  MOZ_CRASH("invalid temporal field name");
}

static bool TemporalFieldConvertValue(JSContext* cx, TemporalField field,
                                      MutableHandle<Value> value) {
  const auto* name = ToCString(field);
  switch (field) {
    case TemporalField::Year:
    case TemporalField::Hour:
    case TemporalField::Minute:
    case TemporalField::Second:
    case TemporalField::Millisecond:
    case TemporalField::Microsecond:
    case TemporalField::Nanosecond:
    case TemporalField::EraYear: {
      double num;
      if (!ToIntegerWithTruncation(cx, value, name, &num)) {
        return false;
      }
      value.setNumber(num);
      return true;
    }

    case TemporalField::Month:
    case TemporalField::Day: {
      double num;
      if (!ToPositiveIntegerWithTruncation(cx, value, name, &num)) {
        return false;
      }
      value.setNumber(num);
      return true;
    }

    case TemporalField::MonthCode:
    case TemporalField::Offset:
    case TemporalField::Era: {
      JSString* str = ToPrimitiveAndRequireString(cx, value);
      if (!str) {
        return false;
      }
      value.setString(str);
      return true;
    }

    case TemporalField::TimeZone:
      // NB: timeZone has no conversion function.
      return true;
  }
  MOZ_CRASH("invalid temporal field name");
}

static int32_t ComparePropertyKey(PropertyKey x, PropertyKey y) {
  MOZ_ASSERT(x.isAtom() || x.isInt());
  MOZ_ASSERT(y.isAtom() || y.isInt());

  if (MOZ_LIKELY(x.isAtom() && y.isAtom())) {
    return CompareStrings(x.toAtom(), y.toAtom());
  }

  if (x.isInt() && y.isInt()) {
    return x.toInt() - y.toInt();
  }

  uint32_t index = uint32_t(x.isInt() ? x.toInt() : y.toInt());
  JSAtom* str = x.isAtom() ? x.toAtom() : y.toAtom();

  char16_t buf[UINT32_CHAR_BUFFER_LENGTH];
  mozilla::RangedPtr<char16_t> end(std::end(buf), buf, std::end(buf));
  mozilla::RangedPtr<char16_t> start = BackfillIndexInCharBuffer(index, end);

  int32_t result = CompareChars(start.get(), end - start, str);
  return x.isInt() ? result : -result;
}

#ifdef DEBUG
static bool IsSorted(const TemporalFieldNames& fieldNames) {
  return std::is_sorted(
      fieldNames.begin(), fieldNames.end(),
      [](auto x, auto y) { return ComparePropertyKey(x, y) < 0; });
}
#endif

template <typename T, size_t N>
static constexpr bool IsSorted(const std::array<T, N>& arr) {
  for (size_t i = 1; i < arr.size(); i++) {
    auto a = std::string_view{ToCString(arr[i - 1])};
    auto b = std::string_view{ToCString(arr[i])};
    if (a.compare(b) >= 0) {
      return false;
    }
  }
  return true;
}

static_assert(IsSorted(js::temporal::detail::sortedTemporalFields));

static void AssignFromFallback(TemporalField fieldName,
                               MutableHandle<TemporalFields> result) {
  // `const` can be changed to `constexpr` when we switch to C++20.
  //
  // Hazard analysis complains when |FallbackValues| is directly contained in
  // loop body of |PrepareTemporalFields|. As a workaround the code was moved
  // into the separate |AssignFromFallback| function.
  const TemporalFields FallbackValues{};

  switch (fieldName) {
    case TemporalField::Year:
      result.year() = FallbackValues.year;
      break;
    case TemporalField::Month:
      result.month() = FallbackValues.month;
      break;
    case TemporalField::MonthCode:
      result.monthCode().set(FallbackValues.monthCode);
      break;
    case TemporalField::Day:
      result.day() = FallbackValues.day;
      break;
    case TemporalField::Hour:
      result.hour() = FallbackValues.hour;
      break;
    case TemporalField::Minute:
      result.minute() = FallbackValues.minute;
      break;
    case TemporalField::Second:
      result.second() = FallbackValues.second;
      break;
    case TemporalField::Millisecond:
      result.millisecond() = FallbackValues.millisecond;
      break;
    case TemporalField::Microsecond:
      result.microsecond() = FallbackValues.microsecond;
      break;
    case TemporalField::Nanosecond:
      result.nanosecond() = FallbackValues.nanosecond;
      break;
    case TemporalField::Offset:
      result.offset().set(FallbackValues.offset);
      break;
    case TemporalField::Era:
      result.era().set(FallbackValues.era);
      break;
    case TemporalField::EraYear:
      result.eraYear() = FallbackValues.eraYear;
      break;
    case TemporalField::TimeZone:
      result.timeZone().set(FallbackValues.timeZone);
      break;
  }
}

// clang-format off
//
// TODO: |fields| is often a built-in Temporal type, so we likely want to
// optimise for this case.
//
// Consider the case when PlainDate.prototype.toPlainMonthDay is called. The
// following steps are applied:
//
// 1. CalendarFields(calendar, «"day", "monthCode"») is called to retrieve the
//    relevant calendar fields. For (most?) built-in calendars this will just
//    return the input list «"day", "monthCode"».
// 2. PrepareTemporalFields(plainDate, «"day", "monthCode"») is called. This
//    will access the properties `plainDate.day` and `plainDate.monthCode`.
//   a. `plainDate.day` will call CalendarDay(calendar, plainDate).
//   b. For built-in calendars, this will simply access `plainDate.[[IsoDay]]`.
//   c. `plainDate.monthCode` will call CalendarMonthCode(calendar, plainDate).
//   d. For built-in calendars, ISOMonthCode(plainDate.[[IsoMonth]]) is called.
// 3. CalendarMonthDayFromFields(calendar, {day, monthCode}) is called.
// 4. For built-in calendars, this calls PrepareTemporalFields({day, monthCode},
//    «"day", "month", "monthCode", "year"», «"day"»).
// 5. The previous PrepareTemporalFields call is a no-op and returns {day, monthCode}.
// 6. Then ISOMonthDayFromFields({day, monthCode}, "constrain") gets called.
// 7. ResolveISOMonth(monthCode) is called to parse the just created `monthCode`.
// 8. RegulateISODate(referenceISOYear, month, day, "constrain") is called.
// 9. Finally CreateTemporalMonthDay is called to create the PlainMonthDay instance.
//
// All these steps could be simplified to just:
// 1. CreateTemporalMonthDay(referenceISOYear, plainDate.[[IsoMonth]], plainDate.[[IsoDay]]).
//
// When the following conditions are true:
// 1. The `plainDate` is a Temporal.PlainDate instance and has no overridden methods.
// 2. The `calendar` is a Temporal.Calendar instance and has no overridden methods.
// 3. Temporal.PlainDate.prototype and Temporal.Calendar.prototype are in their initial state.
// 4. Array iteration is still in its initial state. (Required by CalendarFields)
//
// PlainDate_toPlainMonthDay has an example implementation for this optimisation.
//
// clang-format on

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields )
 */
bool js::temporal::PrepareTemporalFields(
    JSContext* cx, Handle<JSObject*> fields,
    mozilla::EnumSet<TemporalField> fieldNames,
    mozilla::EnumSet<TemporalField> requiredFields,
    MutableHandle<TemporalFields> result) {
  // Steps 1-5. (Not applicable in our implementation.)

  // Step 6.
  Rooted<Value> value(cx);
  for (auto fieldName : SortedTemporalFields{fieldNames}) {
    auto* property = ToPropertyName(cx, fieldName);
    const auto* cstr = ToCString(fieldName);

    // Step 6.a. (Not applicable in our implementation.)

    // Step 6.b.i.
    if (!GetProperty(cx, fields, fields, property, &value)) {
      return false;
    }

    // Steps 6.b.ii-iii.
    if (!value.isUndefined()) {
      // Step 6.b.ii.1. (Not applicable in our implementation.)

      // Steps 6.b.ii.2-3.
      switch (fieldName) {
        case TemporalField::Year:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.year())) {
            return false;
          }
          break;
        case TemporalField::Month:
          if (!ToPositiveIntegerWithTruncation(cx, value, cstr,
                                               &result.month())) {
            return false;
          }
          break;
        case TemporalField::MonthCode: {
          JSString* str = ToPrimitiveAndRequireString(cx, value);
          if (!str) {
            return false;
          }
          result.monthCode().set(str);
          break;
        }
        case TemporalField::Day:
          if (!ToPositiveIntegerWithTruncation(cx, value, cstr,
                                               &result.day())) {
            return false;
          }
          break;
        case TemporalField::Hour:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.hour())) {
            return false;
          }
          break;
        case TemporalField::Minute:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.minute())) {
            return false;
          }
          break;
        case TemporalField::Second:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.second())) {
            return false;
          }
          break;
        case TemporalField::Millisecond:
          if (!ToIntegerWithTruncation(cx, value, cstr,
                                       &result.millisecond())) {
            return false;
          }
          break;
        case TemporalField::Microsecond:
          if (!ToIntegerWithTruncation(cx, value, cstr,
                                       &result.microsecond())) {
            return false;
          }
          break;
        case TemporalField::Nanosecond:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.nanosecond())) {
            return false;
          }
          break;
        case TemporalField::Offset: {
          JSString* str = ToPrimitiveAndRequireString(cx, value);
          if (!str) {
            return false;
          }
          result.offset().set(str);
          break;
        }
        case TemporalField::Era: {
          JSString* str = ToPrimitiveAndRequireString(cx, value);
          if (!str) {
            return false;
          }
          result.era().set(str);
          break;
        }
        case TemporalField::EraYear:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.eraYear())) {
            return false;
          }
          break;
        case TemporalField::TimeZone:
          // NB: TemporalField::TimeZone has no conversion function.
          result.timeZone().set(value);
          break;
      }
    } else {
      // Step 6.b.iii.1.
      if (requiredFields.contains(fieldName)) {
        if (auto chars = QuoteString(cx, cstr)) {
          JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                    JSMSG_TEMPORAL_MISSING_PROPERTY,
                                    chars.get());
        }
        return false;
      }

      // Steps 6.b.iii.2-3.
      AssignFromFallback(fieldName, result);
    }

    // Steps 6.c-d. (Not applicable in our implementation.)
  }

  // Step 7. (Not applicable in our implementation.)

  // Step 8.
  return true;
}

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields [ ,
 * duplicateBehaviour ] )
 */
PlainObject* js::temporal::PrepareTemporalFields(
    JSContext* cx, Handle<JSObject*> fields,
    Handle<TemporalFieldNames> fieldNames) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Rooted<PlainObject*> result(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!result) {
    return nullptr;
  }

  // Step 3. (Not applicable in our implementation.)

  // Step 4. (The list is already sorted in our implementation.)
  MOZ_ASSERT(IsSorted(fieldNames));

  // Step 5. (The list doesn't contain duplicates in our implementation.)
  MOZ_ASSERT(std::adjacent_find(fieldNames.begin(), fieldNames.end()) ==
             fieldNames.end());

  // Step 6.
  Rooted<Value> value(cx);
  for (size_t i = 0; i < fieldNames.length(); i++) {
    Handle<PropertyKey> property = fieldNames[i];

    // Step 6.a.
    MOZ_ASSERT(property != NameToId(cx->names().constructor));
    MOZ_ASSERT(property != NameToId(cx->names().proto_));

    // Step 6.b.i.
    if (!GetProperty(cx, fields, fields, property, &value)) {
      return nullptr;
    }

    // Steps 6.b.ii-iii.
    if (auto fieldName = ToTemporalField(cx, property)) {
      if (!value.isUndefined()) {
        // Step 6.b.ii.1. (Not applicable in our implementation.)

        // Step 6.b.ii.2.
        if (!TemporalFieldConvertValue(cx, *fieldName, &value)) {
          return nullptr;
        }
      } else {
        // Step 6.b.iii.1. (Not applicable in our implementation.)

        // Step 6.b.iii.2.
        value = TemporalFieldDefaultValue(*fieldName);
      }
    }

    // Steps 6.b.ii.3 and 6.b.iii.3.
    if (!DefineDataProperty(cx, result, property, value)) {
      return nullptr;
    }

    // Steps 6.c-d. (Not applicable in our implementation.)
  }

  // Step 7. (Not applicable in our implementation.)

  // Step 8.
  return result;
}

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields [ ,
 * duplicateBehaviour ] )
 */
PlainObject* js::temporal::PrepareTemporalFields(
    JSContext* cx, Handle<JSObject*> fields,
    Handle<TemporalFieldNames> fieldNames,
    mozilla::EnumSet<TemporalField> requiredFields) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Rooted<PlainObject*> result(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!result) {
    return nullptr;
  }

  // Step 3. (Not applicable in our implementation.)

  // Step 4. (The list is already sorted in our implementation.)
  MOZ_ASSERT(IsSorted(fieldNames));

  // Step 5. (The list doesn't contain duplicates in our implementation.)
  MOZ_ASSERT(std::adjacent_find(fieldNames.begin(), fieldNames.end()) ==
             fieldNames.end());

  // Step 6.
  Rooted<Value> value(cx);
  for (size_t i = 0; i < fieldNames.length(); i++) {
    Handle<PropertyKey> property = fieldNames[i];

    // Step 6.a.
    MOZ_ASSERT(property != NameToId(cx->names().constructor));
    MOZ_ASSERT(property != NameToId(cx->names().proto_));

    // Step 6.b.i.
    if (!GetProperty(cx, fields, fields, property, &value)) {
      return nullptr;
    }

    // Steps 6.b.ii-iii.
    if (auto fieldName = ToTemporalField(cx, property)) {
      if (!value.isUndefined()) {
        // Step 6.b.ii.1. (Not applicable in our implementation.)

        // Step 6.b.ii.2.
        if (!TemporalFieldConvertValue(cx, *fieldName, &value)) {
          return nullptr;
        }
      } else {
        // Step 6.b.iii.1.
        if (requiredFields.contains(*fieldName)) {
          if (auto chars = QuoteString(cx, property.toString())) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                      JSMSG_TEMPORAL_MISSING_PROPERTY,
                                      chars.get());
          }
          return nullptr;
        }

        // Step 6.b.iii.2.
        value = TemporalFieldDefaultValue(*fieldName);
      }
    }

    // Steps 6.b.ii.3 and 6.b.iii.3.
    if (!DefineDataProperty(cx, result, property, value)) {
      return nullptr;
    }

    // Steps 6.c-d. (Not applicable in our implementation.)
  }

  // Step 7. (Not applicable in our implementation.)

  // Step 8.
  return result;
}

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields [ ,
 * duplicateBehaviour ] )
 */
PlainObject* js::temporal::PreparePartialTemporalFields(
    JSContext* cx, Handle<JSObject*> fields,
    Handle<TemporalFieldNames> fieldNames) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Rooted<PlainObject*> result(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!result) {
    return nullptr;
  }

  // Step 3.
  bool any = false;

  // Step 4. (The list is already sorted in our implementation.)
  MOZ_ASSERT(IsSorted(fieldNames));

  // Step 5. (The list doesn't contain duplicates in our implementation.)
  MOZ_ASSERT(std::adjacent_find(fieldNames.begin(), fieldNames.end()) ==
             fieldNames.end());

  // Step 6.
  Rooted<Value> value(cx);
  for (size_t i = 0; i < fieldNames.length(); i++) {
    Handle<PropertyKey> property = fieldNames[i];

    // Step 6.a.
    MOZ_ASSERT(property != NameToId(cx->names().constructor));
    MOZ_ASSERT(property != NameToId(cx->names().proto_));

    // Step 6.b.i.
    if (!GetProperty(cx, fields, fields, property, &value)) {
      return nullptr;
    }

    // Steps 6.b.ii-iii.
    if (!value.isUndefined()) {
      // Step 6.b.ii.1.
      any = true;

      // Step 6.b.ii.2.
      if (auto fieldName = ToTemporalField(cx, property)) {
        if (!TemporalFieldConvertValue(cx, *fieldName, &value)) {
          return nullptr;
        }
      }

      // Steps 6.b.ii.3.
      if (!DefineDataProperty(cx, result, property, value)) {
        return nullptr;
      }
    } else {
      // Step 6.b.iii. (Not applicable in our implementation.)
    }

    // Steps 6.c-d. (Not applicable in our implementation.)
  }

  // Step 7.
  if (!any) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_MISSING_TEMPORAL_FIELDS);
    return nullptr;
  }

  // Step 8.
  return result;
}

/**
 * PrepareCalendarFieldsAndFieldNames ( calendarRec, fields, calendarFieldNames
 * [ , nonCalendarFieldNames [ , requiredFieldNames ] ] )
 */
static bool PrepareCalendarFieldsAndFieldNames(
    JSContext* cx, Handle<CalendarRecord> calendar, Handle<JSObject*> fields,
    mozilla::EnumSet<CalendarField> calendarFieldNames,
    mozilla::EnumSet<TemporalField> nonCalendarFieldNames,
    mozilla::EnumSet<TemporalField> requiredFieldNames,
    MutableHandle<PlainObject*> resultFields,
    MutableHandle<TemporalFieldNames> resultFieldNames) {
  // Steps 1-2. (Not applicable in our implementation.)

  // Steps 3-4.
  JS::RootedVector<PropertyKey> fieldNames(cx);
  if (!CalendarFields(cx, calendar, calendarFieldNames, &fieldNames)) {
    return false;
  }

  // Step 5.
  if (nonCalendarFieldNames.size() != 0) {
    if (!AppendSorted(cx, fieldNames.get(), nonCalendarFieldNames)) {
      return false;
    }
  }

  // Step 6.
  PlainObject* flds;
  if (requiredFieldNames.size() == 0) {
    flds = PrepareTemporalFields(cx, fields, fieldNames);
  } else {
    flds = PrepareTemporalFields(cx, fields, fieldNames, requiredFieldNames);
  }
  if (!flds) {
    return false;
  }

  // Step 7.
  resultFields.set(flds);
  resultFieldNames.set(std::move(fieldNames.get()));
  return true;
}

/**
 * PrepareCalendarFieldsAndFieldNames ( calendarRec, fields, calendarFieldNames
 * [ , nonCalendarFieldNames [ , requiredFieldNames ] ] )
 */
bool js::temporal::PrepareCalendarFieldsAndFieldNames(
    JSContext* cx, Handle<CalendarRecord> calendar, Handle<JSObject*> fields,
    mozilla::EnumSet<CalendarField> calendarFieldNames,
    MutableHandle<PlainObject*> resultFields,
    MutableHandle<TemporalFieldNames> resultFieldNames) {
  return ::PrepareCalendarFieldsAndFieldNames(cx, calendar, fields,
                                              calendarFieldNames, {}, {},
                                              resultFields, resultFieldNames);
}

#ifdef DEBUG
static auto AsTemporalFieldSet(mozilla::EnumSet<CalendarField> values) {
  using T = std::underlying_type_t<TemporalField>;
  static_assert(std::is_same_v<T, std::underlying_type_t<CalendarField>>);

  static_assert(static_cast<T>(TemporalField::Year) ==
                static_cast<T>(CalendarField::Year));
  static_assert(static_cast<T>(TemporalField::Month) ==
                static_cast<T>(CalendarField::Month));
  static_assert(static_cast<T>(TemporalField::MonthCode) ==
                static_cast<T>(CalendarField::MonthCode));
  static_assert(static_cast<T>(TemporalField::Day) ==
                static_cast<T>(CalendarField::Day));

  auto result = mozilla::EnumSet<TemporalField>{};
  result.deserialize(values.serialize());
  return result;
}

static constexpr mozilla::EnumSet<TemporalField> NonCalendarFieldNames = {
    TemporalField::Hour,        TemporalField::Minute,
    TemporalField::Second,      TemporalField::Millisecond,
    TemporalField::Microsecond, TemporalField::Nanosecond,
    TemporalField::Offset,      TemporalField::TimeZone,
};
#endif

/**
 * PrepareCalendarFields ( calendarRec, fields, calendarFieldNames,
 * nonCalendarFieldNames, requiredFieldNames )
 */
PlainObject* js::temporal::PrepareCalendarFields(
    JSContext* cx, Handle<CalendarRecord> calendar, Handle<JSObject*> fields,
    mozilla::EnumSet<CalendarField> calendarFieldNames,
    mozilla::EnumSet<TemporalField> nonCalendarFieldNames,
    mozilla::EnumSet<TemporalField> requiredFieldNames) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  //
  // Ensure `nonCalendarFieldNames ⊆ NonCalendarFieldNames`.
  MOZ_ASSERT(NonCalendarFieldNames.contains(nonCalendarFieldNames));

  // Step 3.
  //
  // Ensure `requiredFieldNames ⊆ (calendarFieldNames ∪ nonCalendarFieldNames)`.
  MOZ_ASSERT((AsTemporalFieldSet(calendarFieldNames) + nonCalendarFieldNames)
                 .contains(requiredFieldNames));

  // Steps 4-5.
  Rooted<PlainObject*> resultFields(cx);
  JS::RootedVector<PropertyKey> resultFieldNames(cx);
  if (!::PrepareCalendarFieldsAndFieldNames(
          cx, calendar, fields, calendarFieldNames, nonCalendarFieldNames,
          requiredFieldNames, &resultFields, &resultFieldNames)) {
    return nullptr;
  }
  return resultFields;
}

/**
 * Performs list-concatenation, removes any duplicates, and sorts the result.
 */
bool js::temporal::ConcatTemporalFieldNames(
    const TemporalFieldNames& receiverFieldNames,
    const TemporalFieldNames& inputFieldNames,
    TemporalFieldNames& concatenatedFieldNames) {
  MOZ_ASSERT(IsSorted(receiverFieldNames));
  MOZ_ASSERT(IsSorted(inputFieldNames));
  MOZ_ASSERT(concatenatedFieldNames.empty());

  auto appendUnique = [&](auto key) {
    if (concatenatedFieldNames.empty() ||
        concatenatedFieldNames.back() != key) {
      return concatenatedFieldNames.append(key);
    }
    return true;
  };

  size_t i = 0;
  size_t j = 0;

  // Append the names from |receiverFieldNames| and |inputFieldNames|.
  while (i < receiverFieldNames.length() && j < inputFieldNames.length()) {
    auto x = receiverFieldNames[i];
    auto y = inputFieldNames[j];

    PropertyKey z;
    if (ComparePropertyKey(x, y) <= 0) {
      z = x;
      i++;
    } else {
      z = y;
      j++;
    }
    if (!appendUnique(z)) {
      return false;
    }
  }

  // Append the remaining names from |receiverFieldNames|.
  while (i < receiverFieldNames.length()) {
    if (!appendUnique(receiverFieldNames[i++])) {
      return false;
    }
  }

  // Append the remaining names from |inputFieldNames|.
  while (j < inputFieldNames.length()) {
    if (!appendUnique(inputFieldNames[j++])) {
      return false;
    }
  }

  return true;
}

bool js::temporal::AppendSorted(
    JSContext* cx, TemporalFieldNames& fieldNames,
    mozilla::EnumSet<TemporalField> additionalNames) {
  // |fieldNames| is sorted and doesn't include any duplicates
  MOZ_ASSERT(IsSorted(fieldNames));
  MOZ_ASSERT(std::adjacent_find(fieldNames.begin(), fieldNames.end()) ==
             fieldNames.end());

  // |additionalNames| is non-empty.
  MOZ_ASSERT(additionalNames.size() > 0);

  // Allocate space for entries from |additionalNames|.
  if (!fieldNames.growBy(additionalNames.size())) {
    return false;
  }

  auto sortedAdditionalNames = SortedTemporalFields{additionalNames};
  const auto sortedAdditionalNamesBegin = sortedAdditionalNames.begin();

  const auto* left = std::prev(fieldNames.end(), additionalNames.size());
  auto right = sortedAdditionalNames.end();
  auto* out = fieldNames.end();

  // Write backwards into the newly allocated space.
  while (left != fieldNames.begin() && right != sortedAdditionalNamesBegin) {
    MOZ_ASSERT(out != fieldNames.begin());
    auto x = *std::prev(left);
    auto y = NameToId(ToPropertyName(cx, *std::prev(right)));

    int32_t r = ComparePropertyKey(x, y);

    // Reject duplicates per PrepareTemporalFields, step 6.c.
    if (r == 0) {
      if (auto chars = QuoteString(cx, x)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_TEMPORAL_DUPLICATE_PROPERTY,
                                  chars.get());
      }
      return false;
    }

    // Insert the lexicographically greater key.
    PropertyKey z;
    if (r > 0) {
      z = x;
      left--;
    } else {
      z = y;
      right--;
    }
    *--out = z;
  }

  // Avoid unnecessary copying if possible.
  if (left == out) {
    MOZ_ASSERT(right == sortedAdditionalNamesBegin);
    return true;
  }

  // Prepend the remaining names from |fieldNames|.
  while (left != fieldNames.begin()) {
    MOZ_ASSERT(out != fieldNames.begin());
    *--out = *--left;
  }

  // Prepend the remaining names from |additionalNames|.
  while (right != sortedAdditionalNamesBegin) {
    MOZ_ASSERT(out != fieldNames.begin());
    *--out = NameToId(ToPropertyName(cx, *--right));
  }

  // All field names were written into the result list.
  MOZ_ASSERT(out == fieldNames.begin());

  return true;
}

bool js::temporal::SortTemporalFieldNames(JSContext* cx,
                                          TemporalFieldNames& fieldNames) {
  // Create scratch space for MergeSort().
  TemporalFieldNames scratch(cx);
  if (!scratch.resize(fieldNames.length())) {
    return false;
  }

  // Sort all field names in alphabetical order.
  auto comparator = [](const auto& x, const auto& y, bool* lessOrEqual) {
    *lessOrEqual = ComparePropertyKey(x, y) <= 0;
    return true;
  };
  MOZ_ALWAYS_TRUE(MergeSort(fieldNames.begin(), fieldNames.length(),
                            scratch.begin(), comparator));

  for (size_t i = 0; i < fieldNames.length(); i++) {
    auto property = fieldNames[i];

    // Reject "constructor" and "__proto__" per PrepareTemporalFields, step 6.a.
    if (property == NameToId(cx->names().constructor) ||
        property == NameToId(cx->names().proto_)) {
      if (auto chars = QuoteString(cx, property)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_TEMPORAL_INVALID_PROPERTY, chars.get());
      }
      return false;
    }

    // Reject duplicates per PrepareTemporalFields, step 6.c.
    if (i > 0 && property == fieldNames[i - 1]) {
      if (auto chars = QuoteString(cx, property)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_TEMPORAL_DUPLICATE_PROPERTY,
                                  chars.get());
      }
      return false;
    }
  }

  return true;
}
