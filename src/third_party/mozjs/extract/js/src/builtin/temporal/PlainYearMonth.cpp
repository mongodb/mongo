/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/PlainYearMonth.h"

#include "mozilla/Assertions.h"

#include <type_traits>
#include <utility>

#include "jsnum.h"
#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalFields.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/ToString.h"
#include "builtin/temporal/Wrapped.h"
#include "ds/IdValuePair.h"
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "js/AllocPolicy.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
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
#include "vm/ObjectOperations.h"
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
 * ISOYearMonthWithinLimits ( year, month )
 */
template <typename T>
static bool ISOYearMonthWithinLimits(T year, int32_t month) {
  static_assert(std::is_same_v<T, int32_t> || std::is_same_v<T, double>);

  // Step 1.
  MOZ_ASSERT(IsInteger(year));
  MOZ_ASSERT(1 <= month && month <= 12);

  // Step 2.
  if (year < -271821 || year > 275760) {
    return false;
  }

  // Step 3.
  if (year == -271821 && month < 4) {
    return false;
  }

  // Step 4.
  if (year == 275760 && month > 9) {
    return false;
  }

  // Step 5.
  return true;
}

/**
 * CreateTemporalYearMonth ( isoYear, isoMonth, calendar, referenceISODay [ ,
 * newTarget ] )
 */
static PlainYearMonthObject* CreateTemporalYearMonth(
    JSContext* cx, const CallArgs& args, double isoYear, double isoMonth,
    double isoDay, Handle<CalendarValue> calendar) {
  MOZ_ASSERT(IsInteger(isoYear));
  MOZ_ASSERT(IsInteger(isoMonth));
  MOZ_ASSERT(IsInteger(isoDay));

  // Step 1.
  if (!ThrowIfInvalidISODate(cx, isoYear, isoMonth, isoDay)) {
    return nullptr;
  }

  // FIXME: spec issue - Consider calling ISODateTimeWithinLimits to include
  // testing |referenceISODay|?

  // Step 2.
  if (!ISOYearMonthWithinLimits(isoYear, int32_t(isoMonth))) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_YEAR_MONTH_INVALID);
    return nullptr;
  }

  // Steps 3-4.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_PlainYearMonth,
                                          &proto)) {
    return nullptr;
  }

  auto* obj = NewObjectWithClassProto<PlainYearMonthObject>(cx, proto);
  if (!obj) {
    return nullptr;
  }

  // Step 5.
  obj->setFixedSlot(PlainYearMonthObject::ISO_YEAR_SLOT,
                    Int32Value(int32_t(isoYear)));

  // Step 6.
  obj->setFixedSlot(PlainYearMonthObject::ISO_MONTH_SLOT,
                    Int32Value(int32_t(isoMonth)));

  // Step 7.
  obj->setFixedSlot(PlainYearMonthObject::CALENDAR_SLOT,
                    calendar.toSlotValue());

  // Step 8.
  obj->setFixedSlot(PlainYearMonthObject::ISO_DAY_SLOT,
                    Int32Value(int32_t(isoDay)));

  // Step 9.
  return obj;
}

/**
 * CreateTemporalYearMonth ( isoYear, isoMonth, calendar, referenceISODay [ ,
 * newTarget ] )
 */
PlainYearMonthObject* js::temporal::CreateTemporalYearMonth(
    JSContext* cx, const PlainDate& date, Handle<CalendarValue> calendar) {
  const auto& [isoYear, isoMonth, isoDay] = date;

  // Step 1.
  if (!ThrowIfInvalidISODate(cx, date)) {
    return nullptr;
  }

  // FIXME: spec issue - Consider calling ISODateTimeWithinLimits to include
  // testing |referenceISODay|?

  // Step 2.
  if (!ISOYearMonthWithinLimits(isoYear, isoMonth)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_YEAR_MONTH_INVALID);
    return nullptr;
  }

  // Steps 3-4.
  auto* obj = NewBuiltinClassInstance<PlainYearMonthObject>(cx);
  if (!obj) {
    return nullptr;
  }

  // Step 5.
  obj->setFixedSlot(PlainYearMonthObject::ISO_YEAR_SLOT, Int32Value(isoYear));

  // Step 6.
  obj->setFixedSlot(PlainYearMonthObject::ISO_MONTH_SLOT, Int32Value(isoMonth));

  // Step 7.
  obj->setFixedSlot(PlainYearMonthObject::CALENDAR_SLOT,
                    calendar.toSlotValue());

  // Step 8.
  obj->setFixedSlot(PlainYearMonthObject::ISO_DAY_SLOT, Int32Value(isoDay));

  // Step 9.
  return obj;
}

/**
 * ToTemporalYearMonth ( item [ , options ] )
 */
static Wrapped<PlainYearMonthObject*> ToTemporalYearMonth(
    JSContext* cx, Handle<Value> item,
    Handle<JSObject*> maybeOptions = nullptr) {
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

    // Step 3.a.
    if (itemObj->canUnwrapAs<PlainYearMonthObject>()) {
      return itemObj;
    }

    // Step 3.b.
    Rooted<CalendarValue> calendarValue(cx);
    if (!GetTemporalCalendarWithISODefault(cx, itemObj, &calendarValue)) {
      return nullptr;
    }

    // Step 3.c.
    Rooted<CalendarRecord> calendar(cx);
    if (!CreateCalendarMethodsRecord(cx, calendarValue,
                                     {
                                         CalendarMethod::Fields,
                                         CalendarMethod::YearMonthFromFields,
                                     },
                                     &calendar)) {
      return nullptr;
    }

    // Step 3.d.
    Rooted<PlainObject*> fields(
        cx, PrepareCalendarFields(cx, calendar, itemObj,
                                  {
                                      CalendarField::Month,
                                      CalendarField::MonthCode,
                                      CalendarField::Year,
                                  }));
    if (!fields) {
      return nullptr;
    }

    // Step 3.e.
    if (maybeResolvedOptions) {
      return temporal::CalendarYearMonthFromFields(cx, calendar, fields,
                                                   maybeResolvedOptions);
    }
    return CalendarYearMonthFromFields(cx, calendar, fields);
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
  if (!ParseTemporalYearMonthString(cx, string, &result, &calendarString)) {
    return nullptr;
  }

  // Steps 6-9.
  Rooted<CalendarValue> calendarValue(cx, CalendarValue(CalendarId::ISO8601));
  if (calendarString) {
    if (!ToBuiltinCalendar(cx, calendarString, &calendarValue)) {
      return nullptr;
    }
  }

  // Step 10.
  if (maybeResolvedOptions) {
    TemporalOverflow ignored;
    if (!GetTemporalOverflowOption(cx, maybeResolvedOptions, &ignored)) {
      return nullptr;
    }
  }

  // Step 11.
  Rooted<PlainYearMonthObject*> obj(
      cx, CreateTemporalYearMonth(cx, result, calendarValue));
  if (!obj) {
    return nullptr;
  }

  // Step 12.
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, calendarValue,
                                   {
                                       CalendarMethod::YearMonthFromFields,
                                   },
                                   &calendar)) {
    return nullptr;
  }

  // FIXME: spec issue - reorder note to appear directly before
  // CalendarYearMonthFromFields

  // Steps 13-14.
  return CalendarYearMonthFromFields(cx, calendar, obj);
}

/**
 * ToTemporalYearMonth ( item [ , options ] )
 */
static bool ToTemporalYearMonth(JSContext* cx, Handle<Value> item,
                                PlainDate* result) {
  auto obj = ToTemporalYearMonth(cx, item);
  if (!obj) {
    return false;
  }

  *result = ToPlainDate(&obj.unwrap());
  return true;
}

/**
 * ToTemporalYearMonth ( item [ , options ] )
 */
static bool ToTemporalYearMonth(JSContext* cx, Handle<Value> item,
                                PlainDate* result,
                                MutableHandle<CalendarValue> calendar) {
  auto* obj = ToTemporalYearMonth(cx, item).unwrapOrNull();
  if (!obj) {
    return false;
  }

  *result = ToPlainDate(obj);
  calendar.set(obj->calendar());
  return calendar.wrap(cx);
}

/**
 * DifferenceTemporalPlainYearMonth ( operation, yearMonth, other, options )
 */
static bool DifferenceTemporalPlainYearMonth(JSContext* cx,
                                             TemporalDifference operation,
                                             const CallArgs& args) {
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());

  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  auto otherYearMonth = ToTemporalYearMonth(cx, args.get(0));
  if (!otherYearMonth) {
    return false;
  }
  auto* unwrappedOtherYearMonth = &otherYearMonth.unwrap();
  auto otherYearMonthDate = ToPlainDate(unwrappedOtherYearMonth);

  Rooted<Wrapped<PlainYearMonthObject*>> other(cx, otherYearMonth);
  Rooted<CalendarValue> otherCalendar(cx, unwrappedOtherYearMonth->calendar());
  if (!otherCalendar.wrap(cx)) {
    return false;
  }

  // Step 3.
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 4.
  if (!CalendarEqualsOrThrow(cx, calendar, otherCalendar)) {
    return false;
  }

  // Steps 5-6.
  DifferenceSettings settings;
  Rooted<PlainObject*> resolvedOptions(cx);
  if (args.hasDefined(1)) {
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", ToName(operation), args[1]));
    if (!options) {
      return false;
    }

    // Step 5.
    resolvedOptions = SnapshotOwnProperties(cx, options);
    if (!resolvedOptions) {
      return false;
    }

    // Step 6.
    if (!GetDifferenceSettings(cx, operation, resolvedOptions,
                               TemporalUnitGroup::Date, TemporalUnit::Month,
                               TemporalUnit::Month, TemporalUnit::Year,
                               &settings)) {
      return false;
    }
  } else {
    // Steps 5-6.
    settings = {
        TemporalUnit::Month,
        TemporalUnit::Year,
        TemporalRoundingMode::Trunc,
        Increment{1},
    };
  }

  // Step 7.
  if (ToPlainDate(yearMonth) == otherYearMonthDate) {
    auto* obj = CreateTemporalDuration(cx, {});
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Step 8.
  Rooted<CalendarRecord> calendarRec(cx);
  if (!CreateCalendarMethodsRecord(cx, calendar,
                                   {
                                       CalendarMethod::DateAdd,
                                       CalendarMethod::DateFromFields,
                                       CalendarMethod::DateUntil,
                                       CalendarMethod::Fields,
                                   },
                                   &calendarRec)) {
    return false;
  }

  // Step 9.
  Rooted<PlainObject*> thisFields(cx);
  JS::RootedVector<PropertyKey> thisFieldNames(cx);
  if (!PrepareCalendarFieldsAndFieldNames(cx, calendarRec, yearMonth,
                                          {
                                              CalendarField::MonthCode,
                                              CalendarField::Year,
                                          },
                                          &thisFields, &thisFieldNames)) {
    return false;
  }

  // Step 10.
  Value one = Int32Value(1);
  auto handleOne = Handle<Value>::fromMarkedLocation(&one);
  if (!DefineDataProperty(cx, thisFields, cx->names().day, handleOne)) {
    return false;
  }

  // Step 11.
  Rooted<Wrapped<PlainDateObject*>> thisDate(
      cx, CalendarDateFromFields(cx, calendarRec, thisFields));
  if (!thisDate) {
    return false;
  }

  // Step 12.
  Rooted<PlainObject*> otherFields(
      cx, PrepareTemporalFields(cx, other, thisFieldNames));
  if (!otherFields) {
    return false;
  }

  // Step 13.
  if (!DefineDataProperty(cx, otherFields, cx->names().day, handleOne)) {
    return false;
  }

  // Step 14.
  Rooted<Wrapped<PlainDateObject*>> otherDate(
      cx, CalendarDateFromFields(cx, calendarRec, otherFields));
  if (!otherDate) {
    return false;
  }

  // Steps 15-16.
  DateDuration until;
  if (resolvedOptions) {
    if (!CalendarDateUntil(cx, calendarRec, thisDate, otherDate,
                           settings.largestUnit, resolvedOptions, &until)) {
      return false;
    }
  } else {
    if (!CalendarDateUntil(cx, calendarRec, thisDate, otherDate,
                           settings.largestUnit, &until)) {
      return false;
    }
  }

  // We only care about years and months here, all other fields are set to zero.
  auto dateDuration = DateDuration{until.years, until.months};

  // Step 17.
  if (settings.smallestUnit != TemporalUnit::Month ||
      settings.roundingIncrement != Increment{1}) {
    // Steps 17.a-b.
    NormalizedDuration roundResult;
    if (!RoundDuration(cx, {dateDuration, {}}, settings.roundingIncrement,
                       settings.smallestUnit, settings.roundingMode, thisDate,
                       calendarRec, &roundResult)) {
      return false;
    }

    // Step 17.c.
    auto toBalance =
        DateDuration{roundResult.date.years, roundResult.date.months};
    if (!temporal::BalanceDateDurationRelative(
            cx, toBalance, settings.largestUnit, settings.smallestUnit,
            thisDate, calendarRec, &dateDuration)) {
      return false;
    }
  }

  // Step 18.
  auto duration =
      Duration{double(dateDuration.years), double(dateDuration.months)};
  if (operation == TemporalDifference::Since) {
    duration = duration.negate();
  }

  auto* obj = CreateTemporalDuration(cx, duration);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

enum class PlainYearMonthDuration { Add, Subtract };

/**
 * AddDurationToOrSubtractDurationFromPlainYearMonth ( operation, yearMonth,
 * temporalDurationLike, options )
 */
static bool AddDurationToOrSubtractDurationFromPlainYearMonth(
    JSContext* cx, PlainYearMonthDuration operation, const CallArgs& args) {
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());

  // Step 1.
  Duration duration;
  if (!ToTemporalDurationRecord(cx, args.get(0), &duration)) {
    return false;
  }

  // Step 2.
  if (operation == PlainYearMonthDuration::Subtract) {
    duration = duration.negate();
  }

  // Step 3.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(1)) {
    const char* name =
        operation == PlainYearMonthDuration::Add ? "add" : "subtract";
    options = RequireObjectArg(cx, "options", name, args[1]);
  } else {
    // TODO: Avoid creating an options object if not necessary.
    options = NewPlainObjectWithProto(cx, nullptr);
  }
  if (!options) {
    return false;
  }

  // Step 4.
  auto timeDuration = NormalizeTimeDuration(duration);

  // Step 5.
  auto balancedTime = BalanceTimeDuration(timeDuration, TemporalUnit::Day);

  // Steps 6 and 15. (Reordered)
  auto durationToAdd = DateDuration{
      int64_t(duration.years),
      int64_t(duration.months),
      int64_t(duration.weeks),
      int64_t(duration.days) + balancedTime.days,
  };

  // Step 7.
  int32_t sign = DurationSign(durationToAdd);

  // Step 8.
  Rooted<CalendarValue> calendarValue(cx, yearMonth->calendar());
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, calendarValue,
                                   {
                                       CalendarMethod::DateAdd,
                                       CalendarMethod::DateFromFields,
                                       CalendarMethod::Day,
                                       CalendarMethod::Fields,
                                       CalendarMethod::YearMonthFromFields,
                                   },
                                   &calendar)) {
    return false;
  };

  // Step 9.
  Rooted<PlainObject*> fields(cx);
  JS::RootedVector<PropertyKey> fieldNames(cx);
  if (!PrepareCalendarFieldsAndFieldNames(cx, calendar, yearMonth,
                                          {
                                              CalendarField::MonthCode,
                                              CalendarField::Year,
                                          },
                                          &fields, &fieldNames)) {
    return false;
  }

  // Step 10.
  Rooted<PlainObject*> fieldsCopy(cx, SnapshotOwnProperties(cx, fields));
  if (!fieldsCopy) {
    return false;
  }

  // Step 11.
  Value one = Int32Value(1);
  auto handleOne = Handle<Value>::fromMarkedLocation(&one);
  if (!DefineDataProperty(cx, fields, cx->names().day, handleOne)) {
    return false;
  }

  // Step 12.
  Rooted<Wrapped<PlainDateObject*>> intermediateDate(
      cx, CalendarDateFromFields(cx, calendar, fields));
  if (!intermediateDate) {
    return false;
  }

  // Steps 13-14.
  Rooted<Wrapped<PlainDateObject*>> date(cx);
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

    // Step 13.a.
    auto oneMonthDuration = DateDuration{0, 1};

    // Step 13.b.
    Rooted<Wrapped<PlainDateObject*>> nextMonth(
        cx, CalendarDateAdd(cx, calendar, intermediateDate, oneMonthDuration));
    if (!nextMonth) {
      return false;
    }

    auto* unwrappedNextMonth = nextMonth.unwrap(cx);
    if (!unwrappedNextMonth) {
      return false;
    }
    auto nextMonthDate = ToPlainDate(unwrappedNextMonth);

    // Step 13.c.
    auto endOfMonthISO = BalanceISODate(nextMonthDate.year, nextMonthDate.month,
                                        nextMonthDate.day - 1);

    // Step 13.d.
    Rooted<PlainDateWithCalendar> endOfMonth(cx);
    if (!CreateTemporalDate(cx, endOfMonthISO, calendar.receiver(),
                            &endOfMonth)) {
      return false;
    }

    // Step 13.e.
    Rooted<Value> day(cx);
    if (!CalendarDay(cx, calendar, endOfMonth.date(), &day)) {
      return false;
    }

    // Step 13.f.
    if (!DefineDataProperty(cx, fieldsCopy, cx->names().day, day)) {
      return false;
    }

    // Step 13.g.
    date = CalendarDateFromFields(cx, calendar, fieldsCopy);
    if (!date) {
      return false;
    }
  } else {
    // Step 14.a.
    date = intermediateDate;
  }

  // Step 15. (Moved above)

  // Step 16.
  Rooted<PlainObject*> optionsCopy(cx, SnapshotOwnProperties(cx, options));
  if (!optionsCopy) {
    return false;
  }

  // Step 17.
  Rooted<Wrapped<PlainDateObject*>> addedDate(
      cx, AddDate(cx, calendar, date, durationToAdd, options));
  if (!addedDate) {
    return false;
  }

  // Step 18.
  Rooted<PlainObject*> addedDateFields(
      cx, PrepareTemporalFields(cx, addedDate, fieldNames));
  if (!addedDateFields) {
    return false;
  }

  // Step 19.
  auto obj = temporal::CalendarYearMonthFromFields(
      cx, calendar, addedDateFields, optionsCopy);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainYearMonth ( isoYear, isoMonth [ , calendarLike [ ,
 * referenceISODay ] ] )
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

  // Step 5.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendarWithISODefault(cx, args.get(2), &calendar)) {
    return false;
  }

  // Steps 2 and 6.
  double isoDay = 1;
  if (args.hasDefined(3)) {
    if (!ToIntegerWithTruncation(cx, args[3], "day", &isoDay)) {
      return false;
    }
  }

  // Step 7.
  auto* yearMonth =
      CreateTemporalYearMonth(cx, args, isoYear, isoMonth, isoDay, calendar);
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

    if (auto* yearMonth = item->maybeUnwrapIf<PlainYearMonthObject>()) {
      auto date = ToPlainDate(yearMonth);

      Rooted<CalendarValue> calendar(cx, yearMonth->calendar());
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
      auto* obj = CreateTemporalYearMonth(cx, date, calendar);
      if (!obj) {
        return false;
      }

      args.rval().setObject(*obj);
      return true;
    }
  }

  // Step 3.
  auto obj = ToTemporalYearMonth(cx, args.get(0), options);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainYearMonth.compare ( one, two )
 */
static bool PlainYearMonth_compare(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  PlainDate one;
  if (!ToTemporalYearMonth(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  PlainDate two;
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
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  auto* calendarId = ToTemporalCalendarIdentifier(cx, calendar);
  if (!calendarId) {
    return false;
  }

  args.rval().setString(calendarId);
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
  // Step 3.
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 4.
  return CalendarEra(cx, calendar, yearMonth, args.rval());
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
  // Step 3.
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Steps 4-6.
  return CalendarEraYear(cx, calendar, yearMonth, args.rval());
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
  // Step 3.
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 4.
  return CalendarYear(cx, calendar, yearMonth, args.rval());
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
  // Step 3.
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 4.
  return CalendarMonth(cx, calendar, yearMonth, args.rval());
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
  // Step 3.
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 4.
  return CalendarMonthCode(cx, calendar, yearMonth, args.rval());
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
  // Step 3.
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 4.
  return CalendarDaysInYear(cx, calendar, yearMonth, args.rval());
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
  // Step 3.
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 4.
  return CalendarDaysInMonth(cx, calendar, yearMonth, args.rval());
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
  // Step 3.
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 4.
  return CalendarMonthsInYear(cx, calendar, yearMonth, args.rval());
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
  // Step 3.
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 4.
  return CalendarInLeapYear(cx, calendar, yearMonth, args.rval());
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
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());
  Rooted<CalendarValue> calendarValue(cx, yearMonth->calendar());

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
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, calendarValue,
                                   {
                                       CalendarMethod::Fields,
                                       CalendarMethod::MergeFields,
                                       CalendarMethod::YearMonthFromFields,
                                   },
                                   &calendar)) {
    return false;
  }

  // Step 6.
  Rooted<PlainObject*> fields(cx);
  JS::RootedVector<PropertyKey> fieldNames(cx);
  if (!PrepareCalendarFieldsAndFieldNames(cx, calendar, yearMonth,
                                          {
                                              CalendarField::Month,
                                              CalendarField::MonthCode,
                                              CalendarField::Year,
                                          },
                                          &fields, &fieldNames)) {
    return false;
  }

  // Step 7.
  Rooted<PlainObject*> partialYearMonth(
      cx, PreparePartialTemporalFields(cx, temporalYearMonthLike, fieldNames));
  if (!partialYearMonth) {
    return false;
  }

  // Step 8.
  Rooted<JSObject*> mergedFields(
      cx, CalendarMergeFields(cx, calendar, fields, partialYearMonth));
  if (!mergedFields) {
    return false;
  }

  // Step 9.
  fields = PrepareTemporalFields(cx, mergedFields, fieldNames);
  if (!fields) {
    return false;
  }

  // Step 10.
  auto obj = temporal::CalendarYearMonthFromFields(cx, calendar, fields,
                                                   resolvedOptions);
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
  return AddDurationToOrSubtractDurationFromPlainYearMonth(
      cx, PlainYearMonthDuration::Add, args);
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
  return AddDurationToOrSubtractDurationFromPlainYearMonth(
      cx, PlainYearMonthDuration::Subtract, args);
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
  auto date = ToPlainDate(yearMonth);
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  PlainDate other;
  Rooted<CalendarValue> otherCalendar(cx);
  if (!ToTemporalYearMonth(cx, args.get(0), &other, &otherCalendar)) {
    return false;
  }

  // Steps 4-7.
  bool equals = date == other;
  if (equals && !CalendarEquals(cx, calendar, otherCalendar, &equals)) {
    return false;
  }

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
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());

  // Step 3.
  Rooted<JSObject*> item(
      cx, RequireObjectArg(cx, "item", "toPlainDate", args.get(0)));
  if (!item) {
    return false;
  }

  // Step 4.
  Rooted<CalendarValue> calendarValue(cx, yearMonth->calendar());
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

  // Step 5.
  Rooted<PlainObject*> receiverFields(cx);
  JS::RootedVector<PropertyKey> receiverFieldNames(cx);
  if (!PrepareCalendarFieldsAndFieldNames(cx, calendar, yearMonth,
                                          {
                                              CalendarField::MonthCode,
                                              CalendarField::Year,
                                          },
                                          &receiverFields,
                                          &receiverFieldNames)) {
    return false;
  }

  // Step 6.
  Rooted<PlainObject*> inputFields(cx);
  JS::RootedVector<PropertyKey> inputFieldNames(cx);
  if (!PrepareCalendarFieldsAndFieldNames(cx, calendar, item,
                                          {
                                              CalendarField::Day,
                                          },
                                          &inputFields, &inputFieldNames)) {
    return false;
  }

  // Step 7.
  Rooted<JSObject*> mergedFields(
      cx, CalendarMergeFields(cx, calendar, receiverFields, inputFields));
  if (!mergedFields) {
    return false;
  }

  // Step 8.
  JS::RootedVector<PropertyKey> concatenatedFieldNames(cx);
  if (!ConcatTemporalFieldNames(receiverFieldNames, inputFieldNames,
                                concatenatedFieldNames.get())) {
    return false;
  }

  // Step 9.
  Rooted<PlainObject*> mergedFromConcatenatedFields(
      cx, PrepareTemporalFields(cx, mergedFields, concatenatedFieldNames));
  if (!mergedFromConcatenatedFields) {
    return false;
  }

  // Step 10.
  auto obj = CalendarDateFromFields(cx, calendar, mergedFromConcatenatedFields);
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

/**
 * Temporal.PlainYearMonth.prototype.getISOFields ( )
 */
static bool PlainYearMonth_getISOFields(JSContext* cx, const CallArgs& args) {
  Rooted<PlainYearMonthObject*> yearMonth(
      cx, &args.thisv().toObject().as<PlainYearMonthObject>());
  auto calendar = yearMonth->calendar();

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
  if (!fields.emplaceBack(NameToId(cx->names().isoDay),
                          Int32Value(yearMonth->isoDay()))) {
    return false;
  }

  // Step 6.
  if (!fields.emplaceBack(NameToId(cx->names().isoMonth),
                          Int32Value(yearMonth->isoMonth()))) {
    return false;
  }

  // Step 7.
  if (!fields.emplaceBack(NameToId(cx->names().isoYear),
                          Int32Value(yearMonth->isoYear()))) {
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
 * Temporal.PlainYearMonth.prototype.getISOFields ( )
 */
static bool PlainYearMonth_getISOFields(JSContext* cx, unsigned argc,
                                        Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_getISOFields>(
      cx, args);
}

/**
 * Temporal.PlainYearMonth.prototype.getCalendar ( )
 */
static bool PlainYearMonth_getCalendar(JSContext* cx, const CallArgs& args) {
  auto* yearMonth = &args.thisv().toObject().as<PlainYearMonthObject>();
  Rooted<CalendarValue> calendar(cx, yearMonth->calendar());

  // Step 3.
  auto* obj = ToTemporalCalendarObject(cx, calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainYearMonth.prototype.getCalendar ( )
 */
static bool PlainYearMonth_getCalendar(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainYearMonth, PlainYearMonth_getCalendar>(
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
    JS_FN("getISOFields", PlainYearMonth_getISOFields, 0, 0),
    JS_FN("getCalendar", PlainYearMonth_getCalendar, 0, 0),
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
