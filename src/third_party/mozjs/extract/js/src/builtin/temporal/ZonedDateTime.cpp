/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/ZonedDateTime.h"

#include "mozilla/Assertions.h"
#include "mozilla/EnumSet.h"
#include "mozilla/Maybe.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/intl/DateTimeFormat.h"
#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/CalendarFields.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/Int128.h"
#include "builtin/temporal/PlainDate.h"
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
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "gc/GCEnum.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/Printer.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/Value.h"
#include "vm/BigIntType.h"
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

static inline bool IsZonedDateTime(Handle<Value> v) {
  return v.isObject() && v.toObject().is<ZonedDateTimeObject>();
}

// Returns |RoundNumberToIncrement(offsetNanoseconds, 60 × 10^9, "halfExpand")|.
static int64_t RoundNanosecondsToMinutesIncrement(int64_t offsetNanoseconds) {
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  constexpr int64_t increment = ToNanoseconds(TemporalUnit::Minute);

  int64_t quotient = offsetNanoseconds / increment;
  int64_t remainder = offsetNanoseconds % increment;
  if (std::abs(remainder * 2) >= increment) {
    quotient += (offsetNanoseconds > 0 ? 1 : -1);
  }
  return quotient * increment;
}

/**
 * InterpretISODateTimeOffset ( isoDate, time, offsetBehaviour,
 * offsetNanoseconds, timeZone, disambiguation, offsetOption, matchBehaviour )
 */
bool js::temporal::InterpretISODateTimeOffset(
    JSContext* cx, const ISODateTime& dateTime, OffsetBehaviour offsetBehaviour,
    int64_t offsetNanoseconds, Handle<TimeZoneValue> timeZone,
    TemporalDisambiguation disambiguation, TemporalOffset offsetOption,
    MatchBehaviour matchBehaviour, EpochNanoseconds* result) {
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));
  MOZ_ASSERT(IsValidISODateTime(dateTime));

  // FIXME: spec issue - avoid calling with date-time outside of limits
  // https://github.com/tc39/proposal-temporal/pull/3014
  if (!ISODateTimeWithinLimits(dateTime)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
    return false;
  }

  // Steps 1-2. (Not applicable in our implementation.)

  // Step 3.
  if (offsetBehaviour == OffsetBehaviour::Wall ||
      (offsetBehaviour == OffsetBehaviour::Option &&
       offsetOption == TemporalOffset::Ignore)) {
    // Steps 3.a-b.
    return GetEpochNanosecondsFor(cx, timeZone, dateTime, disambiguation,
                                  result);
  }

  // Step 4.
  if (offsetBehaviour == OffsetBehaviour::Exact ||
      (offsetBehaviour == OffsetBehaviour::Option &&
       offsetOption == TemporalOffset::Use)) {
    // Step 4.a.
    auto epochNanoseconds = GetUTCEpochNanoseconds(dateTime) -
                            EpochDuration::fromNanoseconds(offsetNanoseconds);

    // Step 4.b.
    if (!IsValidEpochNanoseconds(epochNanoseconds)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INSTANT_INVALID);
      return false;
    }

    // Step 4.c.
    *result = epochNanoseconds;
    return true;
  }

  // Step 5.
  MOZ_ASSERT(offsetBehaviour == OffsetBehaviour::Option);

  // Step 6.
  MOZ_ASSERT(offsetOption == TemporalOffset::Prefer ||
             offsetOption == TemporalOffset::Reject);

  // Step 7.
  PossibleEpochNanoseconds possibleEpochNs;
  if (!GetPossibleEpochNanoseconds(cx, timeZone, dateTime, &possibleEpochNs)) {
    return false;
  }

  // Step 8.a.
  for (const auto& candidate : possibleEpochNs) {
    // Step 8.a.i.
    int64_t candidateNanoseconds;
    if (!GetOffsetNanosecondsFor(cx, timeZone, candidate,
                                 &candidateNanoseconds)) {
      return false;
    }
    MOZ_ASSERT(std::abs(candidateNanoseconds) <
               ToNanoseconds(TemporalUnit::Day));

    // Step 8.a.ii.
    if (candidateNanoseconds == offsetNanoseconds) {
      *result = candidate;
      return true;
    }

    // Step 8.a.iii.
    if (matchBehaviour == MatchBehaviour::MatchMinutes) {
      // Step 8.a.iii.1.
      int64_t roundedCandidateNanoseconds =
          RoundNanosecondsToMinutesIncrement(candidateNanoseconds);

      // Step 8.a.iii.2.
      if (roundedCandidateNanoseconds == offsetNanoseconds) {
        // Step 8.a.iii.2.a.
        *result = candidate;
        return true;
      }
    }
  }

  // Step 9.
  if (offsetOption == TemporalOffset::Reject) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_ZONED_DATE_TIME_NO_TIME_FOUND);
    return false;
  }

  // Step 10.
  return DisambiguatePossibleEpochNanoseconds(cx, possibleEpochNs, timeZone,
                                              dateTime, disambiguation, result);
}

/**
 * InterpretISODateTimeOffset ( isoDate, time, offsetBehaviour,
 * offsetNanoseconds, timeZone, disambiguation, offsetOption, matchBehaviour )
 */
bool js::temporal::InterpretISODateTimeOffset(
    JSContext* cx, const ISODate& isoDate, OffsetBehaviour offsetBehaviour,
    int64_t offsetNanoseconds, Handle<TimeZoneValue> timeZone,
    TemporalDisambiguation disambiguation, TemporalOffset offsetOption,
    MatchBehaviour matchBehaviour, EpochNanoseconds* result) {
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));
  MOZ_ASSERT(IsValidISODate(isoDate));

  // Step 1. (Not applicable in our implementation.)

  // Step 2.a.
  MOZ_ASSERT(offsetBehaviour == OffsetBehaviour::Wall);

  // Step 2.b.
  MOZ_ASSERT(offsetNanoseconds == 0);

  // Step 2.c.
  return GetStartOfDay(cx, timeZone, isoDate, result);
}

struct ZonedDateTimeOptions {
  TemporalDisambiguation disambiguation = TemporalDisambiguation::Compatible;
  TemporalOffset offset = TemporalOffset::Reject;
  TemporalOverflow overflow = TemporalOverflow::Constrain;
};

/**
 * ToTemporalZonedDateTime ( item [ , options ] )
 */
static bool ToTemporalZonedDateTimeOptions(JSContext* cx, Handle<Value> options,
                                           ZonedDateTimeOptions* result) {
  if (options.isUndefined()) {
    *result = {};
    return true;
  }

  // NOTE: |options| are only passed from `Temporal.ZonedDateTime.from`.

  Rooted<JSObject*> resolvedOptions(
      cx, RequireObjectArg(cx, "options", "from", options));
  if (!resolvedOptions) {
    return false;
  }

  auto disambiguation = TemporalDisambiguation::Compatible;
  if (!GetTemporalDisambiguationOption(cx, resolvedOptions, &disambiguation)) {
    return false;
  }

  auto offset = TemporalOffset::Reject;
  if (!GetTemporalOffsetOption(cx, resolvedOptions, &offset)) {
    return false;
  }

  auto overflow = TemporalOverflow::Constrain;
  if (!GetTemporalOverflowOption(cx, resolvedOptions, &overflow)) {
    return false;
  }

  *result = {disambiguation, offset, overflow};
  return true;
}

/**
 * ToTemporalZonedDateTime ( item [ , options ] )
 */
static bool ToTemporalZonedDateTime(JSContext* cx, Handle<JSObject*> item,
                                    Handle<Value> options,
                                    MutableHandle<ZonedDateTime> result) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  auto offsetBehaviour = OffsetBehaviour::Option;

  // Step 3.
  auto matchBehaviour = MatchBehaviour::MatchExactly;

  // Step 4.a.
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

    // Steps 4.a.i-v.
    ZonedDateTimeOptions ignoredOptions;
    if (!ToTemporalZonedDateTimeOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Step 4.a.vi.
    result.set(ZonedDateTime{epochNs, timeZone, calendar});
    return true;
  }

  // Step 4.b.
  Rooted<CalendarValue> calendar(cx);
  if (!GetTemporalCalendarWithISODefault(cx, item, &calendar)) {
    return false;
  }

  // Step 4.c.
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
                                 CalendarField::Offset,
                                 CalendarField::TimeZone,
                             },
                             {CalendarField::TimeZone}, &fields)) {
    return false;
  }

  // Step 4.d.
  auto timeZone = fields.timeZone();

  // Step 4.e.
  auto offsetString = fields.offset();

  // Step 4.f.
  if (!fields.has(CalendarField::Offset)) {
    offsetBehaviour = OffsetBehaviour::Wall;
  }

  // Steps 4.g-j.
  ZonedDateTimeOptions resolvedOptions;
  if (!ToTemporalZonedDateTimeOptions(cx, options, &resolvedOptions)) {
    return false;
  }
  auto [disambiguation, offsetOption, overflow] = resolvedOptions;

  // Step 4.k.
  ISODateTime dateTime;
  if (!InterpretTemporalDateTimeFields(cx, calendar, fields, overflow,
                                       &dateTime)) {
    return false;
  }

  // Step 6.
  int64_t offsetNanoseconds = 0;

  // Step 7.
  if (offsetBehaviour == OffsetBehaviour::Option) {
    offsetNanoseconds = int64_t(offsetString);
  }

  // Step 8.
  EpochNanoseconds epochNanoseconds;
  if (!InterpretISODateTimeOffset(
          cx, dateTime, offsetBehaviour, offsetNanoseconds, timeZone,
          disambiguation, offsetOption, matchBehaviour, &epochNanoseconds)) {
    return false;
  }
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  // Step 9.
  result.set(ZonedDateTime{epochNanoseconds, timeZone, calendar});
  return true;
}

/**
 * ToTemporalZonedDateTime ( item [ , options ] )
 */
static bool ToTemporalZonedDateTime(JSContext* cx, Handle<Value> item,
                                    Handle<Value> options,
                                    MutableHandle<ZonedDateTime> result) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  auto offsetBehaviour = OffsetBehaviour::Option;

  // Step 3.
  auto matchBehaviour = MatchBehaviour::MatchExactly;

  // Step 4.
  if (item.isObject()) {
    Rooted<JSObject*> itemObj(cx, &item.toObject());
    return ToTemporalZonedDateTime(cx, itemObj, options, result);
  }

  // Step 5.a.
  if (!item.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, item,
                     nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> string(cx, item.toString());

  // Case 1: 19700101Z[+02:00]
  // { [[Z]]: true, [[OffsetString]]: undefined, [[Name]]: "+02:00" }
  //
  // Case 2: 19700101+00:00[+02:00]
  // { [[Z]]: false, [[OffsetString]]: "+00:00", [[Name]]: "+02:00" }
  //
  // Case 3: 19700101[+02:00]
  // { [[Z]]: false, [[OffsetString]]: undefined, [[Name]]: "+02:00" }
  //
  // Case 4: 19700101Z[Europe/Berlin]
  // { [[Z]]: true, [[OffsetString]]: undefined, [[Name]]: "Europe/Berlin" }
  //
  // Case 5: 19700101+00:00[Europe/Berlin]
  // { [[Z]]: false, [[OffsetString]]: "+00:00", [[Name]]: "Europe/Berlin" }
  //
  // Case 6: 19700101[Europe/Berlin]
  // { [[Z]]: false, [[OffsetString]]: undefined, [[Name]]: "Europe/Berlin" }

  // Steps 5.b-c.
  Rooted<ParsedZonedDateTime> parsed(cx);
  if (!ParseTemporalZonedDateTimeString(cx, string, &parsed)) {
    return false;
  }

  // Step 5.d.
  MOZ_ASSERT(parsed.timeZoneAnnotation());

  // Step 5.e.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZone(cx, parsed.timeZoneAnnotation(), &timeZone)) {
    return false;
  }

  // Step 5.f. (Not applicable in our implementation.)

  // Step 5.g.
  if (parsed.isUTC()) {
    offsetBehaviour = OffsetBehaviour::Exact;
  }

  // Step 5.h.
  else if (!parsed.hasOffset()) {
    offsetBehaviour = OffsetBehaviour::Wall;
  }

  // Steps 5.i-k.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  if (parsed.calendar()) {
    if (!CanonicalizeCalendar(cx, parsed.calendar(), &calendar)) {
      return false;
    }
  }

  // Step 5.l.
  matchBehaviour = MatchBehaviour::MatchMinutes;

  // Steps 5.m-p.
  ZonedDateTimeOptions resolvedOptions;
  if (!ToTemporalZonedDateTimeOptions(cx, options, &resolvedOptions)) {
    return false;
  }
  auto [disambiguation, offsetOption, overflow] = resolvedOptions;

  // Steps 5.q-r. (Not applicable in our implementation.)

  // Step 6.
  int64_t offsetNanoseconds = 0;

  // Step 7.
  if (offsetBehaviour == OffsetBehaviour::Option) {
    MOZ_ASSERT(parsed.hasOffset());
    offsetNanoseconds = parsed.timeZoneOffset();
  }

  // Step 8.
  EpochNanoseconds epochNanoseconds;
  if (parsed.isStartOfDay()) {
    if (!InterpretISODateTimeOffset(cx, parsed.dateTime().date, offsetBehaviour,
                                    offsetNanoseconds, timeZone, disambiguation,
                                    offsetOption, matchBehaviour,
                                    &epochNanoseconds)) {
      return false;
    }
  } else {
    if (!InterpretISODateTimeOffset(
            cx, parsed.dateTime(), offsetBehaviour, offsetNanoseconds, timeZone,
            disambiguation, offsetOption, matchBehaviour, &epochNanoseconds)) {
      return false;
    }
  }
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  // Step 9.
  result.set(ZonedDateTime{epochNanoseconds, timeZone, calendar});
  return true;
}

/**
 * ToTemporalZonedDateTime ( item [ , options ] )
 */
static bool ToTemporalZonedDateTime(JSContext* cx, Handle<Value> item,
                                    MutableHandle<ZonedDateTime> result) {
  return ToTemporalZonedDateTime(cx, item, UndefinedHandleValue, result);
}

/**
 * CreateTemporalZonedDateTime ( epochNanoseconds, timeZone, calendar [ ,
 * newTarget ] )
 */
static ZonedDateTimeObject* CreateTemporalZonedDateTime(
    JSContext* cx, const CallArgs& args, Handle<BigInt*> epochNanoseconds,
    Handle<TimeZoneValue> timeZone, Handle<CalendarValue> calendar) {
  // Step 1.
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  // Steps 3-4.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_ZonedDateTime,
                                          &proto)) {
    return nullptr;
  }

  auto* object = NewObjectWithClassProto<ZonedDateTimeObject>(cx, proto);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  auto epochNs = ToEpochNanoseconds(epochNanoseconds);
  object->setFixedSlot(ZonedDateTimeObject::SECONDS_SLOT,
                       NumberValue(epochNs.seconds));
  object->setFixedSlot(ZonedDateTimeObject::NANOSECONDS_SLOT,
                       Int32Value(epochNs.nanoseconds));

  // Step 5.
  object->setFixedSlot(ZonedDateTimeObject::TIMEZONE_SLOT,
                       timeZone.toSlotValue());

  // Step 6.
  object->setFixedSlot(ZonedDateTimeObject::CALENDAR_SLOT,
                       calendar.toSlotValue());

  // Step 7.
  return object;
}

/**
 * CreateTemporalZonedDateTime ( epochNanoseconds, timeZone, calendar [ ,
 * newTarget ] )
 */
ZonedDateTimeObject* js::temporal::CreateTemporalZonedDateTime(
    JSContext* cx, const EpochNanoseconds& epochNanoseconds,
    Handle<TimeZoneValue> timeZone, Handle<CalendarValue> calendar) {
  // Step 1.
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  // Steps 2-3.
  auto* object = NewBuiltinClassInstance<ZonedDateTimeObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  object->setFixedSlot(ZonedDateTimeObject::SECONDS_SLOT,
                       NumberValue(epochNanoseconds.seconds));
  object->setFixedSlot(ZonedDateTimeObject::NANOSECONDS_SLOT,
                       Int32Value(epochNanoseconds.nanoseconds));

  // Step 5.
  object->setFixedSlot(ZonedDateTimeObject::TIMEZONE_SLOT,
                       timeZone.toSlotValue());

  // Step 6.
  object->setFixedSlot(ZonedDateTimeObject::CALENDAR_SLOT,
                       calendar.toSlotValue());

  // Step 7.
  return object;
}

/**
 * CreateTemporalZonedDateTime ( epochNanoseconds, timeZone, calendar [ ,
 * newTarget ] )
 */
static auto* CreateTemporalZonedDateTime(JSContext* cx,
                                         Handle<ZonedDateTime> zonedDateTime) {
  return CreateTemporalZonedDateTime(cx, zonedDateTime.epochNanoseconds(),
                                     zonedDateTime.timeZone(),
                                     zonedDateTime.calendar());
}

/**
 * AddZonedDateTime ( epochNanoseconds, timeZone, calendar, duration, overflow )
 */
static bool AddZonedDateTime(JSContext* cx, Handle<ZonedDateTime> zonedDateTime,
                             const InternalDuration& duration,
                             TemporalOverflow overflow,
                             EpochNanoseconds* result) {
  MOZ_ASSERT(IsValidDuration(duration));

  // Step 1.
  if (duration.date == DateDuration{}) {
    // Step 1.a.
    return AddInstant(cx, zonedDateTime.epochNanoseconds(), duration.time,
                      result);
  }

  // Step 2.
  ISODateTime isoDateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &isoDateTime)) {
    return false;
  }

  // Step 3.
  ISODate addedDate;
  if (!CalendarDateAdd(cx, zonedDateTime.calendar(), isoDateTime.date,
                       duration.date, overflow, &addedDate)) {
    return false;
  }

  // Step 4.
  auto intermediateDateTime = ISODateTime{addedDate, isoDateTime.time};

  // Step 5.
  if (!ISODateTimeWithinLimits(intermediateDateTime)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
    return false;
  }

  // Step 6.
  EpochNanoseconds intermediateNs;
  if (!GetEpochNanosecondsFor(
          cx, zonedDateTime.timeZone(), intermediateDateTime,
          TemporalDisambiguation::Compatible, &intermediateNs)) {
    return false;
  }

  // Step 7.
  return AddInstant(cx, intermediateNs, duration.time, result);
}

/**
 * AddZonedDateTime ( epochNanoseconds, timeZone, calendar, duration, overflow )
 */
bool js::temporal::AddZonedDateTime(JSContext* cx,
                                    Handle<ZonedDateTime> zonedDateTime,
                                    const InternalDuration& duration,
                                    EpochNanoseconds* result) {
  return ::AddZonedDateTime(cx, zonedDateTime, duration,
                            TemporalOverflow::Constrain, result);
}

/**
 * DifferenceZonedDateTime ( ns1, ns2, timeZone, calendar, largestUnit )
 */
static bool DifferenceZonedDateTime(JSContext* cx, const EpochNanoseconds& ns1,
                                    const EpochNanoseconds& ns2,
                                    Handle<TimeZoneValue> timeZone,
                                    Handle<CalendarValue> calendar,
                                    TemporalUnit largestUnit,
                                    InternalDuration* result) {
  MOZ_ASSERT(IsValidEpochNanoseconds(ns1));
  MOZ_ASSERT(IsValidEpochNanoseconds(ns2));

  // Steps 1.
  if (ns1 == ns2) {
    *result = InternalDuration{{}, {}};
    return true;
  }

  // Step 2.
  ISODateTime startDateTime;
  if (!GetISODateTimeFor(cx, timeZone, ns1, &startDateTime)) {
    return false;
  }

  // Steps 2-3.
  ISODateTime endDateTime;
  if (!GetISODateTimeFor(cx, timeZone, ns2, &endDateTime)) {
    return false;
  }

  // Step 4.
  int32_t sign = (ns2 - ns1 < EpochDuration{}) ? -1 : 1;

  // Step 5.
  int32_t maxDayCorrection = 1 + (sign > 0);

  // Step 6.
  int32_t dayCorrection = 0;

  // Step 7.
  auto timeDuration = DifferenceTime(startDateTime.time, endDateTime.time);

  // Step 8.
  if (TimeDurationSign(timeDuration) == -sign) {
    dayCorrection += 1;
  }

  // Steps 9-10.
  while (dayCorrection <= maxDayCorrection) {
    // Step 10.a.
    auto intermediateDate =
        BalanceISODate(endDateTime.date, -dayCorrection * sign);

    // Step 10.b.
    auto intermediateDateTime =
        ISODateTime{intermediateDate, startDateTime.time};
    if (!ISODateTimeWithinLimits(intermediateDateTime)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
      return false;
    }

    // Step 10.c.
    EpochNanoseconds intermediateNs;
    if (!GetEpochNanosecondsFor(cx, timeZone, intermediateDateTime,
                                TemporalDisambiguation::Compatible,
                                &intermediateNs)) {
      return false;
    }

    // Step 10.d.
    auto timeDuration =
        TimeDurationFromEpochNanosecondsDifference(ns2, intermediateNs);

    // Step 10.e.
    int32_t timeSign = TimeDurationSign(timeDuration);

    // Step 10.f.
    if (sign != -timeSign) {
      // Step 12.
      auto dateLargestUnit = std::min(largestUnit, TemporalUnit::Day);

      // Step 13.
      DateDuration dateDifference;
      if (!CalendarDateUntil(cx, calendar, startDateTime.date, intermediateDate,
                             dateLargestUnit, &dateDifference)) {
        return false;
      }

      // Step 14.
      MOZ_ASSERT(DateDurationSign(dateDifference) *
                     TimeDurationSign(timeDuration) >=
                 0);
      *result = {dateDifference, timeDuration};
      return true;
    }

    // Step 10.g.
    dayCorrection += 1;
  }

  // Step 11.
  JS_ReportErrorNumberASCII(
      cx, GetErrorMessage, nullptr,
      JSMSG_TEMPORAL_ZONED_DATE_TIME_INCONSISTENT_INSTANT);
  return false;
}

/**
 * DifferenceZonedDateTimeWithRounding ( ns1, ns2, timeZone, calendar,
 * largestUnit, roundingIncrement, smallestUnit, roundingMode )
 */
bool js::temporal::DifferenceZonedDateTimeWithRounding(
    JSContext* cx, JS::Handle<ZonedDateTime> zonedDateTime,
    const EpochNanoseconds& ns2, const DifferenceSettings& settings,
    InternalDuration* result) {
  MOZ_ASSERT(IsValidEpochNanoseconds(ns2));
  MOZ_ASSERT(settings.smallestUnit >= settings.largestUnit);

  const auto& ns1 = zonedDateTime.epochNanoseconds();
  auto timeZone = zonedDateTime.timeZone();
  auto calendar = zonedDateTime.calendar();

  // Step 1.
  if (settings.largestUnit > TemporalUnit::Day) {
    // Step 1.a.
    auto difference =
        DifferenceInstant(ns1, ns2, settings.roundingIncrement,
                          settings.smallestUnit, settings.roundingMode);
    *result = InternalDuration{{}, difference};
    return true;
  }

  // Step 2.
  InternalDuration difference;
  if (!DifferenceZonedDateTime(cx, ns1, ns2, timeZone, calendar,
                               settings.largestUnit, &difference)) {
    return false;
  }

  // Step 3.
  if (settings.smallestUnit == TemporalUnit::Nanosecond &&
      settings.roundingIncrement == Increment{1}) {
    // Step 3.a.
    *result = difference;
    return true;
  }

  // Step 4.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, timeZone, ns1, &dateTime)) {
    return false;
  }

  // Step 5.
  return RoundRelativeDuration(
      cx, difference, ns2, dateTime, timeZone, calendar, settings.largestUnit,
      settings.roundingIncrement, settings.smallestUnit, settings.roundingMode,
      result);
}

/**
 * DifferenceZonedDateTimeWithTotal ( ns1, ns2, timeZone, calendar, unit )
 */
bool js::temporal::DifferenceZonedDateTimeWithTotal(
    JSContext* cx, JS::Handle<ZonedDateTime> zonedDateTime,
    const EpochNanoseconds& ns2, TemporalUnit unit, double* result) {
  MOZ_ASSERT(IsValidEpochNanoseconds(ns2));

  const auto& ns1 = zonedDateTime.epochNanoseconds();
  auto timeZone = zonedDateTime.timeZone();
  auto calendar = zonedDateTime.calendar();

  // Step 1.
  if (unit > TemporalUnit::Day) {
    // Step 1.a.
    auto difference = TimeDurationFromEpochNanosecondsDifference(ns2, ns1);
    MOZ_ASSERT(IsValidEpochDuration(difference.to<EpochDuration>()));

    // Step 1.b.
    *result = TotalTimeDuration(difference, unit);
    return true;
  }

  // Step 2.
  InternalDuration difference;
  if (!DifferenceZonedDateTime(cx, ns1, ns2, timeZone, calendar, unit,
                               &difference)) {
    return false;
  }

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, timeZone, ns1, &dateTime)) {
    return false;
  }

  // Step 5.
  return TotalRelativeDuration(cx, difference, ns2, dateTime, timeZone,
                               calendar, unit, result);
}

/**
 * DifferenceTemporalZonedDateTime ( operation, zonedDateTime, other, options )
 */
static bool DifferenceTemporalZonedDateTime(JSContext* cx,
                                            TemporalDifference operation,
                                            const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 1.
  Rooted<ZonedDateTime> other(cx);
  if (!ToTemporalZonedDateTime(cx, args.get(0), &other)) {
    return false;
  }

  // Step 2.
  if (!CalendarEquals(zonedDateTime.calendar(), other.calendar())) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE,
        CalendarIdentifier(zonedDateTime.calendar()).data(),
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
            TemporalUnit::Nanosecond, TemporalUnit::Hour, &settings)) {
      return false;
    }
  } else {
    // Steps 3-4.
    settings = {
        TemporalUnit::Nanosecond,
        TemporalUnit::Hour,
        TemporalRoundingMode::Trunc,
        Increment{1},
    };
  }

  // Step 5.
  if (settings.largestUnit > TemporalUnit::Day) {
    MOZ_ASSERT(settings.smallestUnit >= settings.largestUnit);

    // Step 5.a.
    auto timeDuration =
        DifferenceInstant(zonedDateTime.epochNanoseconds(),
                          other.epochNanoseconds(), settings.roundingIncrement,
                          settings.smallestUnit, settings.roundingMode);

    // Step 5.b.
    Duration result;
    if (!TemporalDurationFromInternal(cx, timeDuration, settings.largestUnit,
                                      &result)) {
      return false;
    }

    // Step 5.c.
    if (operation == TemporalDifference::Since) {
      result = result.negate();
    }

    // Step 5.d.
    auto* obj = CreateTemporalDuration(cx, result);
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Steps 6-7.
  if (!TimeZoneEquals(zonedDateTime.timeZone(), other.timeZone())) {
    if (auto one = QuoteString(cx, zonedDateTime.timeZone().identifier())) {
      if (auto two = QuoteString(cx, other.timeZone().identifier())) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_TEMPORAL_TIMEZONE_INCOMPATIBLE,
                                 one.get(), two.get());
      }
    }
    return false;
  }

  // Step 8.
  if (zonedDateTime.epochNanoseconds() == other.epochNanoseconds()) {
    auto* obj = CreateTemporalDuration(cx, {});
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Step 9.
  InternalDuration internalDuration;
  if (!DifferenceZonedDateTimeWithRounding(cx, zonedDateTime,
                                           other.epochNanoseconds(), settings,
                                           &internalDuration)) {
    return false;
  }
  MOZ_ASSERT(IsValidDuration(internalDuration));

  // Step 10.
  Duration result;
  if (!TemporalDurationFromInternal(cx, internalDuration, TemporalUnit::Hour,
                                    &result)) {
    return false;
  }

  // Step 11.
  if (operation == TemporalDifference::Since) {
    result = result.negate();
  }

  // Step 12.
  auto* obj = CreateTemporalDuration(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * AddDurationToZonedDateTime ( operation, zonedDateTime, temporalDurationLike,
 * options )
 */
static bool AddDurationToZonedDateTime(JSContext* cx,
                                       TemporalAddDuration operation,
                                       const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, &args.thisv().toObject().as<ZonedDateTimeObject>());

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
  auto calendar = zonedDateTime.calendar();

  // Step 6.
  auto timeZone = zonedDateTime.timeZone();

  // Step 7.
  auto internalDuration = ToInternalDurationRecord(duration);

  // Step 8.
  EpochNanoseconds epochNanoseconds;
  if (!::AddZonedDateTime(cx, zonedDateTime, internalDuration, overflow,
                          &epochNanoseconds)) {
    return false;
  }
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  // Step 9.
  auto* result =
      CreateTemporalZonedDateTime(cx, epochNanoseconds, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * FormatUTCOffsetNanoseconds ( offsetNanoseconds )
 */
static JSString* FormatUTCOffsetNanoseconds(JSContext* cx,
                                            int64_t offsetNanoseconds) {
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Step 1.
  char sign = offsetNanoseconds >= 0 ? '+' : '-';

  // Step 2.
  int64_t absoluteNanoseconds = std::abs(offsetNanoseconds);

  // Step 6. (Reordered)
  int32_t subSecondNanoseconds = int32_t(absoluteNanoseconds % 1'000'000'000);

  // Step 5. (Reordered)
  int32_t quotient = int32_t(absoluteNanoseconds / 1'000'000'000);
  int32_t second = quotient % 60;

  // Step 4. (Reordered)
  quotient /= 60;
  int32_t minute = quotient % 60;

  // Step 3.
  int32_t hour = quotient / 60;
  MOZ_ASSERT(hour < 24, "time zone offset mustn't exceed 24-hours");

  // Format: "sign hour{2} : minute{2} : second{2} . fractional{9}"
  constexpr size_t maxLength = 1 + 2 + 1 + 2 + 1 + 2 + 1 + 9;
  char result[maxLength];

  size_t n = 0;

  // Steps 7-8. (Inlined FormatTimeString).
  result[n++] = sign;
  result[n++] = char('0' + (hour / 10));
  result[n++] = char('0' + (hour % 10));
  result[n++] = ':';
  result[n++] = char('0' + (minute / 10));
  result[n++] = char('0' + (minute % 10));

  if (second != 0 || subSecondNanoseconds != 0) {
    result[n++] = ':';
    result[n++] = char('0' + (second / 10));
    result[n++] = char('0' + (second % 10));

    if (uint32_t fractional = subSecondNanoseconds) {
      result[n++] = '.';

      uint32_t k = 100'000'000;
      do {
        result[n++] = char('0' + (fractional / k));
        fractional %= k;
        k /= 10;
      } while (fractional);
    }
  }

  MOZ_ASSERT(n <= maxLength);

  // Step 9.
  return NewStringCopyN<CanGC>(cx, result, n);
}

/**
 * Temporal.ZonedDateTime ( epochNanoseconds, timeZone [ , calendar ] )
 */
static bool ZonedDateTimeConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Temporal.ZonedDateTime")) {
    return false;
  }

  // Step 2.
  Rooted<BigInt*> epochNanoseconds(cx, js::ToBigInt(cx, args.get(0)));
  if (!epochNanoseconds) {
    return false;
  }

  // Step 3.
  if (!IsValidEpochNanoseconds(epochNanoseconds)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 4.
  if (!args.get(1).isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, args.get(1),
                     nullptr, "not a string");
    return false;
  }

  // Step 5.
  Rooted<JSString*> timeZoneString(cx, args[1].toString());
  Rooted<ParsedTimeZone> timeZoneParse(cx);
  if (!ParseTimeZoneIdentifier(cx, timeZoneString, &timeZoneParse)) {
    return false;
  }

  // Steps 6-7.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZone(cx, timeZoneParse, &timeZone)) {
    return false;
  }

  // Steps 8-10.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  if (args.hasDefined(2)) {
    // Step 9.
    if (!args[2].isString()) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, args[2],
                       nullptr, "not a string");
      return false;
    }

    // Step 10.
    Rooted<JSString*> calendarString(cx, args[2].toString());
    if (!CanonicalizeCalendar(cx, calendarString, &calendar)) {
      return false;
    }
  }

  // Step 11.
  auto* obj = CreateTemporalZonedDateTime(cx, args, epochNanoseconds, timeZone,
                                          calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.ZonedDateTime.from ( item [ , options ] )
 */
static bool ZonedDateTime_from(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<ZonedDateTime> zonedDateTime(cx);
  if (!ToTemporalZonedDateTime(cx, args.get(0), args.get(1), &zonedDateTime)) {
    return false;
  }

  auto* result = CreateTemporalZonedDateTime(cx, zonedDateTime);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.compare ( one, two )
 */
static bool ZonedDateTime_compare(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<ZonedDateTime> one(cx);
  if (!ToTemporalZonedDateTime(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  Rooted<ZonedDateTime> two(cx);
  if (!ToTemporalZonedDateTime(cx, args.get(1), &two)) {
    return false;
  }

  // Step 3.
  const auto& oneNs = one.epochNanoseconds();
  const auto& twoNs = two.epochNanoseconds();
  args.rval().setInt32(oneNs > twoNs ? 1 : oneNs < twoNs ? -1 : 0);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.calendarId
 */
static bool ZonedDateTime_calendarId(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  auto* str =
      NewStringCopy<CanGC>(cx, CalendarIdentifier(zonedDateTime->calendar()));
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.calendarId
 */
static bool ZonedDateTime_calendarId(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_calendarId>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.timeZoneId
 */
static bool ZonedDateTime_timeZoneId(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  args.rval().setString(zonedDateTime->timeZone().identifier());
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.timeZoneId
 */
static bool ZonedDateTime_timeZoneId(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_timeZoneId>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.era
 */
static bool ZonedDateTime_era(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarEra(cx, zonedDateTime.calendar(), dateTime.date, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.era
 */
static bool ZonedDateTime_era(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_era>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.eraYear
 */
static bool ZonedDateTime_eraYear(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Steps 4-6.
  return CalendarEraYear(cx, zonedDateTime.calendar(), dateTime.date,
                         args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.eraYear
 */
static bool ZonedDateTime_eraYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_eraYear>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.year
 */
static bool ZonedDateTime_year(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarYear(cx, zonedDateTime.calendar(), dateTime.date, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.year
 */
static bool ZonedDateTime_year(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_year>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.month
 */
static bool ZonedDateTime_month(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarMonth(cx, zonedDateTime.calendar(), dateTime.date,
                       args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.month
 */
static bool ZonedDateTime_month(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_month>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.monthCode
 */
static bool ZonedDateTime_monthCode(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarMonthCode(cx, zonedDateTime.calendar(), dateTime.date,
                           args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.monthCode
 */
static bool ZonedDateTime_monthCode(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_monthCode>(cx,
                                                                        args);
}

/**
 * get Temporal.ZonedDateTime.prototype.day
 */
static bool ZonedDateTime_day(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarDay(cx, zonedDateTime.calendar(), dateTime.date, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.day
 */
static bool ZonedDateTime_day(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_day>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.hour
 */
static bool ZonedDateTime_hour(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  args.rval().setInt32(dateTime.time.hour);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.hour
 */
static bool ZonedDateTime_hour(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_hour>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.minute
 */
static bool ZonedDateTime_minute(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  args.rval().setInt32(dateTime.time.minute);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.minute
 */
static bool ZonedDateTime_minute(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_minute>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.second
 */
static bool ZonedDateTime_second(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  args.rval().setInt32(dateTime.time.second);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.second
 */
static bool ZonedDateTime_second(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_second>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.millisecond
 */
static bool ZonedDateTime_millisecond(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  args.rval().setInt32(dateTime.time.millisecond);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.millisecond
 */
static bool ZonedDateTime_millisecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_millisecond>(cx,
                                                                          args);
}

/**
 * get Temporal.ZonedDateTime.prototype.microsecond
 */
static bool ZonedDateTime_microsecond(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  args.rval().setInt32(dateTime.time.microsecond);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.microsecond
 */
static bool ZonedDateTime_microsecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_microsecond>(cx,
                                                                          args);
}

/**
 * get Temporal.ZonedDateTime.prototype.nanosecond
 */
static bool ZonedDateTime_nanosecond(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  args.rval().setInt32(dateTime.time.nanosecond);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.nanosecond
 */
static bool ZonedDateTime_nanosecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_nanosecond>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.epochMilliseconds
 */
static bool ZonedDateTime_epochMilliseconds(JSContext* cx,
                                            const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  auto epochNs = zonedDateTime->epochNanoseconds();

  // Steps 4-5.
  args.rval().setNumber(epochNs.floorToMilliseconds());
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.epochMilliseconds
 */
static bool ZonedDateTime_epochMilliseconds(JSContext* cx, unsigned argc,
                                            Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_epochMilliseconds>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.epochNanoseconds
 */
static bool ZonedDateTime_epochNanoseconds(JSContext* cx,
                                           const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  auto* nanoseconds = ToBigInt(cx, zonedDateTime->epochNanoseconds());
  if (!nanoseconds) {
    return false;
  }

  args.rval().setBigInt(nanoseconds);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.epochNanoseconds
 */
static bool ZonedDateTime_epochNanoseconds(JSContext* cx, unsigned argc,
                                           Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_epochNanoseconds>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.dayOfWeek
 */
static bool ZonedDateTime_dayOfWeek(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarDayOfWeek(cx, zonedDateTime.calendar(), dateTime.date,
                           args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.dayOfWeek
 */
static bool ZonedDateTime_dayOfWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_dayOfWeek>(cx,
                                                                        args);
}

/**
 * get Temporal.ZonedDateTime.prototype.dayOfYear
 */
static bool ZonedDateTime_dayOfYear(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarDayOfYear(cx, zonedDateTime.calendar(), dateTime.date,
                           args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.dayOfYear
 */
static bool ZonedDateTime_dayOfYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_dayOfYear>(cx,
                                                                        args);
}

/**
 * get Temporal.ZonedDateTime.prototype.weekOfYear
 */
static bool ZonedDateTime_weekOfYear(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Steps 4-6.
  return CalendarWeekOfYear(cx, zonedDateTime.calendar(), dateTime.date,
                            args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.weekOfYear
 */
static bool ZonedDateTime_weekOfYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_weekOfYear>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.yearOfWeek
 */
static bool ZonedDateTime_yearOfWeek(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Steps 4-6.
  return CalendarYearOfWeek(cx, zonedDateTime.calendar(), dateTime.date,
                            args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.yearOfWeek
 */
static bool ZonedDateTime_yearOfWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_yearOfWeek>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.hoursInDay
 */
static bool ZonedDateTime_hoursInDay(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  auto timeZone = zonedDateTime.timeZone();

  // Step 4.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, timeZone, zonedDateTime.epochNanoseconds(),
                         &dateTime)) {
    return false;
  }

  // Step 5.
  const auto& today = dateTime.date;

  // Step 6.
  auto tomorrow = BalanceISODate(today, 1);
  if (!ISODateWithinLimits(tomorrow)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  // Step 7.
  EpochNanoseconds todayNs;
  if (!GetStartOfDay(cx, timeZone, today, &todayNs)) {
    return false;
  }

  // Step 8.
  EpochNanoseconds tomorrowNs;
  if (!GetStartOfDay(cx, timeZone, tomorrow, &tomorrowNs)) {
    return false;
  }

  // Step 9.
  auto diff = tomorrowNs - todayNs;
  MOZ_ASSERT(diff.abs() <= EpochDuration::fromDays(2),
             "maximum day length for repeated days doesn't exceed two days");

  static_assert(EpochDuration::fromDays(2).toNanoseconds() < Int128{INT64_MAX},
                "two days in nanoseconds fits into int64_t");

  // Step 10. (Inlined TotalTimeDuration)
  constexpr auto nsPerHour = ToNanoseconds(TemporalUnit::Hour);
  args.rval().setNumber(
      FractionToDouble(int64_t(diff.toNanoseconds()), nsPerHour));
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.hoursInDay
 */
static bool ZonedDateTime_hoursInDay(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_hoursInDay>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInWeek
 */
static bool ZonedDateTime_daysInWeek(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarDaysInWeek(cx, zonedDateTime.calendar(), dateTime.date,
                            args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInWeek
 */
static bool ZonedDateTime_daysInWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_daysInWeek>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInMonth
 */
static bool ZonedDateTime_daysInMonth(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarDaysInMonth(cx, zonedDateTime.calendar(), dateTime.date,
                             args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInMonth
 */
static bool ZonedDateTime_daysInMonth(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_daysInMonth>(cx,
                                                                          args);
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInYear
 */
static bool ZonedDateTime_daysInYear(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarDaysInYear(cx, zonedDateTime.calendar(), dateTime.date,
                            args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInYear
 */
static bool ZonedDateTime_daysInYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_daysInYear>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.monthsInYear
 */
static bool ZonedDateTime_monthsInYear(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarMonthsInYear(cx, zonedDateTime.calendar(), dateTime.date,
                              args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.monthsInYear
 */
static bool ZonedDateTime_monthsInYear(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_monthsInYear>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.inLeapYear
 */
static bool ZonedDateTime_inLeapYear(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }

  // Step 4.
  return CalendarInLeapYear(cx, zonedDateTime.calendar(), dateTime.date,
                            args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.inLeapYear
 */
static bool ZonedDateTime_inLeapYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_inLeapYear>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.offsetNanoseconds
 */
static bool ZonedDateTime_offsetNanoseconds(JSContext* cx,
                                            const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, zonedDateTime.timeZone(),
                               zonedDateTime.epochNanoseconds(),
                               &offsetNanoseconds)) {
    return false;
  }
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  args.rval().setNumber(offsetNanoseconds);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.offsetNanoseconds
 */
static bool ZonedDateTime_offsetNanoseconds(JSContext* cx, unsigned argc,
                                            Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_offsetNanoseconds>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.offset
 */
static bool ZonedDateTime_offset(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, zonedDateTime.timeZone(),
                               zonedDateTime.epochNanoseconds(),
                               &offsetNanoseconds)) {
    return false;
  }
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Step 4.
  JSString* str = FormatUTCOffsetNanoseconds(cx, offsetNanoseconds);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.offset
 */
static bool ZonedDateTime_offset(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_offset>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.with ( temporalZonedDateTimeLike [ , options
 * ] )
 */
static bool ZonedDateTime_with(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  Rooted<JSObject*> temporalZonedDateTimeLike(
      cx,
      RequireObjectArg(cx, "temporalZonedDateTimeLike", "with", args.get(0)));
  if (!temporalZonedDateTimeLike) {
    return false;
  }
  if (!ThrowIfTemporalLikeObject(cx, temporalZonedDateTimeLike)) {
    return false;
  }

  // Step 4.
  const auto& epochNs = zonedDateTime.epochNanoseconds();

  // Step 5.
  auto timeZone = zonedDateTime.timeZone();

  // Step 6.
  auto calendar = zonedDateTime.calendar();

  // Step 7.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, epochNs, &offsetNanoseconds)) {
    return false;
  }

  // Step 8.
  auto dateTime = GetISODateTimeFor(epochNs, offsetNanoseconds);
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));

  // Step 9.
  Rooted<PlainDate> date(cx, PlainDate{dateTime.date, calendar});
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, date, &fields)) {
    return false;
  }

  // Steps 10-16.
  fields.setHour(dateTime.time.hour);
  fields.setMinute(dateTime.time.minute);
  fields.setSecond(dateTime.time.second);
  fields.setMillisecond(dateTime.time.millisecond);
  fields.setMicrosecond(dateTime.time.microsecond);
  fields.setNanosecond(dateTime.time.nanosecond);
  fields.setOffset(OffsetField{offsetNanoseconds});

  // Step 17.
  Rooted<CalendarFields> partialZonedDateTime(cx);
  if (!PreparePartialCalendarFields(cx, calendar, temporalZonedDateTimeLike,
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
                                        CalendarField::Offset,
                                    },
                                    &partialZonedDateTime)) {
    return false;
  }
  MOZ_ASSERT(!partialZonedDateTime.keys().isEmpty());

  // Step 18.
  fields = CalendarMergeFields(calendar, fields, partialZonedDateTime);

  // Steps 19-22.
  auto disambiguation = TemporalDisambiguation::Compatible;
  auto offset = TemporalOffset::Prefer;
  auto overflow = TemporalOverflow::Constrain;
  if (args.hasDefined(1)) {
    // Step 19.
    Rooted<JSObject*> options(cx,
                              RequireObjectArg(cx, "options", "with", args[1]));
    if (!options) {
      return false;
    }

    // Step 20.
    if (!GetTemporalDisambiguationOption(cx, options, &disambiguation)) {
      return false;
    }

    // Step 21.
    if (!GetTemporalOffsetOption(cx, options, &offset)) {
      return false;
    }

    // Step 22.
    if (!GetTemporalOverflowOption(cx, options, &overflow)) {
      return false;
    }
  }

  // Step 23.
  ISODateTime dateTimeResult;
  if (!InterpretTemporalDateTimeFields(cx, calendar, fields, overflow,
                                       &dateTimeResult)) {
    return false;
  }

  // Step 24.
  int64_t newOffsetNanoseconds = int64_t(fields.offset());

  // Step 25.
  EpochNanoseconds epochNanoseconds;
  if (!InterpretISODateTimeOffset(
          cx, dateTimeResult, OffsetBehaviour::Option, newOffsetNanoseconds,
          timeZone, disambiguation, offset, MatchBehaviour::MatchExactly,
          &epochNanoseconds)) {
    return false;
  }

  // Step 26.
  auto* result =
      CreateTemporalZonedDateTime(cx, epochNanoseconds, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.with ( temporalZonedDateTimeLike [ , options
 * ] )
 */
static bool ZonedDateTime_with(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_with>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.withPlainTime ( [ plainTimeLike ] )
 */
static bool ZonedDateTime_withPlainTime(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  auto timeZone = zonedDateTime.timeZone();

  // Step 4.
  auto calendar = zonedDateTime.calendar();

  // Step 5.
  ISODateTime isoDateTime;
  if (!GetISODateTimeFor(cx, timeZone, zonedDateTime.epochNanoseconds(),
                         &isoDateTime)) {
    return false;
  }

  // Steps 6-7.
  EpochNanoseconds epochNs;
  if (!args.hasDefined(0)) {
    // Step 6.a.
    if (!GetStartOfDay(cx, timeZone, isoDateTime.date, &epochNs)) {
      return false;
    }
  } else {
    // Step 7.a.
    Time time;
    if (!ToTemporalTime(cx, args[0], &time)) {
      return false;
    }

    // Step 7.b.
    auto resultISODateTime = ISODateTime{isoDateTime.date, time};
    if (!ISODateTimeWithinLimits(resultISODateTime)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
      return false;
    }

    // Step 7.c.
    if (!GetEpochNanosecondsFor(cx, timeZone, resultISODateTime,
                                TemporalDisambiguation::Compatible, &epochNs)) {
      return false;
    }
  }

  // Step 8.
  auto* result = CreateTemporalZonedDateTime(cx, epochNs, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.withPlainTime ( [ plainTimeLike ] )
 */
static bool ZonedDateTime_withPlainTime(JSContext* cx, unsigned argc,
                                        Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_withPlainTime>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.withTimeZone ( timeZoneLike )
 */
static bool ZonedDateTime_withTimeZone(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZone(cx, args.get(0), &timeZone)) {
    return false;
  }

  // Step 4.
  auto* result = CreateTemporalZonedDateTime(
      cx, zonedDateTime.epochNanoseconds(), timeZone, zonedDateTime.calendar());
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.withTimeZone ( timeZoneLike )
 */
static bool ZonedDateTime_withTimeZone(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_withTimeZone>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.withCalendar ( calendarLike )
 */
static bool ZonedDateTime_withCalendar(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendar(cx, args.get(0), &calendar)) {
    return false;
  }

  // Step 4.
  auto* result = CreateTemporalZonedDateTime(
      cx, zonedDateTime.epochNanoseconds(), zonedDateTime.timeZone(), calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.withCalendar ( calendarLike )
 */
static bool ZonedDateTime_withCalendar(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_withCalendar>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.add ( temporalDurationLike [ , options ] )
 */
static bool ZonedDateTime_add(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurationToZonedDateTime(cx, TemporalAddDuration::Add, args);
}

/**
 * Temporal.ZonedDateTime.prototype.add ( temporalDurationLike [ , options ] )
 */
static bool ZonedDateTime_add(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_add>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.subtract ( temporalDurationLike [ , options
 * ] )
 */
static bool ZonedDateTime_subtract(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurationToZonedDateTime(cx, TemporalAddDuration::Subtract, args);
}

/**
 * Temporal.ZonedDateTime.prototype.subtract ( temporalDurationLike [ , options
 * ] )
 */
static bool ZonedDateTime_subtract(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_subtract>(cx,
                                                                       args);
}

/**
 * Temporal.ZonedDateTime.prototype.until ( other [ , options ] )
 */
static bool ZonedDateTime_until(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalZonedDateTime(cx, TemporalDifference::Until, args);
}

/**
 * Temporal.ZonedDateTime.prototype.until ( other [ , options ] )
 */
static bool ZonedDateTime_until(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_until>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.since ( other [ , options ] )
 */
static bool ZonedDateTime_since(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalZonedDateTime(cx, TemporalDifference::Since, args);
}

/**
 * Temporal.ZonedDateTime.prototype.since ( other [ , options ] )
 */
static bool ZonedDateTime_since(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_since>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.round ( roundTo )
 */
static bool ZonedDateTime_round(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

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

    // Steps 6-8 and 10-12. (Implicit)
  } else {
    // Steps 3 and 5.a
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
    // Step 13.a.
    auto* result = CreateTemporalZonedDateTime(
        cx, zonedDateTime.epochNanoseconds(), zonedDateTime.timeZone(),
        zonedDateTime.calendar());
    if (!result) {
      return false;
    }

    args.rval().setObject(*result);
    return true;
  }

  // Step 14.
  auto thisNs = zonedDateTime.epochNanoseconds();

  // Step 15.
  auto timeZone = zonedDateTime.timeZone();

  // Step 16.
  auto calendar = zonedDateTime.calendar();

  // Step 17.
  ISODateTime isoDateTime;
  if (!GetISODateTimeFor(cx, timeZone, thisNs, &isoDateTime)) {
    return false;
  }

  // Steps 18-19.
  EpochNanoseconds epochNanoseconds;
  if (smallestUnit == TemporalUnit::Day) {
    // Step 18.a.
    const auto& dateStart = isoDateTime.date;

    // Step 18.b.
    auto dateEnd = BalanceISODate(dateStart, 1);
    if (!ISODateWithinLimits(dateEnd)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
      return false;
    }

    // Step 18.c.
    EpochNanoseconds startNs;
    if (!GetStartOfDay(cx, timeZone, dateStart, &startNs)) {
      return false;
    }

    // Step 18.d.
    MOZ_ASSERT(thisNs >= startNs);

    // Step 18.e.
    EpochNanoseconds endNs;
    if (!GetStartOfDay(cx, timeZone, dateEnd, &endNs)) {
      return false;
    }

    // Step 18.f.
    MOZ_ASSERT(thisNs < endNs);

    // Step 18.g.
    auto dayLengthNs = endNs - startNs;
    MOZ_ASSERT(IsValidEpochDuration(dayLengthNs));
    MOZ_ASSERT(dayLengthNs > EpochDuration{}, "dayLengthNs is positive");

    // Step 18.h. (Inlined TimeDurationFromEpochNanosecondsDifference)
    auto dayProgressNs = thisNs - startNs;
    MOZ_ASSERT(IsValidEpochDuration(dayProgressNs));
    MOZ_ASSERT(dayProgressNs >= EpochDuration{},
               "dayProgressNs is non-negative");

    MOZ_ASSERT(startNs <= thisNs && thisNs < endNs);
    MOZ_ASSERT(dayProgressNs < dayLengthNs);
    MOZ_ASSERT(dayLengthNs <= EpochDuration::fromDays(2),
               "maximum day length for repeated days");

    // Step 18.i. (Inlined RoundTimeDurationToIncrement)
    auto rounded = RoundNumberToIncrement(
        static_cast<int64_t>(dayProgressNs.toNanoseconds()),
        static_cast<int64_t>(dayLengthNs.toNanoseconds()), roundingMode);
    auto roundedDaysNs = EpochDuration::fromNanoseconds(rounded);
    MOZ_ASSERT(roundedDaysNs == EpochDuration{} ||
               roundedDaysNs == dayLengthNs);
    MOZ_ASSERT(IsValidEpochDuration(roundedDaysNs));

    // Step 18.j. (Inlined AddTimeDurationToEpochNanoseconds)
    epochNanoseconds = startNs + roundedDaysNs;
    MOZ_ASSERT(epochNanoseconds == startNs || epochNanoseconds == endNs);
  } else {
    // Step 19.a.
    auto roundResult = RoundISODateTime(isoDateTime, roundingIncrement,
                                        smallestUnit, roundingMode);

    // Step 19.b.
    int64_t offsetNanoseconds;
    if (!GetOffsetNanosecondsFor(cx, timeZone, thisNs, &offsetNanoseconds)) {
      return false;
    }
    MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

    // Step 19.c.
    if (!InterpretISODateTimeOffset(
            cx, roundResult, OffsetBehaviour::Option, offsetNanoseconds,
            timeZone, TemporalDisambiguation::Compatible,
            TemporalOffset::Prefer, MatchBehaviour::MatchExactly,
            &epochNanoseconds)) {
      return false;
    }
  }
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  // Step 20.
  auto* result =
      CreateTemporalZonedDateTime(cx, epochNanoseconds, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.round ( roundTo )
 */
static bool ZonedDateTime_round(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_round>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.equals ( other )
 */
static bool ZonedDateTime_equals(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  Rooted<ZonedDateTime> other(cx);
  if (!ToTemporalZonedDateTime(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 4-6.
  bool equals = zonedDateTime.epochNanoseconds() == other.epochNanoseconds() &&
                TimeZoneEquals(zonedDateTime.timeZone(), other.timeZone()) &&
                CalendarEquals(zonedDateTime.calendar(), other.calendar());

  args.rval().setBoolean(equals);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.equals ( other )
 */
static bool ZonedDateTime_equals(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_equals>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.toString ( [ options ] )
 */
static bool ZonedDateTime_toString(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  SecondsStringPrecision precision = {Precision::Auto(),
                                      TemporalUnit::Nanosecond, Increment{1}};
  auto roundingMode = TemporalRoundingMode::Trunc;
  auto showCalendar = ShowCalendar::Auto;
  auto showTimeZone = ShowTimeZoneName::Auto;
  auto showOffset = ShowOffset::Auto;
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
    if (!GetTemporalShowOffsetOption(cx, options, &showOffset)) {
      return false;
    }

    // Step 8.
    if (!GetRoundingModeOption(cx, options, &roundingMode)) {
      return false;
    }

    // Step 9.
    auto smallestUnit = TemporalUnit::Auto;
    if (!GetTemporalUnitValuedOption(cx, options, TemporalUnitKey::SmallestUnit,
                                     TemporalUnitGroup::Time, &smallestUnit)) {
      return false;
    }

    // Step 10.
    if (smallestUnit == TemporalUnit::Hour) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INVALID_UNIT_OPTION, "hour",
                                "smallestUnit");
      return false;
    }

    // Step 11.
    if (!GetTemporalShowTimeZoneNameOption(cx, options, &showTimeZone)) {
      return false;
    }

    // Step 12.
    precision = ToSecondsStringPrecision(smallestUnit, digits);
  }

  // Step 13.
  JSString* str = TemporalZonedDateTimeToString(
      cx, zonedDateTime, precision.precision, showCalendar, showTimeZone,
      showOffset, precision.increment, precision.unit, roundingMode);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toString ( [ options ] )
 */
static bool ZonedDateTime_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toString>(cx,
                                                                       args);
}

/**
 * Temporal.ZonedDateTime.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool ZonedDateTime_toLocaleString(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Steps 3-6.
  Handle<PropertyName*> required = cx->names().any;
  Handle<PropertyName*> defaults = cx->names().all;
  Rooted<Value> timeZone(cx,
                         StringValue(zonedDateTime.timeZone().identifier()));
  return TemporalObjectToLocaleString(cx, args, required, defaults, timeZone);
}

/**
 * Temporal.ZonedDateTime.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool ZonedDateTime_toLocaleString(JSContext* cx, unsigned argc,
                                         Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toLocaleString>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.toJSON ( )
 */
static bool ZonedDateTime_toJSON(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  JSString* str = TemporalZonedDateTimeToString(
      cx, zonedDateTime, Precision::Auto(), ShowCalendar::Auto,
      ShowTimeZoneName::Auto, ShowOffset::Auto);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toJSON ( )
 */
static bool ZonedDateTime_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toJSON>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.valueOf ( )
 */
static bool ZonedDateTime_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                            "ZonedDateTime", "primitive type");
  return false;
}

/**
 * Temporal.ZonedDateTime.prototype.startOfDay ( )
 */
static bool ZonedDateTime_startOfDay(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  auto timeZone = zonedDateTime.timeZone();

  // Step 4.
  auto calendar = zonedDateTime.calendar();

  // Step 5.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, timeZone, zonedDateTime.epochNanoseconds(),
                         &dateTime)) {
    return false;
  }

  // Step 6.
  EpochNanoseconds epochNs;
  if (!GetStartOfDay(cx, timeZone, dateTime.date, &epochNs)) {
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
 * Temporal.ZonedDateTime.prototype.startOfDay ( )
 */
static bool ZonedDateTime_startOfDay(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_startOfDay>(cx,
                                                                         args);
}

/**
 * Temporal.ZonedDateTime.prototype.getTimeZoneTransition ( directionParam )
 */
static bool ZonedDateTime_getTimeZoneTransition(JSContext* cx,
                                                const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  auto timeZone = zonedDateTime.timeZone();

  // Steps 4-7.
  auto direction = Direction::Next;
  if (args.get(0).isString()) {
    // Steps 5 and 7.
    Rooted<JSString*> directionString(cx, args[0].toString());
    if (!GetDirectionOption(cx, directionString, &direction)) {
      return false;
    }
  } else {
    // Steps 4 and 6.
    Rooted<JSObject*> options(cx, RequireObjectArg(cx, "getTimeZoneTransition",
                                                   "direction", args.get(0)));
    if (!options) {
      return false;
    }

    // Step 7.
    if (!GetDirectionOption(cx, options, &direction)) {
      return false;
    }
  }

  // Step 8.
  if (timeZone.isOffset()) {
    args.rval().setNull();
    return true;
  }

  // Steps 9-10.
  mozilla::Maybe<EpochNanoseconds> transition;
  if (direction == Direction::Next) {
    if (!GetNamedTimeZoneNextTransition(
            cx, timeZone, zonedDateTime.epochNanoseconds(), &transition)) {
      return false;
    }
  } else {
    if (!GetNamedTimeZonePreviousTransition(
            cx, timeZone, zonedDateTime.epochNanoseconds(), &transition)) {
      return false;
    }
  }

  // Step 11.
  if (!transition) {
    args.rval().setNull();
    return true;
  }

  // Step 12.
  auto* result = CreateTemporalZonedDateTime(cx, *transition, timeZone,
                                             zonedDateTime.calendar());
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.getTimeZoneTransition ( directionParam )
 */
static bool ZonedDateTime_getTimeZoneTransition(JSContext* cx, unsigned argc,
                                                Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime,
                              ZonedDateTime_getTimeZoneTransition>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.toInstant ( )
 */
static bool ZonedDateTime_toInstant(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto epochNs = zonedDateTime->epochNanoseconds();

  // Step 3.
  auto* result = CreateTemporalInstant(cx, epochNs);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toInstant ( )
 */
static bool ZonedDateTime_toInstant(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toInstant>(cx,
                                                                        args);
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainDate ( )
 */
static bool ZonedDateTime_toPlainDate(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime temporalDateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &temporalDateTime)) {
    return false;
  }

  // Step 4.
  auto* result =
      CreateTemporalDate(cx, temporalDateTime.date, zonedDateTime.calendar());
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainDate ( )
 */
static bool ZonedDateTime_toPlainDate(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toPlainDate>(cx,
                                                                          args);
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainTime ( )
 */
static bool ZonedDateTime_toPlainTime(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime temporalDateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &temporalDateTime)) {
    return false;
  }

  // Step 4.
  auto* result = CreateTemporalTime(cx, temporalDateTime.time);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainTime ( )
 */
static bool ZonedDateTime_toPlainTime(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toPlainTime>(cx,
                                                                          args);
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainDateTime ( )
 */
static bool ZonedDateTime_toPlainDateTime(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  ISODateTime dateTime;
  if (!GetISODateTimeFor(cx, zonedDateTime.timeZone(),
                         zonedDateTime.epochNanoseconds(), &dateTime)) {
    return false;
  }
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));

  // Step 4.
  auto* result = CreateTemporalDateTime(cx, dateTime, zonedDateTime.calendar());
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainDateTime ( )
 */
static bool ZonedDateTime_toPlainDateTime(JSContext* cx, unsigned argc,
                                          Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toPlainDateTime>(
      cx, args);
}

const JSClass ZonedDateTimeObject::class_ = {
    "Temporal.ZonedDateTime",
    JSCLASS_HAS_RESERVED_SLOTS(ZonedDateTimeObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_ZonedDateTime),
    JS_NULL_CLASS_OPS,
    &ZonedDateTimeObject::classSpec_,
};

const JSClass& ZonedDateTimeObject::protoClass_ = PlainObject::class_;

static const JSFunctionSpec ZonedDateTime_methods[] = {
    JS_FN("from", ZonedDateTime_from, 1, 0),
    JS_FN("compare", ZonedDateTime_compare, 2, 0),
    JS_FS_END,
};

static const JSFunctionSpec ZonedDateTime_prototype_methods[] = {
    JS_FN("with", ZonedDateTime_with, 1, 0),
    JS_FN("withPlainTime", ZonedDateTime_withPlainTime, 0, 0),
    JS_FN("withTimeZone", ZonedDateTime_withTimeZone, 1, 0),
    JS_FN("withCalendar", ZonedDateTime_withCalendar, 1, 0),
    JS_FN("add", ZonedDateTime_add, 1, 0),
    JS_FN("subtract", ZonedDateTime_subtract, 1, 0),
    JS_FN("until", ZonedDateTime_until, 1, 0),
    JS_FN("since", ZonedDateTime_since, 1, 0),
    JS_FN("round", ZonedDateTime_round, 1, 0),
    JS_FN("equals", ZonedDateTime_equals, 1, 0),
    JS_FN("toString", ZonedDateTime_toString, 0, 0),
    JS_FN("toLocaleString", ZonedDateTime_toLocaleString, 0, 0),
    JS_FN("toJSON", ZonedDateTime_toJSON, 0, 0),
    JS_FN("valueOf", ZonedDateTime_valueOf, 0, 0),
    JS_FN("startOfDay", ZonedDateTime_startOfDay, 0, 0),
    JS_FN("getTimeZoneTransition", ZonedDateTime_getTimeZoneTransition, 1, 0),
    JS_FN("toInstant", ZonedDateTime_toInstant, 0, 0),
    JS_FN("toPlainDate", ZonedDateTime_toPlainDate, 0, 0),
    JS_FN("toPlainTime", ZonedDateTime_toPlainTime, 0, 0),
    JS_FN("toPlainDateTime", ZonedDateTime_toPlainDateTime, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec ZonedDateTime_prototype_properties[] = {
    JS_PSG("calendarId", ZonedDateTime_calendarId, 0),
    JS_PSG("timeZoneId", ZonedDateTime_timeZoneId, 0),
    JS_PSG("era", ZonedDateTime_era, 0),
    JS_PSG("eraYear", ZonedDateTime_eraYear, 0),
    JS_PSG("year", ZonedDateTime_year, 0),
    JS_PSG("month", ZonedDateTime_month, 0),
    JS_PSG("monthCode", ZonedDateTime_monthCode, 0),
    JS_PSG("day", ZonedDateTime_day, 0),
    JS_PSG("hour", ZonedDateTime_hour, 0),
    JS_PSG("minute", ZonedDateTime_minute, 0),
    JS_PSG("second", ZonedDateTime_second, 0),
    JS_PSG("millisecond", ZonedDateTime_millisecond, 0),
    JS_PSG("microsecond", ZonedDateTime_microsecond, 0),
    JS_PSG("nanosecond", ZonedDateTime_nanosecond, 0),
    JS_PSG("epochMilliseconds", ZonedDateTime_epochMilliseconds, 0),
    JS_PSG("epochNanoseconds", ZonedDateTime_epochNanoseconds, 0),
    JS_PSG("dayOfWeek", ZonedDateTime_dayOfWeek, 0),
    JS_PSG("dayOfYear", ZonedDateTime_dayOfYear, 0),
    JS_PSG("weekOfYear", ZonedDateTime_weekOfYear, 0),
    JS_PSG("yearOfWeek", ZonedDateTime_yearOfWeek, 0),
    JS_PSG("hoursInDay", ZonedDateTime_hoursInDay, 0),
    JS_PSG("daysInWeek", ZonedDateTime_daysInWeek, 0),
    JS_PSG("daysInMonth", ZonedDateTime_daysInMonth, 0),
    JS_PSG("daysInYear", ZonedDateTime_daysInYear, 0),
    JS_PSG("monthsInYear", ZonedDateTime_monthsInYear, 0),
    JS_PSG("inLeapYear", ZonedDateTime_inLeapYear, 0),
    JS_PSG("offsetNanoseconds", ZonedDateTime_offsetNanoseconds, 0),
    JS_PSG("offset", ZonedDateTime_offset, 0),
    JS_STRING_SYM_PS(toStringTag, "Temporal.ZonedDateTime", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec ZonedDateTimeObject::classSpec_ = {
    GenericCreateConstructor<ZonedDateTimeConstructor, 2,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<ZonedDateTimeObject>,
    ZonedDateTime_methods,
    nullptr,
    ZonedDateTime_prototype_methods,
    ZonedDateTime_prototype_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};
