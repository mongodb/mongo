/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/PlainTime.h"

#include "mozilla/Assertions.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "jsnum.h"
#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/intl/DateTimeFormat.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/PlainDateTime.h"
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
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/Value.h"
#include "vm/BytecodeUtil.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;
using namespace js::temporal;

static inline bool IsPlainTime(Handle<Value> v) {
  return v.isObject() && v.toObject().is<PlainTimeObject>();
}

#ifdef DEBUG
/**
 * IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
template <typename T>
static bool IsValidTime(T hour, T minute, T second, T millisecond,
                        T microsecond, T nanosecond) {
  static_assert(std::is_same_v<T, int32_t> || std::is_same_v<T, double>);

  MOZ_ASSERT(IsInteger(hour));
  MOZ_ASSERT(IsInteger(minute));
  MOZ_ASSERT(IsInteger(second));
  MOZ_ASSERT(IsInteger(millisecond));
  MOZ_ASSERT(IsInteger(microsecond));
  MOZ_ASSERT(IsInteger(nanosecond));

  // Step 1.
  if (hour < 0 || hour > 23) {
    return false;
  }

  // Step 2.
  if (minute < 0 || minute > 59) {
    return false;
  }

  // Step 3.
  if (second < 0 || second > 59) {
    return false;
  }

  // Step 4.
  if (millisecond < 0 || millisecond > 999) {
    return false;
  }

  // Step 5.
  if (microsecond < 0 || microsecond > 999) {
    return false;
  }

  // Step 6.
  if (nanosecond < 0 || nanosecond > 999) {
    return false;
  }

  // Step 7.
  return true;
}

/**
 * IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
bool js::temporal::IsValidTime(const Time& time) {
  const auto& [hour, minute, second, millisecond, microsecond, nanosecond] =
      time;
  return ::IsValidTime(hour, minute, second, millisecond, microsecond,
                       nanosecond);
}

/**
 * IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
bool js::temporal::IsValidTime(double hour, double minute, double second,
                               double millisecond, double microsecond,
                               double nanosecond) {
  return ::IsValidTime(hour, minute, second, millisecond, microsecond,
                       nanosecond);
}
#endif

static void ReportInvalidTimeValue(JSContext* cx, const char* name, int32_t min,
                                   int32_t max, double num) {
  Int32ToCStringBuf minCbuf;
  const char* minStr = Int32ToCString(&minCbuf, min);

  Int32ToCStringBuf maxCbuf;
  const char* maxStr = Int32ToCString(&maxCbuf, max);

  ToCStringBuf numCbuf;
  const char* numStr = NumberToCString(&numCbuf, num);

  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_TEMPORAL_PLAIN_TIME_INVALID_VALUE, name,
                            minStr, maxStr, numStr);
}

static inline bool ThrowIfInvalidTimeValue(JSContext* cx, const char* name,
                                           int32_t min, int32_t max,
                                           double num) {
  if (min <= num && num <= max) {
    return true;
  }
  ReportInvalidTimeValue(cx, name, min, max, num);
  return false;
}

/**
 * IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
bool js::temporal::ThrowIfInvalidTime(JSContext* cx, double hour, double minute,
                                      double second, double millisecond,
                                      double microsecond, double nanosecond) {
  // Step 1.
  MOZ_ASSERT(IsInteger(hour));
  MOZ_ASSERT(IsInteger(minute));
  MOZ_ASSERT(IsInteger(second));
  MOZ_ASSERT(IsInteger(millisecond));
  MOZ_ASSERT(IsInteger(microsecond));
  MOZ_ASSERT(IsInteger(nanosecond));

  // Step 2.
  if (!ThrowIfInvalidTimeValue(cx, "hour", 0, 23, hour)) {
    return false;
  }

  // Step 3.
  if (!ThrowIfInvalidTimeValue(cx, "minute", 0, 59, minute)) {
    return false;
  }

  // Step 4.
  if (!ThrowIfInvalidTimeValue(cx, "second", 0, 59, second)) {
    return false;
  }

  // Step 5.
  if (!ThrowIfInvalidTimeValue(cx, "millisecond", 0, 999, millisecond)) {
    return false;
  }

  // Step 6.
  if (!ThrowIfInvalidTimeValue(cx, "microsecond", 0, 999, microsecond)) {
    return false;
  }

  // Step 7.
  if (!ThrowIfInvalidTimeValue(cx, "nanosecond", 0, 999, nanosecond)) {
    return false;
  }

  // Step 8.
  return true;
}

/**
 * RegulateTime ( hour, minute, second, millisecond, microsecond, nanosecond,
 * overflow )
 */
bool js::temporal::RegulateTime(JSContext* cx, const TemporalTimeLike& time,
                                TemporalOverflow overflow, Time* result) {
  auto [hour, minute, second, millisecond, microsecond, nanosecond] = time;
  MOZ_ASSERT(IsInteger(hour));
  MOZ_ASSERT(IsInteger(minute));
  MOZ_ASSERT(IsInteger(second));
  MOZ_ASSERT(IsInteger(millisecond));
  MOZ_ASSERT(IsInteger(microsecond));
  MOZ_ASSERT(IsInteger(nanosecond));

  // Steps 1-2.
  if (overflow == TemporalOverflow::Constrain) {
    // Step 1.a.
    hour = std::clamp(hour, 0.0, 23.0);

    // Step 1.b.
    minute = std::clamp(minute, 0.0, 59.0);

    // Step 1.c.
    second = std::clamp(second, 0.0, 59.0);

    // Step 1.d.
    millisecond = std::clamp(millisecond, 0.0, 999.0);

    // Step 1.e.
    microsecond = std::clamp(microsecond, 0.0, 999.0);

    // Step 1.f.
    nanosecond = std::clamp(nanosecond, 0.0, 999.0);
  } else {
    // Step 2.a.
    MOZ_ASSERT(overflow == TemporalOverflow::Reject);

    // Step 2.b.
    if (!ThrowIfInvalidTime(cx, hour, minute, second, millisecond, microsecond,
                            nanosecond)) {
      return false;
    }
  }

  // Step 3.
  *result = {
      int32_t(hour),        int32_t(minute),      int32_t(second),
      int32_t(millisecond), int32_t(microsecond), int32_t(nanosecond),
  };
  return true;
}

/**
 * CreateTemporalTime ( time [ , newTarget ] )
 */
static PlainTimeObject* CreateTemporalTime(JSContext* cx, const CallArgs& args,
                                           const Time& time) {
  MOZ_ASSERT(IsValidTime(time));

  // Steps 1-2.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_PlainTime,
                                          &proto)) {
    return nullptr;
  }

  auto* object = NewObjectWithClassProto<PlainTimeObject>(cx, proto);
  if (!object) {
    return nullptr;
  }

  // Step 3.
  auto packedTime = PackedTime::pack(time);
  object->setFixedSlot(
      PlainTimeObject::PACKED_TIME_SLOT,
      DoubleValue(mozilla::BitwiseCast<double>(packedTime.value)));

  // Step 4.
  return object;
}

/**
 * CreateTemporalTime ( time [ , newTarget ] )
 */
PlainTimeObject* js::temporal::CreateTemporalTime(JSContext* cx,
                                                  const Time& time) {
  MOZ_ASSERT(IsValidTime(time));

  // Steps 1-2.
  auto* object = NewBuiltinClassInstance<PlainTimeObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Step 3.
  auto packedTime = PackedTime::pack(time);
  object->setFixedSlot(
      PlainTimeObject::PACKED_TIME_SLOT,
      DoubleValue(mozilla::BitwiseCast<double>(packedTime.value)));

  // Step 4.
  return object;
}

/**
 * BalanceTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
template <typename IntT>
static TimeRecord BalanceTime(IntT hour, IntT minute, IntT second,
                              IntT millisecond, IntT microsecond,
                              IntT nanosecond) {
  // Combined floor'ed division and modulo operation.
  auto divmod = [](IntT dividend, int32_t divisor, int32_t* remainder) {
    MOZ_ASSERT(divisor > 0);

    IntT quotient = dividend / divisor;
    *remainder = dividend % divisor;

    // The remainder is negative, add the divisor and simulate a floor instead
    // of trunc division.
    if (*remainder < 0) {
      *remainder += divisor;
      quotient -= 1;
    }

    return quotient;
  };

  Time time = {};

  // Steps 1-2.
  microsecond += divmod(nanosecond, 1000, &time.nanosecond);

  // Steps 3-4.
  millisecond += divmod(microsecond, 1000, &time.microsecond);

  // Steps 5-6.
  second += divmod(millisecond, 1000, &time.millisecond);

  // Steps 7-8.
  minute += divmod(second, 60, &time.second);

  // Steps 9-10.
  hour += divmod(minute, 60, &time.minute);

  // Steps 11-12.
  int64_t days = divmod(hour, 24, &time.hour);

  // Step 13.
  MOZ_ASSERT(IsValidTime(time));
  return {days, time};
}

/**
 * BalanceTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
static TimeRecord BalanceTime(int32_t hour, int32_t minute, int32_t second,
                              int32_t millisecond, int32_t microsecond,
                              int32_t nanosecond) {
  MOZ_ASSERT(-24 < hour && hour < 2 * 24);
  MOZ_ASSERT(-60 < minute && minute < 2 * 60);
  MOZ_ASSERT(-60 < second && second < 2 * 60);
  MOZ_ASSERT(-1000 < millisecond && millisecond < 2 * 1000);
  MOZ_ASSERT(-1000 < microsecond && microsecond < 2 * 1000);
  MOZ_ASSERT(-1000 < nanosecond && nanosecond < 2 * 1000);

  return BalanceTime<int32_t>(hour, minute, second, millisecond, microsecond,
                              nanosecond);
}

/**
 * BalanceTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
TimeRecord js::temporal::BalanceTime(const Time& time, int64_t nanoseconds) {
  MOZ_ASSERT(IsValidTime(time));
  MOZ_ASSERT(std::abs(nanoseconds) <= ToNanoseconds(TemporalUnit::Day));

  return ::BalanceTime<int64_t>(time.hour, time.minute, time.second,
                                time.millisecond, time.microsecond,
                                time.nanosecond + nanoseconds);
}

/**
 * TimeDurationFromComponents ( hours, minutes, seconds, milliseconds,
 * microseconds, nanoseconds )
 */
static TimeDuration TimeDurationFromComponents(int32_t hours, int32_t minutes,
                                               int32_t seconds,
                                               int32_t milliseconds,
                                               int32_t microseconds,
                                               int32_t nanoseconds) {
  MOZ_ASSERT(std::abs(hours) <= 23);
  MOZ_ASSERT(std::abs(minutes) <= 59);
  MOZ_ASSERT(std::abs(seconds) <= 59);
  MOZ_ASSERT(std::abs(milliseconds) <= 999);
  MOZ_ASSERT(std::abs(microseconds) <= 999);
  MOZ_ASSERT(std::abs(nanoseconds) <= 999);

  // Steps 1-5.
  int64_t nanos = int64_t(hours);
  nanos *= 60;
  nanos += int64_t(minutes);
  nanos *= 60;
  nanos += int64_t(seconds);
  nanos *= 1000;
  nanos += int64_t(milliseconds);
  nanos *= 1000;
  nanos += int64_t(microseconds);
  nanos *= 1000;
  nanos += int64_t(nanoseconds);
  MOZ_ASSERT(std::abs(nanos) < ToNanoseconds(TemporalUnit::Day));

  auto timeDuration = TimeDuration::fromNanoseconds(nanos);

  // Step 6.
  MOZ_ASSERT(IsValidTimeDuration(timeDuration));

  // Step 7.
  return timeDuration;
}

/**
 * DifferenceTime ( time1, time2 )
 */
TimeDuration js::temporal::DifferenceTime(const Time& time1,
                                          const Time& time2) {
  MOZ_ASSERT(IsValidTime(time1));
  MOZ_ASSERT(IsValidTime(time2));

  // Step 1.
  int32_t hours = time2.hour - time1.hour;

  // Step 2.
  int32_t minutes = time2.minute - time1.minute;

  // Step 3.
  int32_t seconds = time2.second - time1.second;

  // Step 4.
  int32_t milliseconds = time2.millisecond - time1.millisecond;

  // Step 5.
  int32_t microseconds = time2.microsecond - time1.microsecond;

  // Step 6.
  int32_t nanoseconds = time2.nanosecond - time1.nanosecond;

  // Step 7.
  auto result = ::TimeDurationFromComponents(
      hours, minutes, seconds, milliseconds, microseconds, nanoseconds);

  // Step 8.
  MOZ_ASSERT(result.abs() < TimeDuration::fromDays(1));

  // Step 9.
  return result;
}

/**
 * ToTemporalTimeRecord ( temporalTimeLike [ , completeness ] )
 */
static bool ToTemporalTimeRecord(JSContext* cx,
                                 Handle<JSObject*> temporalTimeLike,
                                 TemporalTimeLike* result) {
  // Steps 1-3. (Not applicable in our implementation.)

  // Step 4.
  bool any = false;

  Rooted<Value> value(cx);
  auto getTimeProperty = [&](Handle<PropertyName*> property, const char* name,
                             double* num) {
    if (!GetProperty(cx, temporalTimeLike, temporalTimeLike, property,
                     &value)) {
      return false;
    }

    if (!value.isUndefined()) {
      any = true;

      if (!ToIntegerWithTruncation(cx, value, name, num)) {
        return false;
      }
    }
    return true;
  };

  // Steps 5-6.
  if (!getTimeProperty(cx->names().hour, "hour", &result->hour)) {
    return false;
  }

  // Steps 7-8.
  if (!getTimeProperty(cx->names().microsecond, "microsecond",
                       &result->microsecond)) {
    return false;
  }

  // Steps 9-10.
  if (!getTimeProperty(cx->names().millisecond, "millisecond",
                       &result->millisecond)) {
    return false;
  }

  // Steps 11-12.
  if (!getTimeProperty(cx->names().minute, "minute", &result->minute)) {
    return false;
  }

  // Steps 13-14.
  if (!getTimeProperty(cx->names().nanosecond, "nanosecond",
                       &result->nanosecond)) {
    return false;
  }

  // Steps 15-16.
  if (!getTimeProperty(cx->names().second, "second", &result->second)) {
    return false;
  }

  // Step 17.
  if (!any) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_TIME_MISSING_UNIT);
    return false;
  }

  // Step 18.
  return true;
}

struct TimeOptions {
  TemporalOverflow overflow = TemporalOverflow::Constrain;
};

/**
 * ToTemporalTime ( item [ , options ] )
 */
static bool ToTemporalTimeOptions(JSContext* cx, Handle<Value> options,
                                  TimeOptions* result) {
  if (options.isUndefined()) {
    *result = {};
    return true;
  }

  // NOTE: |options| are only passed from `Temporal.PlainTime.from`.

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
 * ToTemporalTime ( item [ , options ] )
 */
static bool ToTemporalTime(JSContext* cx, Handle<JSObject*> item,
                           Handle<Value> options, Time* result) {
  // Step 2.a.
  if (auto* plainTime = item->maybeUnwrapIf<PlainTimeObject>()) {
    auto time = plainTime->time();

    // Steps 2.a.i-ii.
    TimeOptions ignoredOptions;
    if (!ToTemporalTimeOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Step 2.a.iii.
    *result = time;
    return true;
  }

  // Step 2.b.
  if (auto* dateTime = item->maybeUnwrapIf<PlainDateTimeObject>()) {
    auto time = dateTime->time();

    // Steps 2.b.i-ii.
    TimeOptions ignoredOptions;
    if (!ToTemporalTimeOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Step 2.b.iii.
    *result = time;
    return true;
  }

  // Step 2.c.
  if (auto* zonedDateTime = item->maybeUnwrapIf<ZonedDateTimeObject>()) {
    auto epochNs = zonedDateTime->epochNanoseconds();
    Rooted<TimeZoneValue> timeZone(cx, zonedDateTime->timeZone());

    if (!timeZone.wrap(cx)) {
      return false;
    }

    // Steps 2.c.i.
    ISODateTime dateTime;
    if (!GetISODateTimeFor(cx, timeZone, epochNs, &dateTime)) {
      return false;
    }

    // Steps 2.c.ii-iii.
    TimeOptions ignoredOptions;
    if (!ToTemporalTimeOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Step 2.c.iv.
    *result = dateTime.time;
    return true;
  }

  // Step 2.d.
  TemporalTimeLike timeResult{};
  if (!ToTemporalTimeRecord(cx, item, &timeResult)) {
    return false;
  }

  // Steps 2.e-f.
  TimeOptions resolvedOptions;
  if (!ToTemporalTimeOptions(cx, options, &resolvedOptions)) {
    return false;
  }
  auto [overflow] = resolvedOptions;

  // Step 2.g and 4.
  return RegulateTime(cx, timeResult, overflow, result);
}

/**
 * ToTemporalTime ( item [ , options ] )
 */
static bool ToTemporalTime(JSContext* cx, Handle<Value> item,
                           Handle<Value> options, Time* result) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  if (item.isObject()) {
    Rooted<JSObject*> itemObj(cx, &item.toObject());
    return ToTemporalTime(cx, itemObj, options, result);
  }

  // Step 3.a.
  if (!item.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, item,
                     nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> string(cx, item.toString());

  // Steps 3.b-e.
  if (!ParseTemporalTimeString(cx, string, result)) {
    return false;
  }
  MOZ_ASSERT(IsValidTime(*result));

  // Steps 3.f-g.
  TimeOptions ignoredOptions;
  if (!ToTemporalTimeOptions(cx, options, &ignoredOptions)) {
    return false;
  }

  // Step 4.
  return true;
}

/**
 * ToTemporalTime ( item [ , options ] )
 */
bool js::temporal::ToTemporalTime(JSContext* cx, Handle<Value> item,
                                  Time* result) {
  return ToTemporalTime(cx, item, UndefinedHandleValue, result);
}

/**
 * CompareTimeRecord ( time1, time2 )
 */
int32_t js::temporal::CompareTimeRecord(const Time& one, const Time& two) {
  // Steps 1-2.
  if (int32_t diff = one.hour - two.hour) {
    return diff < 0 ? -1 : 1;
  }

  // Steps 3-4.
  if (int32_t diff = one.minute - two.minute) {
    return diff < 0 ? -1 : 1;
  }

  // Steps 5-6.
  if (int32_t diff = one.second - two.second) {
    return diff < 0 ? -1 : 1;
  }

  // Steps 7-8.
  if (int32_t diff = one.millisecond - two.millisecond) {
    return diff < 0 ? -1 : 1;
  }

  // Steps 9-10.
  if (int32_t diff = one.microsecond - two.microsecond) {
    return diff < 0 ? -1 : 1;
  }

  // Steps 11-12.
  if (int32_t diff = one.nanosecond - two.nanosecond) {
    return diff < 0 ? -1 : 1;
  }

  // Step 13.
  return 0;
}

static int64_t TimeToNanos(const Time& time) {
  // No overflow possible because the input is a valid time.
  MOZ_ASSERT(IsValidTime(time));

  int64_t hour = time.hour;
  int64_t minute = time.minute;
  int64_t second = time.second;
  int64_t millisecond = time.millisecond;
  int64_t microsecond = time.microsecond;
  int64_t nanosecond = time.nanosecond;

  int64_t millis = ((hour * 60 + minute) * 60 + second) * 1000 + millisecond;
  return (millis * 1000 + microsecond) * 1000 + nanosecond;
}

/**
 * RoundTime ( time, increment, unit, roundingMode )
 */
TimeRecord js::temporal::RoundTime(const Time& time, Increment increment,
                                   TemporalUnit unit,
                                   TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidTime(time));
  MOZ_ASSERT(unit >= TemporalUnit::Day);
  MOZ_ASSERT_IF(unit > TemporalUnit::Day,
                increment <= MaximumTemporalDurationRoundingIncrement(unit));
  MOZ_ASSERT_IF(unit == TemporalUnit::Day, increment == Increment{1});

  int32_t days = 0;
  auto [hour, minute, second, millisecond, microsecond, nanosecond] = time;

  // Steps 1-6.
  Time quantity;
  int32_t* result;
  switch (unit) {
    case TemporalUnit::Day:
      quantity = time;
      result = &days;
      break;
    case TemporalUnit::Hour:
      quantity = time;
      result = &hour;
      minute = 0;
      second = 0;
      millisecond = 0;
      microsecond = 0;
      nanosecond = 0;
      break;
    case TemporalUnit::Minute:
      quantity = {0, minute, second, millisecond, microsecond, nanosecond};
      result = &minute;
      second = 0;
      millisecond = 0;
      microsecond = 0;
      nanosecond = 0;
      break;
    case TemporalUnit::Second:
      quantity = {0, 0, second, millisecond, microsecond, nanosecond};
      result = &second;
      millisecond = 0;
      microsecond = 0;
      nanosecond = 0;
      break;
    case TemporalUnit::Millisecond:
      quantity = {0, 0, 0, millisecond, microsecond, nanosecond};
      result = &millisecond;
      microsecond = 0;
      nanosecond = 0;
      break;
    case TemporalUnit::Microsecond:
      quantity = {0, 0, 0, 0, microsecond, nanosecond};
      result = &microsecond;
      nanosecond = 0;
      break;
    case TemporalUnit::Nanosecond:
      quantity = {0, 0, 0, 0, 0, nanosecond};
      result = &nanosecond;
      break;

    case TemporalUnit::Auto:
    case TemporalUnit::Year:
    case TemporalUnit::Month:
    case TemporalUnit::Week:
      MOZ_CRASH("unexpected temporal unit");
  }

  int64_t quantityNs = TimeToNanos(quantity);
  MOZ_ASSERT(0 <= quantityNs && quantityNs < ToNanoseconds(TemporalUnit::Day));

  // Step 7.
  int64_t unitLength = ToNanoseconds(unit);
  int64_t incrementNs = increment.value() * unitLength;
  MOZ_ASSERT(incrementNs <= ToNanoseconds(TemporalUnit::Day),
             "incrementNs doesn't overflow time resolution");

  // Step 8.
  int64_t r = RoundNumberToIncrement(quantityNs, incrementNs, roundingMode) /
              unitLength;
  MOZ_ASSERT(r == int64_t(int32_t(r)),
             "can't overflow when inputs are all in range");

  *result = int32_t(r);

  // Step 9.
  if (unit == TemporalUnit::Day) {
    return {int64_t(days), {0, 0, 0, 0, 0, 0}};
  }

  // Steps 10-16.
  return ::BalanceTime(hour, minute, second, millisecond, microsecond,
                       nanosecond);
}

/**
 * AddTime ( time, timeDuration )
 */
TimeRecord js::temporal::AddTime(const Time& time,
                                 const TimeDuration& duration) {
  MOZ_ASSERT(IsValidTime(time));
  MOZ_ASSERT(IsValidTimeDuration(duration));

  auto [seconds, nanoseconds] = duration.denormalize();
  MOZ_ASSERT(std::abs(nanoseconds) <= 999'999'999);

  // Steps 1-2.
  return ::BalanceTime<int64_t>(time.hour, time.minute, time.second + seconds,
                                time.millisecond, time.microsecond,
                                time.nanosecond + nanoseconds);
}

/**
 * DifferenceTemporalPlainTime ( operation, temporalTime, other, options )
 */
static bool DifferenceTemporalPlainTime(JSContext* cx,
                                        TemporalDifference operation,
                                        const CallArgs& args) {
  auto temporalTime = args.thisv().toObject().as<PlainTimeObject>().time();

  // Step 1.
  Time other;
  if (!ToTemporalTime(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 2-3.
  DifferenceSettings settings;
  if (args.hasDefined(1)) {
    // Step 2.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", ToName(operation), args[1]));
    if (!options) {
      return false;
    }

    // Step 3.
    if (!GetDifferenceSettings(cx, operation, options, TemporalUnitGroup::Time,
                               TemporalUnit::Nanosecond, TemporalUnit::Hour,
                               &settings)) {
      return false;
    }
  } else {
    // Steps 2-3.
    settings = {
        TemporalUnit::Nanosecond,
        TemporalUnit::Hour,
        TemporalRoundingMode::Trunc,
        Increment{1},
    };
  }

  // Step 4.
  auto timeDuration = DifferenceTime(temporalTime, other);

  // Step 5.
  timeDuration =
      RoundTimeDuration(timeDuration, settings.roundingIncrement,
                        settings.smallestUnit, settings.roundingMode);

  // Steps 6-7.
  Duration duration;
  if (!TemporalDurationFromInternal(cx, timeDuration, settings.largestUnit,
                                    &duration)) {
    return false;
  }

  // Step 8.
  if (operation == TemporalDifference::Since) {
    duration = duration.negate();
  }

  // Step 9.
  auto* result = CreateTemporalDuration(cx, duration);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * AddDurationToTime ( operation, temporalTime, temporalDurationLike )
 */
static bool AddDurationToTime(JSContext* cx, TemporalAddDuration operation,
                              const CallArgs& args) {
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  auto time = temporalTime->time();

  // Step 1.
  Duration duration;
  if (!ToTemporalDuration(cx, args.get(0), &duration)) {
    return false;
  }

  // Step 2.
  if (operation == TemporalAddDuration::Subtract) {
    duration = duration.negate();
  }

  // Step 3. (Inlined ToInternalDurationRecord)
  auto timeDuration = TimeDurationFromComponents(duration);

  // Step 4.
  auto result = AddTime(time, timeDuration);
  MOZ_ASSERT(IsValidTime(result.time));

  // Step 5.
  auto* obj = CreateTemporalTime(cx, result.time);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainTime ( [ hour [ , minute [ , second [ , millisecond [ ,
 * microsecond [ , nanosecond ] ] ] ] ] ] )
 */
static bool PlainTimeConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Temporal.PlainTime")) {
    return false;
  }

  // Step 2.
  double hour = 0;
  if (args.hasDefined(0)) {
    if (!ToIntegerWithTruncation(cx, args[0], "hour", &hour)) {
      return false;
    }
  }

  // Step 3.
  double minute = 0;
  if (args.hasDefined(1)) {
    if (!ToIntegerWithTruncation(cx, args[1], "minute", &minute)) {
      return false;
    }
  }

  // Step 4.
  double second = 0;
  if (args.hasDefined(2)) {
    if (!ToIntegerWithTruncation(cx, args[2], "second", &second)) {
      return false;
    }
  }

  // Step 5.
  double millisecond = 0;
  if (args.hasDefined(3)) {
    if (!ToIntegerWithTruncation(cx, args[3], "millisecond", &millisecond)) {
      return false;
    }
  }

  // Step 6.
  double microsecond = 0;
  if (args.hasDefined(4)) {
    if (!ToIntegerWithTruncation(cx, args[4], "microsecond", &microsecond)) {
      return false;
    }
  }

  // Step 7.
  double nanosecond = 0;
  if (args.hasDefined(5)) {
    if (!ToIntegerWithTruncation(cx, args[5], "nanosecond", &nanosecond)) {
      return false;
    }
  }

  // Steps 8-9.
  Time time;
  if (!RegulateTime(cx,
                    {
                        hour,
                        minute,
                        second,
                        millisecond,
                        microsecond,
                        nanosecond,
                    },
                    TemporalOverflow::Reject, &time)) {
    return false;
  }

  // Step 10.
  auto* temporalTime = CreateTemporalTime(cx, args, time);
  if (!temporalTime) {
    return false;
  }

  args.rval().setObject(*temporalTime);
  return true;
}

/**
 * Temporal.PlainTime.from ( item [ , options ] )
 */
static bool PlainTime_from(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Time result;
  if (!ToTemporalTime(cx, args.get(0), args.get(1), &result)) {
    return false;
  }
  MOZ_ASSERT(IsValidTime(result));

  auto* obj = temporal::CreateTemporalTime(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainTime.compare ( one, two )
 */
static bool PlainTime_compare(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Time one;
  if (!ToTemporalTime(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  Time two;
  if (!ToTemporalTime(cx, args.get(1), &two)) {
    return false;
  }

  // Step 3.
  args.rval().setInt32(CompareTimeRecord(one, two));
  return true;
}

/**
 * get Temporal.PlainTime.prototype.hour
 */
static bool PlainTime_hour(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  args.rval().setInt32(temporalTime->time().hour);
  return true;
}

/**
 * get Temporal.PlainTime.prototype.hour
 */
static bool PlainTime_hour(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_hour>(cx, args);
}

/**
 * get Temporal.PlainTime.prototype.minute
 */
static bool PlainTime_minute(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  args.rval().setInt32(temporalTime->time().minute);
  return true;
}

/**
 * get Temporal.PlainTime.prototype.minute
 */
static bool PlainTime_minute(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_minute>(cx, args);
}

/**
 * get Temporal.PlainTime.prototype.second
 */
static bool PlainTime_second(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  args.rval().setInt32(temporalTime->time().second);
  return true;
}

/**
 * get Temporal.PlainTime.prototype.second
 */
static bool PlainTime_second(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_second>(cx, args);
}

/**
 * get Temporal.PlainTime.prototype.millisecond
 */
static bool PlainTime_millisecond(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  args.rval().setInt32(temporalTime->time().millisecond);
  return true;
}

/**
 * get Temporal.PlainTime.prototype.millisecond
 */
static bool PlainTime_millisecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_millisecond>(cx, args);
}

/**
 * get Temporal.PlainTime.prototype.microsecond
 */
static bool PlainTime_microsecond(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  args.rval().setInt32(temporalTime->time().microsecond);
  return true;
}

/**
 * get Temporal.PlainTime.prototype.microsecond
 */
static bool PlainTime_microsecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_microsecond>(cx, args);
}

/**
 * get Temporal.PlainTime.prototype.nanosecond
 */
static bool PlainTime_nanosecond(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  args.rval().setInt32(temporalTime->time().nanosecond);
  return true;
}

/**
 * get Temporal.PlainTime.prototype.nanosecond
 */
static bool PlainTime_nanosecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_nanosecond>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.add ( temporalDurationLike )
 */
static bool PlainTime_add(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurationToTime(cx, TemporalAddDuration::Add, args);
}

/**
 * Temporal.PlainTime.prototype.add ( temporalDurationLike )
 */
static bool PlainTime_add(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_add>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.subtract ( temporalDurationLike )
 */
static bool PlainTime_subtract(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurationToTime(cx, TemporalAddDuration::Subtract, args);
}

/**
 * Temporal.PlainTime.prototype.subtract ( temporalDurationLike )
 */
static bool PlainTime_subtract(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_subtract>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.with ( temporalTimeLike [ , options ] )
 */
static bool PlainTime_with(JSContext* cx, const CallArgs& args) {
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  auto time = temporalTime->time();

  // Step 3.
  Rooted<JSObject*> temporalTimeLike(
      cx, RequireObjectArg(cx, "temporalTimeLike", "with", args.get(0)));
  if (!temporalTimeLike) {
    return false;
  }
  if (!ThrowIfTemporalLikeObject(cx, temporalTimeLike)) {
    return false;
  }

  // Steps 4-16.
  TemporalTimeLike partialTime = {
      double(time.hour),        double(time.minute),
      double(time.second),      double(time.millisecond),
      double(time.microsecond), double(time.nanosecond),
  };
  if (!::ToTemporalTimeRecord(cx, temporalTimeLike, &partialTime)) {
    return false;
  }

  // Steps 17-18
  auto overflow = TemporalOverflow::Constrain;
  if (args.hasDefined(1)) {
    // Step 17.
    Rooted<JSObject*> options(cx,
                              RequireObjectArg(cx, "options", "with", args[1]));
    if (!options) {
      return false;
    }

    // Step 18.
    if (!GetTemporalOverflowOption(cx, options, &overflow)) {
      return false;
    }
  }

  // Step 19.
  Time result;
  if (!RegulateTime(cx, partialTime, overflow, &result)) {
    return false;
  }

  // Step 20.
  auto* obj = CreateTemporalTime(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainTime.prototype.with ( temporalTimeLike [ , options ] )
 */
static bool PlainTime_with(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_with>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.until ( other [ , options ] )
 */
static bool PlainTime_until(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalPlainTime(cx, TemporalDifference::Until, args);
}

/**
 * Temporal.PlainTime.prototype.until ( other [ , options ] )
 */
static bool PlainTime_until(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_until>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.since ( other [ , options ] )
 */
static bool PlainTime_since(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalPlainTime(cx, TemporalDifference::Since, args);
}

/**
 * Temporal.PlainTime.prototype.since ( other [ , options ] )
 */
static bool PlainTime_since(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_since>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.round ( roundTo )
 */
static bool PlainTime_round(JSContext* cx, const CallArgs& args) {
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  auto time = temporalTime->time();

  // Steps 3-12.
  auto smallestUnit = TemporalUnit::Auto;
  auto roundingMode = TemporalRoundingMode::HalfExpand;
  auto roundingIncrement = Increment{1};
  if (args.get(0).isString()) {
    // Step 4. (Not applicable in our implementation.)

    // Step 9.
    Rooted<JSString*> paramString(cx, args[0].toString());
    if (!GetTemporalUnitValuedOption(cx, paramString,
                                     TemporalUnitKey::SmallestUnit,
                                     TemporalUnitGroup::Time, &smallestUnit)) {
      return false;
    }

    // Steps 6-8 and 10-12. (Implicit)
  } else {
    // Steps 3 and 5.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "roundTo", "round", args.get(0)));
    if (!options) {
      return false;
    }

    // Steps 6-7.
    if (!GetRoundingIncrementOption(cx, options, &roundingIncrement)) {
      return false;
    }

    // Step 8.
    if (!GetRoundingModeOption(cx, options, &roundingMode)) {
      return false;
    }

    // Step 9.
    if (!GetTemporalUnitValuedOption(cx, options, TemporalUnitKey::SmallestUnit,
                                     TemporalUnitGroup::Time, &smallestUnit)) {
      return false;
    }

    if (smallestUnit == TemporalUnit::Auto) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_MISSING_OPTION, "smallestUnit");
      return false;
    }

    // Steps 10-11.
    auto maximum = MaximumTemporalDurationRoundingIncrement(smallestUnit);

    // Step 12.
    if (!ValidateTemporalRoundingIncrement(cx, roundingIncrement, maximum,
                                           false)) {
      return false;
    }
  }

  // Step 13.
  auto result = RoundTime(time, roundingIncrement, smallestUnit, roundingMode);

  // Step 14.
  auto* obj = CreateTemporalTime(cx, result.time);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainTime.prototype.round ( roundTo )
 */
static bool PlainTime_round(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_round>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.equals ( other )
 */
static bool PlainTime_equals(JSContext* cx, const CallArgs& args) {
  auto temporalTime = args.thisv().toObject().as<PlainTimeObject>().time();

  // Step 3.
  Time other;
  if (!ToTemporalTime(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 4-5.
  args.rval().setBoolean(temporalTime == other);
  return true;
}

/**
 * Temporal.PlainTime.prototype.equals ( other )
 */
static bool PlainTime_equals(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_equals>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.toString ( [ options ] )
 */
static bool PlainTime_toString(JSContext* cx, const CallArgs& args) {
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  auto time = temporalTime->time();

  SecondsStringPrecision precision = {Precision::Auto(),
                                      TemporalUnit::Nanosecond, Increment{1}};
  auto roundingMode = TemporalRoundingMode::Trunc;
  if (args.hasDefined(0)) {
    // Step 3.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", "toString", args[0]));
    if (!options) {
      return false;
    }

    // Steps 4-5.
    auto digits = Precision::Auto();
    if (!GetTemporalFractionalSecondDigitsOption(cx, options, &digits)) {
      return false;
    }

    // Step 6.
    if (!GetRoundingModeOption(cx, options, &roundingMode)) {
      return false;
    }

    // Step 7.
    auto smallestUnit = TemporalUnit::Auto;
    if (!GetTemporalUnitValuedOption(cx, options, TemporalUnitKey::SmallestUnit,
                                     TemporalUnitGroup::Time, &smallestUnit)) {
      return false;
    }

    // Step 8.
    if (smallestUnit == TemporalUnit::Hour) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INVALID_UNIT_OPTION, "hour",
                                "smallestUnit");
      return false;
    }

    // Step 9.
    precision = ToSecondsStringPrecision(smallestUnit, digits);
  }

  // Step 10.
  auto roundedTime =
      RoundTime(time, precision.increment, precision.unit, roundingMode);

  // Step 11.
  JSString* str = TimeRecordToString(cx, roundedTime.time, precision.precision);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.PlainTime.prototype.toString ( [ options ] )
 */
static bool PlainTime_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_toString>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool PlainTime_toLocaleString(JSContext* cx, const CallArgs& args) {
  // Steps 3-4.
  Handle<PropertyName*> required = cx->names().time;
  Handle<PropertyName*> defaults = cx->names().time;
  return TemporalObjectToLocaleString(cx, args, required, defaults);
}

/**
 * Temporal.PlainTime.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool PlainTime_toLocaleString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_toLocaleString>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.toJSON ( )
 */
static bool PlainTime_toJSON(JSContext* cx, const CallArgs& args) {
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  auto time = temporalTime->time();

  // Step 3.
  JSString* str = TimeRecordToString(cx, time, Precision::Auto());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.PlainTime.prototype.toJSON ( )
 */
static bool PlainTime_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_toJSON>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.valueOf ( )
 */
static bool PlainTime_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                            "PlainTime", "primitive type");
  return false;
}

const JSClass PlainTimeObject::class_ = {
    "Temporal.PlainTime",
    JSCLASS_HAS_RESERVED_SLOTS(PlainTimeObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_PlainTime),
    JS_NULL_CLASS_OPS,
    &PlainTimeObject::classSpec_,
};

const JSClass& PlainTimeObject::protoClass_ = PlainObject::class_;

static const JSFunctionSpec PlainTime_methods[] = {
    JS_FN("from", PlainTime_from, 1, 0),
    JS_FN("compare", PlainTime_compare, 2, 0),
    JS_FS_END,
};

static const JSFunctionSpec PlainTime_prototype_methods[] = {
    JS_FN("add", PlainTime_add, 1, 0),
    JS_FN("subtract", PlainTime_subtract, 1, 0),
    JS_FN("with", PlainTime_with, 1, 0),
    JS_FN("until", PlainTime_until, 1, 0),
    JS_FN("since", PlainTime_since, 1, 0),
    JS_FN("round", PlainTime_round, 1, 0),
    JS_FN("equals", PlainTime_equals, 1, 0),
    JS_FN("toString", PlainTime_toString, 0, 0),
    JS_FN("toLocaleString", PlainTime_toLocaleString, 0, 0),
    JS_FN("toJSON", PlainTime_toJSON, 0, 0),
    JS_FN("valueOf", PlainTime_valueOf, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec PlainTime_prototype_properties[] = {
    JS_PSG("hour", PlainTime_hour, 0),
    JS_PSG("minute", PlainTime_minute, 0),
    JS_PSG("second", PlainTime_second, 0),
    JS_PSG("millisecond", PlainTime_millisecond, 0),
    JS_PSG("microsecond", PlainTime_microsecond, 0),
    JS_PSG("nanosecond", PlainTime_nanosecond, 0),
    JS_STRING_SYM_PS(toStringTag, "Temporal.PlainTime", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec PlainTimeObject::classSpec_ = {
    GenericCreateConstructor<PlainTimeConstructor, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<PlainTimeObject>,
    PlainTime_methods,
    nullptr,
    PlainTime_prototype_methods,
    PlainTime_prototype_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};
