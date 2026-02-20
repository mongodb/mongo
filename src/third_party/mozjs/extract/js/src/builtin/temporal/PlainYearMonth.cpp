/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/PlainYearMonth.h"

#include "mozilla/Assertions.h"
#include "mozilla/EnumSet.h"

#include <utility>

#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/intl/DateTimeFormat.h"
#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/CalendarFields.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainMonthDay.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/TimeZone.h"
#include "builtin/temporal/ToString.h"
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

using namespace js;
using namespace js::temporal;

static inline bool IsPlainYearMonth(Handle<Value> v) {
  return v.isObject() && v.toObject().is<PlainYearMonthObject>();
}

/**
 * ISOYearMonthWithinLimits ( isoDate )
 */
bool js::temporal::ISOYearMonthWithinLimits(const ISODate& isoDate) {
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
    return isoDate >= ISODate{min.year, min.month, 1};
  }
  return isoDate < ISODate{max.year, max.month + 1, 1};
}

/**
 * CreateTemporalYearMonth ( isoDate, calendar [ , newTarget ] )
 */
static PlainYearMonthObject* CreateTemporalYearMonth(
    JSContext* cx, const CallArgs& args, const ISODate& isoDate,
    Handle<CalendarValue> calendar) {
  // Step 1.
  if (!ISOYearMonthWithinLimits(isoDate)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_YEAR_MONTH_INVALID);
    return nullptr;
  }

  // Steps 2-3.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_PlainYearMonth,
                                          &proto)) {
    return nullptr;
  }

  auto* object = NewObjectWithClassProto<PlainYearMonthObject>(cx, proto);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  auto packedDate = PackedDate::pack(isoDate);
  object->setFixedSlot(PlainYearMonthObject::PACKED_DATE_SLOT,
                       PrivateUint32Value(packedDate.value));

  // Step 5.
  object->setFixedSlot(PlainYearMonthObject::CALENDAR_SLOT,
                       calendar.toSlotValue());

  // Step 6.
  return object;
}

/**
 * CreateTemporalYearMonth ( isoDate, calendar [ , newTarget ] )
 */
PlainYearMonthObject* js::temporal::CreateTemporalYearMonth(
    JSContext* cx, Handle<PlainYearMonth> yearMonth) {
  MOZ_ASSERT(IsValidISODate(yearMonth));

  // Step 1.
  MOZ_ASSERT(ISOYearMonthWithinLimits(yearMonth));

  // Steps 2-3.
  auto* object = NewBuiltinClassInstance<PlainYearMonthObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  auto packedDate = PackedDate::pack(yearMonth);
  object->setFixedSlot(PlainYearMonthObject::PACKED_DATE_SLOT,
                       PrivateUint32Value(packedDate.value));

  // Step 5.
  object->setFixedSlot(PlainYearMonthObject::CALENDAR_SLOT,
                       yearMonth.calendar().toSlotValue());

  // Step 6.
  return object;
}

/**
 * CreateTemporalYearMonth ( isoDate, calendar [ , newTarget ] )
 */
bool js::temporal::CreateTemporalYearMonth(
    JSContext* cx, const ISODate& isoDate, Handle<CalendarValue> calendar,
    MutableHandle<PlainYearMonth> result) {
  MOZ_ASSERT(IsValidISODate(isoDate));

  // Step 1.
  if (!ISOYearMonthWithinLimits(isoDate)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_YEAR_MONTH_INVALID);
    return false;
  }

  // Steps 2-6.
  result.set(PlainYearMonth{isoDate, calendar});
  return true;
}

struct YearMonthOptions {
  TemporalOverflow overflow = TemporalOverflow::Constrain;
};

/**
 * ToTemporalYearMonth ( item [ , options ] )
 */
static bool ToTemporalYearMonthOptions(JSContext* cx, Handle<Value> options,
                                       YearMonthOptions* result) {
  if (options.isUndefined()) {
    *result = {};
    return true;
  }

  // NOTE: |options| are only passed from `Temporal.PlainYearMonth.from`.

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
 * ToTemporalYearMonth ( item [ , options ] )
 */
static bool ToTemporalYearMonth(JSContext* cx, Handle<JSObject*> item,
                                Handle<Value> options,
                                MutableHandle<PlainYearMonth> result) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.a.
  if (auto* plainYearMonth = item->maybeUnwrapIf<PlainYearMonthObject>()) {
    auto date = plainYearMonth->date();
    Rooted<CalendarValue> calendar(cx, plainYearMonth->calendar());
    if (!calendar.wrap(cx)) {
      return false;
    }

    // Steps 2.a.i-ii.
    YearMonthOptions ignoredOptions;
    if (!ToTemporalYearMonthOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Step 2.a.iii.
    result.set(PlainYearMonth{date, calendar});
    return true;
  }

  // Step 2.b.
  Rooted<CalendarValue> calendar(cx);
  if (!GetTemporalCalendarWithISODefault(cx, item, &calendar)) {
    return false;
  }

  // Step 2.c.
  Rooted<CalendarFields> fields(cx);
  if (!PrepareCalendarFields(cx, calendar, item,
                             {
                                 CalendarField::Year,
                                 CalendarField::Month,
                                 CalendarField::MonthCode,
                             },
                             &fields)) {
    return false;
  }

  // Steps 2.d-e.
  YearMonthOptions resolvedOptions;
  if (!ToTemporalYearMonthOptions(cx, options, &resolvedOptions)) {
    return false;
  }
  auto [overflow] = resolvedOptions;

  // Step 2.f.
  return CalendarYearMonthFromFields(cx, calendar, fields, overflow, result);
}

/**
 * ToTemporalYearMonth ( item [ , options ] )
 */
static bool ToTemporalYearMonth(JSContext* cx, Handle<Value> item,
                                Handle<Value> options,
                                MutableHandle<PlainYearMonth> result) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  if (item.isObject()) {
    Rooted<JSObject*> itemObj(cx, &item.toObject());
    return ToTemporalYearMonth(cx, itemObj, options, result);
  }

  // Step 3.
  if (!item.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, item,
                     nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> string(cx, item.toString());

  // Step 4.
  ISODate date;
  Rooted<JSString*> calendarString(cx);
  if (!ParseTemporalYearMonthString(cx, string, &date, &calendarString)) {
    return false;
  }

  // Steps 5-7.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  if (calendarString) {
    if (!CanonicalizeCalendar(cx, calendarString, &calendar)) {
      return false;
    }
  }

  // Steps 8-9.
  YearMonthOptions ignoredOptions;
  if (!ToTemporalYearMonthOptions(cx, options, &ignoredOptions)) {
    return false;
  }

  // Step 10-11.
  Rooted<PlainYearMonth> yearMonth(cx);
  if (!CreateTemporalYearMonth(cx, date, calendar, &yearMonth)) {
    return false;
  }

  // Step 12.
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, yearMonth, &fields)) {
    return false;
  }

  // Steps 13-14.
  return CalendarYearMonthFromFields(cx, calendar, fields,
                                     TemporalOverflow::Constrain, result);
}

/**
 * ToTemporalYearMonth ( item [ , options ] )
 */
static bool ToTemporalYearMonth(JSContext* cx, Handle<Value> item,
                                MutableHandle<PlainYearMonth> result) {
  return ToTemporalYearMonth(cx, item, UndefinedHandleValue, result);
}

/**
 * DifferenceTemporalPlainYearMonth ( operation, yearMonth, other, options )
 */
static bool DifferenceTemporalPlainYearMonth(JSContext* cx,
                                             TemporalDifference operation,
                                             const CallArgs& args) {
  Rooted<PlainYearMonth> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());

  // Step 1.
  Rooted<PlainYearMonth> other(cx);
  if (!ToTemporalYearMonth(cx, args.get(0), &other)) {
    return false;
  }

  // Step 2.
  auto calendar = yearMonth.calendar();

  // Step 3.
  if (!CalendarEquals(calendar, other.calendar())) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE,
                              CalendarIdentifier(calendar).data(),
                              CalendarIdentifier(other.calendar()).data());
    return false;
  }

  // Steps 4-5.
  DifferenceSettings settings;
  Rooted<PlainObject*> resolvedOptions(cx);
  if (args.hasDefined(1)) {
    // Step 4.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", ToName(operation), args[1]));
    if (!options) {
      return false;
    }

    // Step 5.
    if (!GetDifferenceSettings(cx, operation, options, TemporalUnitGroup::Date,
                               TemporalUnit::Month, TemporalUnit::Month,
                               TemporalUnit::Year, &settings)) {
      return false;
    }
  } else {
    // Steps 4-5.
    settings = {
        TemporalUnit::Month,
        TemporalUnit::Year,
        TemporalRoundingMode::Trunc,
        Increment{1},
    };
  }

  // Step 6.
  if (yearMonth.date() == other.date()) {
    auto* obj = CreateTemporalDuration(cx, {});
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Step 7.
  Rooted<CalendarFields> thisFields(cx);
  if (!ISODateToFields(cx, yearMonth, &thisFields)) {
    return false;
  }

  // Step 8.
  MOZ_ASSERT(!thisFields.has(CalendarField::Day));
  thisFields.setDay(1);

  // Step 9.
  Rooted<PlainDate> thisDate(cx);
  if (!CalendarDateFromFields(cx, calendar, thisFields,
                              TemporalOverflow::Constrain, &thisDate)) {
    return false;
  }

  // Step 10.
  Rooted<CalendarFields> otherFields(cx);
  if (!ISODateToFields(cx, other, &otherFields)) {
    return false;
  }

  // Step 11.
  MOZ_ASSERT(!otherFields.has(CalendarField::Day));
  otherFields.setDay(1);

  // Step 12.
  Rooted<PlainDate> otherDate(cx);
  if (!CalendarDateFromFields(cx, calendar, otherFields,
                              TemporalOverflow::Constrain, &otherDate)) {
    return false;
  }

  // Step 13.
  DateDuration until;
  if (!CalendarDateUntil(cx, calendar, thisDate, otherDate,
                         settings.largestUnit, &until)) {
    return false;
  }

  // Step 14. (Inlined AdjustDateDurationRecord)
  //
  // We only care about years and months here, all other fields are set to zero.
  auto dateDuration = DateDuration{until.years, until.months};

  // Step 15.
  auto duration = InternalDuration{dateDuration, {}};

  // Step 16.
  if (settings.smallestUnit != TemporalUnit::Month ||
      settings.roundingIncrement != Increment{1}) {
    // Step 16.a.
    auto destEpochNs = GetUTCEpochNanoseconds(ISODateTime{otherDate, {}});

    // Steps 16.b-c.
    auto dateTime = ISODateTime{thisDate, {}};

    // Step 16.d.
    Rooted<TimeZoneValue> timeZone(cx, TimeZoneValue{});
    if (!RoundRelativeDuration(
            cx, duration, destEpochNs, dateTime, timeZone, calendar,
            settings.largestUnit, settings.roundingIncrement,
            settings.smallestUnit, settings.roundingMode, &duration)) {
      return false;
    }
  }
  MOZ_ASSERT(IsValidDuration(duration));
  MOZ_ASSERT(duration.date.weeks == 0);
  MOZ_ASSERT(duration.date.days == 0);
  MOZ_ASSERT(duration.time == TimeDuration{});

  // Step 17. (Inlined TemporalDurationFromInternal)
  auto result = duration.date.toDuration();

  // Step 18.
  if (operation == TemporalDifference::Since) {
    result = result.negate();
  }

  // Step 19.
  auto* obj = CreateTemporalDuration(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * AddDurationToYearMonth ( operation, yearMonth, temporalDurationLike, options
 * )
 */
static bool AddDurationToYearMonth(JSContext* cx, TemporalAddDuration operation,
                                   const CallArgs& args) {
  Rooted<PlainYearMonth> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());

  // Step 1.
  Duration duration;
  if (!ToTemporalDuration(cx, args.get(0), &duration)) {
    return false;
  }

  // Step 2.
  if (operation == TemporalAddDuration::Subtract) {
    duration = duration.negate();
  }

  // Steps 3-4.
  auto overflow = TemporalOverflow::Constrain;
  if (args.hasDefined(1)) {
    // Step 3.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", ToName(operation), args[1]));
    if (!options) {
      return false;
    }

    // Step 4.
    if (!GetTemporalOverflowOption(cx, options, &overflow)) {
      return false;
    }
  }

  // Step 5.
  int32_t sign = DurationSign(duration);

  // Step 6.
  auto calendar = yearMonth.calendar();

  // Step 7.
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, yearMonth, &fields)) {
    return false;
  }

  // Step 8.
  MOZ_ASSERT(!fields.has(CalendarField::Day));
  fields.setDay(1);

  // Step 9.
  Rooted<PlainDate> intermediateDate(cx);
  if (!CalendarDateFromFields(cx, calendar, fields, TemporalOverflow::Constrain,
                              &intermediateDate)) {
    return false;
  }

  // Steps 10-11.
  ISODate date;
  if (sign < 0) {
    // |intermediateDate| is initialized to the first day of |yearMonth|'s
    // month. Compute the last day of |yearMonth|'s month by first adding one
    // month and then subtracting one day.
    //
    // This is roughly equivalent to these calls:
    //
    // js> var ym = new Temporal.PlainYearMonth(2023, 1);
    // js> ym.toPlainDate({day: 1}).add({months: 1}).subtract({days: 1}).day
    // 31
    //
    // For many calendars this is equivalent to `ym.daysInMonth`, except when
    // some days are skipped, for example consider the Julian-to-Gregorian
    // calendar transition.

    // Step 10.a.
    auto oneMonthDuration = DateDuration{0, 1};

    // Step 10.b.
    ISODate nextMonth;
    if (!CalendarDateAdd(cx, calendar, intermediateDate, oneMonthDuration,
                         TemporalOverflow::Constrain, &nextMonth)) {
      return false;
    }

    // Step 10.c.
    date = BalanceISODate(nextMonth, -1);

    // Step 10.d.
    MOZ_ASSERT(ISODateWithinLimits(date));
  } else {
    // Step 11.a.
    date = intermediateDate;
  }

  // Steps 12.
  auto durationToAdd = ToDateDurationRecordWithoutTime(duration);

  // Step 13.
  ISODate addedDate;
  if (!CalendarDateAdd(cx, calendar, date, durationToAdd, overflow,
                       &addedDate)) {
    return false;
  }
  MOZ_ASSERT(ISODateWithinLimits(addedDate));

  Rooted<PlainYearMonth> addedYearMonth(cx,
                                        PlainYearMonth{addedDate, calendar});

  // Step 14.
  Rooted<CalendarFields> addedDateFields(cx);
  if (!ISODateToFields(cx, addedYearMonth, &addedDateFields)) {
    return false;
  }

  // Step 15.
  Rooted<PlainYearMonth> result(cx);
  if (!CalendarYearMonthFromFields(cx, calendar, addedDateFields, overflow,
                                   &result)) {
    return false;
  }

  // Step 16.
  auto* obj = CreateTemporalYearMonth(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainYearMonth ( isoYear, isoMonth [ , calendar [ , referenceISODay
 * ] ] )
 */
static bool PlainYearMonthConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Temporal.PlainYearMonth")) {
    return false;
  }

  // Step 3.
  double isoYear;
  if (!ToIntegerWithTruncation(cx, args.get(0), "year", &isoYear)) {
    return false;
  }

  // Step 4.
  double isoMonth;
  if (!ToIntegerWithTruncation(cx, args.get(1), "month", &isoMonth)) {
    return false;
  }

  // Steps 5-7.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  if (args.hasDefined(2)) {
    // Step 6.
    if (!args[2].isString()) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, args[2],
                       nullptr, "not a string");
      return false;
    }

    // Step 7.
    Rooted<JSString*> calendarString(cx, args[2].toString());
    if (!CanonicalizeCalendar(cx, calendarString, &calendar)) {
      return false;
    }
  }

  // Steps 2 and 8.
  double isoDay = 1;
  if (args.hasDefined(3)) {
    if (!ToIntegerWithTruncation(cx, args[3], "day", &isoDay)) {
      return false;
    }
  }

  // Step 9.
  if (!ThrowIfInvalidISODate(cx, isoYear, isoMonth, isoDay)) {
    return false;
  }

  // Step 10.
  auto isoDate = ISODate{int32_t(isoYear), int32_t(isoMonth), int32_t(isoDay)};

  // Step 11.
  auto* yearMonth = CreateTemporalYearMonth(cx, args, isoDate, calendar);
  if (!yearMonth) {
    return false;
  }

  args.rval().setObject(*yearMonth);
  return true;
}

/**
 * Temporal.PlainYearMonth.from ( item [ , options ] )
 */
static bool PlainYearMonth_from(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<PlainYearMonth> yearMonth(cx);
  if (!ToTemporalYearMonth(cx, args.get(0), args.get(1), &yearMonth)) {
    return false;
  }

  auto* result = CreateTemporalYearMonth(cx, yearMonth);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainYearMonth.compare ( one, two )
 */
static bool PlainYearMonth_compare(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<PlainYearMonth> one(cx);
  if (!ToTemporalYearMonth(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  Rooted<PlainYearMonth> two(cx);
  if (!ToTemporalYearMonth(cx, args.get(1), &two)) {
    return false;
  }

  // Step 3.
  args.rval().setInt32(CompareISODate(one, two));
  return true;
}

/**
 * get Temporal.PlainYearMonth.prototype.calendarId
 */
static bool PlainYearMonth_calendarId(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();

  // Step 3.
  auto* str =
      NewStringCopy<CanGC>(cx, CalendarIdentifier(yearMonth->calendar()));
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * get Temporal.PlainYearMonth.prototype.calendarId
 */
static bool PlainYearMonth_calendarId(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_calendarId>(
      cx, args);
}

/**
 * get Temporal.PlainYearMonth.prototype.era
 */
static bool PlainYearMonth_era(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  return CalendarEra(cx, calendar, yearMonth->date(), args.rval());
}

/**
 * get Temporal.PlainYearMonth.prototype.era
 */
static bool PlainYearMonth_era(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_era>(cx, args);
}

/**
 * get Temporal.PlainYearMonth.prototype.eraYear
 */
static bool PlainYearMonth_eraYear(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Steps 3-5.
  return CalendarEraYear(cx, calendar, yearMonth->date(), args.rval());
}

/**
 * get Temporal.PlainYearMonth.prototype.eraYear
 */
static bool PlainYearMonth_eraYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_eraYear>(cx,
                                                                        args);
}

/**
 * get Temporal.PlainYearMonth.prototype.year
 */
static bool PlainYearMonth_year(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  return CalendarYear(cx, calendar, yearMonth->date(), args.rval());
}

/**
 * get Temporal.PlainYearMonth.prototype.year
 */
static bool PlainYearMonth_year(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_year>(cx, args);
}

/**
 * get Temporal.PlainYearMonth.prototype.month
 */
static bool PlainYearMonth_month(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  return CalendarMonth(cx, calendar, yearMonth->date(), args.rval());
}

/**
 * get Temporal.PlainYearMonth.prototype.month
 */
static bool PlainYearMonth_month(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_month>(cx, args);
}

/**
 * get Temporal.PlainYearMonth.prototype.monthCode
 */
static bool PlainYearMonth_monthCode(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  return CalendarMonthCode(cx, calendar, yearMonth->date(), args.rval());
}

/**
 * get Temporal.PlainYearMonth.prototype.monthCode
 */
static bool PlainYearMonth_monthCode(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_monthCode>(cx,
                                                                          args);
}

/**
 * get Temporal.PlainYearMonth.prototype.daysInYear
 */
static bool PlainYearMonth_daysInYear(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  return CalendarDaysInYear(cx, calendar, yearMonth->date(), args.rval());
}

/**
 * get Temporal.PlainYearMonth.prototype.daysInYear
 */
static bool PlainYearMonth_daysInYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_daysInYear>(
      cx, args);
}

/**
 * get Temporal.PlainYearMonth.prototype.daysInMonth
 */
static bool PlainYearMonth_daysInMonth(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  return CalendarDaysInMonth(cx, calendar, yearMonth->date(), args.rval());
}

/**
 * get Temporal.PlainYearMonth.prototype.daysInMonth
 */
static bool PlainYearMonth_daysInMonth(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_daysInMonth>(
      cx, args);
}

/**
 * get Temporal.PlainYearMonth.prototype.monthsInYear
 */
static bool PlainYearMonth_monthsInYear(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  return CalendarMonthsInYear(cx, calendar, yearMonth->date(), args.rval());
}

/**
 * get Temporal.PlainYearMonth.prototype.monthsInYear
 */
static bool PlainYearMonth_monthsInYear(JSContext* cx, unsigned argc,
                                        Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_monthsInYear>(
      cx, args);
}

/**
 * get Temporal.PlainYearMonth.prototype.inLeapYear
 */
static bool PlainYearMonth_inLeapYear(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  return CalendarInLeapYear(cx, calendar, yearMonth->date(), args.rval());
}

/**
 * get Temporal.PlainYearMonth.prototype.inLeapYear
 */
static bool PlainYearMonth_inLeapYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_inLeapYear>(
      cx, args);
}

/**
 * Temporal.PlainYearMonth.prototype.with ( temporalYearMonthLike [ , options ]
 * )
 */
static bool PlainYearMonth_with(JSContext* cx, const CallArgs& args) {
  Rooted<PlainYearMonth> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());

  // Step 3.
  Rooted<JSObject*> temporalYearMonthLike(
      cx, RequireObjectArg(cx, "temporalYearMonthLike", "with", args.get(0)));
  if (!temporalYearMonthLike) {
    return false;
  }
  if (!ThrowIfTemporalLikeObject(cx, temporalYearMonthLike)) {
    return false;
  }

  // Step 4.
  auto calendar = yearMonth.calendar();

  // Step 5.
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, yearMonth, &fields)) {
    return false;
  }

  // Step 6.
  Rooted<CalendarFields> partialYearMonth(cx);
  if (!PreparePartialCalendarFields(cx, calendar, temporalYearMonthLike,
                                    {
                                        CalendarField::Year,
                                        CalendarField::Month,
                                        CalendarField::MonthCode,
                                    },
                                    &partialYearMonth)) {
    return false;
  }
  MOZ_ASSERT(!partialYearMonth.keys().isEmpty());

  // Step 7.
  fields = CalendarMergeFields(calendar, fields, partialYearMonth);

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
  Rooted<PlainYearMonth> result(cx);
  if (!CalendarYearMonthFromFields(cx, calendar, fields, overflow, &result)) {
    return false;
  }

  // Step 11.
  auto* obj = CreateTemporalYearMonth(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainYearMonth.prototype.with ( temporalYearMonthLike [ , options ]
 * )
 */
static bool PlainYearMonth_with(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_with>(cx, args);
}

/**
 * Temporal.PlainYearMonth.prototype.add ( temporalDurationLike [ , options ] )
 */
static bool PlainYearMonth_add(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurationToYearMonth(cx, TemporalAddDuration::Add, args);
}

/**
 * Temporal.PlainYearMonth.prototype.add ( temporalDurationLike [ , options ] )
 */
static bool PlainYearMonth_add(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_add>(cx, args);
}

/**
 * Temporal.PlainYearMonth.prototype.subtract ( temporalDurationLike [ , options
 * ] )
 */
static bool PlainYearMonth_subtract(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurationToYearMonth(cx, TemporalAddDuration::Subtract, args);
}

/**
 * Temporal.PlainYearMonth.prototype.subtract ( temporalDurationLike [ , options
 * ] )
 */
static bool PlainYearMonth_subtract(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_subtract>(cx,
                                                                         args);
}

/**
 * Temporal.PlainYearMonth.prototype.until ( other [ , options ] )
 */
static bool PlainYearMonth_until(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalPlainYearMonth(cx, TemporalDifference::Until, args);
}

/**
 * Temporal.PlainYearMonth.prototype.until ( other [ , options ] )
 */
static bool PlainYearMonth_until(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_until>(cx, args);
}

/**
 * Temporal.PlainYearMonth.prototype.since ( other [ , options ] )
 */
static bool PlainYearMonth_since(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalPlainYearMonth(cx, TemporalDifference::Since, args);
}

/**
 * Temporal.PlainYearMonth.prototype.since ( other [ , options ] )
 */
static bool PlainYearMonth_since(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_since>(cx, args);
}

/**
 * Temporal.PlainYearMonth.prototype.equals ( other )
 */
static bool PlainYearMonth_equals(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();
  auto date = yearMonth->date();
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  Rooted<PlainYearMonth> other(cx);
  if (!ToTemporalYearMonth(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 4-7.
  bool equals =
      date == other.date() && CalendarEquals(calendar, other.calendar());

  args.rval().setBoolean(equals);
  return true;
}

/**
 * Temporal.PlainYearMonth.prototype.equals ( other )
 */
static bool PlainYearMonth_equals(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_equals>(cx,
                                                                       args);
}

/**
 * Temporal.PlainYearMonth.prototype.toString ( [ options ] )
 */
static bool PlainYearMonth_toString(JSContext* cx, const CallArgs& args) {
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());

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
  JSString* str = TemporalYearMonthToString(cx, yearMonth, showCalendar);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.PlainYearMonth.prototype.toString ( [ options ] )
 */
static bool PlainYearMonth_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_toString>(cx,
                                                                         args);
}

/**
 * Temporal.PlainYearMonth.prototype.toLocaleString ( [ locales [ , options ] ]
 * )
 */
static bool PlainYearMonth_toLocaleString(JSContext* cx, const CallArgs& args) {
  // Steps 3-4.
  Handle<PropertyName*> required = cx->names().date;
  Handle<PropertyName*> defaults = cx->names().date;
  return TemporalObjectToLocaleString(cx, args, required, defaults);
}

/**
 * Temporal.PlainYearMonth.prototype.toLocaleString ( [ locales [ , options ] ]
 * )
 */
static bool PlainYearMonth_toLocaleString(JSContext* cx, unsigned argc,
                                          Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_toLocaleString>(
      cx, args);
}

/**
 * Temporal.PlainYearMonth.prototype.toJSON ( )
 */
static bool PlainYearMonth_toJSON(JSContext* cx, const CallArgs& args) {
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());

  // Step 3.
  JSString* str = TemporalYearMonthToString(cx, yearMonth, ShowCalendar::Auto);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.PlainYearMonth.prototype.toJSON ( )
 */
static bool PlainYearMonth_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_toJSON>(cx,
                                                                       args);
}

/**
 *  Temporal.PlainYearMonth.prototype.valueOf ( )
 */
static bool PlainYearMonth_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                            "PlainYearMonth", "primitive type");
  return false;
}

/**
 * Temporal.PlainYearMonth.prototype.toPlainDate ( item )
 */
static bool PlainYearMonth_toPlainDate(JSContext* cx, const CallArgs& args) {
  Rooted<PlainYearMonth> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());

  // Step 3.
  Rooted<JSObject*> item(
      cx, RequireObjectArg(cx, "item", "toPlainDate", args.get(0)));
  if (!item) {
    return false;
  }

  // Step 4.
  auto calendar = yearMonth.calendar();

  // Step 5.
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, yearMonth, &fields)) {
    return false;
  }

  // Step 6.
  Rooted<CalendarFields> inputFields(cx);
  if (!PrepareCalendarFields(cx, calendar, item,
                             {
                                 CalendarField::Day,
                             },
                             &inputFields)) {
    return false;
  }

  // Step 7.
  fields = CalendarMergeFields(calendar, fields, inputFields);

  // Step 8.
  Rooted<PlainDate> result(cx);
  if (!CalendarDateFromFields(cx, calendar, fields, TemporalOverflow::Constrain,
                              &result)) {
    return false;
  }

  // Step 9.
  auto* obj = CreateTemporalDate(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainYearMonth.prototype.toPlainDate ( item )
 */
static bool PlainYearMonth_toPlainDate(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_toPlainDate>(
      cx, args);
}

const JSClass PlainYearMonthObject::class_ = {
    "Temporal.PlainYearMonth",
    JSCLASS_HAS_RESERVED_SLOTS(PlainYearMonthObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_PlainYearMonth),
    JS_NULL_CLASS_OPS,
    &PlainYearMonthObject::classSpec_,
};

const JSClass& PlainYearMonthObject::protoClass_ = PlainObject::class_;

static const JSFunctionSpec PlainYearMonth_methods[] = {
    JS_FN("from", PlainYearMonth_from, 1, 0),
    JS_FN("compare", PlainYearMonth_compare, 2, 0),
    JS_FS_END,
};

static const JSFunctionSpec PlainYearMonth_prototype_methods[] = {
    JS_FN("with", PlainYearMonth_with, 1, 0),
    JS_FN("add", PlainYearMonth_add, 1, 0),
    JS_FN("subtract", PlainYearMonth_subtract, 1, 0),
    JS_FN("until", PlainYearMonth_until, 1, 0),
    JS_FN("since", PlainYearMonth_since, 1, 0),
    JS_FN("equals", PlainYearMonth_equals, 1, 0),
    JS_FN("toString", PlainYearMonth_toString, 0, 0),
    JS_FN("toLocaleString", PlainYearMonth_toLocaleString, 0, 0),
    JS_FN("toJSON", PlainYearMonth_toJSON, 0, 0),
    JS_FN("valueOf", PlainYearMonth_valueOf, 0, 0),
    JS_FN("toPlainDate", PlainYearMonth_toPlainDate, 1, 0),
    JS_FS_END,
};

static const JSPropertySpec PlainYearMonth_prototype_properties[] = {
    JS_PSG("calendarId", PlainYearMonth_calendarId, 0),
    JS_PSG("era", PlainYearMonth_era, 0),
    JS_PSG("eraYear", PlainYearMonth_eraYear, 0),
    JS_PSG("year", PlainYearMonth_year, 0),
    JS_PSG("month", PlainYearMonth_month, 0),
    JS_PSG("monthCode", PlainYearMonth_monthCode, 0),
    JS_PSG("daysInYear", PlainYearMonth_daysInYear, 0),
    JS_PSG("daysInMonth", PlainYearMonth_daysInMonth, 0),
    JS_PSG("monthsInYear", PlainYearMonth_monthsInYear, 0),
    JS_PSG("inLeapYear", PlainYearMonth_inLeapYear, 0),
    JS_STRING_SYM_PS(toStringTag, "Temporal.PlainYearMonth", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec PlainYearMonthObject::classSpec_ = {
    GenericCreateConstructor<PlainYearMonthConstructor, 2,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<PlainYearMonthObject>,
    PlainYearMonth_methods,
    nullptr,
    PlainYearMonth_prototype_methods,
    PlainYearMonth_prototype_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};
