/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/PlainDate.h"

#include "mozilla/Assertions.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/EnumSet.h"

#include <cmath>
#include <cstdlib>
#include <stdint.h>
#include <utility>

#include "jsdate.h"
#include "jsnum.h"
#include "jspubtd.h"
#include "jstypes.h"
#include "NamespaceImports.h"

#include "builtin/intl/DateTimeFormat.h"
#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/CalendarFields.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainMonthDay.h"
#include "builtin/temporal/PlainTime.h"
#include "builtin/temporal/PlainYearMonth.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/TimeZone.h"
#include "builtin/temporal/ToString.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "gc/GCEnum.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
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
bool js::temporal::IsValidISODate(const ISODate& date) {
  const auto& [year, month, day] = date;

  // Step 1.
  if (month < 1 || month > 12) {
    return false;
  }

  // Step 2.
  int32_t daysInMonth = js::temporal::ISODaysInMonth(year, month);

  // Steps 3-4.
  return 1 <= day && day <= daysInMonth;
}
#endif

/**
 * ISODateWithinLimits ( isoDate )
 */
bool js::temporal::ISODateWithinLimits(const ISODate& isoDate) {
  MOZ_ASSERT(IsValidISODate(isoDate));

  constexpr auto min = ISODate::min();
  constexpr auto max = ISODate::max();

  const auto& year = isoDate.year;

  // Fast-path when the input is definitely in range.
  if (min.year < year && year < max.year) {
    return true;
  }

  // Check |isoDate| is within the valid limits.
  if (year < 0) {
    return isoDate >= min;
  }
  return isoDate <= max;
}

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

  if constexpr (std::is_same_v<T, double>) {
    if (!ThrowIfInvalidDateValue(cx, "year", INT32_MIN, INT32_MAX, year)) {
      return false;
    }
  }

  // Step 2.
  if (!ThrowIfInvalidDateValue(cx, "month", 1, 12, month)) {
    return false;
  }

  // Step 3.
  int32_t daysInMonth =
      js::temporal::ISODaysInMonth(int32_t(year), int32_t(month));

  // Steps 4-5.
  return ThrowIfInvalidDateValue(cx, "day", 1, daysInMonth, day);
}

/**
 * IsValidISODate ( year, month, day )
 */
bool js::temporal::ThrowIfInvalidISODate(JSContext* cx, const ISODate& date) {
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
 * CreateTemporalDate ( isoDate, calendar [ , newTarget ] )
 */
static PlainDateObject* CreateTemporalDate(JSContext* cx, const CallArgs& args,
                                           const ISODate& isoDate,
                                           Handle<CalendarValue> calendar) {
  MOZ_ASSERT(IsValidISODate(isoDate));

  // Step 1.
  if (!ISODateWithinLimits(isoDate)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return nullptr;
  }

  // Steps 2-3.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_PlainDate,
                                          &proto)) {
    return nullptr;
  }

  auto* object = NewObjectWithClassProto<PlainDateObject>(cx, proto);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  auto packedDate = PackedDate::pack(isoDate);
  object->setFixedSlot(PlainDateObject::PACKED_DATE_SLOT,
                       PrivateUint32Value(packedDate.value));

  // Step 5.
  object->setFixedSlot(PlainDateObject::CALENDAR_SLOT, calendar.toSlotValue());

  // Step 6.
  return object;
}

/**
 * CreateTemporalDate ( isoDate, calendar [ , newTarget ] )
 */
PlainDateObject* js::temporal::CreateTemporalDate(
    JSContext* cx, const ISODate& isoDate, Handle<CalendarValue> calendar) {
  MOZ_ASSERT(IsValidISODate(isoDate));

  // Step 1.
  if (!ISODateWithinLimits(isoDate)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return nullptr;
  }

  // Steps 2-3.
  auto* object = NewBuiltinClassInstance<PlainDateObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  auto packedDate = PackedDate::pack(isoDate);
  object->setFixedSlot(PlainDateObject::PACKED_DATE_SLOT,
                       PrivateUint32Value(packedDate.value));

  // Step 5.
  object->setFixedSlot(PlainDateObject::CALENDAR_SLOT, calendar.toSlotValue());

  // Step 6.
  return object;
}

/**
 * CreateTemporalDate ( isoDate, calendar [ , newTarget ] )
 */
PlainDateObject* js::temporal::CreateTemporalDate(JSContext* cx,
                                                  Handle<PlainDate> date) {
  MOZ_ASSERT(ISODateWithinLimits(date));
  return CreateTemporalDate(cx, date, date.calendar());
}

/**
 * CreateTemporalDate ( isoDate, calendar [ , newTarget ] )
 */
bool js::temporal::CreateTemporalDate(JSContext* cx, const ISODate& isoDate,
                                      Handle<CalendarValue> calendar,
                                      MutableHandle<PlainDate> result) {
  MOZ_ASSERT(IsValidISODate(isoDate));

  // Step 1.
  if (!ISODateWithinLimits(isoDate)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  // Steps 2-6.
  result.set(PlainDate{isoDate, calendar});
  return true;
}

struct DateOptions {
  TemporalOverflow overflow = TemporalOverflow::Constrain;
};

/**
 * ToTemporalDate ( item [ , options ] )
 */
static bool ToTemporalDateOptions(JSContext* cx, Handle<Value> options,
                                  DateOptions* result) {
  if (options.isUndefined()) {
    *result = {};
    return true;
  }

  // NOTE: |options| are only passed from `Temporal.PlainDate.from`.

  Rooted<JSObject*> resolvedOptions(
      cx, RequireObjectArg(cx, "options", "from", options));
  if (!resolvedOptions) {
    return false;
  }

  auto overflow = TemporalOverflow::Constrain;
  if (!GetTemporalOverflowOption(cx, resolvedOptions, &overflow)) {
    return false;
  }

  *result = {overflow};
  return true;
}

/**
 * ToTemporalDate ( item [ , options ] )
 */
static bool ToTemporalDate(JSContext* cx, Handle<JSObject*> item,
                           Handle<Value> options,
                           MutableHandle<PlainDate> result) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.a.
  if (auto* plainDate = item->maybeUnwrapIf<PlainDateObject>()) {
    auto date = plainDate->date();
    Rooted<CalendarValue> calendar(cx, plainDate->calendar());
    if (!calendar.wrap(cx)) {
      return false;
    }

    // Steps 2.a.i-ii.
    DateOptions ignoredOptions;
    if (!ToTemporalDateOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Step 2.a.iii.
    result.set(PlainDate{date, calendar});
    return true;
  }

  // Step 2.b.
  if (auto* zonedDateTime = item->maybeUnwrapIf<ZonedDateTimeObject>()) {
    auto epochNs = zonedDateTime->epochNanoseconds();
    Rooted<TimeZoneValue> timeZone(cx, zonedDateTime->timeZone());
    Rooted<CalendarValue> calendar(cx, zonedDateTime->calendar());

    if (!timeZone.wrap(cx)) {
      return false;
    }
    if (!calendar.wrap(cx)) {
      return false;
    }

    // Steps 2.b.ii.
    ISODateTime isoDateTime;
    if (!GetISODateTimeFor(cx, timeZone, epochNs, &isoDateTime)) {
      return false;
    }

    // Steps 2.b.ii-iii.
    DateOptions ignoredOptions;
    if (!ToTemporalDateOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Step 2.b.iv.
    result.set(PlainDate{isoDateTime.date, calendar});
    return true;
  }

  // Step 2.c.
  if (auto* dateTime = item->maybeUnwrapIf<PlainDateTimeObject>()) {
    auto date = dateTime->date();
    Rooted<CalendarValue> calendar(cx, dateTime->calendar());
    if (!calendar.wrap(cx)) {
      return false;
    }

    // Steps 2.c.i-ii.
    DateOptions ignoredOptions;
    if (!ToTemporalDateOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Step 2.c.iii.
    result.set(PlainDate{date, calendar});
    return true;
  }

  // Step 2.d.
  Rooted<CalendarValue> calendar(cx);
  if (!GetTemporalCalendarWithISODefault(cx, item, &calendar)) {
    return false;
  }

  // Step 2.e.
  Rooted<CalendarFields> fields(cx);
  if (!PrepareCalendarFields(cx, calendar, item,
                             {
                                 CalendarField::Year,
                                 CalendarField::Month,
                                 CalendarField::MonthCode,
                                 CalendarField::Day,
                             },
                             &fields)) {
    return false;
  }

  // Steps 2.f-g.
  DateOptions resolvedOptions;
  if (!ToTemporalDateOptions(cx, options, &resolvedOptions)) {
    return false;
  }
  auto [overflow] = resolvedOptions;

  // Steps 2.h-i.
  return CalendarDateFromFields(cx, calendar, fields, overflow, result);
}

/**
 * ToTemporalDate ( item [ , options ] )
 */
static bool ToTemporalDate(JSContext* cx, Handle<Value> item,
                           Handle<Value> options,
                           MutableHandle<PlainDate> result) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  if (item.isObject()) {
    Rooted<JSObject*> itemObj(cx, &item.toObject());
    return ToTemporalDate(cx, itemObj, options, result);
  }

  // Step 3.
  if (!item.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, item,
                     nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> string(cx, item.toString());

  // Step 4.
  ISODateTime dateTime;
  Rooted<JSString*> calendarString(cx);
  if (!ParseTemporalDateTimeString(cx, string, &dateTime, &calendarString)) {
    return false;
  }
  MOZ_ASSERT(IsValidISODate(dateTime.date));

  // Steps 5-7.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  if (calendarString) {
    if (!CanonicalizeCalendar(cx, calendarString, &calendar)) {
      return false;
    }
  }

  // Steps 8-9.
  DateOptions ignoredOptions;
  if (!ToTemporalDateOptions(cx, options, &ignoredOptions)) {
    return false;
  }

  // Steps 10-11.
  return CreateTemporalDate(cx, dateTime.date, calendar, result);
}

/**
 * ToTemporalDate ( item [ , options ] )
 */
static bool ToTemporalDate(JSContext* cx, Handle<Value> item,
                           MutableHandle<PlainDate> result) {
  return ToTemporalDate(cx, item, UndefinedHandleValue, result);
}

static bool IsValidISODateEpochMilliseconds(int64_t epochMilliseconds) {
  // Epoch nanoseconds limits, adjusted to the range supported by ISODate.
  constexpr auto oneDay = EpochDuration::fromDays(1);
  constexpr auto min = EpochNanoseconds::min() - oneDay;
  constexpr auto max = EpochNanoseconds::max() + oneDay;

  // NB: Minimum limit is inclusive, whereas maximim limit is exclusive.
  auto epochNs = EpochNanoseconds::fromMilliseconds(epochMilliseconds);
  return min <= epochNs && epochNs < max;
}

/**
 * BalanceISODate ( year, month, day )
 */
bool js::temporal::BalanceISODate(JSContext* cx, const ISODate& date,
                                  int64_t days, ISODate* result) {
  MOZ_ASSERT(IsValidISODate(date));
  MOZ_ASSERT(ISODateWithinLimits(date));

  // Step 1.
  auto epochDays = MakeDay(date) + mozilla::CheckedInt64{days};

  // Step 2.
  auto epochMilliseconds = epochDays * ToMilliseconds(TemporalUnit::Day);
  if (!epochMilliseconds.isValid() ||
      !IsValidISODateEpochMilliseconds(epochMilliseconds.value())) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  // Step 3.
  auto [year, month, day] = ToYearMonthDay(epochMilliseconds.value());

  *result = ISODate{year, month + 1, day};
  MOZ_ASSERT(IsValidISODate(*result));
  MOZ_ASSERT(ISODateWithinLimits(*result));

  return true;
}

/**
 * BalanceISODate ( year, month, day )
 */
ISODate js::temporal::BalanceISODate(const ISODate& date, int32_t days) {
  MOZ_ASSERT(IsValidISODate(date));
  MOZ_ASSERT(ISODateWithinLimits(date));
  MOZ_ASSERT(std::abs(days) <= 400'000'000, "days limit for ToYearMonthDay");

  // Step 1.
  int32_t epochDays = MakeDay(date) + days;

  // Step 2.
  int64_t epochMilliseconds = epochDays * ToMilliseconds(TemporalUnit::Day);

  // Step 3.
  auto [year, month, day] = ToYearMonthDay(epochMilliseconds);

  // NB: The returned date is possibly outside the valid limits!
  auto result = ISODate{year, month + 1, day};
  MOZ_ASSERT(IsValidISODate(result));

  return result;
}

/**
 * CompareISODate ( y1, m1, d1, y2, m2, d2 )
 */
int32_t js::temporal::CompareISODate(const ISODate& one, const ISODate& two) {
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
 * DifferenceTemporalPlainDate ( operation, temporalDate, other, options )
 */
static bool DifferenceTemporalPlainDate(JSContext* cx,
                                        TemporalDifference operation,
                                        const CallArgs& args) {
  Rooted<PlainDate> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());

  // Step 1.
  Rooted<PlainDate> other(cx);
  if (!ToTemporalDate(cx, args.get(0), &other)) {
    return false;
  }

  // Step 2.
  if (!CalendarEquals(temporalDate.calendar(), other.calendar())) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE,
        CalendarIdentifier(temporalDate.calendar()).data(),
        CalendarIdentifier(other.calendar()).data());
    return false;
  }

  // Steps 3-4.
  DifferenceSettings settings;
  if (args.hasDefined(1)) {
    // Step 3.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", ToName(operation), args[1]));
    if (!options) {
      return false;
    }

    // Step 4.
    if (!GetDifferenceSettings(cx, operation, options, TemporalUnitGroup::Date,
                               TemporalUnit::Day, TemporalUnit::Day,
                               &settings)) {
      return false;
    }
  } else {
    // Steps 3-4.
    settings = {
        TemporalUnit::Day,
        TemporalUnit::Day,
        TemporalRoundingMode::Trunc,
        Increment{1},
    };
  }

  // Step 5.
  if (temporalDate.date() == other.date()) {
    auto* obj = CreateTemporalDuration(cx, {});
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Step 6.
  DateDuration dateDifference;
  if (!CalendarDateUntil(cx, temporalDate.calendar(), temporalDate.date(),
                         other.date(), settings.largestUnit, &dateDifference)) {
    return false;
  }

  // Step 7.
  auto duration = InternalDuration{dateDifference, {}};

  // Step 8.
  if (settings.smallestUnit != TemporalUnit::Day ||
      settings.roundingIncrement != Increment{1}) {
    // Step 8.a.
    auto isoDateTime = ISODateTime{temporalDate.date(), {}};

    // Step 8.b.
    auto isoDateTimeOther = ISODateTime{other.date(), {}};

    // Step 8.c.
    auto destEpochNs = GetUTCEpochNanoseconds(isoDateTimeOther);

    // Step 8.d.
    Rooted<TimeZoneValue> timeZone(cx, TimeZoneValue{});
    if (!RoundRelativeDuration(cx, duration, destEpochNs, isoDateTime, timeZone,
                               temporalDate.calendar(), settings.largestUnit,
                               settings.roundingIncrement,
                               settings.smallestUnit, settings.roundingMode,
                               &duration)) {
      return false;
    }
  }
  MOZ_ASSERT(IsValidDuration(duration));
  MOZ_ASSERT(duration.time == TimeDuration{});

  // Step 9. (Inlined TemporalDurationFromInternal)
  auto result = duration.date.toDuration();

  // Step 10.
  if (operation == TemporalDifference::Since) {
    result = result.negate();
  }
  MOZ_ASSERT(IsValidDuration(result));

  // Step 11.
  auto* obj = CreateTemporalDuration(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * AddDurationToDate ( operation, temporalDate, temporalDurationLike, options )
 */
static bool AddDurationToDate(JSContext* cx, TemporalAddDuration operation,
                              const CallArgs& args) {
  Rooted<PlainDate> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());

  // Step 1.
  auto calendar = temporalDate.calendar();

  // Step 2.
  Duration duration;
  if (!ToTemporalDuration(cx, args.get(0), &duration)) {
    return false;
  }

  // Step 3.
  if (operation == TemporalAddDuration::Subtract) {
    duration = duration.negate();
  }

  // Step 4.
  auto dateDuration = ToDateDurationRecordWithoutTime(duration);

  // Steps 6-7.
  auto overflow = TemporalOverflow::Constrain;
  if (args.hasDefined(1)) {
    // Step 6.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", ToName(operation), args[1]));
    if (!options) {
      return false;
    }

    // Step 7.
    if (!GetTemporalOverflowOption(cx, options, &overflow)) {
      return false;
    }
  }

  // Step 8.
  ISODate result;
  if (!CalendarDateAdd(cx, calendar, temporalDate.date(), dateDuration,
                       overflow, &result)) {
    return false;
  }

  // Step 9.
  auto* obj = CreateTemporalDate(cx, result, calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDate ( isoYear, isoMonth, isoDay [ , calendar ] )
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

  // Steps 5-7.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  if (args.hasDefined(3)) {
    // Step 6.
    if (!args[3].isString()) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, args[3],
                       nullptr, "not a string");
      return false;
    }

    // Step 7.
    Rooted<JSString*> calendarString(cx, args[3].toString());
    if (!CanonicalizeCalendar(cx, calendarString, &calendar)) {
      return false;
    }
  }

  // Step 8.
  if (!ThrowIfInvalidISODate(cx, isoYear, isoMonth, isoDay)) {
    return false;
  }

  // Step 9.
  auto isoDate = ISODate{int32_t(isoYear), int32_t(isoMonth), int32_t(isoDay)};

  // Step 10.
  auto* temporalDate = CreateTemporalDate(cx, args, isoDate, calendar);
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
  Rooted<PlainDate> date(cx);
  if (!ToTemporalDate(cx, args.get(0), args.get(1), &date)) {
    return false;
  }

  auto* result = CreateTemporalDate(cx, date);
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
  Rooted<PlainDate> one(cx);
  if (!ToTemporalDate(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  Rooted<PlainDate> two(cx);
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

  // Step 3.
  auto* str =
      NewStringCopy<CanGC>(cx, CalendarIdentifier(temporalDate->calendar()));
  if (!str) {
    return false;
  }

  args.rval().setString(str);
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarEra(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Steps 3-5.
  return CalendarEraYear(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarYear(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarMonth(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarMonthCode(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarDay(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarDayOfWeek(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarDayOfYear(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Steps 3-5.
  return CalendarWeekOfYear(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Steps 3-5.
  return CalendarYearOfWeek(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarDaysInWeek(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarDaysInMonth(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarDaysInYear(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarMonthsInYear(cx, calendar, temporalDate->date(), args.rval());
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
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  return CalendarInLeapYear(cx, calendar, temporalDate->date(), args.rval());
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
  Rooted<PlainDate> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());

  // Step 3.
  auto calendar = temporalDate.calendar();

  // Step 4.
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, temporalDate, &fields)) {
    return false;
  }

  // Step 5.
  Rooted<PlainYearMonth> result(cx);
  if (!CalendarYearMonthFromFields(cx, calendar, fields,
                                   TemporalOverflow::Constrain, &result)) {
    return false;
  }

  // Steps 6-7.
  auto* obj = CreateTemporalYearMonth(cx, result);
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
  Rooted<PlainDate> temporalDate(
      cx, &args.thisv().toObject().as<PlainDateObject>());

  // Step 3.
  auto calendar = temporalDate.calendar();

  // Step 4.
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, temporalDate, &fields)) {
    return false;
  }

  // Step 5.
  Rooted<PlainMonthDay> result(cx);
  if (!CalendarMonthDayFromFields(cx, calendar, fields,
                                  TemporalOverflow::Constrain, &result)) {
    return false;
  }

  // Steps 6-7.
  auto* obj = CreateTemporalMonthDay(cx, result);
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

  // Step 4. (Reordered)
  //
  // Default initialize the time component to all zero.
  auto isoDateTime = ISODateTime{temporalDate->date(), {}};

  // Step 3. (Inlined ToTemporalTimeOrMidnight)
  if (args.hasDefined(0)) {
    if (!ToTemporalTime(cx, args[0], &isoDateTime.time)) {
      return false;
    }
  }

  // Step 4. (Moved above)

  // Step 5.
  auto* obj = CreateTemporalDateTime(cx, isoDateTime, calendar);
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
 * Temporal.PlainDate.prototype.add ( temporalDurationLike [ , options ] )
 */
static bool PlainDate_add(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurationToDate(cx, TemporalAddDuration::Add, args);
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
  // Step 3.
  return AddDurationToDate(cx, TemporalAddDuration::Subtract, args);
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
  Rooted<PlainDate> temporalDate(
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
  auto calendar = temporalDate.calendar();

  // Step 5.
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, temporalDate, &fields)) {
    return false;
  }

  // Step 6.
  Rooted<CalendarFields> partialDate(cx);
  if (!PreparePartialCalendarFields(cx, calendar, temporalDateLike,
                                    {
                                        CalendarField::Year,
                                        CalendarField::Month,
                                        CalendarField::MonthCode,
                                        CalendarField::Day,
                                    },
                                    &partialDate)) {
    return false;
  }
  MOZ_ASSERT(!partialDate.keys().isEmpty());

  // Step 7.
  fields = CalendarMergeFields(calendar, fields, partialDate);

  // Steps 8-9.
  auto overflow = TemporalOverflow::Constrain;
  if (args.hasDefined(1)) {
    // Step 8.
    Rooted<JSObject*> options(cx,
                              RequireObjectArg(cx, "options", "with", args[1]));
    if (!options) {
      return false;
    }

    // Step 9.
    if (!GetTemporalOverflowOption(cx, options, &overflow)) {
      return false;
    }
  }

  // Step 10.
  Rooted<PlainDate> date(cx);
  if (!CalendarDateFromFields(cx, calendar, fields, overflow, &date)) {
    return false;
  }

  // Step 11.
  auto* result = CreateTemporalDate(cx, date);
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
  auto date = temporalDate->date();

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
  auto date = temporalDate->date();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Step 3.
  Rooted<PlainDate> other(cx);
  if (!ToTemporalDate(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 4-5.
  bool equals =
      date == other.date() && CalendarEquals(calendar, other.calendar());

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
 * - |item| is an options object with `timeZone` and `plainTime` properties.
 * - |item| is a time zone identifier string.
 */
static bool PlainDate_toZonedDateTime(JSContext* cx, const CallArgs& args) {
  auto* temporalDate = &args.thisv().toObject().as<PlainDateObject>();
  auto date = temporalDate->date();
  Rooted<CalendarValue> calendar(cx, temporalDate->calendar());

  // Steps 3-4
  Rooted<TimeZoneValue> timeZone(cx);
  Rooted<Value> temporalTime(cx);
  if (args.get(0).isObject()) {
    Rooted<JSObject*> item(cx, &args[0].toObject());

    // Step 3.a.
    Rooted<Value> timeZoneLike(cx);
    if (!GetProperty(cx, item, item, cx->names().timeZone, &timeZoneLike)) {
      return false;
    }

    // Steps 3.b-c.
    if (timeZoneLike.isUndefined()) {
      // Step 3.b.i.
      if (!ToTemporalTimeZone(cx, args[0], &timeZone)) {
        return false;
      }

      // Step 3.b.ii.
      MOZ_ASSERT(temporalTime.isUndefined());
    } else {
      // Step 3.c.i.
      if (!ToTemporalTimeZone(cx, timeZoneLike, &timeZone)) {
        return false;
      }

      // Step 3.c.ii.
      if (!GetProperty(cx, item, item, cx->names().plainTime, &temporalTime)) {
        return false;
      }
    }
  } else {
    // Step 4.a.
    if (!ToTemporalTimeZone(cx, args.get(0), &timeZone)) {
      return false;
    }

    // Step 4.b.
    MOZ_ASSERT(temporalTime.isUndefined());
  }

  // Steps 5-6.
  EpochNanoseconds epochNs;
  if (temporalTime.isUndefined()) {
    // Step 5.a.
    if (!GetStartOfDay(cx, timeZone, date, &epochNs)) {
      return false;
    }
  } else {
    // Step 6.a.
    Time time;
    if (!ToTemporalTime(cx, temporalTime, &time)) {
      return false;
    }

    // Step 6.b.
    auto isoDateTime = ISODateTime{date, time};

    // Step 6.c.
    if (!ISODateTimeWithinLimits(isoDateTime)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
      return false;
    }

    // Step 6.d.
    if (!GetEpochNanosecondsFor(cx, timeZone, isoDateTime,
                                TemporalDisambiguation::Compatible, &epochNs)) {
      return false;
    }
  }

  // Step 7.
  auto* result = CreateTemporalZonedDateTime(cx, epochNs, timeZone, calendar);
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
  // Steps 3-4.
  Handle<PropertyName*> required = cx->names().date;
  Handle<PropertyName*> defaults = cx->names().date;
  return TemporalObjectToLocaleString(cx, args, required, defaults);
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
