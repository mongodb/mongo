/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/PlainDate.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <stdint.h>
#include <type_traits>
#include <utility>

#include "jsnum.h"
#include "jspubtd.h"
#include "jstypes.h"
#include "NamespaceImports.h"

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainMonthDay.h"
#include "builtin/temporal/PlainTime.h"
#include "builtin/temporal/PlainYearMonth.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalFields.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/TimeZone.h"
#include "builtin/temporal/ToString.h"
#include "builtin/temporal/Wrapped.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "ds/IdValuePair.h"
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "js/AllocPolicy.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/Date.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/GCVector.h"
#include "js/Id.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/BytecodeUtil.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"
#include "vm/PropertyInfo.h"
#include "vm/Realm.h"
#include "vm/Shape.h"
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;
using namespace js::temporal;

static inline bool IsPlainDate(Handle<Value> v) {
  return v.isObject() && v.toObject().is<PlainDateObject>();
}

#ifdef DEBUG
/**
 * IsValidISODate ( year, month, day )
 */
template <typename T>
static bool IsValidISODate(T year, T month, T day) {
  static_assert(std::is_same_v<T, int32_t> || std::is_same_v<T, double>);

  // Step 1.
  MOZ_ASSERT(IsInteger(year));
  MOZ_ASSERT(IsInteger(month));
  MOZ_ASSERT(IsInteger(day));

  // Step 2.
  if (month < 1 || month > 12) {
    return false;
  }

  // Step 3.
  int32_t daysInMonth = js::temporal::ISODaysInMonth(year, int32_t(month));

  // Steps 4-5.
  return 1 <= day && day <= daysInMonth;
}

/**
 * IsValidISODate ( year, month, day )
 */
bool js::temporal::IsValidISODate(const PlainDate& date) {
  const auto& [year, month, day] = date;
  return ::IsValidISODate(year, month, day);
}

/**
 * IsValidISODate ( year, month, day )
 */
bool js::temporal::IsValidISODate(double year, double month, double day) {
  return ::IsValidISODate(year, month, day);
}
#endif

static void ReportInvalidDateValue(JSContext* cx, const char* name, int32_t min,
                                   int32_t max, double num) {
  Int32ToCStringBuf minCbuf;
  const char* minStr = Int32ToCString(&minCbuf, min);

  Int32ToCStringBuf maxCbuf;
  const char* maxStr = Int32ToCString(&maxCbuf, max);

  ToCStringBuf numCbuf;
  const char* numStr = NumberToCString(&numCbuf, num);

  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_TEMPORAL_PLAIN_DATE_INVALID_VALUE, name,
                            minStr, maxStr, numStr);
}

template <typename T>
static inline bool ThrowIfInvalidDateValue(JSContext* cx, const char* name,
                                           int32_t min, int32_t max, T num) {
  if (min <= num && num <= max) {
    return true;
  }
  ReportInvalidDateValue(cx, name, min, max, num);
  return false;
}

/**
 * IsValidISODate ( year, month, day )
 */
template <typename T>
static bool ThrowIfInvalidISODate(JSContext* cx, T year, T month, T day) {
  static_assert(std::is_same_v<T, int32_t> || std::is_same_v<T, double>);

  // Step 1.
  MOZ_ASSERT(IsInteger(year));
  MOZ_ASSERT(IsInteger(month));
  MOZ_ASSERT(IsInteger(day));

  // Step 2.
  if (!ThrowIfInvalidDateValue(cx, "month", 1, 12, month)) {
    return false;
  }

  // Step 3.
  int32_t daysInMonth = js::temporal::ISODaysInMonth(year, int32_t(month));

  // Steps 4-5.
  return ThrowIfInvalidDateValue(cx, "day", 1, daysInMonth, day);
}

/**
 * IsValidISODate ( year, month, day )
 */
bool js::temporal::ThrowIfInvalidISODate(JSContext* cx, const PlainDate& date) {
  const auto& [year, month, day] = date;
  return ::ThrowIfInvalidISODate(cx, year, month, day);
}

/**
 * IsValidISODate ( year, month, day )
 */
bool js::temporal::ThrowIfInvalidISODate(JSContext* cx, double year,
                                         double month, double day) {
  return ::ThrowIfInvalidISODate(cx, year, month, day);
}

/**
 * RegulateISODate ( year, month, day, overflow )
 *
 * With |overflow = "constrain"|.
 */
static PlainDate ConstrainISODate(const PlainDate& date) {
  const auto& [year, month, day] = date;

  // Step 1.a.
  int32_t m = std::clamp(month, 1, 12);

  // Step 1.b.
  int32_t daysInMonth = temporal::ISODaysInMonth(year, m);

  // Step 1.c.
  int32_t d = std::clamp(day, 1, daysInMonth);

  // Step 1.d.
  return {year, m, d};
}

/**
 * RegulateISODate ( year, month, day, overflow )
 */
bool js::temporal::RegulateISODate(JSContext* cx, const PlainDate& date,
                                   TemporalOverflow overflow,
                                   PlainDate* result) {
  // Step 1.
  if (overflow == TemporalOverflow::Constrain) {
    *result = ::ConstrainISODate(date);
    return true;
  }

  // Step 2.a.
  MOZ_ASSERT(overflow == TemporalOverflow::Reject);

  // Step 2.b.
  if (!ThrowIfInvalidISODate(cx, date)) {
    return false;
  }

  // Step 2.b. (Inlined call to CreateISODateRecord.)
  *result = date;
  return true;
}

/**
 * RegulateISODate ( year, month, day, overflow )
 */
bool js::temporal::RegulateISODate(JSContext* cx, double year, double month,
                                   double day, TemporalOverflow overflow,
                                   RegulatedISODate* result) {
  MOZ_ASSERT(IsInteger(year));
  MOZ_ASSERT(IsInteger(month));
  MOZ_ASSERT(IsInteger(day));

  // Step 1.
  if (overflow == TemporalOverflow::Constrain) {
    // Step 1.a.
    int32_t m = int32_t(std::clamp(month, 1.0, 12.0));

    // Step 1.b.
    double daysInMonth = double(ISODaysInMonth(year, m));

    // Step 1.c.
    int32_t d = int32_t(std::clamp(day, 1.0, daysInMonth));

    // Step 1.d.
    *result = {year, m, d};
    return true;
  }

  // Step 2.a.
  MOZ_ASSERT(overflow == TemporalOverflow::Reject);

  // Step 2.b.
  if (!ThrowIfInvalidISODate(cx, year, month, day)) {
    return false;
  }

  // Step 2.b. (Inlined call to CreateISODateRecord.)
  *result = {year, int32_t(month), int32_t(day)};
  return true;
}

/**
 * CreateTemporalDate ( isoYear, isoMonth, isoDay, calendar [ , newTarget ] )
 */
static PlainDateObject* CreateTemporalDate(JSContext* cx, const CallArgs& args,
                                           double isoYear, double isoMonth,
                                           double isoDay,
                                           Handle<CalendarValue> calendar) {
  MOZ_ASSERT(IsInteger(isoYear));
  MOZ_ASSERT(IsInteger(isoMonth));
  MOZ_ASSERT(IsInteger(isoDay));

  // Step 1.
  if (!ThrowIfInvalidISODate(cx, isoYear, isoMonth, isoDay)) {
    return nullptr;
  }

  // Step 2.
  if (!ISODateTimeWithinLimits(isoYear, isoMonth, isoDay)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return nullptr;
  }

  // Steps 3-4.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_PlainDate,
                                          &proto)) {
    return nullptr;
  }

  auto* object = NewObjectWithClassProto<PlainDateObject>(cx, proto);
  if (!object) {
    return nullptr;
  }

  // Step 5.
  object->setFixedSlot(PlainDateObject::ISO_YEAR_SLOT,
                       Int32Value(int32_t(isoYear)));

  // Step 6.
  object->setFixedSlot(PlainDateObject::ISO_MONTH_SLOT,
                       Int32Value(int32_t(isoMonth)));

  // Step 7.
  object->setFixedSlot(PlainDateObject::ISO_DAY_SLOT,
                       Int32Value(int32_t(isoDay)));

  // Step 8.
  object->setFixedSlot(PlainDateObject::CALENDAR_SLOT, calendar.toSlotValue());

  // Step 9.
  return object;
}

/**
 * CreateTemporalDate ( isoYear, isoMonth, isoDay, calendar [ , newTarget ] )
 */
PlainDateObject* js::temporal::CreateTemporalDate(
    JSContext* cx, const PlainDate& date, Handle<CalendarValue> calendar) {
  const auto& [isoYear, isoMonth, isoDay] = date;

  // Step 1.
  if (!ThrowIfInvalidISODate(cx, date)) {
    return nullptr;
  }

  // Step 2.
  if (!ISODateTimeWithinLimits(date)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return nullptr;
  }

  // Steps 3-4.
  auto* object = NewBuiltinClassInstance<PlainDateObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Step 5.
  object->setFixedSlot(PlainDateObject::ISO_YEAR_SLOT, Int32Value(isoYear));

  // Step 6.
  object->setFixedSlot(PlainDateObject::ISO_MONTH_SLOT, Int32Value(isoMonth));

  // Step 7.
  object->setFixedSlot(PlainDateObject::ISO_DAY_SLOT, Int32Value(isoDay));

  // Step 8.
  object->setFixedSlot(PlainDateObject::CALENDAR_SLOT, calendar.toSlotValue());

  // Step 9.
  return object;
}

/**
 * CreateTemporalDate ( isoYear, isoMonth, isoDay, calendar [ , newTarget ] )
 */
bool js::temporal::CreateTemporalDate(
    JSContext* cx, const PlainDate& date, Handle<CalendarValue> calendar,
    MutableHandle<PlainDateWithCalendar> result) {
  // Step 1.
  if (!ThrowIfInvalidISODate(cx, date)) {
    return false;
  }

  // Step 2.
  if (!ISODateTimeWithinLimits(date)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  // Steps 3-9.
  result.set(PlainDateWithCalendar{date, calendar});
  return true;
}

/**
 * ToTemporalDate ( item [ , options ] )
 */
static Wrapped<PlainDateObject*> ToTemporalDate(
    JSContext* cx, Handle<JSObject*> item, Handle<PlainObject*> maybeOptions) {
  // Step 1-2. (Not applicable in our implementation.)

  // Step 3.a.
  if (item->canUnwrapAs<PlainDateObject>()) {
    return item;
  }

  // Step 3.b.
  if (auto* zonedDateTime = item->maybeUnwrapIf<ZonedDateTimeObject>()) {
    auto epochInstant = ToInstant(zonedDateTime);
    Rooted<TimeZoneValue> timeZone(cx, zonedDateTime->timeZone());
    Rooted<CalendarValue> calendar(cx, zonedDateTime->calendar());

    if (!timeZone.wrap(cx)) {
      return nullptr;
    }
    if (!calendar.wrap(cx)) {
      return nullptr;
    }

    // Step 3.b.i.
    if (maybeOptions) {
      TemporalOverflow ignored;
      if (!GetTemporalOverflowOption(cx, maybeOptions, &ignored)) {
        return nullptr;
      }
    }

    // Steps 3.b.ii-iv.
    PlainDateTime dateTime;
    if (!GetPlainDateTimeFor(cx, timeZone, epochInstant, &dateTime)) {
      return nullptr;
    }

    // Step 3.b.v.
    return CreateTemporalDate(cx, dateTime.date, calendar);
  }

  // Step 3.c.
  if (auto* dateTime = item->maybeUnwrapIf<PlainDateTimeObject>()) {
    auto date = ToPlainDate(dateTime);
    Rooted<CalendarValue> calendar(cx, dateTime->calendar());
    if (!calendar.wrap(cx)) {
      return nullptr;
    }

    // Step 3.c.i.
    if (maybeOptions) {
      TemporalOverflow ignored;
      if (!GetTemporalOverflowOption(cx, maybeOptions, &ignored)) {
        return nullptr;
      }
    }

    // Step 3.c.ii.
    return CreateTemporalDate(cx, date, calendar);
  }

  // Step 3.d.
  Rooted<CalendarValue> calendarValue(cx);
  if (!GetTemporalCalendarWithISODefault(cx, item, &calendarValue)) {
    return nullptr;
  }

  // Step 3.e.
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, calendarValue,
                                   {
                                       CalendarMethod::DateFromFields,
                                       CalendarMethod::Fields,
                                   },
                                   &calendar)) {
    return nullptr;
  }

  // Step 3.f.
  Rooted<PlainObject*> fields(
      cx, PrepareCalendarFields(cx, calendar, item,
                                {
                                    CalendarField::Day,
                                    CalendarField::Month,
                                    CalendarField::MonthCode,
                                    CalendarField::Year,
                                }));
  if (!fields) {
    return nullptr;
  }

  // Step 3.g.
  if (maybeOptions) {
    return temporal::CalendarDateFromFields(cx, calendar, fields, maybeOptions);
  }
  return temporal::CalendarDateFromFields(cx, calendar, fields);
}

/**
 * ToTemporalDate ( item [ , options ] )
 */
static Wrapped<PlainDateObject*> ToTemporalDate(
    JSContext* cx, Handle<Value> item, Handle<JSObject*> maybeOptions) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Rooted<PlainObject*> maybeResolvedOptions(cx);
  if (maybeOptions) {
    maybeResolvedOptions = SnapshotOwnProperties(cx, maybeOptions);
    if (!maybeResolvedOptions) {
      return nullptr;
    }
  }

  // Step 3.
  if (item.isObject()) {
    Rooted<JSObject*> itemObj(cx, &item.toObject());
    return ::ToTemporalDate(cx, itemObj, maybeResolvedOptions);
  }

  // Step 4.
  if (!item.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, item,
                     nullptr, "not a string");
    return nullptr;
  }
  Rooted<JSString*> string(cx, item.toString());

  // Step 5.
  PlainDate result;
  Rooted<JSString*> calendarString(cx);
  if (!ParseTemporalDateString(cx, string, &result, &calendarString)) {
    return nullptr;
  }

  // Step 6.
  MOZ_ASSERT(IsValidISODate(result));

  // Steps 7-10.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  if (calendarString) {
    if (!ToBuiltinCalendar(cx, calendarString, &calendar)) {
      return nullptr;
    }
  }

  // Step 11.
  if (maybeResolvedOptions) {
    TemporalOverflow ignored;
    if (!GetTemporalOverflowOption(cx, maybeResolvedOptions, &ignored)) {
      return nullptr;
    }
  }

  // Step 12.
  return CreateTemporalDate(cx, result, calendar);
}

/**
 * ToTemporalDate ( item [ , options ] )
 */
static Wrapped<PlainDateObject*> ToTemporalDate(JSContext* cx,
                                                Handle<Value> item) {
  return ::ToTemporalDate(cx, item, nullptr);
}

/**
 * ToTemporalDate ( item [ , options ] )
 */
bool js::temporal::ToTemporalDate(JSContext* cx, Handle<Value> item,
                                  PlainDate* result) {
  auto obj = ::ToTemporalDate(cx, item, nullptr);
  if (!obj) {
    return false;
  }

  *result = ToPlainDate(&obj.unwrap());
  return true;
}

/**
 * ToTemporalDate ( item [ , options ] )
 */
bool js::temporal::ToTemporalDate(JSContext* cx, Handle<Value> item,
                                  MutableHandle<PlainDateWithCalendar> result) {
  auto* obj = ::ToTemporalDate(cx, item, nullptr).unwrapOrNull();
  if (!obj) {
    return false;
  }

  auto date = ToPlainDate(obj);
  Rooted<CalendarValue> calendar(cx, obj->calendar());
  if (!calendar.wrap(cx)) {
    return false;
  }

  result.set(PlainDateWithCalendar{date, calendar});
  return true;
}

/**
 * Mathematical Operations, "modulo" notation.
 */
static int32_t NonNegativeModulo(int64_t x, int32_t y) {
  MOZ_ASSERT(y > 0);

  int32_t result = mozilla::AssertedCast<int32_t>(x % y);
  return (result < 0) ? (result + y) : result;
}

struct BalancedYearMonth final {
  int64_t year = 0;
  int32_t month = 0;
};

/**
 * BalanceISOYearMonth ( year, month )
 */
static BalancedYearMonth BalanceISOYearMonth(int64_t year, int64_t month) {
  MOZ_ASSERT(std::abs(year) < (int64_t(1) << 33),
             "year is the addition of plain-date year with duration years");
  MOZ_ASSERT(std::abs(month) < (int64_t(1) << 33),
             "month is the addition of plain-date month with duration months");

  // Step 1. (Not applicable in our implementation.)

  // Note: If either abs(year) or abs(month) is greater than 2^53 (the double
  // integral precision limit), the additions resp. subtractions below are
  // imprecise. This doesn't matter for us, because the single caller to this
  // function (AddISODate) will throw an error for large values anyway.

  // Step 2.
  int64_t balancedYear = year + temporal::FloorDiv(month - 1, 12);

  // Step 3.
  int32_t balancedMonth = NonNegativeModulo(month - 1, 12) + 1;
  MOZ_ASSERT(1 <= balancedMonth && balancedMonth <= 12);

  // Step 4.
  return {balancedYear, balancedMonth};
}

static bool CanBalanceISOYear(int64_t year) {
  // TODO: Export these values somewhere.
  constexpr int32_t minYear = -271821;
  constexpr int32_t maxYear = 275760;

  // If the year is below resp. above the min-/max-year, no value of |day| will
  // make the resulting date valid.
  return minYear <= year && year <= maxYear;
}

static bool CanBalanceISODay(int64_t day) {
  // The maximum number of seconds from the epoch is 8.64 * 10^12.
  constexpr int64_t maxInstantSeconds = 8'640'000'000'000;

  // In days that makes 10^8.
  constexpr int64_t maxInstantDays = maxInstantSeconds / 60 / 60 / 24;

  // Multiply by two to take both directions into account and add twenty to
  // account for the day number of the minimum date "-271821-02-20".
  constexpr int64_t maximumDayDifference = 2 * maxInstantDays + 20;

  // When |day| is below |maximumDayDifference|, it can be represented as int32.
  static_assert(maximumDayDifference <= INT32_MAX);

  // When the day difference exceeds the maximum valid day difference, the
  // overall result won't be a valid date. Detect this early so we don't have to
  // struggle with floating point precision issues in BalanceISODate.
  //
  // This also means BalanceISODate, step 1 doesn't apply to our implementation.
  return std::abs(day) <= maximumDayDifference;
}

/**
 * BalanceISODate ( year, month, day )
 */
PlainDate js::temporal::BalanceISODateNew(int32_t year, int32_t month,
                                          int32_t day) {
  MOZ_ASSERT(1 <= month && month <= 12);

  // Steps 1-3.
  double ms = double(MakeDate(year, month, day));

  // TODO: Add ISODateToEpochDays & friends which handle larger inputs.

  // TODO: This approach isn't efficient, because MonthFromTime and DayFromTime
  // both recompute YearFromTime.

  // Step 4.
  return {int32_t(JS::YearFromTime(ms)), int32_t(JS::MonthFromTime(ms) + 1),
          int32_t(JS::DayFromTime(ms))};
}

/**
 * BalanceISODate ( year, month, day )
 */
bool js::temporal::BalanceISODate(JSContext* cx, const PlainDate& date,
                                  int64_t days, PlainDate* result) {
  MOZ_ASSERT(IsValidISODate(date));
  MOZ_ASSERT(ISODateTimeWithinLimits(date));

  int64_t day = int64_t(date.day) + days;
  if (!CanBalanceISODay(day)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  *result = BalanceISODate(date.year, date.month, int32_t(day));
  return true;
}

/**
 * BalanceISODate ( year, month, day )
 */
PlainDate js::temporal::BalanceISODate(int32_t year, int32_t month,
                                       int32_t day) {
  // Check no inputs can lead to floating point precision issues below. This
  // also ensures all loops can finish in reasonable time, so we don't need to
  // worry about interrupts here. And it ensures there won't be overflows when
  // using int32_t values.
  MOZ_ASSERT(CanBalanceISOYear(year));
  MOZ_ASSERT(1 <= month && month <= 12);
  MOZ_ASSERT(CanBalanceISODay(day));

  // TODO: BalanceISODate now works using ISODateToEpochDays & friends.
  // TODO: Can't use JS::MakeDate, because it expects valid month/day values.

  // Step 1. (Not applicable in our implementation.)

  // Steps 3-4. (Not applicable in our implementation.)

  constexpr int32_t daysInNonLeapYear = 365;

  // Skip steps 5-11 for the common case when abs(day) doesn't exceed 365.
  if (std::abs(day) > daysInNonLeapYear) {
    // Step 5. (Note)

    // Steps 6-7.
    int32_t testYear = month > 2 ? year : year - 1;

    // Step 8.
    while (day < -ISODaysInYear(testYear)) {
      // Step 8.a.
      day += ISODaysInYear(testYear);

      // Step 8.b.
      year -= 1;

      // Step 8.c.
      testYear -= 1;
    }

    // Step 9. (Note)

    // Step 10.
    testYear += 1;

    // Step 11.
    while (day > ISODaysInYear(testYear)) {
      // Step 11.a.
      day -= ISODaysInYear(testYear);

      // Step 11.b.
      year += 1;

      // Step 11.c.
      testYear += 1;
    }
  }

  // Step 12. (Note)

  // Step 13.
  while (day < 1) {
    // Steps 13.a-b. (Inlined call to BalanceISOYearMonth.)
    if (--month == 0) {
      month = 12;
      year -= 1;
    }

    // Step 13.d
    day += ISODaysInMonth(year, month);
  }

  // Step 14. (Note)

  // Step 15.
  while (day > ISODaysInMonth(year, month)) {
    // Step 15.a.
    day -= ISODaysInMonth(year, month);

    // Steps 15.b-d. (Inlined call to BalanceISOYearMonth.)
    if (++month == 13) {
      month = 1;
      year += 1;
    }
  }

  MOZ_ASSERT(1 <= month && month <= 12);
  MOZ_ASSERT(1 <= day && day <= 31);

  // Step 16.
  return {year, month, day};
}

/**
 * AddISODate ( year, month, day, years, months, weeks, days, overflow )
 */
bool js::temporal::AddISODate(JSContext* cx, const PlainDate& date,
                              const DateDuration& duration,
                              TemporalOverflow overflow, PlainDate* result) {
  MOZ_ASSERT(IsValidISODate(date));
  MOZ_ASSERT(ISODateTimeWithinLimits(date));

  // TODO: Not quite sure if this holds for all callers. But if it does hold,
  // then we can directly reject any numbers which can't be represented with
  // int32_t. That in turn avoids the precision loss issue noted in
  // BalanceISODate.
  MOZ_ASSERT(IsValidDuration(duration));

  // Steps 1-2. (Not applicable in our implementation.)

  // Step 3.
  auto yearMonth = BalanceISOYearMonth(date.year + duration.years,
                                       date.month + duration.months);
  MOZ_ASSERT(1 <= yearMonth.month && yearMonth.month <= 12);

  // FIXME: spec issue?
  // new Temporal.PlainDate(2021, 5, 31).subtract({months:1, days:1}).toString()
  // returns "2021-04-29", but "2021-04-30" seems more likely expected.
  // Note: "2021-04-29" agrees with java.time, though.
  //
  // Example where this creates inconsistent results:
  //
  // clang-format off
  //
  // js> Temporal.PlainDate.from("2021-05-31").since("2021-04-30", {largestUnit:"months"}).toString()
  // "P1M1D"
  // js> Temporal.PlainDate.from("2021-05-31").subtract("P1M1D").toString()
  // "2021-04-29"
  //
  // clang-format on
  //
  // Later: This now returns "P1M" instead "P1M1D", so the results are at least
  // consistent. Let's add a test case for this behaviour.
  //
  // Revisit when <https://github.com/tc39/proposal-temporal/issues/2535> has
  // been addressed.

  // |yearMonth.year| can only exceed the valid years range when called from
  // `Temporal.Calendar.prototype.dateAdd`. And because `dateAdd` uses the
  // result of AddISODate to create a new Temporal.PlainDate, we can directly
  // throw an error if the result isn't within the valid date-time limits. This
  // in turn allows to work on integer values and we don't have to worry about
  // imprecise double value computations.
  if (!CanBalanceISOYear(yearMonth.year)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  // Step 4.
  PlainDate regulated;
  if (!RegulateISODate(cx, {int32_t(yearMonth.year), yearMonth.month, date.day},
                       overflow, &regulated)) {
    return false;
  }

  // NB: BalanceISODate will reject too large days, so we don't have to worry
  // about imprecise number arithmetic here.

  // Steps 5-6.
  int64_t d = regulated.day + (duration.days + duration.weeks * 7);

  // Just as with |yearMonth.year|, also directly throw an error if the |days|
  // value is too large.
  if (!CanBalanceISODay(d)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  // Step 7.
  auto balanced = BalanceISODate(regulated.year, regulated.month, int32_t(d));
  MOZ_ASSERT(IsValidISODate(balanced));

  *result = balanced;
  return true;
}

struct YearMonthDuration {
  int32_t years = 0;
  int32_t months = 0;
};

/**
 * AddISODate ( year, month, day, years, months, weeks, days, overflow )
 *
 * With |overflow = "constrain"|.
 */
static PlainDate AddISODate(const PlainDate& date,
                            const YearMonthDuration& duration) {
  MOZ_ASSERT(IsValidISODate(date));
  MOZ_ASSERT(ISODateTimeWithinLimits(date));

  MOZ_ASSERT_IF(duration.years < 0, duration.months <= 0);
  MOZ_ASSERT_IF(duration.years > 0, duration.months >= 0);

  // TODO: Export these values somewhere.
  [[maybe_unused]] constexpr int32_t minYear = -271821;
  [[maybe_unused]] constexpr int32_t maxYear = 275760;

  MOZ_ASSERT(std::abs(duration.years) <= (maxYear - minYear),
             "years doesn't exceed the maximum duration between valid years");
  MOZ_ASSERT(std::abs(duration.months) <= 12,
             "months duration is at most one year");

  // Steps 1-2. (Not applicable)

  // Step 3. (Inlined BalanceISOYearMonth)
  int32_t year = date.year + duration.years;
  int32_t month = date.month + duration.months;
  MOZ_ASSERT(-11 <= month && month <= 24);

  if (month > 12) {
    month -= 12;
    year += 1;
  } else if (month <= 0) {
    month += 12;
    year -= 1;
  }

  MOZ_ASSERT(1 <= month && month <= 12);
  MOZ_ASSERT(CanBalanceISOYear(year));

  // Steps 4-7.
  return ::ConstrainISODate({year, month, date.day});
}

static bool HasYearsMonthsOrWeeks(const Duration& duration) {
  return duration.years != 0 || duration.months != 0 || duration.weeks != 0;
}

static bool HasYearsMonthsOrWeeks(const DateDuration& duration) {
  return duration.years != 0 || duration.months != 0 || duration.weeks != 0;
}

/**
 * AddDate ( calendarRec, plainDate, duration [ , options ] )
 */
static bool AddDate(JSContext* cx, const PlainDate& date,
                    const NormalizedDuration& duration,
                    TemporalOverflow overflow, PlainDate* result) {
  MOZ_ASSERT(!HasYearsMonthsOrWeeks(duration.date));
  MOZ_ASSERT(IsValidDuration(duration));

  // Steps 1-4. (Not applicable)

  // Step 5. (Not applicable)
  const auto& timeDuration = duration.time;

  // Step 6.
  int64_t balancedDays =
      BalanceTimeDuration(timeDuration, TemporalUnit::Day).days;
  int64_t days = duration.date.days + balancedDays;

  // Step 7.
  return AddISODate(cx, date, {0, 0, 0, days}, overflow, result);
}

static bool AddDate(JSContext* cx, Handle<Wrapped<PlainDateObject*>> date,
                    const NormalizedDuration& duration,
                    TemporalOverflow overflow, PlainDate* result) {
  auto* unwrappedDate = date.unwrap(cx);
  if (!unwrappedDate) {
    return false;
  }
  return ::AddDate(cx, ToPlainDate(unwrappedDate), duration, overflow, result);
}

/**
 * AddDate ( calendarRec, plainDate, duration [ , options ] )
 */
static Wrapped<PlainDateObject*> AddDate(JSContext* cx,
                                         Handle<CalendarRecord> calendar,
                                         Handle<Wrapped<PlainDateObject*>> date,
                                         const Duration& duration,
                                         Handle<JSObject*> options) {
  // Step 1.
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));

  // Step 2. (Not applicable in our implementation.)

  // Step 3.
  if (HasYearsMonthsOrWeeks(duration)) {
    return temporal::CalendarDateAdd(cx, calendar, date, duration, options);
  }

  // Step 4.
  auto overflow = TemporalOverflow::Constrain;
  if (!GetTemporalOverflowOption(cx, options, &overflow)) {
    return nullptr;
  }

  // Step 5.
  auto normalized = CreateNormalizedDurationRecord(duration);

  // Steps 6-7.
  PlainDate resultDate;
  if (!::AddDate(cx, date, normalized, overflow, &resultDate)) {
    return nullptr;
  }

  // Step 8.
  return CreateTemporalDate(cx, resultDate, calendar.receiver());
}

/**
 * AddDate ( calendarRec, plainDate, duration [ , options ] )
 */
Wrapped<PlainDateObject*> js::temporal::AddDate(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<Wrapped<PlainDateObject*>> date, const DateDuration& duration) {
  // Step 1.
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));

  // Step 2. (Not applicable in our implementation.)

  // Step 3.
  if (HasYearsMonthsOrWeeks(duration)) {
    return CalendarDateAdd(cx, calendar, date, duration);
  }

  // Step 4.
  auto overflow = TemporalOverflow::Constrain;

  // Step 5.
  auto normalized = NormalizedDuration{duration};

  // Steps 6-7.
  PlainDate resultDate;
  if (!::AddDate(cx, date, normalized, overflow, &resultDate)) {
    return nullptr;
  }

  // Step 8.
  return CreateTemporalDate(cx, resultDate, calendar.receiver());
}

/**
 * AddDate ( calendarRec, plainDate, duration [ , options ] )
 */
Wrapped<PlainDateObject*> js::temporal::AddDate(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<Wrapped<PlainDateObject*>> date, const DateDuration& duration,
    Handle<JSObject*> options) {
  return ::AddDate(cx, calendar, date, duration.toDuration(), options);
}

/**
 * AddDate ( calendarRec, plainDate, duration [ , options ] )
 */
static Wrapped<PlainDateObject*> AddDate(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<Wrapped<PlainDateObject*>> date,
    Handle<Wrapped<DurationObject*>> durationObj, Handle<JSObject*> options) {
  auto* unwrappedDuration = durationObj.unwrap(cx);
  if (!unwrappedDuration) {
    return nullptr;
  }
  auto duration = ToDuration(unwrappedDuration);

  // Step 1.
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));

  // Step 2. (Not applicable in our implementation.)

  // Step 3.
  if (HasYearsMonthsOrWeeks(duration)) {
    return temporal::CalendarDateAdd(cx, calendar, date, durationObj, options);
  }

  // Step 4.
  auto overflow = TemporalOverflow::Constrain;
  if (!GetTemporalOverflowOption(cx, options, &overflow)) {
    return nullptr;
  }

  // Step 5.
  auto normalized = CreateNormalizedDurationRecord(duration);

  // Steps 6-7.
  PlainDate resultDate;
  if (!::AddDate(cx, date, normalized, overflow, &resultDate)) {
    return nullptr;
  }

  // Step 8.
  return CreateTemporalDate(cx, resultDate, calendar.receiver());
}

/**
 * AddDate ( calendarRec, plainDate, duration [ , options ] )
 */
bool js::temporal::AddDate(JSContext* cx, Handle<CalendarRecord> calendar,
                           const PlainDate& date, const DateDuration& duration,
                           Handle<JSObject*> options, PlainDate* result) {
  // Step 1.
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));

  // Step 2. (Not applicable in our implementation.)

  // Step 3.
  if (HasYearsMonthsOrWeeks(duration)) {
    return temporal::CalendarDateAdd(cx, calendar, date, duration, options,
                                     result);
  }

  // Step 4.
  auto overflow = TemporalOverflow::Constrain;
  if (!GetTemporalOverflowOption(cx, options, &overflow)) {
    return false;
  }

  // Step 5.
  auto normalized = NormalizedDuration{duration};

  // Steps 5-8.
  return ::AddDate(cx, date, normalized, overflow, result);
}

/**
 * AddDate ( calendarRec, plainDate, duration [ , options ] )
 */
bool js::temporal::AddDate(JSContext* cx, Handle<CalendarRecord> calendar,
                           Handle<Wrapped<PlainDateObject*>> date,
                           const DateDuration& duration, PlainDate* result) {
  // Step 1.
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));

  // Step 2. (Not applicable in our implementation.)

  // Step 3.
  if (HasYearsMonthsOrWeeks(duration)) {
    return CalendarDateAdd(cx, calendar, date, duration, result);
  }

  // Step 4.
  auto overflow = TemporalOverflow::Constrain;

  // Step 5.
  auto normalized = NormalizedDuration{duration};

  // Steps 6-8.
  return ::AddDate(cx, date, normalized, overflow, result);
}

/**
 * DifferenceDate ( calendarRec, one, two, options )
 */
bool js::temporal::DifferenceDate(JSContext* cx,
                                  Handle<CalendarRecord> calendar,
                                  Handle<Wrapped<PlainDateObject*>> one,
                                  Handle<Wrapped<PlainDateObject*>> two,
                                  TemporalUnit largestUnit,
                                  Handle<PlainObject*> options,
                                  DateDuration* result) {
  auto* unwrappedOne = one.unwrap(cx);
  if (!unwrappedOne) {
    return false;
  }
  auto oneDate = ToPlainDate(unwrappedOne);

  auto* unwrappedTwo = two.unwrap(cx);
  if (!unwrappedTwo) {
    return false;
  }
  auto twoDate = ToPlainDate(unwrappedTwo);

  // Steps 1-2. (Not applicable in our implementation.)

  // Step 3.
  MOZ_ASSERT(options->staticPrototype() == nullptr);

  // Step 4. (Not applicable in our implementation.)

  // Step 5.
  if (oneDate == twoDate) {
    *result = {};
    return true;
  }

  // Step 6.
  if (largestUnit == TemporalUnit::Day) {
    // Step 6.a.
    int32_t days = DaysUntil(oneDate, twoDate);

    // Step 6.b.
    *result = {0, 0, 0, days};
    return true;
  }

  // Step 7.
  return CalendarDateUntil(cx, calendar, one, two, largestUnit, options,
                           result);
}

/**
 * DifferenceDate ( calendarRec, one, two, options )
 */
bool js::temporal::DifferenceDate(JSContext* cx,
                                  Handle<CalendarRecord> calendar,
                                  Handle<Wrapped<PlainDateObject*>> one,
                                  Handle<Wrapped<PlainDateObject*>> two,
                                  TemporalUnit largestUnit,
                                  DateDuration* result) {
  auto* unwrappedOne = one.unwrap(cx);
  if (!unwrappedOne) {
    return false;
  }
  auto oneDate = ToPlainDate(unwrappedOne);

  auto* unwrappedTwo = two.unwrap(cx);
  if (!unwrappedTwo) {
    return false;
  }
  auto twoDate = ToPlainDate(unwrappedTwo);

  // Steps 1-4. (Not applicable in our implementation.)

  // Step 5.
  if (oneDate == twoDate) {
    *result = {};
    return true;
  }

  // Step 6.
  if (largestUnit == TemporalUnit::Day) {
    // Step 6.a.
    int32_t days = DaysUntil(oneDate, twoDate);

    // Step 6.b.
    *result = {0, 0, 0, days};
    return true;
  }

  // Step 7.
  return CalendarDateUntil(cx, calendar, one, two, largestUnit, result);
}

/**
 * DifferenceDate ( calendarRec, one, two, options )
 */
bool js::temporal::DifferenceDate(JSContext* cx,
                                  Handle<CalendarRecord> calendar,
                                  const PlainDate& one, const PlainDate& two,
                                  TemporalUnit largestUnit,
                                  Handle<PlainObject*> options,
                                  DateDuration* result) {
  // Steps 1-4. (Not applicable in our implementation.)

  // Step 5.
  if (one == two) {
    *result = {};
    return true;
  }

  // Step 6.
  if (largestUnit == TemporalUnit::Day) {
    // Step 6.a.
    int32_t days = DaysUntil(one, two);

    // Step 6.b.
    *result = {0, 0, 0, days};
    return true;
  }

  // Step 7.
  return CalendarDateUntil(cx, calendar, one, two, largestUnit, options,
                           result);
}

/**
 * DifferenceDate ( calendarRec, one, two, options )
 */
bool js::temporal::DifferenceDate(JSContext* cx,
                                  Handle<CalendarRecord> calendar,
                                  const PlainDate& one, const PlainDate& two,
                                  TemporalUnit largestUnit,
                                  DateDuration* result) {
  // Steps 1-4. (Not applicable in our implementation.)

  // Step 5.
  if (one == two) {
    *result = {};
    return true;
  }

  // Step 6.
  if (largestUnit == TemporalUnit::Day) {
    // Step 6.a.
    int32_t days = DaysUntil(one, two);

    // Step 6.b.
    *result = {0, 0, 0, days};
    return true;
  }

  // Step 7.
  return CalendarDateUntil(cx, calendar, one, two, largestUnit, result);
}

/**
 * CompareISODate ( y1, m1, d1, y2, m2, d2 )
 */
int32_t js::temporal::CompareISODate(const PlainDate& one,
                                     const PlainDate& two) {
  // Steps 1-2.
  if (one.year != two.year) {
    return one.year < two.year ? -1 : 1;
  }

  // Steps 3-4.
  if (one.month != two.month) {
    return one.month < two.month ? -1 : 1;
  }

  // Steps 5-6.
  if (one.day != two.day) {
    return one.day < two.day ? -1 : 1;
  }

  // Step 7.
  return 0;
}

/**
 * CreateDateDurationRecord ( years, months, weeks, days )
 */
static DateDuration CreateDateDurationRecord(int32_t years, int32_t months,
                                             int32_t weeks, int32_t days) {
  MOZ_ASSERT(IsValidDuration(
      Duration{double(years), double(months), double(weeks), double(days)}));
  return {years, months, weeks, days};
}

/**
 * DifferenceISODate ( y1, m1, d1, y2, m2, d2, largestUnit )
 */
DateDuration js::temporal::DifferenceISODate(const PlainDate& start,
                                             const PlainDate& end,
                                             TemporalUnit largestUnit) {
  // Steps 1-2.
  MOZ_ASSERT(IsValidISODate(start));
  MOZ_ASSERT(IsValidISODate(end));

  // Both inputs are also within the date-time limits.
  MOZ_ASSERT(ISODateTimeWithinLimits(start));
  MOZ_ASSERT(ISODateTimeWithinLimits(end));

  // Because both inputs are valid dates, we don't need to worry about integer
  // overflow in any of the computations below.

  MOZ_ASSERT(TemporalUnit::Year <= largestUnit &&
             largestUnit <= TemporalUnit::Day);

  // Step 3.
  if (largestUnit == TemporalUnit::Year || largestUnit == TemporalUnit::Month) {
    // Step 3.a.
    int32_t sign = -CompareISODate(start, end);

    // Step 3.b.
    if (sign == 0) {
      return CreateDateDurationRecord(0, 0, 0, 0);
    }

    // FIXME: spec issue - results can be ambiguous, is this intentional?
    // https://github.com/tc39/proposal-temporal/issues/2535
    //
    // clang-format off
    // js> var end = new Temporal.PlainDate(1970, 2, 28)
    // js> var start = new Temporal.PlainDate(1970, 1, 28)
    // js> start.calendar.dateUntil(start, end, {largestUnit:"months"}).toString()
    // "P1M"
    // js> var start = new Temporal.PlainDate(1970, 1, 29)
    // js> start.calendar.dateUntil(start, end, {largestUnit:"months"}).toString()
    // "P1M"
    // js> var start = new Temporal.PlainDate(1970, 1, 30)
    // js> start.calendar.dateUntil(start, end, {largestUnit:"months"}).toString()
    // "P1M"
    // js> var start = new Temporal.PlainDate(1970, 1, 31)
    // js> start.calendar.dateUntil(start, end, {largestUnit:"months"}).toString()
    // "P1M"
    //
    // Compare to java.time.temporal
    //
    // jshell> import java.time.LocalDate
    // jshell> var end = LocalDate.of(1970, 2, 28)
    // end ==> 1970-02-28
    // jshell> var start = LocalDate.of(1970, 1, 28)
    // start ==> 1970-01-28
    // jshell> start.until(end)
    // $27 ==> P1M
    // jshell> var start = LocalDate.of(1970, 1, 29)
    // start ==> 1970-01-29
    // jshell> start.until(end)
    // $29 ==> P30D
    // jshell> var start = LocalDate.of(1970, 1, 30)
    // start ==> 1970-01-30
    // jshell> start.until(end)
    // $31 ==> P29D
    // jshell> var start = LocalDate.of(1970, 1, 31)
    // start ==> 1970-01-31
    // jshell> start.until(end)
    // $33 ==> P28D
    //
    // Also compare to:
    //
    // js> var end = new Temporal.PlainDate(1970, 2, 27)
    // js> var start = new Temporal.PlainDate(1970, 1, 27)
    // js> start.calendar.dateUntil(start, end, {largestUnit:"months"}).toString()
    // "P1M"
    // js> var start = new Temporal.PlainDate(1970, 1, 28)
    // js> start.calendar.dateUntil(start, end, {largestUnit:"months"}).toString()
    // "P30D"
    // js> var start = new Temporal.PlainDate(1970, 1, 29)
    // js> start.calendar.dateUntil(start, end, {largestUnit:"months"}).toString()
    // "P29D"
    //
    // clang-format on

    // Steps 3.c-d. (Not applicable in our implementation.)

    // FIXME: spec issue - consistently use either |end.[[Year]]| or |y2|.

    // Step 3.e.
    int32_t years = end.year - start.year;

    // TODO: We could inline this, because the AddISODate call is just a more
    // complicated way to perform:
    // mid = ConstrainISODate(end.year, start.month, start.day)
    //
    // The remaining computations can probably simplified similarily.

    // Step 3.f.
    auto mid = ::AddISODate(start, {years, 0});

    // Step 3.g.
    int32_t midSign = -CompareISODate(mid, end);

    // Step 3.h.
    if (midSign == 0) {
      // Step 3.h.i.
      if (largestUnit == TemporalUnit::Year) {
        return CreateDateDurationRecord(years, 0, 0, 0);
      }

      // Step 3.h.ii.
      return CreateDateDurationRecord(0, years * 12, 0, 0);
    }

    // Step 3.i.
    int32_t months = end.month - start.month;

    // Step 3.j.
    if (midSign != sign) {
      // Step 3.j.i.
      years -= sign;

      // Step 3.j.ii.
      months += sign * 12;
    }

    // Step 3.k.
    mid = ::AddISODate(start, {years, months});

    // Step 3.l.
    midSign = -CompareISODate(mid, end);

    // Step 3.m.
    if (midSign == 0) {
      // Step 3.m.i.
      if (largestUnit == TemporalUnit::Year) {
        return CreateDateDurationRecord(years, months, 0, 0);
      }

      // Step 3.m.ii.
      return CreateDateDurationRecord(0, months + years * 12, 0, 0);
    }

    // Step 3.n.
    if (midSign != sign) {
      // Step 3.n.i.
      months -= sign;

      // Step 3.n.ii.
      mid = ::AddISODate(start, {years, months});
    }

    // Steps 3.o-q.
    int32_t days;
    if (mid.month == end.month) {
      MOZ_ASSERT(mid.year == end.year);

      days = end.day - mid.day;
    } else if (sign < 0) {
      days = -mid.day - (ISODaysInMonth(end.year, end.month) - end.day);
    } else {
      days = end.day + (ISODaysInMonth(mid.year, mid.month) - mid.day);
    }

    // Step 3.r.
    if (largestUnit == TemporalUnit::Month) {
      // Step 3.r.i.
      months += years * 12;

      // Step 3.r.ii.
      years = 0;
    }

    // Step 3.s.
    return CreateDateDurationRecord(years, months, 0, days);
  }

  // Step 4.a.
  MOZ_ASSERT(largestUnit == TemporalUnit::Week ||
             largestUnit == TemporalUnit::Day);

  // Step 4.b.
  int32_t epochDaysStart = MakeDay(start);

  // Step 4.c.
  int32_t epochDaysEnd = MakeDay(end);

  // Step 4.d.
  int32_t days = epochDaysEnd - epochDaysStart;

  // Step 4.e.
  int32_t weeks = 0;

  // Step 4.f.
  if (largestUnit == TemporalUnit::Week) {
    // Step 4.f.i
    weeks = days / 7;

    // Step 4.f.ii.
    days = days % 7;
  }

  // Step 4.g.
  return CreateDateDurationRecord(0, 0, weeks, days);
}

/**
 * DifferenceTemporalPlainDate ( operation, temporalDate, other, options )
 */
static bool DifferenceTemporalPlainDate(JSContext* cx,
                                        TemporalDifference operation,
                                        const CallArgs& args) {
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendarValue(cx, temporalDate->calendar());

  // Step 1. (Not applicable in our implementation)

  // Step 2.
  auto wrappedOther = ::ToTemporalDate(cx, args.get(0));
  if (!wrappedOther) {
    return false;
  }
  auto* unwrappedOther = &wrappedOther.unwrap();
  auto otherDate = ToPlainDate(unwrappedOther);

  Rooted<Wrapped<PlainDateObject*>> other(cx, wrappedOther);
  Rooted<CalendarValue> otherCalendar(cx, unwrappedOther->calendar());
  if (!otherCalendar.wrap(cx)) {
    return false;
  }

  // Step 3.
  if (!CalendarEqualsOrThrow(cx, calendarValue, otherCalendar)) {
    return false;
  }

  // Steps 4-5.
  DifferenceSettings settings;
  Rooted<PlainObject*> resolvedOptions(cx);
  if (args.hasDefined(1)) {
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", ToName(operation), args[1]));
    if (!options) {
      return false;
    }

    // Step 4.
    resolvedOptions = SnapshotOwnProperties(cx, options);
    if (!resolvedOptions) {
      return false;
    }

    // Step 5.
    if (!GetDifferenceSettings(cx, operation, resolvedOptions,
                               TemporalUnitGroup::Date, TemporalUnit::Day,
                               TemporalUnit::Day, &settings)) {
      return false;
    }
  } else {
    // Steps 4-5.
    settings = {
        TemporalUnit::Day,
        TemporalUnit::Day,
        TemporalRoundingMode::Trunc,
        Increment{1},
    };
  }

  // Step 6.
  if (ToPlainDate(temporalDate) == otherDate) {
    auto* obj = CreateTemporalDuration(cx, {});
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Step 7.
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, calendarValue,
                                   {
                                       CalendarMethod::DateAdd,
                                       CalendarMethod::DateUntil,
                                   },
                                   &calendar)) {
    return false;
  }

  // Steps 8-9.
  DateDuration difference;
  if (resolvedOptions) {
    // Steps 8-9.
    if (!DifferenceDate(cx, calendar, temporalDate, other, settings.largestUnit,
                        resolvedOptions, &difference)) {
      return false;
    }
  } else {
    // Steps 8-9.
    if (!DifferenceDate(cx, calendar, temporalDate, other, settings.largestUnit,
                        &difference)) {
      return false;
    }
  }

  // Step 10.
  bool roundingGranularityIsNoop = settings.smallestUnit == TemporalUnit::Day &&
                                   settings.roundingIncrement == Increment{1};

  // Step 11.
  if (!roundingGranularityIsNoop) {
    // Steps 11.a-b.
    NormalizedDuration roundResult;
    if (!temporal::RoundDuration(cx, {difference, {}},
                                 settings.roundingIncrement,
                                 settings.smallestUnit, settings.roundingMode,
                                 temporalDate, calendar, &roundResult)) {
      return false;
    }

    // Step 11.c.
    DateDuration balanceResult;
    if (!temporal::BalanceDateDurationRelative(
            cx, roundResult.date, settings.largestUnit, settings.smallestUnit,
            temporalDate, calendar, &balanceResult)) {
      return false;
    }
    difference = balanceResult;
  }

  // Step 12.
  auto duration = difference.toDuration();
  if (operation == TemporalDifference::Since) {
    duration = duration.negate();
  }
  MOZ_ASSERT(IsValidDuration(duration));

  auto* obj = CreateTemporalDuration(cx, duration);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDate ( isoYear, isoMonth, isoDay [ , calendarLike ] )
 */
static bool PlainDateConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Temporal.PlainDate")) {
    return false;
  }

  // Step 2.
  double isoYear;
  if (!ToIntegerWithTruncation(cx, args.get(0), "year", &isoYear)) {
    return false;
  }

  // Step 3.
  double isoMonth;
  if (!ToIntegerWithTruncation(cx, args.get(1), "month", &isoMonth)) {
    return false;
  }

  // Step 4.
  double isoDay;
  if (!ToIntegerWithTruncation(cx, args.get(2), "day", &isoDay)) {
    return false;
  }

  // Step 5.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendarWithISODefault(cx, args.get(3), &calendar)) {
    return false;
  }

  // Step 6.
  auto* temporalDate =
      CreateTemporalDate(cx, args, isoYear, isoMonth, isoDay, calendar);
  if (!temporalDate) {
    return false;
  }

  args.rval().setObject(*temporalDate);
  return true;
}

/**
 * Temporal.PlainDate.from ( item [ , options ] )
 */
static bool PlainDate_from(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(1)) {
    options = RequireObjectArg(cx, "options", "from", args[1]);
    if (!options) {
      return false;
    }
  }

  // Step 2.
  if (args.get(0).isObject()) {
    JSObject* item = &args[0].toObject();
    if (auto* temporalDate = item->maybeUnwrapIf<PlainDateObject>()) {
      auto date = ToPlainDate(temporalDate);

      Rooted<CalendarValue> calendar(cx, temporalDate->calendar());
      if (!calendar.wrap(cx)) {
        return false;
      }

      if (options) {
        // Step 2.a.
        TemporalOverflow ignored;
        if (!GetTemporalOverflowOption(cx, options, &ignored)) {
          return false;
        }
      }

      // Step 2.b.
      auto* result = CreateTemporalDate(cx, date, calendar);
      if (!result) {
        return false;
      }

      args.rval().setObject(*result);
      return true;
    }
  }

  // Step 3.
  auto result = ToTemporalDate(cx, args.get(0), options);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainDate.compare ( one, two )
 */
static bool PlainDate_compare(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  PlainDate one;
  if (!ToTemporalDate(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  PlainDate two;
  if (!ToTemporalDate(cx, args.get(1), &two)) {
    return false;
  }

  // Step 3.
  args.rval().setInt32(CompareISODate(one, two));
  return true;
}

/**
 * get Temporal.PlainDate.prototype.calendarId
 */
static bool PlainDate_calendarId(JSContext* cx, const CallArgs& args) {
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  auto* calendarId = ToTemporalCalendarIdentifier(cx, calendar);
  if (!calendarId) {
    return false;
  }

  args.rval().setString(calendarId);
  return true;
}

/**
 * get Temporal.PlainDate.prototype.calendarId
 */
static bool PlainDate_calendarId(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_calendarId>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.era
 */
static bool PlainDate_era(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarEra(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.era
 */
static bool PlainDate_era(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_era>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.eraYear
 */
static bool PlainDate_eraYear(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Steps 4-6.
  return CalendarEraYear(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.eraYear
 */
static bool PlainDate_eraYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_eraYear>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.year
 */
static bool PlainDate_year(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarYear(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.year
 */
static bool PlainDate_year(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_year>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.month
 */
static bool PlainDate_month(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarMonth(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.month
 */
static bool PlainDate_month(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_month>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.monthCode
 */
static bool PlainDate_monthCode(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarMonthCode(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.monthCode
 */
static bool PlainDate_monthCode(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_monthCode>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.day
 */
static bool PlainDate_day(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarDay(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.day
 */
static bool PlainDate_day(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_day>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.dayOfWeek
 */
static bool PlainDate_dayOfWeek(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarDayOfWeek(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.dayOfWeek
 */
static bool PlainDate_dayOfWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_dayOfWeek>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.dayOfYear
 */
static bool PlainDate_dayOfYear(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarDayOfYear(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.dayOfYear
 */
static bool PlainDate_dayOfYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_dayOfYear>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.weekOfYear
 */
static bool PlainDate_weekOfYear(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Steps 4-6.
  return CalendarWeekOfYear(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.weekOfYear
 */
static bool PlainDate_weekOfYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_weekOfYear>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.yearOfWeek
 */
static bool PlainDate_yearOfWeek(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Steps 4-6.
  return CalendarYearOfWeek(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.yearOfWeek
 */
static bool PlainDate_yearOfWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_yearOfWeek>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.daysInWeek
 */
static bool PlainDate_daysInWeek(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarDaysInWeek(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.daysInWeek
 */
static bool PlainDate_daysInWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_daysInWeek>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.daysInMonth
 */
static bool PlainDate_daysInMonth(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarDaysInMonth(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.daysInMonth
 */
static bool PlainDate_daysInMonth(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_daysInMonth>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.daysInYear
 */
static bool PlainDate_daysInYear(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarDaysInYear(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.daysInYear
 */
static bool PlainDate_daysInYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_daysInYear>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.monthsInYear
 */
static bool PlainDate_monthsInYear(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarMonthsInYear(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.monthsInYear
 */
static bool PlainDate_monthsInYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_monthsInYear>(cx, args);
}

/**
 * get Temporal.PlainDate.prototype.inLeapYear
 */
static bool PlainDate_inLeapYear(JSContext* cx, const CallArgs& args) {
  // Step 3.
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 4.
  return CalendarInLeapYear(cx, calendar, temporalDate, args.rval());
}

/**
 * get Temporal.PlainDate.prototype.inLeapYear
 */
static bool PlainDate_inLeapYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_inLeapYear>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.toPlainYearMonth ( )
 */
static bool PlainDate_toPlainYearMonth(JSContext* cx, const CallArgs& args) {
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendarValue(cx, temporalDate->calendar());

  // Step 3.
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, calendarValue,
                                   {
                                       CalendarMethod::Fields,
                                       CalendarMethod::YearMonthFromFields,
                                   },
                                   &calendar)) {
    return false;
  }

  // Step 4.
  Rooted<PlainObject*> fields(
      cx,
      PrepareCalendarFields(cx, calendar, temporalDate,
                            {CalendarField::MonthCode, CalendarField::Year}));
  if (!fields) {
    return false;
  }

  // Steps 5-6.
  auto obj = CalendarYearMonthFromFields(cx, calendar, fields);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDate.prototype.toPlainYearMonth ( )
 */
static bool PlainDate_toPlainYearMonth(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_toPlainYearMonth>(cx,
                                                                       args);
}

/**
 * Temporal.PlainDate.prototype.toPlainMonthDay ( )
 */
static bool PlainDate_toPlainMonthDay(JSContext* cx, const CallArgs& args) {
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendarValue(cx, temporalDate->calendar());

  // Example for the optimisation described in TemporalFields.cpp

  // Optimization for built-in objects.
  do {
    // Step 4.
    static constexpr std::initializer_list<CalendarField> fieldNames = {
        CalendarField::Day, CalendarField::MonthCode};

    // Step 5.
    if (calendarValue.isObject()) {
      Rooted<JSObject*> calendarObj(cx, calendarValue.toObject());
      if (!calendarObj->is<CalendarObject>()) {
        break;
      }
      auto builtinCalendar = calendarObj.as<CalendarObject>();

      // Step 5.
      if (!IsBuiltinAccess(cx, builtinCalendar, fieldNames)) {
        break;
      }
    }
    if (!IsBuiltinAccess(cx, temporalDate, fieldNames)) {
      break;
    }

    // Step 6.
    auto date = ToPlainDate(temporalDate);
    auto result = PlainDate{1972 /* referenceISOYear */, date.month, date.day};

    auto* obj = CreateTemporalMonthDay(cx, result, calendarValue);
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  } while (false);

  // Step 3.
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, calendarValue,
                                   {
                                       CalendarMethod::Fields,
                                       CalendarMethod::MonthDayFromFields,
                                   },
                                   &calendar)) {
    return false;
  }

  // Step 4.
  Rooted<PlainObject*> fields(
      cx,
      PrepareCalendarFields(cx, calendar, temporalDate,
                            {CalendarField::Day, CalendarField::MonthCode}));
  if (!fields) {
    return false;
  }

  // Steps 5-6.
  auto obj = CalendarMonthDayFromFields(cx, calendar, fields);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDate.prototype.toPlainMonthDay ( )
 */
static bool PlainDate_toPlainMonthDay(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_toPlainMonthDay>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.toPlainDateTime ( [ temporalTime ] )
 */
static bool PlainDate_toPlainDateTime(JSContext* cx, const CallArgs& args) {
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Default initialize the time component to all zero.
  PlainDateTime dateTime = {ToPlainDate(temporalDate), {}};

  // Step 3. (Inlined ToTemporalTimeOrMidnight)
  if (args.hasDefined(0)) {
    if (!ToTemporalTime(cx, args[0], &dateTime.time)) {
      return false;
    }
  }

  // Step 4.
  auto* obj = CreateTemporalDateTime(cx, dateTime, calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDate.prototype.toPlainDateTime ( [ temporalTime ] )
 */
static bool PlainDate_toPlainDateTime(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_toPlainDateTime>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.getISOFields ( )
 */
static bool PlainDate_getISOFields(JSContext* cx, const CallArgs& args) {
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  auto date = ToPlainDate(temporalDate);
  auto calendar = temporalDate->calendar();

  // Step 3.
  Rooted<IdValueVector> fields(cx, IdValueVector(cx));

  // Step 4.
  Rooted<Value> cal(cx);
  if (!ToTemporalCalendar(cx, calendar, &cal)) {
    return false;
  }
  if (!fields.emplaceBack(NameToId(cx->names().calendar), cal)) {
    return false;
  }

  // Step 5.
  if (!fields.emplaceBack(NameToId(cx->names().isoDay), Int32Value(date.day))) {
    return false;
  }

  // Step 6.
  if (!fields.emplaceBack(NameToId(cx->names().isoMonth),
                          Int32Value(date.month))) {
    return false;
  }

  // Step 7.
  if (!fields.emplaceBack(NameToId(cx->names().isoYear),
                          Int32Value(date.year))) {
    return false;
  }

  // Step 8.
  auto* obj = NewPlainObjectWithUniqueNames(cx, fields);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDate.prototype.getISOFields ( )
 */
static bool PlainDate_getISOFields(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_getISOFields>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.getCalendar ( )
 */
static bool PlainDate_getCalendar(JSContext* cx, const CallArgs& args) {
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  auto* obj = ToTemporalCalendarObject(cx, calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDate.prototype.getCalendar ( )
 */
static bool PlainDate_getCalendar(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_getCalendar>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.add ( temporalDurationLike [ , options ] )
 */
static bool PlainDate_add(JSContext* cx, const CallArgs& args) {
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendarValue(cx, temporalDate->calendar());

  // Step 3.
  Rooted<Wrapped<DurationObject*>> duration(
      cx, ToTemporalDuration(cx, args.get(0)));
  if (!duration) {
    return false;
  }

  // Step 4.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(1)) {
    options = RequireObjectArg(cx, "options", "add", args[1]);
  } else {
    options = NewPlainObjectWithProto(cx, nullptr);
  }
  if (!options) {
    return false;
  }

  // Step 5.
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, calendarValue,
                                   {
                                       CalendarMethod::DateAdd,
                                   },
                                   &calendar)) {
    return false;
  }

  // Step 6.
  auto result = AddDate(cx, calendar, temporalDate, duration, options);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainDate.prototype.add ( temporalDurationLike [ , options ] )
 */
static bool PlainDate_add(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_add>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.subtract ( temporalDurationLike [ , options ] )
 */
static bool PlainDate_subtract(JSContext* cx, const CallArgs& args) {
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());
  Rooted<CalendarValue> calendarValue(cx, temporalDate->calendar());

  // Step 3.
  Duration duration;
  if (!ToTemporalDuration(cx, args.get(0), &duration)) {
    return false;
  }

  // Step 4.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(1)) {
    options = RequireObjectArg(cx, "options", "subtract", args[1]);
  } else {
    options = NewPlainObjectWithProto(cx, nullptr);
  }
  if (!options) {
    return false;
  }

  // Step 5.
  auto negatedDuration = duration.negate();

  // Step 6.
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, calendarValue,
                                   {
                                       CalendarMethod::DateAdd,
                                   },
                                   &calendar)) {
    return false;
  }

  // Step 7.
  auto result = ::AddDate(cx, calendar, temporalDate, negatedDuration, options);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainDate.prototype.subtract ( temporalDurationLike [ , options ] )
 */
static bool PlainDate_subtract(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_subtract>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.with ( temporalDateLike [ , options ] )
 */
static bool PlainDate_with(JSContext* cx, const CallArgs& args) {
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());

  // Step 3.
  Rooted<JSObject*> temporalDateLike(
      cx, RequireObjectArg(cx, "temporalDateLike", "with", args.get(0)));
  if (!temporalDateLike) {
    return false;
  }
  if (!ThrowIfTemporalLikeObject(cx, temporalDateLike)) {
    return false;
  }

  // Step 4.
  Rooted<PlainObject*> resolvedOptions(cx);
  if (args.hasDefined(1)) {
    Rooted<JSObject*> options(cx,
                              RequireObjectArg(cx, "options", "with", args[1]));
    if (!options) {
      return false;
    }
    resolvedOptions = SnapshotOwnProperties(cx, options);
  } else {
    resolvedOptions = NewPlainObjectWithProto(cx, nullptr);
  }
  if (!resolvedOptions) {
    return false;
  }

  // Step 5.
  Rooted<CalendarValue> calendarValue(cx, temporalDate->calendar());
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, calendarValue,
                                   {
                                       CalendarMethod::DateFromFields,
                                       CalendarMethod::Fields,
                                       CalendarMethod::MergeFields,
                                   },
                                   &calendar)) {
    return false;
  }

  // Step 6.
  Rooted<PlainObject*> fields(cx);
  JS::RootedVector<PropertyKey> fieldNames(cx);
  if (!PrepareCalendarFieldsAndFieldNames(cx, calendar, temporalDate,
                                          {
                                              CalendarField::Day,
                                              CalendarField::Month,
                                              CalendarField::MonthCode,
                                              CalendarField::Year,
                                          },
                                          &fields, &fieldNames)) {
    return false;
  }

  // Step 7.
  Rooted<PlainObject*> partialDate(
      cx, PreparePartialTemporalFields(cx, temporalDateLike, fieldNames));
  if (!partialDate) {
    return false;
  }

  // Step 8.
  Rooted<JSObject*> mergedFields(
      cx, CalendarMergeFields(cx, calendar, fields, partialDate));
  if (!mergedFields) {
    return false;
  }

  // Step 9.
  fields = PrepareTemporalFields(cx, mergedFields, fieldNames);
  if (!fields) {
    return false;
  }

  // Step 10.
  auto result =
      temporal::CalendarDateFromFields(cx, calendar, fields, resolvedOptions);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainDate.prototype.with ( temporalDateLike [ , options ] )
 */
static bool PlainDate_with(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_with>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.withCalendar ( calendar )
 */
static bool PlainDate_withCalendar(JSContext* cx, const CallArgs& args) {
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  auto date = ToPlainDate(temporalDate);

  // Step 3.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendar(cx, args.get(0), &calendar)) {
    return false;
  }

  // Step 4.
  auto* result = CreateTemporalDate(cx, date, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainDate.prototype.withCalendar ( calendar )
 */
static bool PlainDate_withCalendar(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_withCalendar>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.until ( other [ , options ] )
 */
static bool PlainDate_until(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalPlainDate(cx, TemporalDifference::Until, args);
}

/**
 * Temporal.PlainDate.prototype.until ( other [ , options ] )
 */
static bool PlainDate_until(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_until>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.since ( other [ , options ] )
 */
static bool PlainDate_since(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalPlainDate(cx, TemporalDifference::Since, args);
}

/**
 * Temporal.PlainDate.prototype.since ( other [ , options ] )
 */
static bool PlainDate_since(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_since>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.equals ( other )
 */
static bool PlainDate_equals(JSContext* cx, const CallArgs& args) {
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  auto date = ToPlainDate(temporalDate);
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  Rooted<PlainDateWithCalendar> other(cx);
  if (!ToTemporalDate(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 4-7.
  bool equals = date == other.date();
  if (equals && !CalendarEquals(cx, calendar, other.calendar(), &equals)) {
    return false;
  }

  args.rval().setBoolean(equals);
  return true;
}

/**
 * Temporal.PlainDate.prototype.equals ( other )
 */
static bool PlainDate_equals(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_equals>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.toZonedDateTime ( item )
 *
 * The |item| argument represents either a time zone or an options object. The
 * following cases are supported:
 * - |item| is a `Temporal.TimeZone` object.
 * - |item| is a user-defined time zone object.
 * - |item| is an options object with `timeZone` and `plainTime` properties.
 * - |item| is a time zone identifier string.
 *
 * User-defined time zone objects are distinguished from options objects by the
 * `timeZone` property, i.e. if a `timeZone` property is present, the object is
 * treated as an options object, otherwise an object is treated as a
 * user-defined time zone.
 */
static bool PlainDate_toZonedDateTime(JSContext* cx, const CallArgs& args) {
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  auto date = ToPlainDate(temporalDate);
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Steps 3-4
  Rooted<TimeZoneValue> timeZone(cx);
  PlainTime time = {};
  if (args.get(0).isObject()) {
    Rooted<JSObject*> item(cx, &args[0].toObject());

    // Steps 3.a-b.
    if (item->canUnwrapAs<TimeZoneObject>()) {
      // Step 3.a.i.
      timeZone.set(TimeZoneValue(item));

      // Step 3.a.ii. (Not applicable in our implementation.)
    } else {
      // Step 3.b.i.
      Rooted<Value> timeZoneLike(cx);
      if (!GetProperty(cx, item, item, cx->names().timeZone, &timeZoneLike)) {
        return false;
      }

      // Steps 3.b.ii-iii.
      if (timeZoneLike.isUndefined()) {
        // Step 3.b.ii.1.
        if (!ToTemporalTimeZone(cx, args[0], &timeZone)) {
          return false;
        }

        // Step 3.b.ii.2.  (Not applicable in our implementation.)
      } else {
        // Step 3.b.iii.1.
        if (!ToTemporalTimeZone(cx, timeZoneLike, &timeZone)) {
          return false;
        }

        // Step 3.b.iii.2.
        Rooted<Value> temporalTime(cx);
        if (!GetProperty(cx, item, item, cx->names().plainTime,
                         &temporalTime)) {
          return false;
        }

        // Step 5. (Inlined ToTemporalTimeOrMidnight)
        if (!temporalTime.isUndefined()) {
          if (!ToTemporalTime(cx, temporalTime, &time)) {
            return false;
          }
        }
      }
    }
  } else {
    // Step 4.a.
    if (!ToTemporalTimeZone(cx, args.get(0), &timeZone)) {
      return false;
    }

    // Step 4.b. (Not applicable in our implementation.)
  }

  // Step 5. (Moved next to step 3.b.iii.2.)

  // Step 6.
  Rooted<PlainDateTimeWithCalendar> temporalDateTime(cx);
  if (!CreateTemporalDateTime(cx, {date, time}, calendar, &temporalDateTime)) {
    return false;
  }

  // Steps 7-8.
  Instant instant;
  if (!GetInstantFor(cx, timeZone, temporalDateTime,
                     TemporalDisambiguation::Compatible, &instant)) {
    return false;
  }

  // Step 9.
  auto* result = CreateTemporalZonedDateTime(cx, instant, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainDate.prototype.toZonedDateTime ( item )
 */
static bool PlainDate_toZonedDateTime(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_toZonedDateTime>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.toString ( [ options ] )
 */
static bool PlainDate_toString(JSContext* cx, const CallArgs& args) {
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());

  auto showCalendar = ShowCalendar::Auto;
  if (args.hasDefined(0)) {
    // Step 3.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", "toString", args[0]));
    if (!options) {
      return false;
    }

    // Step 4.
    if (!GetTemporalShowCalendarNameOption(cx, options, &showCalendar)) {
      return false;
    }
  }

  // Step 5.
  JSString* str = TemporalDateToString(cx, temporalDate, showCalendar);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.PlainDate.prototype.toString ( [ options ] )
 */
static bool PlainDate_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_toString>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool PlainDate_toLocaleString(JSContext* cx, const CallArgs& args) {
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());

  // Step 3.
  JSString* str = TemporalDateToString(cx, temporalDate, ShowCalendar::Auto);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.PlainDate.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool PlainDate_toLocaleString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_toLocaleString>(cx, args);
}

/**
 * Temporal.PlainDate.prototype.toJSON ( )
 */
static bool PlainDate_toJSON(JSContext* cx, const CallArgs& args) {
  Rooted<PlainDateObject*> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());

  // Step 3.
  JSString* str = TemporalDateToString(cx, temporalDate, ShowCalendar::Auto);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.PlainDate.prototype.toJSON ( )
 */
static bool PlainDate_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDate, PlainDate_toJSON>(cx, args);
}

/**
 *  Temporal.PlainDate.prototype.valueOf ( )
 */
static bool PlainDate_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                            "PlainDate", "primitive type");
  return false;
}

const JSClass PlainDateObject::class_ = {
    "Temporal.PlainDate",
    JSCLASS_HAS_RESERVED_SLOTS(PlainDateObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_PlainDate),
    JS_NULL_CLASS_OPS,
    &PlainDateObject::classSpec_,
};

const JSClass& PlainDateObject::protoClass_ = PlainObject::class_;

static const JSFunctionSpec PlainDate_methods[] = {
    JS_FN("from", PlainDate_from, 1, 0),
    JS_FN("compare", PlainDate_compare, 2, 0),
    JS_FS_END,
};

static const JSFunctionSpec PlainDate_prototype_methods[] = {
    JS_FN("toPlainMonthDay", PlainDate_toPlainMonthDay, 0, 0),
    JS_FN("toPlainYearMonth", PlainDate_toPlainYearMonth, 0, 0),
    JS_FN("toPlainDateTime", PlainDate_toPlainDateTime, 0, 0),
    JS_FN("getISOFields", PlainDate_getISOFields, 0, 0),
    JS_FN("getCalendar", PlainDate_getCalendar, 0, 0),
    JS_FN("add", PlainDate_add, 1, 0),
    JS_FN("subtract", PlainDate_subtract, 1, 0),
    JS_FN("with", PlainDate_with, 1, 0),
    JS_FN("withCalendar", PlainDate_withCalendar, 1, 0),
    JS_FN("until", PlainDate_until, 1, 0),
    JS_FN("since", PlainDate_since, 1, 0),
    JS_FN("equals", PlainDate_equals, 1, 0),
    JS_FN("toZonedDateTime", PlainDate_toZonedDateTime, 1, 0),
    JS_FN("toString", PlainDate_toString, 0, 0),
    JS_FN("toLocaleString", PlainDate_toLocaleString, 0, 0),
    JS_FN("toJSON", PlainDate_toJSON, 0, 0),
    JS_FN("valueOf", PlainDate_valueOf, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec PlainDate_prototype_properties[] = {
    JS_PSG("calendarId", PlainDate_calendarId, 0),
    JS_PSG("era", PlainDate_era, 0),
    JS_PSG("eraYear", PlainDate_eraYear, 0),
    JS_PSG("year", PlainDate_year, 0),
    JS_PSG("month", PlainDate_month, 0),
    JS_PSG("monthCode", PlainDate_monthCode, 0),
    JS_PSG("day", PlainDate_day, 0),
    JS_PSG("dayOfWeek", PlainDate_dayOfWeek, 0),
    JS_PSG("dayOfYear", PlainDate_dayOfYear, 0),
    JS_PSG("weekOfYear", PlainDate_weekOfYear, 0),
    JS_PSG("yearOfWeek", PlainDate_yearOfWeek, 0),
    JS_PSG("daysInWeek", PlainDate_daysInWeek, 0),
    JS_PSG("daysInMonth", PlainDate_daysInMonth, 0),
    JS_PSG("daysInYear", PlainDate_daysInYear, 0),
    JS_PSG("monthsInYear", PlainDate_monthsInYear, 0),
    JS_PSG("inLeapYear", PlainDate_inLeapYear, 0),
    JS_STRING_SYM_PS(toStringTag, "Temporal.PlainDate", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec PlainDateObject::classSpec_ = {
    GenericCreateConstructor<PlainDateConstructor, 3, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<PlainDateObject>,
    PlainDate_methods,
    nullptr,
    PlainDate_prototype_methods,
    PlainDate_prototype_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

struct PlainDateNameAndNative final {
  PropertyName* name;
  JSNative native;
};

static PlainDateNameAndNative GetPlainDateNameAndNative(
    JSContext* cx, CalendarField fieldName) {
  switch (fieldName) {
    case CalendarField::Year:
      return {cx->names().year, PlainDate_year};
    case CalendarField::Month:
      return {cx->names().month, PlainDate_month};
    case CalendarField::MonthCode:
      return {cx->names().monthCode, PlainDate_monthCode};
    case CalendarField::Day:
      return {cx->names().day, PlainDate_day};
  }
  MOZ_CRASH("invalid temporal field name");
}

bool js::temporal::IsBuiltinAccess(
    JSContext* cx, Handle<PlainDateObject*> date,
    std::initializer_list<CalendarField> fieldNames) {
  // Don't optimize when the object has any own properties which may shadow the
  // built-in methods.
  if (date->shape()->propMapLength() > 0) {
    return false;
  }

  JSObject* proto = cx->global()->maybeGetPrototype(JSProto_PlainDate);

  // Don't attempt to optimize when the class isn't yet initialized.
  if (!proto) {
    return false;
  }

  // Don't optimize when the prototype isn't the built-in prototype.
  if (date->staticPrototype() != proto) {
    return false;
  }

  auto* nproto = &proto->as<NativeObject>();
  for (auto fieldName : fieldNames) {
    auto [name, native] = GetPlainDateNameAndNative(cx, fieldName);
    auto prop = nproto->lookupPure(name);

    // Return if the property isn't a data property.
    if (!prop || !prop->isDataProperty()) {
      return false;
    }

    // Return if the property isn't the initial method.
    if (!IsNativeFunction(nproto->getSlot(prop->slot()), native)) {
      return false;
    }
  }

  // Success! The access can be optimized.
  return true;
}
