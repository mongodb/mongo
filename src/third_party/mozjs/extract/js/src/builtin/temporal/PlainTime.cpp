/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/PlainTime.h"

#include "mozilla/Assertions.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <type_traits>
#include <utility>

#include "jsnum.h"
#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/temporal/Duration.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/TimeZone.h"
#include "builtin/temporal/ToString.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "ds/IdValuePair.h"
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "js/AllocPolicy.h"
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
#include "vm/StringType.h"

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

  // Step 1.
  MOZ_ASSERT(IsInteger(hour));
  MOZ_ASSERT(IsInteger(minute));
  MOZ_ASSERT(IsInteger(second));
  MOZ_ASSERT(IsInteger(millisecond));
  MOZ_ASSERT(IsInteger(microsecond));
  MOZ_ASSERT(IsInteger(nanosecond));

  // Step 2.
  if (hour < 0 || hour > 23) {
    return false;
  }

  // Step 3.
  if (minute < 0 || minute > 59) {
    return false;
  }

  // Step 4.
  if (second < 0 || second > 59) {
    return false;
  }

  // Step 5.
  if (millisecond < 0 || millisecond > 999) {
    return false;
  }

  // Step 6.
  if (microsecond < 0 || microsecond > 999) {
    return false;
  }

  // Step 7.
  if (nanosecond < 0 || nanosecond > 999) {
    return false;
  }

  // Step 8.
  return true;
}

/**
 * IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
bool js::temporal::IsValidTime(const PlainTime& time) {
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

template <typename T>
static inline bool ThrowIfInvalidTimeValue(JSContext* cx, const char* name,
                                           int32_t min, int32_t max, T num) {
  if (min <= num && num <= max) {
    return true;
  }
  ReportInvalidTimeValue(cx, name, min, max, num);
  return false;
}

/**
 * IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
template <typename T>
static bool ThrowIfInvalidTime(JSContext* cx, T hour, T minute, T second,
                               T millisecond, T microsecond, T nanosecond) {
  static_assert(std::is_same_v<T, int32_t> || std::is_same_v<T, double>);

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
 * IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
bool js::temporal::ThrowIfInvalidTime(JSContext* cx, const PlainTime& time) {
  const auto& [hour, minute, second, millisecond, microsecond, nanosecond] =
      time;
  return ::ThrowIfInvalidTime(cx, hour, minute, second, millisecond,
                              microsecond, nanosecond);
}

/**
 * IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
bool js::temporal::ThrowIfInvalidTime(JSContext* cx, double hour, double minute,
                                      double second, double millisecond,
                                      double microsecond, double nanosecond) {
  return ::ThrowIfInvalidTime(cx, hour, minute, second, millisecond,
                              microsecond, nanosecond);
}

/**
 * ConstrainTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
static PlainTime ConstrainTime(double hour, double minute, double second,
                               double millisecond, double microsecond,
                               double nanosecond) {
  // Step 1.
  MOZ_ASSERT(IsInteger(hour));
  MOZ_ASSERT(IsInteger(minute));
  MOZ_ASSERT(IsInteger(second));
  MOZ_ASSERT(IsInteger(millisecond));
  MOZ_ASSERT(IsInteger(microsecond));
  MOZ_ASSERT(IsInteger(nanosecond));

  // Steps 2-8.
  return {
      int32_t(std::clamp(hour, 0.0, 23.0)),
      int32_t(std::clamp(minute, 0.0, 59.0)),
      int32_t(std::clamp(second, 0.0, 59.0)),
      int32_t(std::clamp(millisecond, 0.0, 999.0)),
      int32_t(std::clamp(microsecond, 0.0, 999.0)),
      int32_t(std::clamp(nanosecond, 0.0, 999.0)),
  };
}

/**
 * RegulateTime ( hour, minute, second, millisecond, microsecond, nanosecond,
 * overflow )
 */
bool js::temporal::RegulateTime(JSContext* cx, const TemporalTimeLike& time,
                                TemporalOverflow overflow, PlainTime* result) {
  const auto& [hour, minute, second, millisecond, microsecond, nanosecond] =
      time;

  // Step 1.
  MOZ_ASSERT(IsInteger(hour));
  MOZ_ASSERT(IsInteger(minute));
  MOZ_ASSERT(IsInteger(second));
  MOZ_ASSERT(IsInteger(millisecond));
  MOZ_ASSERT(IsInteger(microsecond));
  MOZ_ASSERT(IsInteger(nanosecond));

  // Step 2. (Not applicable in our implementation.)

  // Step 3.
  if (overflow == TemporalOverflow::Constrain) {
    *result = ConstrainTime(hour, minute, second, millisecond, microsecond,
                            nanosecond);
    return true;
  }

  // Step 4.a.
  MOZ_ASSERT(overflow == TemporalOverflow::Reject);

  // Step 4.b.
  if (!ThrowIfInvalidTime(cx, hour, minute, second, millisecond, microsecond,
                          nanosecond)) {
    return false;
  }

  // Step 4.c.
  *result = {
      int32_t(hour),        int32_t(minute),      int32_t(second),
      int32_t(millisecond), int32_t(microsecond), int32_t(nanosecond),
  };
  return true;
}

/**
 * CreateTemporalTime ( hour, minute, second, millisecond, microsecond,
 * nanosecond [ , newTarget ] )
 */
static PlainTimeObject* CreateTemporalTime(JSContext* cx, const CallArgs& args,
                                           double hour, double minute,
                                           double second, double millisecond,
                                           double microsecond,
                                           double nanosecond) {
  MOZ_ASSERT(IsInteger(hour));
  MOZ_ASSERT(IsInteger(minute));
  MOZ_ASSERT(IsInteger(second));
  MOZ_ASSERT(IsInteger(millisecond));
  MOZ_ASSERT(IsInteger(microsecond));
  MOZ_ASSERT(IsInteger(nanosecond));

  // Step 1.
  if (!ThrowIfInvalidTime(cx, hour, minute, second, millisecond, microsecond,
                          nanosecond)) {
    return nullptr;
  }

  // Steps 2-3.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_PlainTime,
                                          &proto)) {
    return nullptr;
  }

  auto* object = NewObjectWithClassProto<PlainTimeObject>(cx, proto);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  object->setFixedSlot(PlainTimeObject::ISO_HOUR_SLOT,
                       Int32Value(int32_t(hour)));

  // Step 5.
  object->setFixedSlot(PlainTimeObject::ISO_MINUTE_SLOT,
                       Int32Value(int32_t(minute)));

  // Step 6.
  object->setFixedSlot(PlainTimeObject::ISO_SECOND_SLOT,
                       Int32Value(int32_t(second)));

  // Step 7.
  object->setFixedSlot(PlainTimeObject::ISO_MILLISECOND_SLOT,
                       Int32Value(int32_t(millisecond)));

  // Step 8.
  object->setFixedSlot(PlainTimeObject::ISO_MICROSECOND_SLOT,
                       Int32Value(int32_t(microsecond)));

  // Step 9.
  object->setFixedSlot(PlainTimeObject::ISO_NANOSECOND_SLOT,
                       Int32Value(int32_t(nanosecond)));

  // Step 10.
  return object;
}

/**
 * CreateTemporalTime ( hour, minute, second, millisecond, microsecond,
 * nanosecond [ , newTarget ] )
 */
PlainTimeObject* js::temporal::CreateTemporalTime(JSContext* cx,
                                                  const PlainTime& time) {
  const auto& [hour, minute, second, millisecond, microsecond, nanosecond] =
      time;

  // Step 1.
  if (!ThrowIfInvalidTime(cx, time)) {
    return nullptr;
  }

  // Steps 2-3.
  auto* object = NewBuiltinClassInstance<PlainTimeObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  object->setFixedSlot(PlainTimeObject::ISO_HOUR_SLOT, Int32Value(hour));

  // Step 5.
  object->setFixedSlot(PlainTimeObject::ISO_MINUTE_SLOT, Int32Value(minute));

  // Step 6.
  object->setFixedSlot(PlainTimeObject::ISO_SECOND_SLOT, Int32Value(second));

  // Step 7.
  object->setFixedSlot(PlainTimeObject::ISO_MILLISECOND_SLOT,
                       Int32Value(millisecond));

  // Step 8.
  object->setFixedSlot(PlainTimeObject::ISO_MICROSECOND_SLOT,
                       Int32Value(microsecond));

  // Step 9.
  object->setFixedSlot(PlainTimeObject::ISO_NANOSECOND_SLOT,
                       Int32Value(nanosecond));

  // Step 10.
  return object;
}

/**
 * BalanceTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
template <typename IntT>
static BalancedTime BalanceTime(IntT hour, IntT minute, IntT second,
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

  PlainTime time = {};

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
  int32_t days = divmod(hour, 24, &time.hour);

  // Step 13.
  MOZ_ASSERT(IsValidTime(time));
  return {days, time};
}

/**
 * BalanceTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
static BalancedTime BalanceTime(int32_t hour, int32_t minute, int32_t second,
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
BalancedTime js::temporal::BalanceTime(const PlainTime& time,
                                       int64_t nanoseconds) {
  MOZ_ASSERT(IsValidTime(time));
  MOZ_ASSERT(std::abs(nanoseconds) <= ToNanoseconds(TemporalUnit::Day));

  return ::BalanceTime<int64_t>(time.hour, time.minute, time.second,
                                time.millisecond, time.microsecond,
                                time.nanosecond + nanoseconds);
}

/**
 * DifferenceTime ( h1, min1, s1, ms1, mus1, ns1, h2, min2, s2, ms2, mus2, ns2 )
 */
NormalizedTimeDuration js::temporal::DifferenceTime(const PlainTime& time1,
                                                    const PlainTime& time2) {
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
  auto result = NormalizeTimeDuration(hours, minutes, seconds, milliseconds,
                                      microseconds, nanoseconds);

  // Step 8.
  MOZ_ASSERT(result.abs().toNanoseconds() <
             Int128{ToNanoseconds(TemporalUnit::Day)});

  // Step 9.
  return result;
}

/**
 * ToTemporalTime ( item [ , overflow ] )
 */
static bool ToTemporalTime(JSContext* cx, Handle<Value> item,
                           TemporalOverflow overflow, PlainTime* result) {
  // Steps 1-2. (Not applicable in our implementation.)

  // Steps 3-4.
  if (item.isObject()) {
    // Step 3.
    Rooted<JSObject*> itemObj(cx, &item.toObject());

    // Step 3.a.
    if (auto* time = itemObj->maybeUnwrapIf<PlainTimeObject>()) {
      *result = ToPlainTime(time);
      return true;
    }

    // Step 3.b.
    if (auto* zonedDateTime = itemObj->maybeUnwrapIf<ZonedDateTimeObject>()) {
      auto epochInstant = ToInstant(zonedDateTime);
      Rooted<TimeZoneValue> timeZone(cx, zonedDateTime->timeZone());

      if (!timeZone.wrap(cx)) {
        return false;
      }

      // Steps 3.b.i-iii.
      PlainDateTime dateTime;
      if (!GetPlainDateTimeFor(cx, timeZone, epochInstant, &dateTime)) {
        return false;
      }

      // Step 3.b.iv.
      *result = dateTime.time;
      return true;
    }

    // Step 3.c.
    if (auto* dateTime = itemObj->maybeUnwrapIf<PlainDateTimeObject>()) {
      *result = ToPlainTime(dateTime);
      return true;
    }

    // Step 3.d.
    TemporalTimeLike timeResult;
    if (!ToTemporalTimeRecord(cx, itemObj, &timeResult)) {
      return false;
    }

    // Step 3.e.
    if (!RegulateTime(cx, timeResult, overflow, result)) {
      return false;
    }
  } else {
    // Step 4.

    // Step 4.a.
    if (!item.isString()) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, item,
                       nullptr, "not a string");
      return false;
    }
    Rooted<JSString*> string(cx, item.toString());

    // Step 4.b.
    if (!ParseTemporalTimeString(cx, string, result)) {
      return false;
    }

    // Step 4.c.
    MOZ_ASSERT(IsValidTime(*result));
  }

  // Step 5.
  return true;
}

/**
 * ToTemporalTime ( item [ , overflow ] )
 */
static PlainTimeObject* ToTemporalTime(JSContext* cx, Handle<Value> item,
                                       TemporalOverflow overflow) {
  PlainTime time;
  if (!ToTemporalTime(cx, item, overflow, &time)) {
    return nullptr;
  }
  MOZ_ASSERT(IsValidTime(time));

  return CreateTemporalTime(cx, time);
}

/**
 * ToTemporalTime ( item [ , overflow ] )
 */
bool js::temporal::ToTemporalTime(JSContext* cx, Handle<Value> item,
                                  PlainTime* result) {
  return ToTemporalTime(cx, item, TemporalOverflow::Constrain, result);
}

/**
 * CompareTemporalTime ( h1, min1, s1, ms1, mus1, ns1, h2, min2, s2, ms2, mus2,
 * ns2 )
 */
int32_t js::temporal::CompareTemporalTime(const PlainTime& one,
                                          const PlainTime& two) {
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

/**
 * ToTemporalTimeRecord ( temporalTimeLike [ , completeness ] )
 */
static bool ToTemporalTimeRecord(JSContext* cx,
                                 Handle<JSObject*> temporalTimeLike,
                                 TemporalTimeLike* result) {
  // Steps 1 and 3-4. (Not applicable in our implementation.)

  // Step 2. (Inlined call to PrepareTemporalFields.)
  // PrepareTemporalFields, step 1. (Not applicable in our implementation.)

  // PrepareTemporalFields, step 2.
  bool any = false;

  // PrepareTemporalFields, steps 3-4. (Loop unrolled)
  Rooted<Value> value(cx);
  auto getTimeProperty = [&](Handle<PropertyName*> property, const char* name,
                             double* num) {
    // Step 4.a.
    if (!GetProperty(cx, temporalTimeLike, temporalTimeLike, property,
                     &value)) {
      return false;
    }

    // Step 4.b.
    if (!value.isUndefined()) {
      // Step 4.b.i.
      any = true;

      // Step 4.b.ii.2.
      if (!ToIntegerWithTruncation(cx, value, name, num)) {
        return false;
      }
    }
    return true;
  };

  if (!getTimeProperty(cx->names().hour, "hour", &result->hour)) {
    return false;
  }
  if (!getTimeProperty(cx->names().microsecond, "microsecond",
                       &result->microsecond)) {
    return false;
  }
  if (!getTimeProperty(cx->names().millisecond, "millisecond",
                       &result->millisecond)) {
    return false;
  }
  if (!getTimeProperty(cx->names().minute, "minute", &result->minute)) {
    return false;
  }
  if (!getTimeProperty(cx->names().nanosecond, "nanosecond",
                       &result->nanosecond)) {
    return false;
  }
  if (!getTimeProperty(cx->names().second, "second", &result->second)) {
    return false;
  }

  // PrepareTemporalFields, step 5.
  if (!any) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_TIME_MISSING_UNIT);
    return false;
  }

  // Steps 5-16. (Performed implicitly in our implementation.)

  // Step 17.
  return true;
}

/**
 * ToTemporalTimeRecord ( temporalTimeLike [ , completeness ] )
 */
bool js::temporal::ToTemporalTimeRecord(JSContext* cx,
                                        Handle<JSObject*> temporalTimeLike,
                                        TemporalTimeLike* result) {
  // Step 3.a. (Set all fields to zero.)
  *result = {};

  // Steps 1-2 and 4-17.
  return ::ToTemporalTimeRecord(cx, temporalTimeLike, result);
}

static int64_t TimeToNanos(const PlainTime& time) {
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
 * RoundTime ( hour, minute, second, millisecond, microsecond, nanosecond,
 * increment, unit, roundingMode )
 */
RoundedTime js::temporal::RoundTime(const PlainTime& time, Increment increment,
                                    TemporalUnit unit,
                                    TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidTime(time));
  MOZ_ASSERT(unit >= TemporalUnit::Day);
  MOZ_ASSERT_IF(unit > TemporalUnit::Day,
                increment <= MaximumTemporalDurationRoundingIncrement(unit));
  MOZ_ASSERT_IF(unit == TemporalUnit::Day, increment == Increment{1});

  int32_t days = 0;
  auto [hour, minute, second, millisecond, microsecond, nanosecond] = time;

  // Take the same approach as used in RoundDuration() to perform exact
  // mathematical operations without possible loss of precision.

  // Steps 1-6.
  PlainTime quantity;
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
  auto balanced =
      ::BalanceTime(hour, minute, second, millisecond, microsecond, nanosecond);
  return {int64_t(balanced.days), balanced.time};
}

/**
 * AddTime ( hour, minute, second, millisecond, microsecond, nanosecond, norm )
 */
AddedTime js::temporal::AddTime(const PlainTime& time,
                                const NormalizedTimeDuration& duration) {
  MOZ_ASSERT(IsValidTime(time));
  MOZ_ASSERT(IsValidNormalizedTimeDuration(duration));

  auto [seconds, nanoseconds] = duration;
  if (seconds < 0 && nanoseconds > 0) {
    seconds += 1;
    nanoseconds -= 1'000'000'000;
  }
  MOZ_ASSERT(std::abs(nanoseconds) <= 999'999'999);

  // Step 1.
  int64_t second = time.second + seconds;

  // Step 2.
  int32_t nanosecond = time.nanosecond + nanoseconds;

  // Step 3.
  auto balanced =
      ::BalanceTime<int64_t>(time.hour, time.minute, second, time.millisecond,
                             time.microsecond, nanosecond);
  return {balanced.days, balanced.time};
}

/**
 * DifferenceTemporalPlainTime ( operation, temporalTime, other, options )
 */
static bool DifferenceTemporalPlainTime(JSContext* cx,
                                        TemporalDifference operation,
                                        const CallArgs& args) {
  auto temporalTime =
      ToPlainTime(&args.thisv().toObject().as<PlainTimeObject>());

  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  PlainTime other;
  if (!ToTemporalTime(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 3-4.
  DifferenceSettings settings;
  if (args.hasDefined(1)) {
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", ToName(operation), args[1]));
    if (!options) {
      return false;
    }

    // Step 3.
    Rooted<PlainObject*> resolvedOptions(cx,
                                         SnapshotOwnProperties(cx, options));
    if (!resolvedOptions) {
      return false;
    }

    // Step 4.
    if (!GetDifferenceSettings(
            cx, operation, resolvedOptions, TemporalUnitGroup::Time,
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
  auto diff = DifferenceTime(temporalTime, other);

  // Step 6.
  if (settings.smallestUnit != TemporalUnit::Nanosecond ||
      settings.roundingIncrement != Increment{1}) {
    // Steps 6.a-b.
    diff = RoundDuration(diff, settings.roundingIncrement,
                         settings.smallestUnit, settings.roundingMode);
  }

  // Step 7.
  TimeDuration balancedDuration;
  if (!BalanceTimeDuration(cx, diff, settings.largestUnit, &balancedDuration)) {
    return false;
  }

  // Step 8.
  auto duration = balancedDuration.toDuration();
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

enum class PlainTimeDuration { Add, Subtract };

/**
 * AddDurationToOrSubtractDurationFromPlainTime ( operation, temporalTime,
 * temporalDurationLike )
 */
static bool AddDurationToOrSubtractDurationFromPlainTime(
    JSContext* cx, PlainTimeDuration operation, const CallArgs& args) {
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  auto time = ToPlainTime(temporalTime);

  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Duration duration;
  if (!ToTemporalDurationRecord(cx, args.get(0), &duration)) {
    return false;
  }

  // Step 3.
  if (operation == PlainTimeDuration::Subtract) {
    duration = duration.negate();
  }
  auto timeDuration = NormalizeTimeDuration(duration);

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

  // Step 8.
  auto* temporalTime = CreateTemporalTime(cx, args, hour, minute, second,
                                          millisecond, microsecond, nanosecond);
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

  // Step 1. (Not applicable)

  auto overflow = TemporalOverflow::Constrain;
  if (args.hasDefined(1)) {
    // Step 2.
    Rooted<JSObject*> options(cx,
                              RequireObjectArg(cx, "options", "from", args[1]));
    if (!options) {
      return false;
    }

    // Step 3.
    if (!GetTemporalOverflowOption(cx, options, &overflow)) {
      return false;
    }
  }

  // Steps 4-5.
  auto* result = ToTemporalTime(cx, args.get(0), overflow);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainTime.compare ( one, two )
 */
static bool PlainTime_compare(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  PlainTime one;
  if (!ToTemporalTime(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  PlainTime two;
  if (!ToTemporalTime(cx, args.get(1), &two)) {
    return false;
  }

  // Step 3.
  args.rval().setInt32(CompareTemporalTime(one, two));
  return true;
}

/**
 * get Temporal.PlainTime.prototype.hour
 */
static bool PlainTime_hour(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  args.rval().setInt32(temporalTime->isoHour());
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
  args.rval().setInt32(temporalTime->isoMinute());
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
  args.rval().setInt32(temporalTime->isoSecond());
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
  args.rval().setInt32(temporalTime->isoMillisecond());
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
  args.rval().setInt32(temporalTime->isoMicrosecond());
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
  args.rval().setInt32(temporalTime->isoNanosecond());
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
  return AddDurationToOrSubtractDurationFromPlainTime(
      cx, PlainTimeDuration::Add, args);
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
  return AddDurationToOrSubtractDurationFromPlainTime(
      cx, PlainTimeDuration::Subtract, args);
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
  auto time = ToPlainTime(temporalTime);

  // Step 3.
  Rooted<JSObject*> temporalTimeLike(
      cx, RequireObjectArg(cx, "temporalTimeLike", "with", args.get(0)));
  if (!temporalTimeLike) {
    return false;
  }
  if (!ThrowIfTemporalLikeObject(cx, temporalTimeLike)) {
    return false;
  }

  auto overflow = TemporalOverflow::Constrain;
  if (args.hasDefined(1)) {
    // Step 4.
    Rooted<JSObject*> options(cx,
                              RequireObjectArg(cx, "options", "with", args[1]));
    if (!options) {
      return false;
    }

    // Step 5.
    if (!GetTemporalOverflowOption(cx, options, &overflow)) {
      return false;
    }
  }

  // Steps 6-18.
  TemporalTimeLike partialTime = {
      double(time.hour),        double(time.minute),
      double(time.second),      double(time.millisecond),
      double(time.microsecond), double(time.nanosecond),
  };
  if (!::ToTemporalTimeRecord(cx, temporalTimeLike, &partialTime)) {
    return false;
  }

  // Step 19.
  PlainTime result;
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
  auto time = ToPlainTime(temporalTime);

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
  auto temporalTime =
      ToPlainTime(&args.thisv().toObject().as<PlainTimeObject>());

  // Step 3.
  PlainTime other;
  if (!ToTemporalTime(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 4-10.
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
 * Temporal.PlainTime.prototype.toPlainDateTime ( temporalDate )
 */
static bool PlainTime_toPlainDateTime(JSContext* cx, const CallArgs& args) {
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  auto time = ToPlainTime(temporalTime);

  // Step 3.
  Rooted<PlainDateWithCalendar> plainDate(cx);
  if (!ToTemporalDate(cx, args.get(0), &plainDate)) {
    return false;
  }
  auto date = plainDate.date();
  auto calendar = plainDate.calendar();

  // Step 4.
  auto* result = CreateTemporalDateTime(cx, {date, time}, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainTime.prototype.toPlainDateTime ( temporalDate )
 */
static bool PlainTime_toPlainDateTime(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_toPlainDateTime>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.toZonedDateTime ( item )
 *
 * |item| is an options object with `plainDate` and `timeZone` properties.
 */
static bool PlainTime_toZonedDateTime(JSContext* cx, const CallArgs& args) {
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  auto time = ToPlainTime(temporalTime);

  // Step 3.
  Rooted<JSObject*> itemObj(
      cx, RequireObjectArg(cx, "item", "toZonedDateTime", args.get(0)));
  if (!itemObj) {
    return false;
  }

  // Step 4.
  Rooted<Value> temporalDateLike(cx);
  if (!GetProperty(cx, itemObj, args[0], cx->names().plainDate,
                   &temporalDateLike)) {
    return false;
  }

  // Step 5.
  if (temporalDateLike.isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_MISSING_PROPERTY, "plainDate");
    return false;
  }

  // Step 6.
  Rooted<PlainDateWithCalendar> plainDate(cx);
  if (!ToTemporalDate(cx, temporalDateLike, &plainDate)) {
    return false;
  }
  auto date = plainDate.date();
  auto calendar = plainDate.calendar();

  // Step 7.
  Rooted<Value> temporalTimeZoneLike(cx);
  if (!GetProperty(cx, itemObj, itemObj, cx->names().timeZone,
                   &temporalTimeZoneLike)) {
    return false;
  }

  // Step 8.
  if (temporalTimeZoneLike.isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_MISSING_PROPERTY, "timeZone");
    return false;
  }

  // Step 9.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZone(cx, temporalTimeZoneLike, &timeZone)) {
    return false;
  }

  // Step 10.
  Rooted<PlainDateTimeWithCalendar> temporalDateTime(cx);
  if (!CreateTemporalDateTime(cx, {date, time}, calendar, &temporalDateTime)) {
    return false;
  }

  // Steps 11-12.
  Instant instant;
  if (!GetInstantFor(cx, timeZone, temporalDateTime,
                     TemporalDisambiguation::Compatible, &instant)) {
    return false;
  }

  // Step 13.
  auto* result = CreateTemporalZonedDateTime(cx, instant, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.PlainTime.prototype.toZonedDateTime ( item )
 */
static bool PlainTime_toZonedDateTime(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_toZonedDateTime>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.getISOFields ( )
 */
static bool PlainTime_getISOFields(JSContext* cx, const CallArgs& args) {
  Rooted<PlainTimeObject*> temporalTime(
      cx, &args.thisv().toObject().as<PlainTimeObject>());
  auto time = ToPlainTime(temporalTime);

  // Step 3.
  Rooted<IdValueVector> fields(cx, IdValueVector(cx));

  // Step 4.
  if (!fields.emplaceBack(NameToId(cx->names().isoHour),
                          Int32Value(time.hour))) {
    return false;
  }

  // Step 5.
  if (!fields.emplaceBack(NameToId(cx->names().isoMicrosecond),
                          Int32Value(time.microsecond))) {
    return false;
  }

  // Step 6.
  if (!fields.emplaceBack(NameToId(cx->names().isoMillisecond),
                          Int32Value(time.millisecond))) {
    return false;
  }

  // Step 7.
  if (!fields.emplaceBack(NameToId(cx->names().isoMinute),
                          Int32Value(time.minute))) {
    return false;
  }

  // Step 8.
  if (!fields.emplaceBack(NameToId(cx->names().isoNanosecond),
                          Int32Value(time.nanosecond))) {
    return false;
  }

  // Step 9.
  if (!fields.emplaceBack(NameToId(cx->names().isoSecond),
                          Int32Value(time.second))) {
    return false;
  }

  // Step 10.
  auto* obj = NewPlainObjectWithUniqueNames(cx, fields);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainTime.prototype.getISOFields ( )
 */
static bool PlainTime_getISOFields(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainTime, PlainTime_getISOFields>(cx, args);
}

/**
 * Temporal.PlainTime.prototype.toString ( [ options ] )
 */
static bool PlainTime_toString(JSContext* cx, const CallArgs& args) {
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  auto time = ToPlainTime(temporalTime);

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
  JSString* str =
      TemporalTimeToString(cx, roundedTime.time, precision.precision);
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
  auto* temporalTime = &args.thisv().toObject().as<PlainTimeObject>();
  auto time = ToPlainTime(temporalTime);

  // Step 3.
  JSString* str = TemporalTimeToString(cx, time, Precision::Auto());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
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
  auto time = ToPlainTime(temporalTime);

  // Step 3.
  JSString* str = TemporalTimeToString(cx, time, Precision::Auto());
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
    JS_FN("toPlainDateTime", PlainTime_toPlainDateTime, 1, 0),
    JS_FN("toZonedDateTime", PlainTime_toZonedDateTime, 1, 0),
    JS_FN("getISOFields", PlainTime_getISOFields, 0, 0),
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
