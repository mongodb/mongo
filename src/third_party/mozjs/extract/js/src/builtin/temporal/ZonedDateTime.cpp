/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/ZonedDateTime.h"

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"

#include <cstdlib>
#include <limits>
#include <utility>

#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/Int96.h"
#include "builtin/temporal/PlainDate.h"
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
#include "ds/IdValuePair.h"
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "js/AllocPolicy.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/ComparisonOperators.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/GCVector.h"
#include "js/Id.h"
#include "js/Printer.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "js/Value.h"
#include "vm/BigIntType.h"
#include "vm/BytecodeUtil.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/ObjectOperations.h"
#include "vm/PlainObject.h"
#include "vm/StringType.h"

#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"

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
 * InterpretISODateTimeOffset ( year, month, day, hour, minute, second,
 * millisecond, microsecond, nanosecond, offsetBehaviour, offsetNanoseconds,
 * timeZoneRec, disambiguation, offsetOption, matchBehaviour )
 */
bool js::temporal::InterpretISODateTimeOffset(
    JSContext* cx, const PlainDateTime& dateTime,
    OffsetBehaviour offsetBehaviour, int64_t offsetNanoseconds,
    Handle<TimeZoneRecord> timeZone, TemporalDisambiguation disambiguation,
    TemporalOffset offsetOption, MatchBehaviour matchBehaviour,
    Instant* result) {
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Step 1.
  MOZ_ASSERT(IsValidISODate(dateTime.date));

  // Step 2.
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetOffsetNanosecondsFor));

  // Step 3.
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetPossibleInstantsFor));

  // Step 4.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  Rooted<PlainDateTimeWithCalendar> temporalDateTime(cx);
  if (!CreateTemporalDateTime(cx, dateTime, calendar, &temporalDateTime)) {
    return false;
  }

  // Step 5.
  if (offsetBehaviour == OffsetBehaviour::Wall ||
      (offsetBehaviour == OffsetBehaviour::Option &&
       offsetOption == TemporalOffset::Ignore)) {
    // Steps 5.a-b.
    return GetInstantFor(cx, timeZone, temporalDateTime, disambiguation,
                         result);
  }

  // Step 6.
  if (offsetBehaviour == OffsetBehaviour::Exact ||
      (offsetBehaviour == OffsetBehaviour::Option &&
       offsetOption == TemporalOffset::Use)) {
    // Step 6.a.
    auto epochNanoseconds = GetUTCEpochNanoseconds(
        dateTime, InstantSpan::fromNanoseconds(offsetNanoseconds));

    // Step 6.b.
    if (!IsValidEpochInstant(epochNanoseconds)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INSTANT_INVALID);
      return false;
    }

    // Step 6.c.
    *result = epochNanoseconds;
    return true;
  }

  // Step 7.
  MOZ_ASSERT(offsetBehaviour == OffsetBehaviour::Option);

  // Step 8.
  MOZ_ASSERT(offsetOption == TemporalOffset::Prefer ||
             offsetOption == TemporalOffset::Reject);

  // Step 9.
  Rooted<InstantVector> possibleInstants(cx, InstantVector(cx));
  if (!GetPossibleInstantsFor(cx, timeZone, temporalDateTime,
                              &possibleInstants)) {
    return false;
  }

  // Step 10.
  if (!possibleInstants.empty()) {
    // Step 10.a.
    Rooted<Wrapped<InstantObject*>> candidate(cx);
    for (size_t i = 0; i < possibleInstants.length(); i++) {
      candidate = possibleInstants[i];

      // Step 10.a.i.
      int64_t candidateNanoseconds;
      if (!GetOffsetNanosecondsFor(cx, timeZone, candidate,
                                   &candidateNanoseconds)) {
        return false;
      }
      MOZ_ASSERT(std::abs(candidateNanoseconds) <
                 ToNanoseconds(TemporalUnit::Day));

      // Step 10.a.ii.
      if (candidateNanoseconds == offsetNanoseconds) {
        auto* unwrapped = candidate.unwrap(cx);
        if (!unwrapped) {
          return false;
        }

        *result = ToInstant(unwrapped);
        return true;
      }

      // Step 10.a.iii.
      if (matchBehaviour == MatchBehaviour::MatchMinutes) {
        // Step 10.a.iii.1.
        int64_t roundedCandidateNanoseconds =
            RoundNanosecondsToMinutesIncrement(candidateNanoseconds);

        // Step 10.a.iii.2.
        if (roundedCandidateNanoseconds == offsetNanoseconds) {
          auto* unwrapped = candidate.unwrap(cx);
          if (!unwrapped) {
            return false;
          }

          // Step 10.a.iii.2.a.
          *result = ToInstant(unwrapped);
          return true;
        }
      }
    }
  }

  // Step 11.
  if (offsetOption == TemporalOffset::Reject) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_ZONED_DATE_TIME_NO_TIME_FOUND);
    return false;
  }

  // Step 12.
  Rooted<Wrapped<InstantObject*>> instant(cx);
  if (!DisambiguatePossibleInstants(cx, possibleInstants, timeZone,
                                    ToPlainDateTime(temporalDateTime),
                                    disambiguation, &instant)) {
    return false;
  }

  auto* unwrappedInstant = instant.unwrap(cx);
  if (!unwrappedInstant) {
    return false;
  }

  // Step 13.
  *result = ToInstant(unwrappedInstant);
  return true;
}

/**
 * ToTemporalZonedDateTime ( item [ , options ] )
 */
static bool ToTemporalZonedDateTime(JSContext* cx, Handle<Value> item,
                                    Handle<JSObject*> maybeOptions,
                                    MutableHandle<ZonedDateTime> result) {
  // Step 1. (Not applicable in our implementation)

  // Step 2.
  Rooted<PlainObject*> maybeResolvedOptions(cx);
  if (maybeOptions) {
    maybeResolvedOptions = SnapshotOwnProperties(cx, maybeOptions);
    if (!maybeResolvedOptions) {
      return false;
    }
  }

  // Step 3.
  auto offsetBehaviour = OffsetBehaviour::Option;

  // Step 4.
  auto matchBehaviour = MatchBehaviour::MatchExactly;

  // Step 7. (Reordered)
  int64_t offsetNanoseconds = 0;

  // Step 5.
  Rooted<CalendarValue> calendar(cx);
  Rooted<TimeZoneValue> timeZone(cx);
  PlainDateTime dateTime;
  auto disambiguation = TemporalDisambiguation::Compatible;
  auto offsetOption = TemporalOffset::Reject;
  if (item.isObject()) {
    Rooted<JSObject*> itemObj(cx, &item.toObject());

    // Step 5.a.
    if (auto* zonedDateTime = itemObj->maybeUnwrapIf<ZonedDateTimeObject>()) {
      auto instant = ToInstant(zonedDateTime);
      Rooted<TimeZoneValue> timeZone(cx, zonedDateTime->timeZone());
      Rooted<CalendarValue> calendar(cx, zonedDateTime->calendar());

      if (!timeZone.wrap(cx)) {
        return false;
      }
      if (!calendar.wrap(cx)) {
        return false;
      }

      result.set(ZonedDateTime{instant, timeZone, calendar});
      return true;
    }

    // Step 5.b.
    if (!GetTemporalCalendarWithISODefault(cx, itemObj, &calendar)) {
      return false;
    }

    // Step 5.c.
    Rooted<CalendarRecord> calendarRec(cx);
    if (!CreateCalendarMethodsRecord(cx, calendar,
                                     {
                                         CalendarMethod::DateFromFields,
                                         CalendarMethod::Fields,
                                     },
                                     &calendarRec)) {
      return false;
    }

    // Step 5.d.
    Rooted<PlainObject*> fields(
        cx, PrepareCalendarFields(cx, calendarRec, itemObj,
                                  {
                                      CalendarField::Day,
                                      CalendarField::Month,
                                      CalendarField::MonthCode,
                                      CalendarField::Year,
                                  },
                                  {
                                      TemporalField::Hour,
                                      TemporalField::Microsecond,
                                      TemporalField::Millisecond,
                                      TemporalField::Minute,
                                      TemporalField::Nanosecond,
                                      TemporalField::Offset,
                                      TemporalField::Second,
                                      TemporalField::TimeZone,
                                  },
                                  {TemporalField::TimeZone}));
    if (!fields) {
      return false;
    }

    // Step 5.e.
    Rooted<Value> timeZoneValue(cx);
    if (!GetProperty(cx, fields, fields, cx->names().timeZone,
                     &timeZoneValue)) {
      return false;
    }

    // Step 5.f.
    if (!ToTemporalTimeZone(cx, timeZoneValue, &timeZone)) {
      return false;
    }

    // Step 5.g.
    Rooted<Value> offsetValue(cx);
    if (!GetProperty(cx, fields, fields, cx->names().offset, &offsetValue)) {
      return false;
    }

    // Step 5.h.
    MOZ_ASSERT(offsetValue.isString() || offsetValue.isUndefined());

    // Step 5.i.
    Rooted<JSString*> offsetString(cx);
    if (offsetValue.isString()) {
      offsetString = offsetValue.toString();
    } else {
      offsetBehaviour = OffsetBehaviour::Wall;
    }

    if (maybeResolvedOptions) {
      // Steps 5.j-k.
      if (!GetTemporalDisambiguationOption(cx, maybeResolvedOptions,
                                           &disambiguation)) {
        return false;
      }

      // Step 5.l.
      if (!GetTemporalOffsetOption(cx, maybeResolvedOptions, &offsetOption)) {
        return false;
      }

      // Step 5.m.
      if (!InterpretTemporalDateTimeFields(cx, calendarRec, fields,
                                           maybeResolvedOptions, &dateTime)) {
        return false;
      }
    } else {
      // Steps 5.j-l. (Not applicable)

      // Step 5.k.
      if (!InterpretTemporalDateTimeFields(cx, calendarRec, fields,
                                           &dateTime)) {
        return false;
      }
    }

    // Step 8.
    if (offsetBehaviour == OffsetBehaviour::Option) {
      if (!ParseDateTimeUTCOffset(cx, offsetString, &offsetNanoseconds)) {
        return false;
      }
    }
  } else {
    // Step 6.a.
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

    // Steps 6.b-c.
    bool isUTC;
    bool hasOffset;
    int64_t timeZoneOffset;
    Rooted<ParsedTimeZone> timeZoneAnnotation(cx);
    Rooted<JSString*> calendarString(cx);
    if (!ParseTemporalZonedDateTimeString(
            cx, string, &dateTime, &isUTC, &hasOffset, &timeZoneOffset,
            &timeZoneAnnotation, &calendarString)) {
      return false;
    }

    // Step 6.d.
    MOZ_ASSERT(timeZoneAnnotation);

    // Step 6.e.
    if (!ToTemporalTimeZone(cx, timeZoneAnnotation, &timeZone)) {
      return false;
    }

    // Step 6.f. (Not applicable in our implementation.)

    // Step 6.g.
    if (isUTC) {
      offsetBehaviour = OffsetBehaviour::Exact;
    }

    // Step 6.h.
    else if (!hasOffset) {
      offsetBehaviour = OffsetBehaviour::Wall;
    }

    // Steps 6.i-l.
    if (calendarString) {
      if (!ToBuiltinCalendar(cx, calendarString, &calendar)) {
        return false;
      }
    } else {
      calendar.set(CalendarValue(CalendarId::ISO8601));
    }

    // Step 6.m.
    matchBehaviour = MatchBehaviour::MatchMinutes;

    if (maybeResolvedOptions) {
      // Step 6.n.
      if (!GetTemporalDisambiguationOption(cx, maybeResolvedOptions,
                                           &disambiguation)) {
        return false;
      }

      // Step 6.o.
      if (!GetTemporalOffsetOption(cx, maybeResolvedOptions, &offsetOption)) {
        return false;
      }

      // Step 6.p.
      TemporalOverflow ignored;
      if (!GetTemporalOverflowOption(cx, maybeResolvedOptions, &ignored)) {
        return false;
      }
    }

    // Step 8.
    if (offsetBehaviour == OffsetBehaviour::Option) {
      MOZ_ASSERT(hasOffset);
      offsetNanoseconds = timeZoneOffset;
    }
  }

  // Step 9.
  Rooted<TimeZoneRecord> timeZoneRec(cx);
  if (!CreateTimeZoneMethodsRecord(cx, timeZone,
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZoneRec)) {
    return false;
  }

  // Step 10.
  Instant epochNanoseconds;
  if (!InterpretISODateTimeOffset(
          cx, dateTime, offsetBehaviour, offsetNanoseconds, timeZoneRec,
          disambiguation, offsetOption, matchBehaviour, &epochNanoseconds)) {
    return false;
  }

  // Step 11.
  result.set(ZonedDateTime{epochNanoseconds, timeZone, calendar});
  return true;
}

/**
 * ToTemporalZonedDateTime ( item [ , options ] )
 */
static bool ToTemporalZonedDateTime(JSContext* cx, Handle<Value> item,
                                    MutableHandle<ZonedDateTime> result) {
  return ToTemporalZonedDateTime(cx, item, nullptr, result);
}

/**
 * ToTemporalZonedDateTime ( item [ , options ] )
 */
static ZonedDateTimeObject* ToTemporalZonedDateTime(
    JSContext* cx, Handle<Value> item, Handle<JSObject*> maybeOptions) {
  Rooted<ZonedDateTime> result(cx);
  if (!ToTemporalZonedDateTime(cx, item, maybeOptions, &result)) {
    return nullptr;
  }
  return CreateTemporalZonedDateTime(cx, result.instant(), result.timeZone(),
                                     result.calendar());
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

  auto* obj = NewObjectWithClassProto<ZonedDateTimeObject>(cx, proto);
  if (!obj) {
    return nullptr;
  }

  // Step 4.
  auto instant = ToInstant(epochNanoseconds);
  obj->setFixedSlot(ZonedDateTimeObject::SECONDS_SLOT,
                    NumberValue(instant.seconds));
  obj->setFixedSlot(ZonedDateTimeObject::NANOSECONDS_SLOT,
                    Int32Value(instant.nanoseconds));

  // Step 5.
  obj->setFixedSlot(ZonedDateTimeObject::TIMEZONE_SLOT, timeZone.toSlotValue());

  // Step 6.
  obj->setFixedSlot(ZonedDateTimeObject::CALENDAR_SLOT, calendar.toSlotValue());

  // Step 7.
  return obj;
}

/**
 * CreateTemporalZonedDateTime ( epochNanoseconds, timeZone, calendar [ ,
 * newTarget ] )
 */
ZonedDateTimeObject* js::temporal::CreateTemporalZonedDateTime(
    JSContext* cx, const Instant& instant, Handle<TimeZoneValue> timeZone,
    Handle<CalendarValue> calendar) {
  // Step 1.
  MOZ_ASSERT(IsValidEpochInstant(instant));

  // Steps 2-3.
  auto* obj = NewBuiltinClassInstance<ZonedDateTimeObject>(cx);
  if (!obj) {
    return nullptr;
  }

  // Step 4.
  obj->setFixedSlot(ZonedDateTimeObject::SECONDS_SLOT,
                    NumberValue(instant.seconds));
  obj->setFixedSlot(ZonedDateTimeObject::NANOSECONDS_SLOT,
                    Int32Value(instant.nanoseconds));

  // Step 5.
  obj->setFixedSlot(ZonedDateTimeObject::TIMEZONE_SLOT, timeZone.toSlotValue());

  // Step 6.
  obj->setFixedSlot(ZonedDateTimeObject::CALENDAR_SLOT, calendar.toSlotValue());

  // Step 7.
  return obj;
}

struct PlainDateTimeAndInstant {
  PlainDateTime dateTime;
  Instant instant;
};

/**
 * AddDaysToZonedDateTime ( instant, dateTime, timeZoneRec, calendar, days [ ,
 * overflow ] )
 */
static bool AddDaysToZonedDateTime(JSContext* cx, const Instant& instant,
                                   const PlainDateTime& dateTime,
                                   Handle<TimeZoneRecord> timeZone,
                                   Handle<CalendarValue> calendar, int64_t days,
                                   TemporalOverflow overflow,
                                   PlainDateTimeAndInstant* result) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2. (Not applicable)

  // Step 3.
  if (days == 0) {
    *result = {dateTime, instant};
    return true;
  }

  // Step 4.
  PlainDate addedDate;
  if (!AddISODate(cx, dateTime.date, {0, 0, 0, days}, overflow, &addedDate)) {
    return false;
  }

  // Step 5.
  Rooted<PlainDateTimeWithCalendar> dateTimeResult(cx);
  if (!CreateTemporalDateTime(cx, {addedDate, dateTime.time}, calendar,
                              &dateTimeResult)) {
    return false;
  }

  // Step 6.
  Instant instantResult;
  if (!GetInstantFor(cx, timeZone, dateTimeResult,
                     TemporalDisambiguation::Compatible, &instantResult)) {
    return false;
  }

  // Step 7.
  *result = {ToPlainDateTime(dateTimeResult), instantResult};
  return true;
}

/**
 * AddDaysToZonedDateTime ( instant, dateTime, timeZoneRec, calendar, days [ ,
 * overflow ] )
 */
bool js::temporal::AddDaysToZonedDateTime(
    JSContext* cx, const Instant& instant, const PlainDateTime& dateTime,
    Handle<TimeZoneRecord> timeZone, Handle<CalendarValue> calendar,
    int64_t days, TemporalOverflow overflow, Instant* result) {
  // Steps 1-7.
  PlainDateTimeAndInstant dateTimeAndInstant;
  if (!::AddDaysToZonedDateTime(cx, instant, dateTime, timeZone, calendar, days,
                                overflow, &dateTimeAndInstant)) {
    return false;
  }

  *result = dateTimeAndInstant.instant;
  return true;
}

/**
 * AddDaysToZonedDateTime ( instant, dateTime, timeZoneRec, calendar, days [ ,
 * overflow ] )
 */
bool js::temporal::AddDaysToZonedDateTime(JSContext* cx, const Instant& instant,
                                          const PlainDateTime& dateTime,
                                          Handle<TimeZoneRecord> timeZone,
                                          Handle<CalendarValue> calendar,
                                          int64_t days, Instant* result) {
  // Step 2.
  auto overflow = TemporalOverflow::Constrain;

  // Steps 1 and 3-7.
  return AddDaysToZonedDateTime(cx, instant, dateTime, timeZone, calendar, days,
                                overflow, result);
}

/**
 * AddZonedDateTime ( epochNanoseconds, timeZoneRec, calendarRec, years, months,
 * weeks, days, norm [ , precalculatedPlainDateTime [ , options ] ] )
 */
static bool AddZonedDateTime(JSContext* cx, const Instant& epochNanoseconds,
                             Handle<TimeZoneRecord> timeZone,
                             Handle<CalendarRecord> calendar,
                             const NormalizedDuration& duration,
                             mozilla::Maybe<const PlainDateTime&> dateTime,
                             Handle<JSObject*> maybeOptions, Instant* result) {
  MOZ_ASSERT(IsValidEpochInstant(epochNanoseconds));
  MOZ_ASSERT(IsValidDuration(duration));

  // Step 1.
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetPossibleInstantsFor));

  // Steps 2-3.
  MOZ_ASSERT_IF(!dateTime,
                TimeZoneMethodsRecordHasLookedUp(
                    timeZone, TimeZoneMethod::GetOffsetNanosecondsFor));

  // Steps 4-5. (Not applicable in our implementation)

  // Step 6.
  if (duration.date == DateDuration{}) {
    // Step 6.a.
    return AddInstant(cx, epochNanoseconds, duration.time, result);
  }

  // Step 7. (Not applicable in our implementation)

  // Steps 8-9.
  PlainDateTime temporalDateTime;
  if (dateTime) {
    // Step 8.a.
    temporalDateTime = *dateTime;
  } else {
    // Step 9.a.
    if (!GetPlainDateTimeFor(cx, timeZone, epochNanoseconds,
                             &temporalDateTime)) {
      return false;
    }
  }
  auto& [date, time] = temporalDateTime;

  // Step 10.
  if (duration.date.years == 0 && duration.date.months == 0 &&
      duration.date.weeks == 0) {
    // Step 10.a.
    auto overflow = TemporalOverflow::Constrain;
    if (maybeOptions) {
      if (!GetTemporalOverflowOption(cx, maybeOptions, &overflow)) {
        return false;
      }
    }

    // Step 10.b.
    Instant intermediate;
    if (!AddDaysToZonedDateTime(cx, epochNanoseconds, temporalDateTime,
                                timeZone, calendar.receiver(),
                                duration.date.days, overflow, &intermediate)) {
      return false;
    }

    // Step 10.c.
    return AddInstant(cx, intermediate, duration.time, result);
  }

  // Step 11.
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));

  // Step 12.
  const auto& datePart = date;

  // Step 13.
  const auto& dateDuration = duration.date;

  // Step 14.
  PlainDate addedDate;
  if (maybeOptions) {
    if (!temporal::CalendarDateAdd(cx, calendar, datePart, dateDuration,
                                   maybeOptions, &addedDate)) {
      return false;
    }
  } else {
    if (!CalendarDateAdd(cx, calendar, datePart, dateDuration, &addedDate)) {
      return false;
    }
  }

  // Step 15.
  Rooted<PlainDateTimeWithCalendar> intermediateDateTime(cx);
  if (!CreateTemporalDateTime(cx, {addedDate, time}, calendar.receiver(),
                              &intermediateDateTime)) {
    return false;
  }

  // Step 16.
  Instant intermediateInstant;
  if (!GetInstantFor(cx, timeZone, intermediateDateTime,
                     TemporalDisambiguation::Compatible,
                     &intermediateInstant)) {
    return false;
  }

  // Step 17.
  return AddInstant(cx, intermediateInstant, duration.time, result);
}

/**
 * AddZonedDateTime ( epochNanoseconds, timeZoneRec, calendarRec, years, months,
 * weeks, days, norm [ , precalculatedPlainDateTime [ , options ] ] )
 */
static bool AddZonedDateTime(JSContext* cx, const Instant& epochNanoseconds,
                             Handle<TimeZoneRecord> timeZone,
                             Handle<CalendarRecord> calendar,
                             const NormalizedDuration& duration,
                             Handle<JSObject*> maybeOptions, Instant* result) {
  return ::AddZonedDateTime(cx, epochNanoseconds, timeZone, calendar, duration,
                            mozilla::Nothing(), maybeOptions, result);
}

/**
 * AddZonedDateTime ( epochNanoseconds, timeZoneRec, calendarRec, years, months,
 * weeks, days, norm [ , precalculatedPlainDateTime [ , options ] ] )
 */
bool js::temporal::AddZonedDateTime(JSContext* cx,
                                    const Instant& epochNanoseconds,
                                    Handle<TimeZoneRecord> timeZone,
                                    Handle<CalendarRecord> calendar,
                                    const NormalizedDuration& duration,
                                    Instant* result) {
  return ::AddZonedDateTime(cx, epochNanoseconds, timeZone, calendar, duration,
                            mozilla::Nothing(), nullptr, result);
}

/**
 * AddZonedDateTime ( epochNanoseconds, timeZoneRec, calendarRec, years, months,
 * weeks, days, norm [ , precalculatedPlainDateTime [ , options ] ] )
 */
bool js::temporal::AddZonedDateTime(JSContext* cx,
                                    const Instant& epochNanoseconds,
                                    Handle<TimeZoneRecord> timeZone,
                                    Handle<CalendarRecord> calendar,
                                    const NormalizedDuration& duration,
                                    const PlainDateTime& dateTime,
                                    Instant* result) {
  return ::AddZonedDateTime(cx, epochNanoseconds, timeZone, calendar, duration,
                            mozilla::SomeRef(dateTime), nullptr, result);
}

/**
 * AddZonedDateTime ( epochNanoseconds, timeZoneRec, calendarRec, years, months,
 * weeks, days, norm [ , precalculatedPlainDateTime [ , options ] ] )
 */
bool js::temporal::AddZonedDateTime(JSContext* cx,
                                    const Instant& epochNanoseconds,
                                    Handle<TimeZoneRecord> timeZone,
                                    Handle<CalendarRecord> calendar,
                                    const DateDuration& duration,
                                    Instant* result) {
  return ::AddZonedDateTime(cx, epochNanoseconds, timeZone, calendar,
                            {duration, {}}, mozilla::Nothing(), nullptr,
                            result);
}

/**
 * AddZonedDateTime ( epochNanoseconds, timeZoneRec, calendarRec, years, months,
 * weeks, days, norm [ , precalculatedPlainDateTime [ , options ] ] )
 */
bool js::temporal::AddZonedDateTime(JSContext* cx,
                                    const Instant& epochNanoseconds,
                                    Handle<TimeZoneRecord> timeZone,
                                    Handle<CalendarRecord> calendar,
                                    const DateDuration& duration,
                                    const PlainDateTime& dateTime,
                                    Instant* result) {
  return ::AddZonedDateTime(cx, epochNanoseconds, timeZone, calendar,
                            {duration, {}}, mozilla::SomeRef(dateTime), nullptr,
                            result);
}

/**
 * NormalizedTimeDurationToDays ( norm, zonedRelativeTo, timeZoneRec [ ,
 * precalculatedPlainDateTime ] )
 */
static bool NormalizedTimeDurationToDays(
    JSContext* cx, const NormalizedTimeDuration& duration,
    Handle<ZonedDateTime> zonedRelativeTo, Handle<TimeZoneRecord> timeZone,
    mozilla::Maybe<const PlainDateTime&> precalculatedPlainDateTime,
    NormalizedTimeAndDays* result) {
  MOZ_ASSERT(IsValidNormalizedTimeDuration(duration));

  // Step 1.
  int32_t sign = NormalizedTimeDurationSign(duration);

  // Step 2.
  if (sign == 0) {
    *result = {int64_t(0), int64_t(0), ToNanoseconds(TemporalUnit::Day)};
    return true;
  }

  // Step 3.
  const auto& startNs = zonedRelativeTo.instant();

  // Step 5.
  Instant endNs;
  if (!AddInstant(cx, startNs, duration, &endNs)) {
    return false;
  }

  // Steps 4 and 7.
  PlainDateTime startDateTime;
  if (!precalculatedPlainDateTime) {
    if (!GetPlainDateTimeFor(cx, timeZone, startNs, &startDateTime)) {
      return false;
    }
  } else {
    startDateTime = *precalculatedPlainDateTime;
  }

  // Steps 6 and 8.
  PlainDateTime endDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, endNs, &endDateTime)) {
    return false;
  }

  // Steps 9-10. (Not applicable in our implementation.)

  // Step 11.
  int32_t days = DaysUntil(startDateTime.date, endDateTime.date);
  MOZ_ASSERT(std::abs(days) <= MaxEpochDaysDuration);

  // Step 12.
  int32_t timeSign = CompareTemporalTime(startDateTime.time, endDateTime.time);

  // Steps 13-14.
  if (days > 0 && timeSign > 0) {
    days -= 1;
  } else if (days < 0 && timeSign < 0) {
    days += 1;
  }

  // Step 15.
  PlainDateTimeAndInstant relativeResult;
  if (!::AddDaysToZonedDateTime(cx, startNs, startDateTime, timeZone,
                                zonedRelativeTo.calendar(), days,
                                TemporalOverflow::Constrain, &relativeResult)) {
    return false;
  }
  MOZ_ASSERT(IsValidISODateTime(relativeResult.dateTime));
  MOZ_ASSERT(IsValidEpochInstant(relativeResult.instant));

  // Step 16.
  if (sign > 0 && days > 0 && relativeResult.instant > endNs) {
    // Step 16.a.
    days -= 1;

    // Step 16.b.
    if (!::AddDaysToZonedDateTime(
            cx, startNs, startDateTime, timeZone, zonedRelativeTo.calendar(),
            days, TemporalOverflow::Constrain, &relativeResult)) {
      return false;
    }
    MOZ_ASSERT(IsValidISODateTime(relativeResult.dateTime));
    MOZ_ASSERT(IsValidEpochInstant(relativeResult.instant));

    // Step 16.c.
    if (days > 0 && relativeResult.instant > endNs) {
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr,
          JSMSG_TEMPORAL_ZONED_DATE_TIME_INCONSISTENT_INSTANT);
      return false;
    }
    MOZ_ASSERT_IF(days > 0, relativeResult.instant <= endNs);
  }

  MOZ_ASSERT_IF(days == 0, relativeResult.instant == startNs);

  // Step 17. (Inlined NormalizedTimeDurationFromEpochNanosecondsDifference)
  auto ns = endNs - relativeResult.instant;
  MOZ_ASSERT(IsValidInstantSpan(ns));

  // Step 18.
  PlainDateTimeAndInstant oneDayFarther;
  if (!::AddDaysToZonedDateTime(cx, relativeResult.instant,
                                relativeResult.dateTime, timeZone,
                                zonedRelativeTo.calendar(), sign,
                                TemporalOverflow::Constrain, &oneDayFarther)) {
    return false;
  }
  MOZ_ASSERT(IsValidISODateTime(oneDayFarther.dateTime));
  MOZ_ASSERT(IsValidEpochInstant(oneDayFarther.instant));

  // Step 19. (Inlined NormalizedTimeDurationFromEpochNanosecondsDifference)
  auto dayLengthNs = oneDayFarther.instant - relativeResult.instant;
  MOZ_ASSERT(IsValidInstantSpan(dayLengthNs));

  // clang-format off
  //
  // ns = endNs - relativeResult.instant
  // dayLengthNs = oneDayFarther.instant - relativeResult.instant
  // oneDayLess = ns - dayLengthNs
  //            = (endNs - relativeResult.instant) - (oneDayFarther.instant - relativeResult.instant)
  //            = endNs - relativeResult.instant - oneDayFarther.instant + relativeResult.instant
  //            = endNs - oneDayFarther.instant
  //
  // |endNs| and |oneDayFarther.instant| are both valid epoch instant values,
  // so the difference |oneDayLess| is a valid epoch instant difference value.
  //
  // clang-format on

  // Step 20. (Inlined SubtractNormalizedTimeDuration)
  auto oneDayLess = ns - dayLengthNs;
  MOZ_ASSERT(IsValidInstantSpan(oneDayLess));
  MOZ_ASSERT(oneDayLess == (endNs - oneDayFarther.instant));

  // Step 21.
  if (oneDayLess == InstantSpan{} ||
      ((oneDayLess < InstantSpan{}) == (sign < 0))) {
    // Step 21.a.
    ns = oneDayLess;

    // Step 21.b.
    relativeResult = oneDayFarther;

    // Step 21.c.
    days += sign;

    // Step 21.d.
    PlainDateTimeAndInstant oneDayFarther;
    if (!::AddDaysToZonedDateTime(
            cx, relativeResult.instant, relativeResult.dateTime, timeZone,
            zonedRelativeTo.calendar(), sign, TemporalOverflow::Constrain,
            &oneDayFarther)) {
      return false;
    }
    MOZ_ASSERT(IsValidISODateTime(oneDayFarther.dateTime));
    MOZ_ASSERT(IsValidEpochInstant(oneDayFarther.instant));

    // Step 21.e. (Inlined NormalizedTimeDurationFromEpochNanosecondsDifference)
    dayLengthNs = oneDayFarther.instant - relativeResult.instant;
    MOZ_ASSERT(IsValidInstantSpan(dayLengthNs));

    // clang-format off
    //
    // ns = oneDayLess'
    //    = endNs - oneDayFarther.instant'
    // relativeResult.instant = oneDayFarther.instant'
    // dayLengthNs = oneDayFarther.instant - relativeResult.instant
    //             = oneDayFarther.instant - oneDayFarther.instant'
    // oneDayLess = ns - dayLengthNs
    //            = (endNs - oneDayFarther.instant') - (oneDayFarther.instant - oneDayFarther.instant')
    //            = endNs - oneDayFarther.instant' - oneDayFarther.instant + oneDayFarther.instant'
    //            = endNs - oneDayFarther.instant
    //
    // Where |oneDayLess'| and |oneDayFarther.instant'| denote the variables
    // from before this if-statement block.
    //
    // |endNs| and |oneDayFarther.instant| are both valid epoch instant values,
    // so the difference |oneDayLess| is a valid epoch instant difference value.
    //
    // clang-format on

    // Step 21.f.
    auto oneDayLess = ns - dayLengthNs;
    if (oneDayLess == InstantSpan{} ||
        ((oneDayLess < InstantSpan{}) == (sign < 0))) {
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr,
          JSMSG_TEMPORAL_ZONED_DATE_TIME_INCONSISTENT_INSTANT);
      return false;
    }
  }

  // Step 22.
  if (days < 0 && sign > 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_ZONED_DATE_TIME_INCORRECT_SIGN,
                              "days");
    return false;
  }

  // Step 23.
  if (days > 0 && sign < 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_ZONED_DATE_TIME_INCORRECT_SIGN,
                              "days");
    return false;
  }

  MOZ_ASSERT(IsValidInstantSpan(dayLengthNs));
  MOZ_ASSERT(IsValidInstantSpan(ns));

  // Steps 24-25.
  if (sign < 0) {
    if (ns > InstantSpan{}) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_ZONED_DATE_TIME_INCORRECT_SIGN,
                                "nanoseconds");
      return false;
    }
  } else {
    MOZ_ASSERT(ns >= InstantSpan{});
  }

  // Steps 26-27.
  dayLengthNs = dayLengthNs.abs();
  MOZ_ASSERT(ns.abs() < dayLengthNs);

  // Step 28.
  constexpr auto maxDayLength = Int128{1} << 53;
  auto dayLengthNanos = dayLengthNs.toNanoseconds();
  if (dayLengthNanos >= maxDayLength) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_ZONED_DATE_TIME_INCORRECT_SIGN,
                              "days");
    return false;
  }

  auto timeNanos = ns.toNanoseconds();
  MOZ_ASSERT(timeNanos == Int128{int64_t(timeNanos)},
             "abs(ns) < dayLengthNs < 2**53 implies that |ns| fits in int64");

  // Step 29.
  static_assert(std::numeric_limits<decltype(days)>::max() <=
                ((int64_t(1) << 53) / (24 * 60 * 60)));

  // Step 30.
  *result = {int64_t(days), int64_t(timeNanos), int64_t(dayLengthNanos)};
  return true;
}

/**
 * NormalizedTimeDurationToDays ( norm, zonedRelativeTo, timeZoneRec [ ,
 * precalculatedPlainDateTime ] )
 */
bool js::temporal::NormalizedTimeDurationToDays(
    JSContext* cx, const NormalizedTimeDuration& duration,
    Handle<ZonedDateTime> zonedRelativeTo, Handle<TimeZoneRecord> timeZone,
    NormalizedTimeAndDays* result) {
  return ::NormalizedTimeDurationToDays(cx, duration, zonedRelativeTo, timeZone,
                                        mozilla::Nothing(), result);
}

/**
 * NormalizedTimeDurationToDays ( norm, zonedRelativeTo, timeZoneRec [ ,
 * precalculatedPlainDateTime ] )
 */
bool js::temporal::NormalizedTimeDurationToDays(
    JSContext* cx, const NormalizedTimeDuration& duration,
    Handle<ZonedDateTime> zonedRelativeTo, Handle<TimeZoneRecord> timeZone,
    const PlainDateTime& precalculatedPlainDateTime,
    NormalizedTimeAndDays* result) {
  return ::NormalizedTimeDurationToDays(
      cx, duration, zonedRelativeTo, timeZone,
      mozilla::SomeRef(precalculatedPlainDateTime), result);
}

/**
 * DifferenceZonedDateTime ( ns1, ns2, timeZoneRec, calendarRec, largestUnit,
 * options, precalculatedPlainDateTime )
 */
static bool DifferenceZonedDateTime(
    JSContext* cx, const Instant& ns1, const Instant& ns2,
    Handle<TimeZoneRecord> timeZone, Handle<CalendarRecord> calendar,
    TemporalUnit largestUnit, Handle<PlainObject*> maybeOptions,
    const PlainDateTime& precalculatedPlainDateTime,
    NormalizedDuration* result) {
  MOZ_ASSERT(IsValidEpochInstant(ns1));
  MOZ_ASSERT(IsValidEpochInstant(ns2));

  // Steps 1.
  if (ns1 == ns2) {
    *result = CreateNormalizedDurationRecord({}, {});
    return true;
  }

  // FIXME: spec issue - precalculatedPlainDateTime is never undefined
  // https://github.com/tc39/proposal-temporal/issues/2822

  // Steps 2-3.
  const auto& startDateTime = precalculatedPlainDateTime;

  // Steps 4-5.
  PlainDateTime endDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, ns2, &endDateTime)) {
    return false;
  }

  // Step 6.
  int32_t sign = (ns2 - ns1 < InstantSpan{}) ? -1 : 1;

  // Step 7.
  int32_t maxDayCorrection = 1 + (sign > 0);

  // Step 8.
  int32_t dayCorrection = 0;

  // Step 9.
  auto timeDuration = DifferenceTime(startDateTime.time, endDateTime.time);

  // Step 10.
  if (NormalizedTimeDurationSign(timeDuration) == -sign) {
    dayCorrection += 1;
  }

  // Steps 11-12.
  Rooted<PlainDateTimeWithCalendar> intermediateDateTime(cx);
  while (dayCorrection <= maxDayCorrection) {
    // Step 12.a.
    auto intermediateDate =
        BalanceISODate(endDateTime.date.year, endDateTime.date.month,
                       endDateTime.date.day - dayCorrection * sign);

    // FIXME: spec issue - CreateTemporalDateTime is fallible
    // https://github.com/tc39/proposal-temporal/issues/2824

    // Step 12.b.
    if (!CreateTemporalDateTime(cx, {intermediateDate, startDateTime.time},
                                calendar.receiver(), &intermediateDateTime)) {
      return false;
    }

    // Steps 12.c-d.
    Instant intermediateInstant;
    if (!GetInstantFor(cx, timeZone, intermediateDateTime,
                       TemporalDisambiguation::Compatible,
                       &intermediateInstant)) {
      return false;
    }

    // Step 12.e.
    auto norm = NormalizedTimeDurationFromEpochNanosecondsDifference(
        ns2, intermediateInstant);

    // Step 12.f.
    int32_t timeSign = NormalizedTimeDurationSign(norm);

    // Step 12.g.
    if (sign != -timeSign) {
      // Step 13.a.
      const auto& date1 = startDateTime.date;
      MOZ_ASSERT(ISODateTimeWithinLimits(date1));

      // Step 13.b.
      const auto& date2 = intermediateDate;
      MOZ_ASSERT(ISODateTimeWithinLimits(date2));

      // Step 13.c.
      auto dateLargestUnit = std::min(largestUnit, TemporalUnit::Day);

      // Steps 13.d-e.
      //
      // The spec performs an unnecessary copy operation. As an optimization, we
      // omit this copy.
      auto untilOptions = maybeOptions;

      // Step 13.f.
      DateDuration dateDifference;
      if (untilOptions) {
        if (!DifferenceDate(cx, calendar, date1, date2, dateLargestUnit,
                            untilOptions, &dateDifference)) {
          return false;
        }
      } else {
        if (!DifferenceDate(cx, calendar, date1, date2, dateLargestUnit,
                            &dateDifference)) {
          return false;
        }
      }

      // Step 13.g.
      return CreateNormalizedDurationRecord(cx, dateDifference, norm, result);
    }

    // Step 12.h.
    dayCorrection += 1;
  }

  // Steps 14-15.
  JS_ReportErrorNumberASCII(
      cx, GetErrorMessage, nullptr,
      JSMSG_TEMPORAL_ZONED_DATE_TIME_INCONSISTENT_INSTANT);
  return false;
}

/**
 * DifferenceZonedDateTime ( ns1, ns2, timeZoneRec, calendarRec, largestUnit,
 * options, precalculatedPlainDateTime )
 */
bool js::temporal::DifferenceZonedDateTime(
    JSContext* cx, const Instant& ns1, const Instant& ns2,
    Handle<TimeZoneRecord> timeZone, Handle<CalendarRecord> calendar,
    TemporalUnit largestUnit, const PlainDateTime& precalculatedPlainDateTime,
    NormalizedDuration* result) {
  return ::DifferenceZonedDateTime(cx, ns1, ns2, timeZone, calendar,
                                   largestUnit, nullptr,
                                   precalculatedPlainDateTime, result);
}

/**
 * TimeZoneEquals ( one, two )
 */
static bool TimeZoneEqualsOrThrow(JSContext* cx, Handle<TimeZoneValue> one,
                                  Handle<TimeZoneValue> two) {
  // Step 1.
  if (one.isObject() && two.isObject() && one.toObject() == two.toObject()) {
    return true;
  }

  // Step 2.
  Rooted<JSString*> timeZoneOne(cx, ToTemporalTimeZoneIdentifier(cx, one));
  if (!timeZoneOne) {
    return false;
  }

  // Step 3.
  Rooted<JSString*> timeZoneTwo(cx, ToTemporalTimeZoneIdentifier(cx, two));
  if (!timeZoneTwo) {
    return false;
  }

  // Steps 4-9.
  bool equals;
  if (!TimeZoneEquals(cx, timeZoneOne, timeZoneTwo, &equals)) {
    return false;
  }
  if (equals) {
    return true;
  }

  // Throw an error when the time zone identifiers don't match. Used when
  // unequal time zones throw a RangeError.
  if (auto charsOne = QuoteString(cx, timeZoneOne)) {
    if (auto charsTwo = QuoteString(cx, timeZoneTwo)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_TIMEZONE_INCOMPATIBLE,
                               charsOne.get(), charsTwo.get());
    }
  }
  return false;
}

/**
 * DifferenceTemporalZonedDateTime ( operation, zonedDateTime, other, options )
 */
static bool DifferenceTemporalZonedDateTime(JSContext* cx,
                                            TemporalDifference operation,
                                            const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Rooted<ZonedDateTime> other(cx);
  if (!ToTemporalZonedDateTime(cx, args.get(0), &other)) {
    return false;
  }

  // Step 3.
  if (!CalendarEqualsOrThrow(cx, zonedDateTime.calendar(), other.calendar())) {
    return false;
  }

  // Steps 4-5.
  Rooted<PlainObject*> resolvedOptions(cx);
  DifferenceSettings settings;
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
    if (!GetDifferenceSettings(
            cx, operation, resolvedOptions, TemporalUnitGroup::DateTime,
            TemporalUnit::Nanosecond, TemporalUnit::Hour, &settings)) {
      return false;
    }
  } else {
    // Steps 4-5.
    settings = {
        TemporalUnit::Nanosecond,
        TemporalUnit::Hour,
        TemporalRoundingMode::Trunc,
        Increment{1},
    };
  }

  // Step 6.
  if (settings.largestUnit > TemporalUnit::Day) {
    MOZ_ASSERT(settings.smallestUnit >= settings.largestUnit);

    // Step 6.a.
    auto difference = DifferenceInstant(
        zonedDateTime.instant(), other.instant(), settings.roundingIncrement,
        settings.smallestUnit, settings.roundingMode);

    // Step 6.b.
    TimeDuration balancedTime;
    if (!BalanceTimeDuration(cx, difference, settings.largestUnit,
                             &balancedTime)) {
      return false;
    }

    // Step 6.c.
    auto duration = balancedTime.toDuration();
    if (operation == TemporalDifference::Since) {
      duration = duration.negate();
    }

    auto* result = CreateTemporalDuration(cx, duration);
    if (!result) {
      return false;
    }

    args.rval().setObject(*result);
    return true;
  }

  // Steps 7-8.
  if (!TimeZoneEqualsOrThrow(cx, zonedDateTime.timeZone(), other.timeZone())) {
    return false;
  }

  // Step 9.
  if (zonedDateTime.instant() == other.instant()) {
    auto* obj = CreateTemporalDuration(cx, {});
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Step 10.
  Rooted<TimeZoneRecord> timeZone(cx);
  if (!CreateTimeZoneMethodsRecord(cx, zonedDateTime.timeZone(),
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZone)) {
    return false;
  }

  // Step 11.
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, zonedDateTime.calendar(),
                                   {
                                       CalendarMethod::DateAdd,
                                       CalendarMethod::DateUntil,
                                   },
                                   &calendar)) {
    return false;
  }

  // Steps 12-13.
  PlainDateTime precalculatedPlainDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, zonedDateTime.instant(),
                           &precalculatedPlainDateTime)) {
    return false;
  }

  // Step 14.
  Rooted<PlainDateObject*> plainRelativeTo(
      cx, CreateTemporalDate(cx, precalculatedPlainDateTime.date,
                             calendar.receiver()));
  if (!plainRelativeTo) {
    return false;
  }

  // Step 15.
  NormalizedDuration difference;
  if (!::DifferenceZonedDateTime(cx, zonedDateTime.instant(), other.instant(),
                                 timeZone, calendar, settings.largestUnit,
                                 resolvedOptions, precalculatedPlainDateTime,
                                 &difference)) {
    return false;
  }

  // Step 16.
  bool roundingGranularityIsNoop =
      settings.smallestUnit == TemporalUnit::Nanosecond &&
      settings.roundingIncrement == Increment{1};

  // Step 17.
  if (!roundingGranularityIsNoop) {
    // Steps 17.a-b.
    NormalizedDuration roundResult;
    if (!RoundDuration(cx, difference, settings.roundingIncrement,
                       settings.smallestUnit, settings.roundingMode,
                       plainRelativeTo, calendar, zonedDateTime, timeZone,
                       precalculatedPlainDateTime, &roundResult)) {
      return false;
    }

    // Step 17.c.
    NormalizedTimeAndDays timeAndDays;
    if (!NormalizedTimeDurationToDays(cx, roundResult.time, zonedDateTime,
                                      timeZone, &timeAndDays)) {
      return false;
    }

    // Step 17.d.
    int64_t days = roundResult.date.days + timeAndDays.days;

    // Step 17.e.
    auto toAdjust = NormalizedDuration{
        {
            roundResult.date.years,
            roundResult.date.months,
            roundResult.date.weeks,
            days,
        },
        NormalizedTimeDuration::fromNanoseconds(timeAndDays.time),
    };
    NormalizedDuration adjustResult;
    if (!AdjustRoundedDurationDays(cx, toAdjust, settings.roundingIncrement,
                                   settings.smallestUnit, settings.roundingMode,
                                   zonedDateTime, calendar, timeZone,
                                   precalculatedPlainDateTime, &adjustResult)) {
      return false;
    }

    // Step 17.f.
    DateDuration balanceResult;
    if (!temporal::BalanceDateDurationRelative(
            cx, adjustResult.date, settings.largestUnit, settings.smallestUnit,
            plainRelativeTo, calendar, &balanceResult)) {
      return false;
    }

    // Step 17.g.
    if (!CombineDateAndNormalizedTimeDuration(cx, balanceResult,
                                              adjustResult.time, &difference)) {
      return false;
    }
  }

  // Step 18.
  auto timeDuration = BalanceTimeDuration(difference.time, TemporalUnit::Hour);

  // Step 19.
  auto duration = Duration{
      double(difference.date.years), double(difference.date.months),
      double(difference.date.weeks), double(difference.date.days),
      double(timeDuration.hours),    double(timeDuration.minutes),
      double(timeDuration.seconds),  double(timeDuration.milliseconds),
      timeDuration.microseconds,     timeDuration.nanoseconds,
  };
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

enum class ZonedDateTimeDuration { Add, Subtract };

/**
 * AddDurationToOrSubtractDurationFromZonedDateTime ( operation, zonedDateTime,
 * temporalDurationLike, options )
 */
static bool AddDurationToOrSubtractDurationFromZonedDateTime(
    JSContext* cx, ZonedDateTimeDuration operation, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, &args.thisv().toObject().as<ZonedDateTimeObject>());

  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Duration duration;
  if (!ToTemporalDurationRecord(cx, args.get(0), &duration)) {
    return false;
  }

  // Step 3.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(1)) {
    const char* name =
        operation == ZonedDateTimeDuration::Add ? "add" : "subtract";
    options = RequireObjectArg(cx, "options", name, args[1]);
  } else {
    options = NewPlainObjectWithProto(cx, nullptr);
  }
  if (!options) {
    return false;
  }

  // Step 4.
  Rooted<TimeZoneRecord> timeZone(cx);
  if (!CreateTimeZoneMethodsRecord(cx, zonedDateTime.timeZone(),
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZone)) {
    return false;
  }

  // Step 5.
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, zonedDateTime.calendar(),
                                   {
                                       CalendarMethod::DateAdd,
                                   },
                                   &calendar)) {
    return false;
  }

  // Step 6.
  if (operation == ZonedDateTimeDuration::Subtract) {
    duration = duration.negate();
  }
  auto normalized = CreateNormalizedDurationRecord(duration);

  // Step 7.
  Instant resultInstant;
  if (!::AddZonedDateTime(cx, zonedDateTime.instant(), timeZone, calendar,
                          normalized, options, &resultInstant)) {
    return false;
  }
  MOZ_ASSERT(IsValidEpochInstant(resultInstant));

  // Step 8.
  auto* result = CreateTemporalZonedDateTime(
      cx, resultInstant, timeZone.receiver(), calendar.receiver());
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime ( epochNanoseconds, timeZoneLike [ , calendarLike ] )
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
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZone(cx, args.get(1), &timeZone)) {
    return false;
  }

  // Step 5.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendarWithISODefault(cx, args.get(2), &calendar)) {
    return false;
  }

  // Step 6.
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
    if (auto* zonedDateTime = item->maybeUnwrapIf<ZonedDateTimeObject>()) {
      auto epochInstant = ToInstant(zonedDateTime);
      Rooted<TimeZoneValue> timeZone(cx, zonedDateTime->timeZone());
      Rooted<CalendarValue> calendar(cx, zonedDateTime->calendar());

      if (!timeZone.wrap(cx)) {
        return false;
      }
      if (!calendar.wrap(cx)) {
        return false;
      }

      if (options) {
        // Steps 2.a-b.
        TemporalDisambiguation ignoredDisambiguation;
        if (!GetTemporalDisambiguationOption(cx, options,
                                             &ignoredDisambiguation)) {
          return false;
        }

        // Step 2.c.
        TemporalOffset ignoredOffset;
        if (!GetTemporalOffsetOption(cx, options, &ignoredOffset)) {
          return false;
        }

        // Step 2.d.
        TemporalOverflow ignoredOverflow;
        if (!GetTemporalOverflowOption(cx, options, &ignoredOverflow)) {
          return false;
        }
      }

      // Step 2.e.
      auto* result =
          CreateTemporalZonedDateTime(cx, epochInstant, timeZone, calendar);
      if (!result) {
        return false;
      }

      args.rval().setObject(*result);
      return true;
    }
  }

  // Step 3.
  auto* result = ToTemporalZonedDateTime(cx, args.get(0), options);
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
  const auto& oneNs = one.instant();
  const auto& twoNs = two.instant();
  args.rval().setInt32(oneNs > twoNs ? 1 : oneNs < twoNs ? -1 : 0);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.calendarId
 */
static bool ZonedDateTime_calendarId(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  Rooted<CalendarValue> calendar(cx, zonedDateTime->calendar());
  auto* calendarId = ToTemporalCalendarIdentifier(cx, calendar);
  if (!calendarId) {
    return false;
  }

  args.rval().setString(calendarId);
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
  Rooted<TimeZoneValue> timeZone(cx, zonedDateTime->timeZone());
  auto* timeZoneId = ToTemporalTimeZoneIdentifier(cx, timeZone);
  if (!timeZoneId) {
    return false;
  }

  args.rval().setString(timeZoneId);
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarEra(cx, zonedDateTime.calendar(), dateTime, args.rval());
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Steps 7-9.
  return CalendarEraYear(cx, zonedDateTime.calendar(), dateTime, args.rval());
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarYear(cx, zonedDateTime.calendar(), dateTime, args.rval());
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarMonth(cx, zonedDateTime.calendar(), dateTime, args.rval());
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarMonthCode(cx, zonedDateTime.calendar(), dateTime, args.rval());
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

  // Step 4. (Reordered)
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, zonedDateTime.calendar(),
                                   {
                                       CalendarMethod::Day,
                                   },
                                   &calendar)) {
    return false;
  }

  // Steps 3 and 5-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarDay(cx, calendar, dateTime, args.rval());
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
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
 * get Temporal.ZonedDateTime.prototype.epochSeconds
 */
static bool ZonedDateTime_epochSeconds(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  auto instant = ToInstant(zonedDateTime);

  // Steps 4-5.
  args.rval().setNumber(instant.seconds);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.epochSeconds
 */
static bool ZonedDateTime_epochSeconds(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_epochSeconds>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.epochMilliseconds
 */
static bool ZonedDateTime_epochMilliseconds(JSContext* cx,
                                            const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  auto instant = ToInstant(zonedDateTime);

  // Steps 4-5.
  args.rval().setNumber(instant.floorToMilliseconds());
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
 * get Temporal.ZonedDateTime.prototype.epochMicroseconds
 */
static bool ZonedDateTime_epochMicroseconds(JSContext* cx,
                                            const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  auto instant = ToInstant(zonedDateTime);

  // Step 4.
  auto* microseconds =
      BigInt::createFromInt64(cx, instant.floorToMicroseconds());
  if (!microseconds) {
    return false;
  }

  // Step 5.
  args.rval().setBigInt(microseconds);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.epochMicroseconds
 */
static bool ZonedDateTime_epochMicroseconds(JSContext* cx, unsigned argc,
                                            Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_epochMicroseconds>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.epochNanoseconds
 */
static bool ZonedDateTime_epochNanoseconds(JSContext* cx,
                                           const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  auto* nanoseconds = ToEpochNanoseconds(cx, ToInstant(zonedDateTime));
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarDayOfWeek(cx, zonedDateTime.calendar(), dateTime, args.rval());
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarDayOfYear(cx, zonedDateTime.calendar(), dateTime, args.rval());
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Steps 7-9.
  return CalendarWeekOfYear(cx, zonedDateTime.calendar(), dateTime,
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Steps 7-9.
  return CalendarYearOfWeek(cx, zonedDateTime.calendar(), dateTime,
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
  Rooted<TimeZoneRecord> timeZone(cx);
  if (!CreateTimeZoneMethodsRecord(cx, zonedDateTime.timeZone(),
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZone)) {
    return false;
  }

  // Step 4.
  const auto& instant = zonedDateTime.instant();

  // Step 5.
  PlainDateTime temporalDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &temporalDateTime)) {
    return false;
  }

  // Steps 6-8.
  const auto& date = temporalDateTime.date;
  Rooted<CalendarValue> isoCalendar(cx, CalendarValue(CalendarId::ISO8601));

  // Step 9.
  Rooted<PlainDateTimeWithCalendar> today(cx);
  if (!CreateTemporalDateTime(cx, {date, {}}, isoCalendar, &today)) {
    return false;
  }

  // Step 10.
  auto tomorrowFields = BalanceISODate(date.year, date.month, date.day + 1);

  // Step 11.
  Rooted<PlainDateTimeWithCalendar> tomorrow(cx);
  if (!CreateTemporalDateTime(cx, {tomorrowFields, {}}, isoCalendar,
                              &tomorrow)) {
    return false;
  }

  // Step 12.
  Instant todayInstant;
  if (!GetInstantFor(cx, timeZone, today, TemporalDisambiguation::Compatible,
                     &todayInstant)) {
    return false;
  }

  // Step 13.
  Instant tomorrowInstant;
  if (!GetInstantFor(cx, timeZone, tomorrow, TemporalDisambiguation::Compatible,
                     &tomorrowInstant)) {
    return false;
  }

  // Step 14.
  auto diff = tomorrowInstant - todayInstant;
  MOZ_ASSERT(IsValidInstantSpan(diff));

  // Step 15.
  constexpr auto nsPerHour = Int128{ToNanoseconds(TemporalUnit::Hour)};
  args.rval().setNumber(FractionToDouble(diff.toNanoseconds(), nsPerHour));
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarDaysInWeek(cx, zonedDateTime.calendar(), dateTime,
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarDaysInMonth(cx, zonedDateTime.calendar(), dateTime,
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarDaysInYear(cx, zonedDateTime.calendar(), dateTime,
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarMonthsInYear(cx, zonedDateTime.calendar(), dateTime,
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

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &dateTime)) {
    return false;
  }

  // Step 7.
  return CalendarInLeapYear(cx, zonedDateTime.calendar(), dateTime,
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
  auto timeZone = zonedDateTime.timeZone();

  // Step 4.
  const auto& instant = zonedDateTime.instant();

  // Step 5.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, instant, &offsetNanoseconds)) {
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
  auto timeZone = zonedDateTime.timeZone();

  // Step 4.
  const auto& instant = zonedDateTime.instant();

  // Step 5.
  JSString* str = GetOffsetStringFor(cx, timeZone, instant);
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
  if (!CreateCalendarMethodsRecord(cx, zonedDateTime.calendar(),
                                   {
                                       CalendarMethod::DateFromFields,
                                       CalendarMethod::Fields,
                                       CalendarMethod::MergeFields,
                                   },
                                   &calendar)) {
    return false;
  }

  // Step 6.
  Rooted<TimeZoneRecord> timeZone(cx);
  if (!CreateTimeZoneMethodsRecord(cx, zonedDateTime.timeZone(),
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZone)) {
    return false;
  }

  // Step 7.
  const auto& instant = zonedDateTime.instant();

  // Step 8.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, instant, &offsetNanoseconds)) {
    return false;
  }

  // Step 9.
  Rooted<PlainDateTimeObject*> dateTime(
      cx,
      GetPlainDateTimeFor(cx, instant, calendar.receiver(), offsetNanoseconds));
  if (!dateTime) {
    return false;
  }

  // Step 10.
  Rooted<PlainObject*> fields(cx);
  JS::RootedVector<PropertyKey> fieldNames(cx);
  if (!PrepareCalendarFieldsAndFieldNames(cx, calendar, dateTime,
                                          {
                                              CalendarField::Day,
                                              CalendarField::Month,
                                              CalendarField::MonthCode,
                                              CalendarField::Year,
                                          },
                                          &fields, &fieldNames)) {
    return false;
  }

  // Steps 11-16.
  struct TimeField {
    using FieldName = ImmutableTenuredPtr<PropertyName*> JSAtomState::*;

    FieldName name;
    int32_t value;
  } timeFields[] = {
      {&JSAtomState::hour, dateTime->isoHour()},
      {&JSAtomState::minute, dateTime->isoMinute()},
      {&JSAtomState::second, dateTime->isoSecond()},
      {&JSAtomState::millisecond, dateTime->isoMillisecond()},
      {&JSAtomState::microsecond, dateTime->isoMicrosecond()},
      {&JSAtomState::nanosecond, dateTime->isoNanosecond()},
  };

  Rooted<Value> timeFieldValue(cx);
  for (const auto& timeField : timeFields) {
    Handle<PropertyName*> name = cx->names().*(timeField.name);
    timeFieldValue.setInt32(timeField.value);

    if (!DefineDataProperty(cx, fields, name, timeFieldValue)) {
      return false;
    }
  }

  // Step 17.
  JSString* fieldsOffset = FormatUTCOffsetNanoseconds(cx, offsetNanoseconds);
  if (!fieldsOffset) {
    return false;
  }

  timeFieldValue.setString(fieldsOffset);
  if (!DefineDataProperty(cx, fields, cx->names().offset, timeFieldValue)) {
    return false;
  }

  // Step 18.
  if (!AppendSorted(cx, fieldNames.get(),
                    {
                        TemporalField::Hour,
                        TemporalField::Microsecond,
                        TemporalField::Millisecond,
                        TemporalField::Minute,
                        TemporalField::Nanosecond,
                        TemporalField::Offset,
                        TemporalField::Second,
                    })) {
    return false;
  }

  // Step 19.
  Rooted<PlainObject*> partialZonedDateTime(
      cx,
      PreparePartialTemporalFields(cx, temporalZonedDateTimeLike, fieldNames));
  if (!partialZonedDateTime) {
    return false;
  }

  // Step 20.
  Rooted<JSObject*> mergedFields(
      cx, CalendarMergeFields(cx, calendar, fields, partialZonedDateTime));
  if (!mergedFields) {
    return false;
  }

  // Step 21.
  fields = PrepareTemporalFields(cx, mergedFields, fieldNames,
                                 {TemporalField::Offset});
  if (!fields) {
    return false;
  }

  // Step 22-23.
  auto disambiguation = TemporalDisambiguation::Compatible;
  if (!GetTemporalDisambiguationOption(cx, resolvedOptions, &disambiguation)) {
    return false;
  }

  // Step 24.
  auto offset = TemporalOffset::Prefer;
  if (!GetTemporalOffsetOption(cx, resolvedOptions, &offset)) {
    return false;
  }

  // Step 25.
  PlainDateTime dateTimeResult;
  if (!InterpretTemporalDateTimeFields(cx, calendar, fields, resolvedOptions,
                                       &dateTimeResult)) {
    return false;
  }

  // Step 26.
  Rooted<Value> offsetString(cx);
  if (!GetProperty(cx, fields, fields, cx->names().offset, &offsetString)) {
    return false;
  }

  // Step 27.
  MOZ_ASSERT(offsetString.isString());

  // Step 28.
  Rooted<JSString*> offsetStr(cx, offsetString.toString());
  int64_t newOffsetNanoseconds;
  if (!ParseDateTimeUTCOffset(cx, offsetStr, &newOffsetNanoseconds)) {
    return false;
  }

  // Step 29.
  Instant epochNanoseconds;
  if (!InterpretISODateTimeOffset(
          cx, dateTimeResult, OffsetBehaviour::Option, newOffsetNanoseconds,
          timeZone, disambiguation, offset, MatchBehaviour::MatchExactly,
          &epochNanoseconds)) {
    return false;
  }

  // Step 30.
  auto* result = CreateTemporalZonedDateTime(
      cx, epochNanoseconds, timeZone.receiver(), calendar.receiver());
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

  // Step 3. (Inlined ToTemporalTimeOrMidnight)
  PlainTime time = {};
  if (args.hasDefined(0)) {
    if (!ToTemporalTime(cx, args[0], &time)) {
      return false;
    }
  }

  // Step 4.
  Rooted<TimeZoneRecord> timeZone(cx);
  if (!CreateTimeZoneMethodsRecord(cx, zonedDateTime.timeZone(),
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZone)) {
    return false;
  }

  // Steps 5 and 7.
  PlainDateTime plainDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, zonedDateTime.instant(),
                           &plainDateTime)) {
    return false;
  }

  // Step 6.
  auto calendar = zonedDateTime.calendar();

  // Step 8.
  Rooted<PlainDateTimeWithCalendar> resultPlainDateTime(cx);
  if (!CreateTemporalDateTime(cx, {plainDateTime.date, time}, calendar,
                              &resultPlainDateTime)) {
    return false;
  }

  // Step 9.
  Instant instant;
  if (!GetInstantFor(cx, timeZone, resultPlainDateTime,
                     TemporalDisambiguation::Compatible, &instant)) {
    return false;
  }

  // Step 10.
  auto* result =
      CreateTemporalZonedDateTime(cx, instant, timeZone.receiver(), calendar);
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
 * Temporal.ZonedDateTime.prototype.withPlainDate ( plainDateLike )
 */
static bool ZonedDateTime_withPlainDate(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  Rooted<PlainDateWithCalendar> plainDate(cx);
  if (!ToTemporalDate(cx, args.get(0), &plainDate)) {
    return false;
  }
  auto date = plainDate.date();

  // Step 4.
  Rooted<TimeZoneRecord> timeZone(cx);
  if (!CreateTimeZoneMethodsRecord(cx, zonedDateTime.timeZone(),
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZone)) {
    return false;
  }

  // Steps 5-6.
  PlainDateTime plainDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, zonedDateTime.instant(),
                           &plainDateTime)) {
    return false;
  }

  // Step 7.
  Rooted<CalendarValue> calendar(cx);
  if (!ConsolidateCalendars(cx, zonedDateTime.calendar(), plainDate.calendar(),
                            &calendar)) {
    return false;
  }

  // Step 8.
  Rooted<PlainDateTimeWithCalendar> resultPlainDateTime(cx);
  if (!CreateTemporalDateTime(cx, {date, plainDateTime.time}, calendar,
                              &resultPlainDateTime)) {
    return false;
  }

  // Step 9.
  Instant instant;
  if (!GetInstantFor(cx, timeZone, resultPlainDateTime,
                     TemporalDisambiguation::Compatible, &instant)) {
    return false;
  }

  // Step 10.
  auto* result =
      CreateTemporalZonedDateTime(cx, instant, timeZone.receiver(), calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.withPlainDate ( plainDateLike )
 */
static bool ZonedDateTime_withPlainDate(JSContext* cx, unsigned argc,
                                        Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_withPlainDate>(
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
      cx, zonedDateTime.instant(), timeZone, zonedDateTime.calendar());
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
      cx, zonedDateTime.instant(), zonedDateTime.timeZone(), calendar);
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
  return AddDurationToOrSubtractDurationFromZonedDateTime(
      cx, ZonedDateTimeDuration::Add, args);
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
  return AddDurationToOrSubtractDurationFromZonedDateTime(
      cx, ZonedDateTimeDuration::Subtract, args);
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
    auto* result = CreateTemporalZonedDateTime(cx, zonedDateTime.instant(),
                                               zonedDateTime.timeZone(),
                                               zonedDateTime.calendar());
    if (!result) {
      return false;
    }

    args.rval().setObject(*result);
    return true;
  }

  // Step 14.
  Rooted<TimeZoneRecord> timeZone(cx);
  if (!CreateTimeZoneMethodsRecord(cx, zonedDateTime.timeZone(),
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZone)) {
    return false;
  }

  // Step 16. (Reordered)
  auto calendar = zonedDateTime.calendar();

  // Steps 15 and 17.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, zonedDateTime.instant(),
                               &offsetNanoseconds)) {
    return false;
  }
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Step 18.
  auto temporalDateTime =
      GetPlainDateTimeFor(zonedDateTime.instant(), offsetNanoseconds);

  // Step 19.
  Instant epochNanoseconds;
  if (smallestUnit == TemporalUnit::Day) {
    // Step 19.a.
    Rooted<CalendarValue> isoCalendar(cx, CalendarValue(CalendarId::ISO8601));
    Rooted<PlainDateTimeWithCalendar> dtStart(cx);
    if (!CreateTemporalDateTime(cx, {temporalDateTime.date, {}}, isoCalendar,
                                &dtStart)) {
      return false;
    }

    // Step 19.b.
    auto dateEnd =
        BalanceISODate(temporalDateTime.date.year, temporalDateTime.date.month,
                       temporalDateTime.date.day + 1);

    // Step 19.c.
    Rooted<PlainDateTimeWithCalendar> dtEnd(cx);
    if (!CreateTemporalDateTime(cx, {dateEnd, {}}, isoCalendar, &dtEnd)) {
      return false;
    }

    // Step 19.d.
    const auto& thisNs = zonedDateTime.instant();

    // Steps 19.e-f.
    Instant startNs;
    if (!GetInstantFor(cx, timeZone, dtStart,
                       TemporalDisambiguation::Compatible, &startNs)) {
      return false;
    }

    // Step 19.g.
    if (thisNs < startNs) {
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr,
          JSMSG_TEMPORAL_ZONED_DATE_TIME_INCONSISTENT_INSTANT);
      return false;
    }

    // Steps 19.h-i.
    Instant endNs;
    if (!GetInstantFor(cx, timeZone, dtEnd, TemporalDisambiguation::Compatible,
                       &endNs)) {
      return false;
    }

    // Step 19.j.
    if (thisNs >= endNs) {
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr,
          JSMSG_TEMPORAL_ZONED_DATE_TIME_INCONSISTENT_INSTANT);
      return false;
    }

    // Step 19.k.
    auto dayLengthNs = endNs - startNs;
    MOZ_ASSERT(IsValidInstantSpan(dayLengthNs));
    MOZ_ASSERT(dayLengthNs > InstantSpan{}, "dayLengthNs is positive");

    // Step 19.l. (Inlined NormalizedTimeDurationFromEpochNanosecondsDifference)
    auto dayProgressNs = thisNs - startNs;
    MOZ_ASSERT(IsValidInstantSpan(dayProgressNs));
    MOZ_ASSERT(dayProgressNs >= InstantSpan{}, "dayProgressNs is non-negative");

    MOZ_ASSERT(startNs <= thisNs && thisNs < endNs);
    MOZ_ASSERT(dayProgressNs < dayLengthNs);

    // Step 19.m. (Inlined RoundNormalizedTimeDurationToIncrement)
    auto rounded =
        RoundNumberToIncrement(dayProgressNs.toNanoseconds(),
                               dayLengthNs.toNanoseconds(), roundingMode);
    auto roundedDaysNs = InstantSpan::fromNanoseconds(rounded);
    MOZ_ASSERT(roundedDaysNs == InstantSpan{} || roundedDaysNs == dayLengthNs);
    MOZ_ASSERT(IsValidInstantSpan(roundedDaysNs));

    // Step 19.n.
    epochNanoseconds = startNs + roundedDaysNs;
    MOZ_ASSERT(epochNanoseconds == startNs || epochNanoseconds == endNs);
  } else {
    // Step 20.a.
    auto roundResult = RoundISODateTime(temporalDateTime, roundingIncrement,
                                        smallestUnit, roundingMode);

    // Step 20.b.
    if (!InterpretISODateTimeOffset(
            cx, roundResult, OffsetBehaviour::Option, offsetNanoseconds,
            timeZone, TemporalDisambiguation::Compatible,
            TemporalOffset::Prefer, MatchBehaviour::MatchExactly,
            &epochNanoseconds)) {
      return false;
    }
  }
  MOZ_ASSERT(IsValidEpochInstant(epochNanoseconds));

  // Step 22.
  auto* result = CreateTemporalZonedDateTime(cx, epochNanoseconds,
                                             timeZone.receiver(), calendar);
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
  bool equals = zonedDateTime.instant() == other.instant();
  if (equals && !TimeZoneEquals(cx, zonedDateTime.timeZone(), other.timeZone(),
                                &equals)) {
    return false;
  }
  if (equals && !CalendarEquals(cx, zonedDateTime.calendar(), other.calendar(),
                                &equals)) {
    return false;
  }

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
  Rooted<TimeZoneRecord> timeZone(cx);
  if (!CreateTimeZoneMethodsRecord(cx, zonedDateTime.timeZone(),
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZone)) {
    return false;
  }

  // Step 4.
  auto calendar = zonedDateTime.calendar();

  // Step 5.
  const auto& instant = zonedDateTime.instant();

  // Steps 5-6.
  PlainDateTime temporalDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &temporalDateTime)) {
    return false;
  }

  // Step 7.
  Rooted<PlainDateTimeWithCalendar> startDateTime(cx);
  if (!CreateTemporalDateTime(cx, {temporalDateTime.date, {}}, calendar,
                              &startDateTime)) {
    return false;
  }

  // Step 8.
  Instant startInstant;
  if (!GetInstantFor(cx, timeZone, startDateTime,
                     TemporalDisambiguation::Compatible, &startInstant)) {
    return false;
  }

  // Step 9.
  auto* result = CreateTemporalZonedDateTime(cx, startInstant,
                                             timeZone.receiver(), calendar);
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
 * Temporal.ZonedDateTime.prototype.toInstant ( )
 */
static bool ZonedDateTime_toInstant(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);

  // Step 3.
  auto* result = CreateTemporalInstant(cx, instant);
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

  // Steps 3-6.
  PlainDateTime temporalDateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &temporalDateTime)) {
    return false;
  }

  // Step 7.
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

  // Steps 3-6.
  PlainDateTime temporalDateTime;
  if (!GetPlainDateTimeFor(cx, zonedDateTime.timeZone(),
                           zonedDateTime.instant(), &temporalDateTime)) {
    return false;
  }

  // Step 7.
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

  // Steps 3-5.
  auto* result =
      GetPlainDateTimeFor(cx, zonedDateTime.timeZone(), zonedDateTime.instant(),
                          zonedDateTime.calendar());
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

/**
 * Temporal.ZonedDateTime.prototype.toPlainYearMonth ( )
 */
static bool ZonedDateTime_toPlainYearMonth(JSContext* cx,
                                           const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, zonedDateTime.calendar(),
                                   {
                                       CalendarMethod::Fields,
                                       CalendarMethod::YearMonthFromFields,
                                   },
                                   &calendar)) {
    return false;
  }

  // Steps 4-6.
  Rooted<PlainDateTimeObject*> temporalDateTime(
      cx,
      GetPlainDateTimeFor(cx, zonedDateTime.timeZone(), zonedDateTime.instant(),
                          zonedDateTime.calendar()));
  if (!temporalDateTime) {
    return false;
  }

  // Step 7.
  Rooted<PlainObject*> fields(
      cx,
      PrepareCalendarFields(cx, calendar, temporalDateTime,
                            {CalendarField::MonthCode, CalendarField::Year}));
  if (!fields) {
    return false;
  }

  // Steps 8-9.
  auto result = CalendarYearMonthFromFields(cx, calendar, fields);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainYearMonth ( )
 */
static bool ZonedDateTime_toPlainYearMonth(JSContext* cx, unsigned argc,
                                           Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toPlainYearMonth>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainMonthDay ( )
 */
static bool ZonedDateTime_toPlainMonthDay(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  Rooted<CalendarRecord> calendar(cx);
  if (!CreateCalendarMethodsRecord(cx, zonedDateTime.calendar(),
                                   {
                                       CalendarMethod::Fields,
                                       CalendarMethod::MonthDayFromFields,
                                   },
                                   &calendar)) {
    return false;
  }

  // Steps 4-6.
  Rooted<PlainDateTimeObject*> temporalDateTime(
      cx,
      GetPlainDateTimeFor(cx, zonedDateTime.timeZone(), zonedDateTime.instant(),
                          zonedDateTime.calendar()));
  if (!temporalDateTime) {
    return false;
  }

  // Step 7.
  Rooted<PlainObject*> fields(
      cx,
      PrepareCalendarFields(cx, calendar, temporalDateTime,
                            {CalendarField::Day, CalendarField::MonthCode}));
  if (!fields) {
    return false;
  }

  // Steps 8-9.
  auto result = CalendarMonthDayFromFields(cx, calendar, fields);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainMonthDay ( )
 */
static bool ZonedDateTime_toPlainMonthDay(JSContext* cx, unsigned argc,
                                          Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toPlainMonthDay>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.getISOFields ( )
 */
static bool ZonedDateTime_getISOFields(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTime> zonedDateTime(
      cx, ZonedDateTime{&args.thisv().toObject().as<ZonedDateTimeObject>()});

  // Step 3.
  Rooted<IdValueVector> fields(cx, IdValueVector(cx));

  // Step 4.
  const auto& instant = zonedDateTime.instant();

  // Step 5.
  auto calendar = zonedDateTime.calendar();

  // Step 6.
  auto timeZone = zonedDateTime.timeZone();

  // Step 7.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, instant, &offsetNanoseconds)) {
    return false;
  }

  // Step 8.
  auto temporalDateTime = GetPlainDateTimeFor(instant, offsetNanoseconds);

  // Step 9.
  Rooted<JSString*> offset(cx,
                           FormatUTCOffsetNanoseconds(cx, offsetNanoseconds));
  if (!offset) {
    return false;
  }

  // Step 10.
  Rooted<Value> cal(cx);
  if (!ToTemporalCalendar(cx, calendar, &cal)) {
    return false;
  }
  if (!fields.emplaceBack(NameToId(cx->names().calendar), cal)) {
    return false;
  }

  // Step 11.
  if (!fields.emplaceBack(NameToId(cx->names().isoDay),
                          Int32Value(temporalDateTime.date.day))) {
    return false;
  }

  // Step 12.
  if (!fields.emplaceBack(NameToId(cx->names().isoHour),
                          Int32Value(temporalDateTime.time.hour))) {
    return false;
  }

  // Step 13.
  if (!fields.emplaceBack(NameToId(cx->names().isoMicrosecond),
                          Int32Value(temporalDateTime.time.microsecond))) {
    return false;
  }

  // Step 14.
  if (!fields.emplaceBack(NameToId(cx->names().isoMillisecond),
                          Int32Value(temporalDateTime.time.millisecond))) {
    return false;
  }

  // Step 15.
  if (!fields.emplaceBack(NameToId(cx->names().isoMinute),
                          Int32Value(temporalDateTime.time.minute))) {
    return false;
  }

  // Step 16.
  if (!fields.emplaceBack(NameToId(cx->names().isoMonth),
                          Int32Value(temporalDateTime.date.month))) {
    return false;
  }

  // Step 17.
  if (!fields.emplaceBack(NameToId(cx->names().isoNanosecond),
                          Int32Value(temporalDateTime.time.nanosecond))) {
    return false;
  }

  // Step 18.
  if (!fields.emplaceBack(NameToId(cx->names().isoSecond),
                          Int32Value(temporalDateTime.time.second))) {
    return false;
  }

  // Step 19.
  if (!fields.emplaceBack(NameToId(cx->names().isoYear),
                          Int32Value(temporalDateTime.date.year))) {
    return false;
  }

  // Step 20.
  if (!fields.emplaceBack(NameToId(cx->names().offset), StringValue(offset))) {
    return false;
  }

  // Step 21.
  if (!fields.emplaceBack(NameToId(cx->names().timeZone), timeZone.toValue())) {
    return false;
  }

  // Step 22.
  auto* obj = NewPlainObjectWithUniqueNames(cx, fields);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.getISOFields ( )
 */
static bool ZonedDateTime_getISOFields(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_getISOFields>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.getCalendar ( )
 */
static bool ZonedDateTime_getCalendar(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  Rooted<CalendarValue> calendar(cx, zonedDateTime->calendar());

  // Step 3.
  auto* obj = ToTemporalCalendarObject(cx, calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.getCalendar ( )
 */
static bool ZonedDateTime_getCalendar(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_getCalendar>(cx,
                                                                          args);
}

/**
 * Temporal.ZonedDateTime.prototype.getTimeZone ( )
 */
static bool ZonedDateTime_getTimeZone(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  Rooted<TimeZoneValue> timeZone(cx, zonedDateTime->timeZone());

  // Step 3.
  auto* obj = ToTemporalTimeZoneObject(cx, timeZone);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.getTimeZone ( )
 */
static bool ZonedDateTime_getTimeZone(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_getTimeZone>(cx,
                                                                          args);
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
    JS_FN("withPlainDate", ZonedDateTime_withPlainDate, 1, 0),
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
    JS_FN("toInstant", ZonedDateTime_toInstant, 0, 0),
    JS_FN("toPlainDate", ZonedDateTime_toPlainDate, 0, 0),
    JS_FN("toPlainTime", ZonedDateTime_toPlainTime, 0, 0),
    JS_FN("toPlainDateTime", ZonedDateTime_toPlainDateTime, 0, 0),
    JS_FN("toPlainYearMonth", ZonedDateTime_toPlainYearMonth, 0, 0),
    JS_FN("toPlainMonthDay", ZonedDateTime_toPlainMonthDay, 0, 0),
    JS_FN("getISOFields", ZonedDateTime_getISOFields, 0, 0),
    JS_FN("getCalendar", ZonedDateTime_getCalendar, 0, 0),
    JS_FN("getTimeZone", ZonedDateTime_getTimeZone, 0, 0),
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
    JS_PSG("epochSeconds", ZonedDateTime_epochSeconds, 0),
    JS_PSG("epochMilliseconds", ZonedDateTime_epochMilliseconds, 0),
    JS_PSG("epochMicroseconds", ZonedDateTime_epochMicroseconds, 0),
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
