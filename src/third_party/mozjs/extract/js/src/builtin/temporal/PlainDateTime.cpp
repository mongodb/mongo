/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/PlainDateTime.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/EnumSet.h"

#include <algorithm>
#include <utility>

#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/intl/DateTimeFormat.h"
#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/CalendarFields.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/PlainDate.h"
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

using namespace js;
using namespace js::temporal;

static inline bool IsPlainDateTime(Handle<Value> v) {
  return v.isObject() && v.toObject().is<PlainDateTimeObject>();
}

#ifdef DEBUG
/**
 * IsValidISODate ( year, month, day )
 * IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
bool js::temporal::IsValidISODateTime(const ISODateTime& isoDateTime) {
  return IsValidISODate(isoDateTime.date) && IsValidTime(isoDateTime.time);
}
#endif

/**
 * ISODateTimeWithinLimits ( isoDateTime )
 */
bool js::temporal::ISODateTimeWithinLimits(const ISODateTime& isoDateTime) {
  MOZ_ASSERT(IsValidISODateTime(isoDateTime));

  constexpr auto min = ISODate::min();
  constexpr auto max = ISODate::max();

  const auto& year = isoDateTime.date.year;

  // Fast-path when the input is definitely in range.
  if (min.year < year && year < max.year) {
    return true;
  }

  // Check |isoDateTime| is within the valid limits.
  if (year < 0) {
    if (isoDateTime.date != min) {
      return isoDateTime.date > min;
    }

    // Needs to be past midnight.
    return isoDateTime.time != Time{};
  }
  return isoDateTime.date <= max;
}

/**
 * CreateTemporalDateTime ( isoDateTime, calendar [ , newTarget ] )
 */
static PlainDateTimeObject* CreateTemporalDateTime(
    JSContext* cx, const CallArgs& args, const ISODateTime& isoDateTime,
    Handle<CalendarValue> calendar) {
  MOZ_ASSERT(IsValidISODateTime(isoDateTime));

  // Step 1.
  if (!ISODateTimeWithinLimits(isoDateTime)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
    return nullptr;
  }

  // Steps 2-3.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_PlainDateTime,
                                          &proto)) {
    return nullptr;
  }

  auto* object = NewObjectWithClassProto<PlainDateTimeObject>(cx, proto);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  auto packedDate = PackedDate::pack(isoDateTime.date);
  auto packedTime = PackedTime::pack(isoDateTime.time);
  object->setFixedSlot(PlainDateTimeObject::PACKED_DATE_SLOT,
                       PrivateUint32Value(packedDate.value));
  object->setFixedSlot(
      PlainDateTimeObject::PACKED_TIME_SLOT,
      DoubleValue(mozilla::BitwiseCast<double>(packedTime.value)));

  // Step 5.
  object->setFixedSlot(PlainDateTimeObject::CALENDAR_SLOT,
                       calendar.toSlotValue());

  // Step 6.
  return object;
}

/**
 * CreateTemporalDateTime ( isoDateTime, calendar [ , newTarget ] )
 */
PlainDateTimeObject* js::temporal::CreateTemporalDateTime(
    JSContext* cx, const ISODateTime& isoDateTime,
    Handle<CalendarValue> calendar) {
  MOZ_ASSERT(IsValidISODateTime(isoDateTime));

  // Step 1.
  if (!ISODateTimeWithinLimits(isoDateTime)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
    return nullptr;
  }

  // Steps 2-3.
  auto* object = NewBuiltinClassInstance<PlainDateTimeObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  auto packedDate = PackedDate::pack(isoDateTime.date);
  auto packedTime = PackedTime::pack(isoDateTime.time);
  object->setFixedSlot(PlainDateTimeObject::PACKED_DATE_SLOT,
                       PrivateUint32Value(packedDate.value));
  object->setFixedSlot(
      PlainDateTimeObject::PACKED_TIME_SLOT,
      DoubleValue(mozilla::BitwiseCast<double>(packedTime.value)));

  // Step 5.
  object->setFixedSlot(PlainDateTimeObject::CALENDAR_SLOT,
                       calendar.toSlotValue());

  // Step 6.
  return object;
}

/**
 * CreateTemporalDateTime ( isoDateTime, calendar [ , newTarget ] )
 */
static PlainDateTimeObject* CreateTemporalDateTime(
    JSContext* cx, Handle<PlainDateTime> dateTime) {
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));
  return CreateTemporalDateTime(cx, dateTime, dateTime.calendar());
}

/**
 * CreateTemporalDateTime ( isoDateTime, calendar [ , newTarget ] )
 */
static bool CreateTemporalDateTime(JSContext* cx, const ISODateTime& dateTime,
                                   Handle<CalendarValue> calendar,
                                   MutableHandle<PlainDateTime> result) {
  MOZ_ASSERT(IsValidISODateTime(dateTime));

  // Step 1.
  if (!ISODateTimeWithinLimits(dateTime)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
    return false;
  }

  // Step 2-6.
  result.set(PlainDateTime{dateTime, calendar});
  return true;
}

/**
 * InterpretTemporalDateTimeFields ( calendar, fields, overflow )
 */
bool js::temporal::InterpretTemporalDateTimeFields(
    JSContext* cx, Handle<CalendarValue> calendar,
    Handle<CalendarFields> fields, TemporalOverflow overflow,
    ISODateTime* result) {
  // Step 1.
  Rooted<PlainDate> temporalDate(cx);
  if (!CalendarDateFromFields(cx, calendar, fields, overflow, &temporalDate)) {
    return false;
  }

  // Step 2.
  auto timeLike = TemporalTimeLike{
      fields.hour(),        fields.minute(),      fields.second(),
      fields.millisecond(), fields.microsecond(), fields.nanosecond(),
  };
  Time time;
  if (!RegulateTime(cx, timeLike, overflow, &time)) {
    return false;
  }

  // Step 3.
  *result = {temporalDate.date(), time};
  return true;
}

struct DateTimeOptions {
  TemporalOverflow overflow = TemporalOverflow::Constrain;
};

/**
 * ToTemporalDateTime ( item [ , options ] )
 */
static bool ToTemporalDateTimeOptions(JSContext* cx, Handle<Value> options,
                                      DateTimeOptions* result) {
  if (options.isUndefined()) {
    *result = {};
    return true;
  }

  // NOTE: |options| are only passed from `Temporal.PlainDateTime.from`.

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
 * ToTemporalDateTime ( item [ , options ] )
 */
static bool ToTemporalDateTime(JSContext* cx, Handle<JSObject*> item,
                               Handle<Value> options,
                               MutableHandle<PlainDateTime> result) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.a.
  if (auto* plainDateTime = item->maybeUnwrapIf<PlainDateTimeObject>()) {
    auto dateTime = plainDateTime->dateTime();
    Rooted<CalendarValue> calendar(cx, plainDateTime->calendar());
    if (!calendar.wrap(cx)) {
      return false;
    }

    // Steps 2.a.i-ii.
    DateTimeOptions ignoredOptions;
    if (!ToTemporalDateTimeOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Step 2.a.iii.
    result.set(PlainDateTime{dateTime, calendar});
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

    // Step 2.b.i.
    ISODateTime dateTime;
    if (!GetISODateTimeFor(cx, timeZone, epochNs, &dateTime)) {
      return false;
    }

    // Steps 2.b.ii-iii.
    DateTimeOptions ignoredOptions;
    if (!ToTemporalDateTimeOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Step 2.b.iv.
    result.set(PlainDateTime{dateTime, calendar});
    return true;
  }

  // Step 2.c.
  if (auto* plainDate = item->maybeUnwrapIf<PlainDateObject>()) {
    auto date = plainDate->date();
    Rooted<CalendarValue> calendar(cx, plainDate->calendar());
    if (!calendar.wrap(cx)) {
      return false;
    }

    // Steps 2.c.i-ii.
    DateTimeOptions ignoredOptions;
    if (!ToTemporalDateTimeOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Steps 2.c.iii-iv.
    return CreateTemporalDateTime(cx, ISODateTime{date}, calendar, result);
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
                                 CalendarField::Hour,
                                 CalendarField::Minute,
                                 CalendarField::Second,
                                 CalendarField::Millisecond,
                                 CalendarField::Microsecond,
                                 CalendarField::Nanosecond,
                             },
                             &fields)) {
    return false;
  }

  // Steps 2.f-g.
  DateTimeOptions resolvedOptions;
  if (!ToTemporalDateTimeOptions(cx, options, &resolvedOptions)) {
    return false;
  }
  auto [overflow] = resolvedOptions;

  // Step 2.h.
  ISODateTime dateTime;
  if (!InterpretTemporalDateTimeFields(cx, calendar, fields, overflow,
                                       &dateTime)) {
    return false;
  }

  // Step 2.i.
  return CreateTemporalDateTime(cx, dateTime, calendar, result);
}

/**
 * ToTemporalDateTime ( item [ , options ] )
 */
static bool ToTemporalDateTime(JSContext* cx, Handle<Value> item,
                               Handle<Value> options,
                               MutableHandle<PlainDateTime> result) {
  // Step 1. (Not applicable)

  // Step 2.
  if (item.isObject()) {
    Rooted<JSObject*> itemObj(cx, &item.toObject());
    return ToTemporalDateTime(cx, itemObj, options, result);
  }

  // Step 3.
  if (!item.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, item,
                     nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> string(cx, item.toString());

  // Steps 4-5.
  ISODateTime dateTime;
  Rooted<JSString*> calendarString(cx);
  if (!ParseTemporalDateTimeString(cx, string, &dateTime, &calendarString)) {
    return false;
  }
  MOZ_ASSERT(IsValidISODateTime(dateTime));

  // Steps 6-8.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  if (calendarString) {
    if (!CanonicalizeCalendar(cx, calendarString, &calendar)) {
      return false;
    }
  }

  // Steps 9-10.
  DateTimeOptions ignoredOptions;
  if (!ToTemporalDateTimeOptions(cx, options, &ignoredOptions)) {
    return false;
  }

  // Steps 11-13.
  return CreateTemporalDateTime(cx, dateTime, calendar, result);
}

/**
 * ToTemporalDateTime ( item [ , options ] )
 */
static bool ToTemporalDateTime(JSContext* cx, Handle<Value> item,
                               MutableHandle<PlainDateTime> result) {
  return ToTemporalDateTime(cx, item, UndefinedHandleValue, result);
}

/**
 * CompareISODateTime ( isoDateTime1, isoDateTime2 )
 */
static int32_t CompareISODateTime(const ISODateTime& isoDateTime1,
                                  const ISODateTime& isoDateTime2) {
  // Steps 1-2.
  if (int32_t dateResult =
          CompareISODate(isoDateTime1.date, isoDateTime2.date)) {
    return dateResult;
  }

  // Steps 3.
  return CompareTimeRecord(isoDateTime1.time, isoDateTime2.time);
}

/**
 * Add24HourDaysToTimeDuration ( d, days )
 */
static TimeDuration Add24HourDaysToTimeDuration(const TimeDuration& d,
                                                int32_t days) {
  // Step 1.
  auto result = d + TimeDuration::fromDays(days);

  // Step 2.
  MOZ_ASSERT(result.abs() <= TimeDuration::max());

  // Step 3.
  return result;
}

/**
 * DifferenceISODateTime ( isoDateTime1, isoDateTime2, calendar, largestUnit )
 */
static bool DifferenceISODateTime(JSContext* cx,
                                  const ISODateTime& isoDateTime1,
                                  const ISODateTime& isoDateTime2,
                                  Handle<CalendarValue> calendar,
                                  TemporalUnit largestUnit,
                                  InternalDuration* result) {
  MOZ_ASSERT(isoDateTime1 != isoDateTime2,
             "fast-path for same date-time case handled in caller");

  // Steps 1-2.
  MOZ_ASSERT(IsValidISODateTime(isoDateTime1));
  MOZ_ASSERT(IsValidISODateTime(isoDateTime2));
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime1));
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime2));

  // Step 3.
  auto timeDuration = DifferenceTime(isoDateTime1.time, isoDateTime2.time);

  // Step 4.
  int32_t timeSign = TimeDurationSign(timeDuration);

  // Step 5.
  int32_t dateSign = CompareISODate(isoDateTime1.date, isoDateTime2.date);

  // Step 6.
  auto adjustedDate = isoDateTime2.date;

  // Step 7.
  if (timeSign == dateSign) {
    // Step 7.a.
    adjustedDate = BalanceISODate(adjustedDate, timeSign);

    // Step 7.b.
    timeDuration = Add24HourDaysToTimeDuration(timeDuration, -timeSign);
  }

  MOZ_ASSERT(IsValidISODate(adjustedDate));
  MOZ_ASSERT(ISODateWithinLimits(adjustedDate));

  // Step 8.
  auto dateLargestUnit = std::min(TemporalUnit::Day, largestUnit);

  // Step 9.
  DateDuration dateDifference;
  if (!CalendarDateUntil(cx, calendar, isoDateTime1.date, adjustedDate,
                         dateLargestUnit, &dateDifference)) {
    return false;
  }

  // Steps 10.
  if (largestUnit > TemporalUnit::Day) {
    // Step 10.a.
    auto days = mozilla::AssertedCast<int32_t>(dateDifference.days);
    timeDuration = Add24HourDaysToTimeDuration(timeDuration, days);

    // Step 10.b.
    dateDifference.days = 0;
  }

  // Step 11.
  MOZ_ASSERT(
      DateDurationSign(dateDifference) * TimeDurationSign(timeDuration) >= 0);
  *result = {dateDifference, timeDuration};
  return true;
}

/**
 * RoundISODateTime ( isoDateTime, increment, unit, roundingMode )
 */
ISODateTime js::temporal::RoundISODateTime(const ISODateTime& isoDateTime,
                                           Increment increment,
                                           TemporalUnit unit,
                                           TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidISODateTime(isoDateTime));

  // Step 1.
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime));

  // Step 2.
  auto roundedTime = RoundTime(isoDateTime.time, increment, unit, roundingMode);
  MOZ_ASSERT(0 <= roundedTime.days && roundedTime.days <= 1);

  // Step 3.
  auto balanceResult = BalanceISODate(isoDateTime.date, roundedTime.days);

  // Step 4.
  return {balanceResult, roundedTime.time};
}

/**
 * DifferencePlainDateTimeWithRounding ( isoDateTime1, isoDateTime2, calendar,
 * largestUnit, roundingIncrement, smallestUnit, roundingMode )
 */
bool js::temporal::DifferencePlainDateTimeWithRounding(
    JSContext* cx, const ISODateTime& isoDateTime1,
    const ISODateTime& isoDateTime2, Handle<CalendarValue> calendar,
    const DifferenceSettings& settings, InternalDuration* result) {
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime1));
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime2));

  // Step 1.
  if (isoDateTime1 == isoDateTime2) {
    // Step 1.a.
    *result = {};
    return true;
  }

  // Step 2. (Not applicable in our implementation.)

  // Step 3.
  InternalDuration diff;
  if (!DifferenceISODateTime(cx, isoDateTime1, isoDateTime2, calendar,
                             settings.largestUnit, &diff)) {
    return false;
  }

  // Step 4.
  if (settings.smallestUnit == TemporalUnit::Nanosecond &&
      settings.roundingIncrement == Increment{1}) {
    *result = diff;
    return true;
  }

  // Step 5.
  auto destEpochNs = GetUTCEpochNanoseconds(isoDateTime2);

  // Step 6.
  Rooted<TimeZoneValue> timeZone(cx, TimeZoneValue{});
  return RoundRelativeDuration(
      cx, diff, destEpochNs, isoDateTime1, timeZone, calendar,
      settings.largestUnit, settings.roundingIncrement, settings.smallestUnit,
      settings.roundingMode, result);
}

/**
 * DifferencePlainDateTimeWithTotal ( isoDateTime1, isoDateTime2, calendar, unit
 * )
 */
bool js::temporal::DifferencePlainDateTimeWithTotal(
    JSContext* cx, const ISODateTime& isoDateTime1,
    const ISODateTime& isoDateTime2, Handle<CalendarValue> calendar,
    TemporalUnit unit, double* result) {
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime1));
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime2));

  // Step 1.
  if (isoDateTime1 == isoDateTime2) {
    // Step 1.a.
    *result = 0;
    return true;
  }

  // Step 2. (Not applicable in our implementation.)

  // Step 3.
  InternalDuration diff;
  if (!DifferenceISODateTime(cx, isoDateTime1, isoDateTime2, calendar, unit,
                             &diff)) {
    return false;
  }

  // Step 4. (Optimized to avoid GetUTCEpochNanoseconds for non-calendar units.)
  if (unit > TemporalUnit::Day) {
    MOZ_ASSERT(diff.date == DateDuration{});

    // TotalRelativeDuration, steps 1-2. (Not applicable)

    // TotalRelativeDuration, step 3.
    *result = TotalTimeDuration(diff.time, unit);
    return true;
  } else if (unit == TemporalUnit::Day) {
    // TotalRelativeDuration, step 1. (Not applicable)

    // TotalRelativeDuration, step 2.
    auto days = mozilla::AssertedCast<int32_t>(diff.date.days);
    auto timeDuration = Add24HourDaysToTimeDuration(diff.time, days);

    // TotalRelativeDuration, step 3.
    *result = TotalTimeDuration(timeDuration, unit);
    return true;
  }

  // Step 5.
  auto destEpochNs = GetUTCEpochNanoseconds(isoDateTime2);

  // Step 6.
  Rooted<TimeZoneValue> timeZone(cx, TimeZoneValue{});
  return TotalRelativeDuration(cx, diff, destEpochNs, isoDateTime1, timeZone,
                               calendar, unit, result);
}

/**
 * DifferenceTemporalPlainDateTime ( operation, dateTime, other, options )
 */
static bool DifferenceTemporalPlainDateTime(JSContext* cx,
                                            TemporalDifference operation,
                                            const CallArgs& args) {
  Rooted<PlainDateTime> dateTime(
      cx, &args.thisv().toObject().as<PlainDateTimeObject>());

  // Step 1.
  Rooted<PlainDateTime> other(cx);
  if (!ToTemporalDateTime(cx, args.get(0), &other)) {
    return false;
  }

  // Step 2.
  if (!CalendarEquals(dateTime.calendar(), other.calendar())) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE,
                              CalendarIdentifier(dateTime.calendar()).data(),
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
    if (!GetDifferenceSettings(
            cx, operation, options, TemporalUnitGroup::DateTime,
            TemporalUnit::Nanosecond, TemporalUnit::Day, &settings)) {
      return false;
    }
  } else {
    // Steps 3-4.
    settings = {
        TemporalUnit::Nanosecond,
        TemporalUnit::Day,
        TemporalRoundingMode::Trunc,
        Increment{1},
    };
  }

  // Step 5.
  if (dateTime.dateTime() == other.dateTime()) {
    auto* obj = CreateTemporalDuration(cx, {});
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Step 6.
  InternalDuration internalDuration;
  if (!DifferencePlainDateTimeWithRounding(cx, dateTime, other,
                                           dateTime.calendar(), settings,
                                           &internalDuration)) {
    return false;
  }
  MOZ_ASSERT(IsValidDuration(internalDuration));

  // Step 7.
  Duration result;
  if (!TemporalDurationFromInternal(cx, internalDuration, settings.largestUnit,
                                    &result)) {
    return false;
  }
  MOZ_ASSERT(IsValidDuration(result));

  // Step 8.
  if (operation == TemporalDifference::Since) {
    result = result.negate();
  }

  // Step 9.
  auto* obj = CreateTemporalDuration(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * AddDurationToDateTime ( operation, dateTime, temporalDurationLike, options )
 */
static bool AddDurationToDateTime(JSContext* cx, TemporalAddDuration operation,
                                  const CallArgs& args) {
  Rooted<PlainDateTime> dateTime(
      cx, &args.thisv().toObject().as<PlainDateTimeObject>());

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
  auto internalDuration = ToInternalDurationRecordWith24HourDays(duration);
  MOZ_ASSERT(IsValidDuration(internalDuration));

  // Step 6.
  auto timeResult = AddTime(dateTime.time(), internalDuration.time);

  // Step 7. (Inlined AdjustDateDurationRecord)
  if (std::abs(timeResult.days) > TimeDuration::max().toDays()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_INVALID_NORMALIZED_TIME);
    return false;
  }
  auto dateDuration = DateDuration{
      internalDuration.date.years,
      internalDuration.date.months,
      internalDuration.date.weeks,
      timeResult.days,
  };
  MOZ_ASSERT(IsValidDuration(dateDuration));

  // Step 8.
  ISODate addedDate;
  if (!CalendarDateAdd(cx, dateTime.calendar(), dateTime.date(), dateDuration,
                       overflow, &addedDate)) {
    return false;
  }

  // Step 9.
  auto result = ISODateTime{addedDate, timeResult.time};
  MOZ_ASSERT(IsValidISODateTime(result));

  // Step 10.
  auto* obj = CreateTemporalDateTime(cx, result, dateTime.calendar());
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDateTime ( isoYear, isoMonth, isoDay [ , hour [ , minute [ ,
 * second [ , millisecond [ , microsecond [ , nanosecond [ , calendar ] ] ] ] ]
 * ] ] )
 */
static bool PlainDateTimeConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Temporal.PlainDateTime")) {
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
  double hour = 0;
  if (args.hasDefined(3)) {
    if (!ToIntegerWithTruncation(cx, args[3], "hour", &hour)) {
      return false;
    }
  }

  // Step 6.
  double minute = 0;
  if (args.hasDefined(4)) {
    if (!ToIntegerWithTruncation(cx, args[4], "minute", &minute)) {
      return false;
    }
  }

  // Step 7.
  double second = 0;
  if (args.hasDefined(5)) {
    if (!ToIntegerWithTruncation(cx, args[5], "second", &second)) {
      return false;
    }
  }

  // Step 8.
  double millisecond = 0;
  if (args.hasDefined(6)) {
    if (!ToIntegerWithTruncation(cx, args[6], "millisecond", &millisecond)) {
      return false;
    }
  }

  // Step 9.
  double microsecond = 0;
  if (args.hasDefined(7)) {
    if (!ToIntegerWithTruncation(cx, args[7], "microsecond", &microsecond)) {
      return false;
    }
  }

  // Step 10.
  double nanosecond = 0;
  if (args.hasDefined(8)) {
    if (!ToIntegerWithTruncation(cx, args[8], "nanosecond", &nanosecond)) {
      return false;
    }
  }

  // Steps 11-13.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  if (args.hasDefined(9)) {
    // Step 12.
    if (!args[9].isString()) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, args[9],
                       nullptr, "not a string");
      return false;
    }

    // Step 13.
    Rooted<JSString*> calendarString(cx, args[9].toString());
    if (!CanonicalizeCalendar(cx, calendarString, &calendar)) {
      return false;
    }
  }

  // Step 14.
  if (!ThrowIfInvalidISODate(cx, isoYear, isoMonth, isoDay)) {
    return false;
  }

  // Step 15.
  auto isoDate = ISODate{int32_t(isoYear), int32_t(isoMonth), int32_t(isoDay)};

  // Step 16.
  if (!ThrowIfInvalidTime(cx, hour, minute, second, millisecond, microsecond,
                          nanosecond)) {
    return false;
  }

  // Step 17.
  auto time =
      Time{int32_t(hour),        int32_t(minute),      int32_t(second),
           int32_t(millisecond), int32_t(microsecond), int32_t(nanosecond)};

  // Step 18.
  auto isoDateTime = ISODateTime{isoDate, time};

  // Step 19.
  auto* temporalDateTime =
      CreateTemporalDateTime(cx, args, isoDateTime, calendar);
  if (!temporalDateTime) {
    return false;
  }

  args.rval().setObject(*temporalDateTime);
  return true;
}

/**
 * Temporal.PlainDateTime.from ( item [ , options ] )
 */
static bool PlainDateTime_from(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<PlainDateTime> dateTime(cx);
  if (!ToTemporalDateTime(cx, args.get(0), args.get(1), &dateTime)) {
    return false;
  }

  auto* result = CreateTemporalDateTime(cx, dateTime);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainDateTime.compare ( one, two )
 */
static bool PlainDateTime_compare(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<PlainDateTime> one(cx);
  if (!ToTemporalDateTime(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  Rooted<PlainDateTime> two(cx);
  if (!ToTemporalDateTime(cx, args.get(1), &two)) {
    return false;
  }

  // Step 3.
  args.rval().setInt32(CompareISODateTime(one, two));
  return true;
}

/**
 * get Temporal.PlainDateTime.prototype.calendarId
 */
static bool PlainDateTime_calendarId(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();

  // Step 3.
  auto* str =
      NewStringCopy<CanGC>(cx, CalendarIdentifier(dateTime->calendar()));
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * get Temporal.PlainDateTime.prototype.calendarId
 */
static bool PlainDateTime_calendarId(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_calendarId>(cx,
                                                                         args);
}

/**
 * get Temporal.PlainDateTime.prototype.era
 */
static bool PlainDateTime_era(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarEra(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.era
 */
static bool PlainDateTime_era(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_era>(cx, args);
}

/**
 * get Temporal.PlainDateTime.prototype.eraYear
 */
static bool PlainDateTime_eraYear(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Steps 3-5.
  return CalendarEraYear(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.eraYear
 */
static bool PlainDateTime_eraYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_eraYear>(cx, args);
}

/**
 * get Temporal.PlainDateTime.prototype.year
 */
static bool PlainDateTime_year(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarYear(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.year
 */
static bool PlainDateTime_year(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_year>(cx, args);
}

/**
 * get Temporal.PlainDateTime.prototype.month
 */
static bool PlainDateTime_month(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarMonth(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.month
 */
static bool PlainDateTime_month(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_month>(cx, args);
}

/**
 * get Temporal.PlainDateTime.prototype.monthCode
 */
static bool PlainDateTime_monthCode(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarMonthCode(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.monthCode
 */
static bool PlainDateTime_monthCode(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_monthCode>(cx,
                                                                        args);
}

/**
 * get Temporal.PlainDateTime.prototype.day
 */
static bool PlainDateTime_day(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarDay(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.day
 */
static bool PlainDateTime_day(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_day>(cx, args);
}

/**
 * get Temporal.PlainDateTime.prototype.hour
 */
static bool PlainDateTime_hour(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  args.rval().setInt32(dateTime->time().hour);
  return true;
}

/**
 * get Temporal.PlainDateTime.prototype.hour
 */
static bool PlainDateTime_hour(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_hour>(cx, args);
}

/**
 * get Temporal.PlainDateTime.prototype.minute
 */
static bool PlainDateTime_minute(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  args.rval().setInt32(dateTime->time().minute);
  return true;
}

/**
 * get Temporal.PlainDateTime.prototype.minute
 */
static bool PlainDateTime_minute(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_minute>(cx, args);
}

/**
 * get Temporal.PlainDateTime.prototype.second
 */
static bool PlainDateTime_second(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  args.rval().setInt32(dateTime->time().second);
  return true;
}

/**
 * get Temporal.PlainDateTime.prototype.second
 */
static bool PlainDateTime_second(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_second>(cx, args);
}

/**
 * get Temporal.PlainDateTime.prototype.millisecond
 */
static bool PlainDateTime_millisecond(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  args.rval().setInt32(dateTime->time().millisecond);
  return true;
}

/**
 * get Temporal.PlainDateTime.prototype.millisecond
 */
static bool PlainDateTime_millisecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_millisecond>(cx,
                                                                          args);
}

/**
 * get Temporal.PlainDateTime.prototype.microsecond
 */
static bool PlainDateTime_microsecond(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  args.rval().setInt32(dateTime->time().microsecond);
  return true;
}

/**
 * get Temporal.PlainDateTime.prototype.microsecond
 */
static bool PlainDateTime_microsecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_microsecond>(cx,
                                                                          args);
}

/**
 * get Temporal.PlainDateTime.prototype.nanosecond
 */
static bool PlainDateTime_nanosecond(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  args.rval().setInt32(dateTime->time().nanosecond);
  return true;
}

/**
 * get Temporal.PlainDateTime.prototype.nanosecond
 */
static bool PlainDateTime_nanosecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_nanosecond>(cx,
                                                                         args);
}

/**
 * get Temporal.PlainDateTime.prototype.dayOfWeek
 */
static bool PlainDateTime_dayOfWeek(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarDayOfWeek(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.dayOfWeek
 */
static bool PlainDateTime_dayOfWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_dayOfWeek>(cx,
                                                                        args);
}

/**
 * get Temporal.PlainDateTime.prototype.dayOfYear
 */
static bool PlainDateTime_dayOfYear(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarDayOfYear(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.dayOfYear
 */
static bool PlainDateTime_dayOfYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_dayOfYear>(cx,
                                                                        args);
}

/**
 * get Temporal.PlainDateTime.prototype.weekOfYear
 */
static bool PlainDateTime_weekOfYear(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Steps 3-5.
  return CalendarWeekOfYear(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.weekOfYear
 */
static bool PlainDateTime_weekOfYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_weekOfYear>(cx,
                                                                         args);
}

/**
 * get Temporal.PlainDateTime.prototype.yearOfWeek
 */
static bool PlainDateTime_yearOfWeek(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Steps 3-5.
  return CalendarYearOfWeek(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.yearOfWeek
 */
static bool PlainDateTime_yearOfWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_yearOfWeek>(cx,
                                                                         args);
}

/**
 * get Temporal.PlainDateTime.prototype.daysInWeek
 */
static bool PlainDateTime_daysInWeek(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarDaysInWeek(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.daysInWeek
 */
static bool PlainDateTime_daysInWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_daysInWeek>(cx,
                                                                         args);
}

/**
 * get Temporal.PlainDateTime.prototype.daysInMonth
 */
static bool PlainDateTime_daysInMonth(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarDaysInMonth(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.daysInMonth
 */
static bool PlainDateTime_daysInMonth(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_daysInMonth>(cx,
                                                                          args);
}

/**
 * get Temporal.PlainDateTime.prototype.daysInYear
 */
static bool PlainDateTime_daysInYear(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarDaysInYear(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.daysInYear
 */
static bool PlainDateTime_daysInYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_daysInYear>(cx,
                                                                         args);
}

/**
 * get Temporal.PlainDateTime.prototype.monthsInYear
 */
static bool PlainDateTime_monthsInYear(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarMonthsInYear(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.monthsInYear
 */
static bool PlainDateTime_monthsInYear(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_monthsInYear>(
      cx, args);
}

/**
 * get Temporal.PlainDateTime.prototype.inLeapYear
 */
static bool PlainDateTime_inLeapYear(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  return CalendarInLeapYear(cx, calendar, dateTime->date(), args.rval());
}

/**
 * get Temporal.PlainDateTime.prototype.inLeapYear
 */
static bool PlainDateTime_inLeapYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_inLeapYear>(cx,
                                                                         args);
}

/**
 * Temporal.PlainDateTime.prototype.with ( temporalDateTimeLike [ , options ] )
 */
static bool PlainDateTime_with(JSContext* cx, const CallArgs& args) {
  Rooted<PlainDateTime> dateTime(
      cx, &args.thisv().toObject().as<PlainDateTimeObject>());

  // Step 3.
  Rooted<JSObject*> temporalDateTimeLike(
      cx, RequireObjectArg(cx, "temporalDateTimeLike", "with", args.get(0)));
  if (!temporalDateTimeLike) {
    return false;
  }
  if (!ThrowIfTemporalLikeObject(cx, temporalDateTimeLike)) {
    return false;
  }

  // Step 4.
  auto calendar = dateTime.calendar();

  // Step 5.
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, dateTime, &fields)) {
    return false;
  }

  // Steps 6-11.
  fields.setHour(dateTime.time().hour);
  fields.setMinute(dateTime.time().minute);
  fields.setSecond(dateTime.time().second);
  fields.setMillisecond(dateTime.time().millisecond);
  fields.setMicrosecond(dateTime.time().microsecond);
  fields.setNanosecond(dateTime.time().nanosecond);

  // Step 12.
  Rooted<CalendarFields> partialDateTime(cx);
  if (!PreparePartialCalendarFields(cx, calendar, temporalDateTimeLike,
                                    {
                                        CalendarField::Year,
                                        CalendarField::Month,
                                        CalendarField::MonthCode,
                                        CalendarField::Day,
                                        CalendarField::Hour,
                                        CalendarField::Minute,
                                        CalendarField::Second,
                                        CalendarField::Millisecond,
                                        CalendarField::Microsecond,
                                        CalendarField::Nanosecond,
                                    },
                                    &partialDateTime)) {
    return false;
  }
  MOZ_ASSERT(!partialDateTime.keys().isEmpty());

  // Step 13.
  fields = CalendarMergeFields(calendar, fields, partialDateTime);

  // Steps 14-15.
  auto overflow = TemporalOverflow::Constrain;
  if (args.hasDefined(1)) {
    // Step 14.
    Rooted<JSObject*> options(cx,
                              RequireObjectArg(cx, "options", "with", args[1]));
    if (!options) {
      return false;
    }

    // Step 15.
    if (!GetTemporalOverflowOption(cx, options, &overflow)) {
      return false;
    }
  }

  // Step 16.
  ISODateTime result;
  if (!InterpretTemporalDateTimeFields(cx, calendar, fields, overflow,
                                       &result)) {
    return false;
  }
  MOZ_ASSERT(IsValidISODateTime(result));

  // Step 21.
  auto* obj = CreateTemporalDateTime(cx, result, calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDateTime.prototype.with ( temporalDateTimeLike [ , options ] )
 */
static bool PlainDateTime_with(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_with>(cx, args);
}

/**
 * Temporal.PlainDateTime.prototype.withPlainTime ( [ plainTimeLike ] )
 */
static bool PlainDateTime_withPlainTime(JSContext* cx, const CallArgs& args) {
  auto* temporalDateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  auto date = temporalDateTime->date();
  Rooted<CalendarValue> calendar(cx, temporalDateTime->calendar());

  // Step 3. (Inlined ToTemporalTimeOrMidnight)
  Time time = {};
  if (args.hasDefined(0)) {
    if (!ToTemporalTime(cx, args[0], &time)) {
      return false;
    }
  }

  // Step 4.
  auto isoDateTime = ISODateTime{date, time};

  // Step 5.
  auto* obj = CreateTemporalDateTime(cx, isoDateTime, calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDateTime.prototype.withPlainTime ( [ plainTimeLike ] )
 */
static bool PlainDateTime_withPlainTime(JSContext* cx, unsigned argc,
                                        Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_withPlainTime>(
      cx, args);
}

/**
 * Temporal.PlainDateTime.prototype.withCalendar ( calendar )
 */
static bool PlainDateTime_withCalendar(JSContext* cx, const CallArgs& args) {
  auto* temporalDateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  auto dateTime = temporalDateTime->dateTime();

  // Step 3.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendar(cx, args.get(0), &calendar)) {
    return false;
  }

  // Step 4.
  auto* result = CreateTemporalDateTime(cx, dateTime, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainDateTime.prototype.withCalendar ( calendar )
 */
static bool PlainDateTime_withCalendar(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_withCalendar>(
      cx, args);
}

/**
 * Temporal.PlainDateTime.prototype.add ( temporalDurationLike [ , options ] )
 */
static bool PlainDateTime_add(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurationToDateTime(cx, TemporalAddDuration::Add, args);
}

/**
 * Temporal.PlainDateTime.prototype.add ( temporalDurationLike [ , options ] )
 */
static bool PlainDateTime_add(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_add>(cx, args);
}

/**
 * Temporal.PlainDateTime.prototype.subtract ( temporalDurationLike [ , options
 * ] )
 */
static bool PlainDateTime_subtract(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurationToDateTime(cx, TemporalAddDuration::Subtract, args);
}

/**
 * Temporal.PlainDateTime.prototype.subtract ( temporalDurationLike [ , options
 * ] )
 */
static bool PlainDateTime_subtract(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_subtract>(cx,
                                                                       args);
}

/**
 * Temporal.PlainDateTime.prototype.until ( other [ , options ] )
 */
static bool PlainDateTime_until(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalPlainDateTime(cx, TemporalDifference::Until, args);
}

/**
 * Temporal.PlainDateTime.prototype.until ( other [ , options ] )
 */
static bool PlainDateTime_until(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_until>(cx, args);
}

/**
 * Temporal.PlainDateTime.prototype.since ( other [ , options ] )
 */
static bool PlainDateTime_since(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalPlainDateTime(cx, TemporalDifference::Since, args);
}

/**
 * Temporal.PlainDateTime.prototype.since ( other [ , options ] )
 */
static bool PlainDateTime_since(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_since>(cx, args);
}

/**
 * Temporal.PlainDateTime.prototype.round ( roundTo )
 */
static bool PlainDateTime_round(JSContext* cx, const CallArgs& args) {
  auto* temporalDateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  auto dateTime = temporalDateTime->dateTime();
  Rooted<CalendarValue> calendar(cx, temporalDateTime->calendar());

  // Steps 3-12.
  auto smallestUnit = TemporalUnit::Auto;
  auto roundingMode = TemporalRoundingMode::HalfExpand;
  auto roundingIncrement = Increment{1};
  if (args.get(0).isString()) {
    // Step 4. (Not applicable in our implementation.)

    // Step 9.
    Rooted<JSString*> paramString(cx, args[0].toString());
    if (!GetTemporalUnitValuedOption(
            cx, paramString, TemporalUnitKey::SmallestUnit,
            TemporalUnitGroup::DayTime, &smallestUnit)) {
      return false;
    }

    MOZ_ASSERT(TemporalUnit::Day <= smallestUnit &&
               smallestUnit <= TemporalUnit::Nanosecond);

    // Steps 6-8 and 10-12. (Implicit)
  } else {
    // Steps 3 and 5.
    Rooted<JSObject*> roundTo(
        cx, RequireObjectArg(cx, "roundTo", "round", args.get(0)));
    if (!roundTo) {
      return false;
    }

    // Steps 6-7.
    if (!GetRoundingIncrementOption(cx, roundTo, &roundingIncrement)) {
      return false;
    }

    // Step 8.
    if (!GetRoundingModeOption(cx, roundTo, &roundingMode)) {
      return false;
    }

    // Step 9.
    if (!GetTemporalUnitValuedOption(cx, roundTo, TemporalUnitKey::SmallestUnit,
                                     TemporalUnitGroup::DayTime,
                                     &smallestUnit)) {
      return false;
    }

    if (smallestUnit == TemporalUnit::Auto) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_MISSING_OPTION, "smallestUnit");
      return false;
    }

    MOZ_ASSERT(TemporalUnit::Day <= smallestUnit &&
               smallestUnit <= TemporalUnit::Nanosecond);

    // Steps 10-11.
    auto maximum = Increment{1};
    bool inclusive = true;
    if (smallestUnit > TemporalUnit::Day) {
      maximum = MaximumTemporalDurationRoundingIncrement(smallestUnit);
      inclusive = false;
    }

    // Step 12.
    if (!ValidateTemporalRoundingIncrement(cx, roundingIncrement, maximum,
                                           inclusive)) {
      return false;
    }
  }

  // Step 13.
  if (smallestUnit == TemporalUnit::Nanosecond &&
      roundingIncrement == Increment{1}) {
    auto* obj = CreateTemporalDateTime(cx, dateTime, calendar);
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Step 14.
  auto result =
      RoundISODateTime(dateTime, roundingIncrement, smallestUnit, roundingMode);

  // Step 15.
  auto* obj = CreateTemporalDateTime(cx, result, calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDateTime.prototype.round ( roundTo )
 */
static bool PlainDateTime_round(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_round>(cx, args);
}

/**
 * Temporal.PlainDateTime.prototype.equals ( other )
 */
static bool PlainDateTime_equals(JSContext* cx, const CallArgs& args) {
  auto* temporalDateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  auto dateTime = temporalDateTime->dateTime();
  Rooted<CalendarValue> calendar(cx, temporalDateTime->calendar());

  // Step 3.
  Rooted<PlainDateTime> other(cx);
  if (!ToTemporalDateTime(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 4-5.
  bool equals = dateTime == other.dateTime() &&
                CalendarEquals(calendar, other.calendar());

  args.rval().setBoolean(equals);
  return true;
}

/**
 * Temporal.PlainDateTime.prototype.equals ( other )
 */
static bool PlainDateTime_equals(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_equals>(cx, args);
}

/**
 * Temporal.PlainDateTime.prototype.toString ( [ options ] )
 */
static bool PlainDateTime_toString(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  auto dt = dateTime->dateTime();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  SecondsStringPrecision precision = {Precision::Auto(),
                                      TemporalUnit::Nanosecond, Increment{1}};
  auto roundingMode = TemporalRoundingMode::Trunc;
  auto showCalendar = ShowCalendar::Auto;
  if (args.hasDefined(0)) {
    // Step 3.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", "toString", args[0]));
    if (!options) {
      return false;
    }

    // Steps 4-5.
    if (!GetTemporalShowCalendarNameOption(cx, options, &showCalendar)) {
      return false;
    }

    // Step 6.
    auto digits = Precision::Auto();
    if (!GetTemporalFractionalSecondDigitsOption(cx, options, &digits)) {
      return false;
    }

    // Step 7.
    if (!GetRoundingModeOption(cx, options, &roundingMode)) {
      return false;
    }

    // Step 8.
    auto smallestUnit = TemporalUnit::Auto;
    if (!GetTemporalUnitValuedOption(cx, options, TemporalUnitKey::SmallestUnit,
                                     TemporalUnitGroup::Time, &smallestUnit)) {
      return false;
    }

    // Step 9.
    if (smallestUnit == TemporalUnit::Hour) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INVALID_UNIT_OPTION, "hour",
                                "smallestUnit");
      return false;
    }

    // Step 10.
    precision = ToSecondsStringPrecision(smallestUnit, digits);
  }

  // Step 11.
  auto result =
      RoundISODateTime(dt, precision.increment, precision.unit, roundingMode);

  // Step 12.
  if (!ISODateTimeWithinLimits(result)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
    return false;
  }

  // Step 13.
  JSString* str = ISODateTimeToString(cx, result, calendar, precision.precision,
                                      showCalendar);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.PlainDateTime.prototype.toString ( [ options ] )
 */
static bool PlainDateTime_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_toString>(cx,
                                                                       args);
}

/**
 * Temporal.PlainDateTime.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool PlainDateTime_toLocaleString(JSContext* cx, const CallArgs& args) {
  // Steps 3-4.
  Handle<PropertyName*> required = cx->names().any;
  Handle<PropertyName*> defaults = cx->names().all;
  return TemporalObjectToLocaleString(cx, args, required, defaults);
}

/**
 * Temporal.PlainDateTime.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool PlainDateTime_toLocaleString(JSContext* cx, unsigned argc,
                                         Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_toLocaleString>(
      cx, args);
}

/**
 * Temporal.PlainDateTime.prototype.toJSON ( )
 */
static bool PlainDateTime_toJSON(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  auto dt = dateTime->dateTime();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  JSString* str = ISODateTimeToString(cx, dt, calendar, Precision::Auto(),
                                      ShowCalendar::Auto);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.PlainDateTime.prototype.toJSON ( )
 */
static bool PlainDateTime_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_toJSON>(cx, args);
}

/**
 * Temporal.PlainDateTime.prototype.valueOf ( )
 */
static bool PlainDateTime_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                            "PlainDateTime", "primitive type");
  return false;
}

/**
 * Temporal.PlainDateTime.prototype.toZonedDateTime ( temporalTimeZoneLike [ ,
 * options ] )
 */
static bool PlainDateTime_toZonedDateTime(JSContext* cx, const CallArgs& args) {
  auto* temporalDateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  auto isoDateTime = temporalDateTime->dateTime();
  Rooted<CalendarValue> calendar(cx, temporalDateTime->calendar());

  // Step 3.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZone(cx, args.get(0), &timeZone)) {
    return false;
  }

  auto disambiguation = TemporalDisambiguation::Compatible;
  if (args.hasDefined(1)) {
    // Step 4.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", "toZonedDateTime", args[1]));
    if (!options) {
      return false;
    }

    // Step 5.
    if (!GetTemporalDisambiguationOption(cx, options, &disambiguation)) {
      return false;
    }
  }

  // Step 6.
  EpochNanoseconds epochNs;
  if (!GetEpochNanosecondsFor(cx, timeZone, isoDateTime, disambiguation,
                              &epochNs)) {
    return false;
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
 * Temporal.PlainDateTime.prototype.toZonedDateTime ( temporalTimeZoneLike [ ,
 * options ] )
 */
static bool PlainDateTime_toZonedDateTime(JSContext* cx, unsigned argc,
                                          Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_toZonedDateTime>(
      cx, args);
}

/**
 * Temporal.PlainDateTime.prototype.toPlainDate ( )
 */
static bool PlainDateTime_toPlainDate(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, dateTime->calendar());

  // Step 3.
  auto* obj = CreateTemporalDate(cx, dateTime->date(), calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDateTime.prototype.toPlainDate ( )
 */
static bool PlainDateTime_toPlainDate(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_toPlainDate>(cx,
                                                                          args);
}

/**
 * Temporal.PlainDateTime.prototype.toPlainTime ( )
 */
static bool PlainDateTime_toPlainTime(JSContext* cx, const CallArgs& args) {
  auto* dateTime = &args.thisv().toObject().as<PlainDateTimeObject>();

  // Step 3.
  auto* obj = CreateTemporalTime(cx, dateTime->time());
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainDateTime.prototype.toPlainTime ( )
 */
static bool PlainDateTime_toPlainTime(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainDateTime, PlainDateTime_toPlainTime>(cx,
                                                                          args);
}

const JSClass PlainDateTimeObject::class_ = {
    "Temporal.PlainDateTime",
    JSCLASS_HAS_RESERVED_SLOTS(PlainDateTimeObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_PlainDateTime),
    JS_NULL_CLASS_OPS,
    &PlainDateTimeObject::classSpec_,
};

const JSClass& PlainDateTimeObject::protoClass_ = PlainObject::class_;

static const JSFunctionSpec PlainDateTime_methods[] = {
    JS_FN("from", PlainDateTime_from, 1, 0),
    JS_FN("compare", PlainDateTime_compare, 2, 0),
    JS_FS_END,
};

static const JSFunctionSpec PlainDateTime_prototype_methods[] = {
    JS_FN("with", PlainDateTime_with, 1, 0),
    JS_FN("withPlainTime", PlainDateTime_withPlainTime, 0, 0),
    JS_FN("withCalendar", PlainDateTime_withCalendar, 1, 0),
    JS_FN("add", PlainDateTime_add, 1, 0),
    JS_FN("subtract", PlainDateTime_subtract, 1, 0),
    JS_FN("until", PlainDateTime_until, 1, 0),
    JS_FN("since", PlainDateTime_since, 1, 0),
    JS_FN("round", PlainDateTime_round, 1, 0),
    JS_FN("equals", PlainDateTime_equals, 1, 0),
    JS_FN("toString", PlainDateTime_toString, 0, 0),
    JS_FN("toLocaleString", PlainDateTime_toLocaleString, 0, 0),
    JS_FN("toJSON", PlainDateTime_toJSON, 0, 0),
    JS_FN("valueOf", PlainDateTime_valueOf, 0, 0),
    JS_FN("toZonedDateTime", PlainDateTime_toZonedDateTime, 1, 0),
    JS_FN("toPlainDate", PlainDateTime_toPlainDate, 0, 0),
    JS_FN("toPlainTime", PlainDateTime_toPlainTime, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec PlainDateTime_prototype_properties[] = {
    JS_PSG("calendarId", PlainDateTime_calendarId, 0),
    JS_PSG("era", PlainDateTime_era, 0),
    JS_PSG("eraYear", PlainDateTime_eraYear, 0),
    JS_PSG("year", PlainDateTime_year, 0),
    JS_PSG("month", PlainDateTime_month, 0),
    JS_PSG("monthCode", PlainDateTime_monthCode, 0),
    JS_PSG("day", PlainDateTime_day, 0),
    JS_PSG("hour", PlainDateTime_hour, 0),
    JS_PSG("minute", PlainDateTime_minute, 0),
    JS_PSG("second", PlainDateTime_second, 0),
    JS_PSG("millisecond", PlainDateTime_millisecond, 0),
    JS_PSG("microsecond", PlainDateTime_microsecond, 0),
    JS_PSG("nanosecond", PlainDateTime_nanosecond, 0),
    JS_PSG("dayOfWeek", PlainDateTime_dayOfWeek, 0),
    JS_PSG("dayOfYear", PlainDateTime_dayOfYear, 0),
    JS_PSG("weekOfYear", PlainDateTime_weekOfYear, 0),
    JS_PSG("yearOfWeek", PlainDateTime_yearOfWeek, 0),
    JS_PSG("daysInWeek", PlainDateTime_daysInWeek, 0),
    JS_PSG("daysInMonth", PlainDateTime_daysInMonth, 0),
    JS_PSG("daysInYear", PlainDateTime_daysInYear, 0),
    JS_PSG("monthsInYear", PlainDateTime_monthsInYear, 0),
    JS_PSG("inLeapYear", PlainDateTime_inLeapYear, 0),
    JS_STRING_SYM_PS(toStringTag, "Temporal.PlainDateTime", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec PlainDateTimeObject::classSpec_ = {
    GenericCreateConstructor<PlainDateTimeConstructor, 3,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<PlainDateTimeObject>,
    PlainDateTime_methods,
    nullptr,
    PlainDateTime_prototype_methods,
    PlainDateTime_prototype_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};
