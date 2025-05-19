/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/TemporalNow.h"

#include "mozilla/Assertions.h"
#include "mozilla/Result.h"

#include <cstdlib>
#include <stdint.h>
#include <string_view>
#include <utility>

#include "jsdate.h"
#include "jspubtd.h"
#include "jstypes.h"
#include "NamespaceImports.h"

#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/FormatBuffer.h"
#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainTime.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TimeZone.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "gc/Barrier.h"
#include "gc/GCEnum.h"
#include "js/AllocPolicy.h"
#include "js/CallArgs.h"
#include "js/Class.h"
#include "js/Date.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "vm/DateTime.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::temporal;

static bool SystemTimeZoneOffset(JSContext* cx, int32_t* offset) {
  auto rawOffset =
      DateTimeInfo::getRawOffsetMs(DateTimeInfo::forceUTC(cx->realm()));
  if (rawOffset.isErr()) {
    intl::ReportInternalError(cx);
    return false;
  }

  *offset = rawOffset.unwrap();
  return true;
}

/**
 * 6.4.3 DefaultTimeZone ()
 *
 * Returns the IANA time zone name for the host environment's current time zone.
 *
 * ES2017 Intl draft rev 4a23f407336d382ed5e3471200c690c9b020b5f3
 */
static JSString* SystemTimeZoneIdentifier(JSContext* cx) {
  intl::FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> formatBuffer(cx);
  auto result = DateTimeInfo::timeZoneId(DateTimeInfo::forceUTC(cx->realm()),
                                         formatBuffer);
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return nullptr;
  }

  Rooted<JSString*> timeZone(cx, formatBuffer.toString(cx));
  if (!timeZone) {
    return nullptr;
  }

  Rooted<JSAtom*> validTimeZone(cx);
  if (!IsValidTimeZoneName(cx, timeZone, &validTimeZone)) {
    return nullptr;
  }
  if (validTimeZone) {
    return CanonicalizeTimeZoneName(cx, validTimeZone);
  }

  // See DateTimeFormat.js for the JS implementation.
  // TODO: Move the JS implementation into C++.

  // Before defaulting to "UTC", try to represent the system time zone using
  // the Etc/GMT + offset format. This format only accepts full hour offsets.
  int32_t offset;
  if (!SystemTimeZoneOffset(cx, &offset)) {
    return nullptr;
  }

  constexpr int32_t msPerHour = 60 * 60 * 1000;
  int32_t offsetHours = std::abs(offset / msPerHour);
  int32_t offsetHoursFraction = offset % msPerHour;
  if (offsetHoursFraction == 0 && offsetHours < 24) {
    // Etc/GMT + offset uses POSIX-style signs, i.e. a positive offset
    // means a location west of GMT.
    constexpr std::string_view etcGMT = "Etc/GMT";

    char offsetString[etcGMT.length() + 3];

    size_t n = etcGMT.copy(offsetString, etcGMT.length());
    offsetString[n++] = offset < 0 ? '+' : '-';
    if (offsetHours >= 10) {
      offsetString[n++] = char('0' + (offsetHours / 10));
    }
    offsetString[n++] = char('0' + (offsetHours % 10));

    MOZ_ASSERT(n == etcGMT.length() + 2 || n == etcGMT.length() + 3);

    timeZone = NewStringCopyN<CanGC>(cx, offsetString, n);
    if (!timeZone) {
      return nullptr;
    }

    // Check if the fallback is valid.
    if (!IsValidTimeZoneName(cx, timeZone, &validTimeZone)) {
      return nullptr;
    }
    if (validTimeZone) {
      return CanonicalizeTimeZoneName(cx, validTimeZone);
    }
  }

  // Fallback to "UTC" if everything else fails.
  return cx->names().UTC;
}

static BuiltinTimeZoneObject* SystemTimeZoneObject(JSContext* cx) {
  Rooted<JSString*> timeZoneIdentifier(cx, SystemTimeZoneIdentifier(cx));
  if (!timeZoneIdentifier) {
    return nullptr;
  }

  return CreateTemporalTimeZone(cx, timeZoneIdentifier);
}

/**
 * SystemUTCEpochNanoseconds ( )
 */
static bool SystemUTCEpochNanoseconds(JSContext* cx, Instant* result) {
  // Step 1.
  JS::ClippedTime nowMillis = DateNow(cx);
  MOZ_ASSERT(nowMillis.isValid());

  // Step 2.
  MOZ_ASSERT(nowMillis.toDouble() >= js::StartOfTime);
  MOZ_ASSERT(nowMillis.toDouble() <= js::EndOfTime);

  // Step 3.
  *result = Instant::fromMilliseconds(int64_t(nowMillis.toDouble()));
  return true;
}

/**
 * SystemInstant ( )
 */
static bool SystemInstant(JSContext* cx, Instant* result) {
  // Steps 1-2.
  return SystemUTCEpochNanoseconds(cx, result);
}

/**
 * SystemInstant ( )
 */
static InstantObject* SystemInstant(JSContext* cx) {
  // Step 1.
  Instant instant;
  if (!SystemUTCEpochNanoseconds(cx, &instant)) {
    return nullptr;
  }

  // Step 2.
  return CreateTemporalInstant(cx, instant);
}

/**
 * SystemDateTime ( temporalTimeZoneLike, calendarLike )
 * SystemZonedDateTime ( temporalTimeZoneLike, calendarLike )
 */
static bool ToTemporalTimeZoneOrSystemTimeZone(
    JSContext* cx, Handle<Value> temporalTimeZoneLike,
    MutableHandle<TimeZoneValue> timeZone) {
  // Step 1.
  if (temporalTimeZoneLike.isUndefined()) {
    auto* timeZoneObj = SystemTimeZoneObject(cx);
    if (!timeZoneObj) {
      return false;
    }
    timeZone.set(TimeZoneValue(timeZoneObj));
    return true;
  }

  // Step 2.
  return ToTemporalTimeZone(cx, temporalTimeZoneLike, timeZone);
}

/**
 * SystemDateTime ( temporalTimeZoneLike, calendarLike )
 */
static bool SystemDateTime(JSContext* cx, Handle<TimeZoneValue> timeZone,
                           PlainDateTime* dateTime) {
  // SystemDateTime, step 4.
  Instant instant;
  if (!SystemInstant(cx, &instant)) {
    return false;
  }

  // SystemDateTime, steps 5-6.
  return GetPlainDateTimeFor(cx, timeZone, instant, dateTime);
}

/**
 * Temporal.Now.timeZoneId ( )
 */
static bool Temporal_Now_timeZoneId(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  auto* result = SystemTimeZoneIdentifier(cx);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

/**
 * Temporal.Now.instant ( )
 */
static bool Temporal_Now_instant(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  auto* result = SystemInstant(cx);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Now.plainDateTime ( calendar [ , temporalTimeZoneLike ] )
 */
static bool Temporal_Now_plainDateTime(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. (Inlined call to SystemDateTime)

  // SystemDateTime, steps 1-2.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZoneOrSystemTimeZone(cx, args.get(1), &timeZone)) {
    return false;
  }

  // SystemDateTime, step 3.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendar(cx, args.get(0), &calendar)) {
    return false;
  }

  // SystemDateTime, steps 4-5.
  PlainDateTime dateTime;
  if (!SystemDateTime(cx, timeZone, &dateTime)) {
    return false;
  }

  auto* result = CreateTemporalDateTime(cx, dateTime, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Now.plainDateTimeISO ( [ temporalTimeZoneLike ] )
 */
static bool Temporal_Now_plainDateTimeISO(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. (Inlined call to SystemDateTime)

  // SystemDateTime, steps 1-2.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZoneOrSystemTimeZone(cx, args.get(0), &timeZone)) {
    return false;
  }

  // SystemDateTime, step 3.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));

  // SystemDateTime, steps 4-5.
  PlainDateTime dateTime;
  if (!SystemDateTime(cx, timeZone, &dateTime)) {
    return false;
  }

  auto* result = CreateTemporalDateTime(cx, dateTime, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Now.zonedDateTime ( calendar [ , temporalTimeZoneLike ] )
 */
static bool Temporal_Now_zonedDateTime(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. (Inlined call to SystemZonedDateTime)

  // SystemZonedDateTime, steps 1-2.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZoneOrSystemTimeZone(cx, args.get(1), &timeZone)) {
    return false;
  }

  // SystemZonedDateTime, step 3.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendar(cx, args.get(0), &calendar)) {
    return false;
  }

  // SystemZonedDateTime, step 4.
  Instant instant;
  if (!SystemUTCEpochNanoseconds(cx, &instant)) {
    return false;
  }

  // SystemZonedDateTime, step 5.
  auto* result = CreateTemporalZonedDateTime(cx, instant, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Now.zonedDateTimeISO ( [ temporalTimeZoneLike ] )
 */
static bool Temporal_Now_zonedDateTimeISO(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. (Inlined call to SystemZonedDateTime)

  // SystemZonedDateTime, steps 1-2.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZoneOrSystemTimeZone(cx, args.get(0), &timeZone)) {
    return false;
  }

  // SystemZonedDateTime, step 3.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));

  // SystemZonedDateTime, step 4.
  Instant instant;
  if (!SystemUTCEpochNanoseconds(cx, &instant)) {
    return false;
  }

  // SystemZonedDateTime, step 5.
  auto* result = CreateTemporalZonedDateTime(cx, instant, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Now.plainDate ( calendar [ , temporalTimeZoneLike ] )
 */
static bool Temporal_Now_plainDate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. (Inlined call to SystemDateTime)

  // SystemDateTime, steps 1-2.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZoneOrSystemTimeZone(cx, args.get(1), &timeZone)) {
    return false;
  }

  // SystemDateTime, step 3.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendar(cx, args.get(0), &calendar)) {
    return false;
  }

  // SystemDateTime, steps 4-5.
  PlainDateTime dateTime;
  if (!SystemDateTime(cx, timeZone, &dateTime)) {
    return false;
  }

  // Step 2.
  auto* result = CreateTemporalDate(cx, dateTime.date, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Now.plainDateISO ( [ temporalTimeZoneLike ] )
 */
static bool Temporal_Now_plainDateISO(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. (Inlined call to SystemDateTime)

  // SystemDateTime, steps 1-2.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZoneOrSystemTimeZone(cx, args.get(0), &timeZone)) {
    return false;
  }

  // SystemDateTime, step 3.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));

  // SystemDateTime, steps 4-5.
  PlainDateTime dateTime;
  if (!SystemDateTime(cx, timeZone, &dateTime)) {
    return false;
  }

  // Step 2.
  auto* result = CreateTemporalDate(cx, dateTime.date, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Now.plainTimeISO ( [ temporalTimeZoneLike ] )
 */
static bool Temporal_Now_plainTimeISO(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. (Inlined call to SystemDateTime)

  // SystemDateTime, steps 1-2.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZoneOrSystemTimeZone(cx, args.get(0), &timeZone)) {
    return false;
  }

  // SystemDateTime, step 3. (Not applicable)

  // SystemDateTime, steps 4-5.
  PlainDateTime dateTime;
  if (!SystemDateTime(cx, timeZone, &dateTime)) {
    return false;
  }

  // Step 2.
  auto* result = CreateTemporalTime(cx, dateTime.time);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

const JSClass TemporalNowObject::class_ = {
    "Temporal.Now",
    JSCLASS_HAS_CACHED_PROTO(JSProto_TemporalNow),
    JS_NULL_CLASS_OPS,
    &TemporalNowObject::classSpec_,
};

static const JSFunctionSpec TemporalNow_methods[] = {
    JS_FN("timeZoneId", Temporal_Now_timeZoneId, 0, 0),
    JS_FN("instant", Temporal_Now_instant, 0, 0),
    JS_FN("plainDateTime", Temporal_Now_plainDateTime, 1, 0),
    JS_FN("plainDateTimeISO", Temporal_Now_plainDateTimeISO, 0, 0),
    JS_FN("zonedDateTime", Temporal_Now_zonedDateTime, 1, 0),
    JS_FN("zonedDateTimeISO", Temporal_Now_zonedDateTimeISO, 0, 0),
    JS_FN("plainDate", Temporal_Now_plainDate, 1, 0),
    JS_FN("plainDateISO", Temporal_Now_plainDateISO, 0, 0),
    JS_FN("plainTimeISO", Temporal_Now_plainTimeISO, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec TemporalNow_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Temporal.Now", JSPROP_READONLY),
    JS_PS_END,
};

static JSObject* CreateTemporalNowObject(JSContext* cx, JSProtoKey key) {
  Rooted<JSObject*> proto(cx, &cx->global()->getObjectPrototype());
  return NewTenuredObjectWithGivenProto(cx, &TemporalNowObject::class_, proto);
}

const ClassSpec TemporalNowObject::classSpec_ = {
    CreateTemporalNowObject,
    nullptr,
    TemporalNow_methods,
    TemporalNow_properties,
    nullptr,
    nullptr,
    nullptr,
    ClassSpec::DontDefineConstructor,
};
