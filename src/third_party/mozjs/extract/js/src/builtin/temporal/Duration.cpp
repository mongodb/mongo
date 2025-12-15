/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/Duration.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/CheckedInt.h"
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
#include "NamespaceImports.h"

#include "builtin/intl/DurationFormat.h"
#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/CalendarFields.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/Int128.h"
#include "builtin/temporal/Int96.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainTime.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/TimeZone.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "gc/GCEnum.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/Printer.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/Value.h"
#include "util/StringBuilder.h"
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

static inline bool IsDuration(Handle<Value> v) {
  return v.isObject() && v.toObject().is<DurationObject>();
}

#ifdef DEBUG
static bool IsIntegerOrInfinity(double d) {
  return IsInteger(d) || std::isinf(d);
}

static bool IsIntegerOrInfinityDuration(const Duration& duration) {
  const auto& [years, months, weeks, days, hours, minutes, seconds,
               milliseconds, microseconds, nanoseconds] = duration;

  // Integers exceeding the Number range are represented as infinity.

  return IsIntegerOrInfinity(years) && IsIntegerOrInfinity(months) &&
         IsIntegerOrInfinity(weeks) && IsIntegerOrInfinity(days) &&
         IsIntegerOrInfinity(hours) && IsIntegerOrInfinity(minutes) &&
         IsIntegerOrInfinity(seconds) && IsIntegerOrInfinity(milliseconds) &&
         IsIntegerOrInfinity(microseconds) && IsIntegerOrInfinity(nanoseconds);
}

static bool IsIntegerDuration(const Duration& duration) {
  const auto& [years, months, weeks, days, hours, minutes, seconds,
               milliseconds, microseconds, nanoseconds] = duration;

  return IsInteger(years) && IsInteger(months) && IsInteger(weeks) &&
         IsInteger(days) && IsInteger(hours) && IsInteger(minutes) &&
         IsInteger(seconds) && IsInteger(milliseconds) &&
         IsInteger(microseconds) && IsInteger(nanoseconds);
}
#endif

/**
 * DurationSign ( duration )
 */
int32_t js::temporal::DurationSign(const Duration& duration) {
  MOZ_ASSERT(IsIntegerOrInfinityDuration(duration));

  const auto& [years, months, weeks, days, hours, minutes, seconds,
               milliseconds, microseconds, nanoseconds] = duration;

  // Step 1.
  for (auto v : {years, months, weeks, days, hours, minutes, seconds,
                 milliseconds, microseconds, nanoseconds}) {
    // Step 1.a.
    if (v < 0) {
      return -1;
    }

    // Step 1.b.
    if (v > 0) {
      return 1;
    }
  }

  // Step 2.
  return 0;
}

/**
 * DateDurationSign ( dateDuration )
 */
int32_t js::temporal::DateDurationSign(const DateDuration& duration) {
  const auto& [years, months, weeks, days] = duration;

  // Step 1.
  for (auto v : {years, months, weeks, days}) {
    // Step 1.a.
    if (v < 0) {
      return -1;
    }

    // Step 1.b.
    if (v > 0) {
      return 1;
    }
  }

  // Step 2.
  return 0;
}

/**
 * InternalDurationSign ( internalDuration )
 */
static int32_t InternalDurationSign(const InternalDuration& duration) {
  MOZ_ASSERT(IsValidDuration(duration));

  if (int32_t sign = DateDurationSign(duration.date)) {
    return sign;
  }
  return TimeDurationSign(duration.time);
}

/**
 * Create a time duration from a nanoseconds amount.
 */
static TimeDuration TimeDurationFromNanoseconds(const Int96& nanoseconds) {
  // Split into seconds and nanoseconds.
  auto [seconds, nanos] = nanoseconds / ToNanoseconds(TemporalUnit::Second);

  return {{seconds, nanos}};
}

/**
 * Create a time duration from a nanoseconds amount. Return Nothing if the value
 * is too large.
 */
static mozilla::Maybe<TimeDuration> TimeDurationFromNanoseconds(
    double nanoseconds) {
  MOZ_ASSERT(IsInteger(nanoseconds));

  if (auto int96 = Int96::fromInteger(nanoseconds)) {
    // The number of time duration seconds must not exceed `2**53 - 1`.
    constexpr auto limit =
        Int96{uint64_t(1) << 53} * ToNanoseconds(TemporalUnit::Second);

    if (int96->abs() < limit) {
      return mozilla::Some(TimeDurationFromNanoseconds(*int96));
    }
  }
  return mozilla::Nothing();
}

/**
 * Create a time duration from a microseconds amount.
 */
static TimeDuration TimeDurationFromMicroseconds(const Int96& microseconds) {
  // Split into seconds and microseconds.
  auto [seconds, micros] = microseconds / ToMicroseconds(TemporalUnit::Second);

  // Scale microseconds to nanoseconds.
  int32_t nanos = micros * int32_t(ToNanoseconds(TemporalUnit::Microsecond));

  return {{seconds, nanos}};
}

/**
 * Create a time duration from a microseconds amount. Return Nothing if the
 * value is too large.
 */
static mozilla::Maybe<TimeDuration> TimeDurationFromMicroseconds(
    double microseconds) {
  MOZ_ASSERT(IsInteger(microseconds));

  if (auto int96 = Int96::fromInteger(microseconds)) {
    // The number of time duration seconds must not exceed `2**53 - 1`.
    constexpr auto limit =
        Int96{uint64_t(1) << 53} * ToMicroseconds(TemporalUnit::Second);

    if (int96->abs() < limit) {
      return mozilla::Some(TimeDurationFromMicroseconds(*int96));
    }
  }
  return mozilla::Nothing();
}

/**
 * Create a time duration from a duration. Return Nothing if any duration
 * value is too large.
 */
static mozilla::Maybe<TimeDuration> TimeDurationFromDuration(
    const Duration& duration) {
  do {
    auto nanoseconds = TimeDurationFromNanoseconds(duration.nanoseconds);
    if (!nanoseconds) {
      break;
    }
    MOZ_ASSERT(IsValidTimeDuration(*nanoseconds));

    auto microseconds = TimeDurationFromMicroseconds(duration.microseconds);
    if (!microseconds) {
      break;
    }
    MOZ_ASSERT(IsValidTimeDuration(*microseconds));

    // Overflows for millis/seconds/minutes/hours/days always result in an
    // invalid time duration.

    int64_t milliseconds;
    if (!mozilla::NumberEqualsInt64(duration.milliseconds, &milliseconds)) {
      break;
    }

    int64_t seconds;
    if (!mozilla::NumberEqualsInt64(duration.seconds, &seconds)) {
      break;
    }

    int64_t minutes;
    if (!mozilla::NumberEqualsInt64(duration.minutes, &minutes)) {
      break;
    }

    int64_t hours;
    if (!mozilla::NumberEqualsInt64(duration.hours, &hours)) {
      break;
    }

    int64_t days;
    if (!mozilla::NumberEqualsInt64(duration.days, &days)) {
      break;
    }

    // Compute the overall amount of milliseconds.
    mozilla::CheckedInt64 millis = days;
    millis *= 24;
    millis += hours;
    millis *= 60;
    millis += minutes;
    millis *= 60;
    millis += seconds;
    millis *= 1000;
    millis += milliseconds;
    if (!millis.isValid()) {
      break;
    }

    auto milli = TimeDuration::fromMilliseconds(millis.value());
    if (!IsValidTimeDuration(milli)) {
      break;
    }

    // Compute the overall time duration.
    auto result = milli + *microseconds + *nanoseconds;
    if (!IsValidTimeDuration(result)) {
      break;
    }

    return mozilla::Some(result);
  } while (false);

  return mozilla::Nothing();
}

/**
 * TimeDurationFromComponents ( hours, minutes, seconds, milliseconds,
 * microseconds, nanoseconds )
 */
static TimeDuration TimeDurationFromComponents(double hours, double minutes,
                                               double seconds,
                                               double milliseconds,
                                               double microseconds,
                                               double nanoseconds) {
  MOZ_ASSERT(IsInteger(hours));
  MOZ_ASSERT(IsInteger(minutes));
  MOZ_ASSERT(IsInteger(seconds));
  MOZ_ASSERT(IsInteger(milliseconds));
  MOZ_ASSERT(IsInteger(microseconds));
  MOZ_ASSERT(IsInteger(nanoseconds));

  // Steps 1-3.
  mozilla::CheckedInt64 millis = int64_t(hours);
  millis *= 60;
  millis += int64_t(minutes);
  millis *= 60;
  millis += int64_t(seconds);
  millis *= 1000;
  millis += int64_t(milliseconds);
  MOZ_ASSERT(millis.isValid());

  auto timeDuration = TimeDuration::fromMilliseconds(millis.value());

  // Step 4.
  auto micros = Int96::fromInteger(microseconds);
  MOZ_ASSERT(micros);

  timeDuration += TimeDurationFromMicroseconds(*micros);

  // Step 5.
  auto nanos = Int96::fromInteger(nanoseconds);
  MOZ_ASSERT(nanos);

  timeDuration += TimeDurationFromNanoseconds(*nanos);

  // Step 6.
  MOZ_ASSERT(IsValidTimeDuration(timeDuration));

  // Step 7.
  return timeDuration;
}

/**
 * TimeDurationFromComponents ( hours, minutes, seconds, milliseconds,
 * microseconds, nanoseconds )
 */
TimeDuration js::temporal::TimeDurationFromComponents(
    const Duration& duration) {
  MOZ_ASSERT(IsValidDuration(duration));

  return ::TimeDurationFromComponents(
      duration.hours, duration.minutes, duration.seconds, duration.milliseconds,
      duration.microseconds, duration.nanoseconds);
}

/**
 * Add24HourDaysToTimeDuration ( d, days )
 */
static bool Add24HourDaysToTimeDuration(JSContext* cx, const TimeDuration& d,
                                        int64_t days, TimeDuration* result) {
  MOZ_ASSERT(IsValidTimeDuration(d));

  // Step 1.
  if (days > TimeDuration::max().toDays()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_INVALID_NORMALIZED_TIME);
    return false;
  }

  auto timeDurationDays = TimeDuration::fromDays(days);
  if (!IsValidTimeDuration(timeDurationDays)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_INVALID_NORMALIZED_TIME);
    return false;
  }

  // Step 2.
  auto sum = d + timeDurationDays;
  if (!IsValidTimeDuration(sum)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_INVALID_NORMALIZED_TIME);
    return false;
  }

  // Step 3.
  *result = sum;
  return true;
}

/**
 * ToInternalDurationRecordWith24HourDays ( duration )
 */
InternalDuration js::temporal::ToInternalDurationRecordWith24HourDays(
    const Duration& duration) {
  MOZ_ASSERT(IsValidDuration(duration));

  // Step 1.
  auto timeDuration = TimeDurationFromComponents(duration);

  // Step 2. (Inlined Add24HourDaysToTimeDuration)
  timeDuration += TimeDuration::fromDays(int64_t(duration.days));

  // Step 3.
  auto dateDuration = DateDuration{
      int64_t(duration.years),
      int64_t(duration.months),
      int64_t(duration.weeks),
      0,
  };

  // Step 4. (Inlined CombineDateAndTimeDuration)
  return InternalDuration{dateDuration, timeDuration};
}

/**
 * ToDateDurationRecordWithoutTime ( duration )
 */
DateDuration js::temporal::ToDateDurationRecordWithoutTime(
    const Duration& duration) {
  // Step 1.
  auto internalDuration = ToInternalDurationRecordWith24HourDays(duration);

  // Step 2.
  int64_t days = internalDuration.time.toDays();

  // Step 3.
  auto result = DateDuration{
      internalDuration.date.years,
      internalDuration.date.months,
      internalDuration.date.weeks,
      days,
  };
  MOZ_ASSERT(IsValidDuration(result));

  return result;
}

/**
 * TemporalDurationFromInternal ( internalDuration, largestUnit )
 */
static Duration TemporalDurationFromInternal(const TimeDuration& timeDuration,
                                             TemporalUnit largestUnit) {
  MOZ_ASSERT(IsValidTimeDuration(timeDuration));
  MOZ_ASSERT(largestUnit <= TemporalUnit::Second,
             "fallible fractional seconds units");

  auto [seconds, nanoseconds] = timeDuration.denormalize();

  // Step 1.
  int64_t days = 0;
  int64_t hours = 0;
  int64_t minutes = 0;
  int64_t milliseconds = 0;
  int64_t microseconds = 0;

  // Steps 2-3. (Not applicable in our implementation.)
  //
  // We don't need to convert to positive numbers, because integer division
  // truncates and the %-operator has modulo semantics.

  // Steps 4-11.
  switch (largestUnit) {
    // Step 4.
    case TemporalUnit::Year:
    case TemporalUnit::Month:
    case TemporalUnit::Week:
    case TemporalUnit::Day: {
      // Step 4.a.
      microseconds = nanoseconds / 1000;

      // Step 4.b.
      nanoseconds = nanoseconds % 1000;

      // Step 4.c.
      milliseconds = microseconds / 1000;

      // Step 4.d.
      microseconds = microseconds % 1000;

      // Steps 4.e-f. (Not applicable)
      MOZ_ASSERT(std::abs(milliseconds) <= 999);

      // Step 4.g.
      minutes = seconds / 60;

      // Step 4.h.
      seconds = seconds % 60;

      // Step 4.i.
      hours = minutes / 60;

      // Step 4.j.
      minutes = minutes % 60;

      // Step 4.k.
      days = hours / 24;

      // Step 4.l.
      hours = hours % 24;

      break;
    }

      // Step 5.
    case TemporalUnit::Hour: {
      // Step 5.a.
      microseconds = nanoseconds / 1000;

      // Step 5.b.
      nanoseconds = nanoseconds % 1000;

      // Step 5.c.
      milliseconds = microseconds / 1000;

      // Step 5.d.
      microseconds = microseconds % 1000;

      // Steps 5.e-f. (Not applicable)
      MOZ_ASSERT(std::abs(milliseconds) <= 999);

      // Step 5.g.
      minutes = seconds / 60;

      // Step 5.h.
      seconds = seconds % 60;

      // Step 5.i.
      hours = minutes / 60;

      // Step 5.j.
      minutes = minutes % 60;

      break;
    }

    case TemporalUnit::Minute: {
      // Step 6.a.
      microseconds = nanoseconds / 1000;

      // Step 6.b.
      nanoseconds = nanoseconds % 1000;

      // Step 6.c.
      milliseconds = microseconds / 1000;

      // Step 6.d.
      microseconds = microseconds % 1000;

      // Steps 6.e-f. (Not applicable)
      MOZ_ASSERT(std::abs(milliseconds) <= 999);

      // Step 6.g.
      minutes = seconds / 60;

      // Step 6.h.
      seconds = seconds % 60;

      break;
    }

    // Step 7.
    case TemporalUnit::Second: {
      // Step 7.a.
      microseconds = nanoseconds / 1000;

      // Step 7.b.
      nanoseconds = nanoseconds % 1000;

      // Step 7.c.
      milliseconds = microseconds / 1000;

      // Step 7.d.
      microseconds = microseconds % 1000;

      // Steps 7.e-f. (Not applicable)
      MOZ_ASSERT(std::abs(milliseconds) <= 999);

      break;
    }

    // Steps 8-11. (Not applicable in our implementation)
    case TemporalUnit::Millisecond:
    case TemporalUnit::Microsecond:
    case TemporalUnit::Nanosecond:
    case TemporalUnit::Auto:
      MOZ_CRASH("Unexpected temporal unit");
  }

  // Step 12.
  auto result = Duration{
      0,
      0,
      0,
      double(days),
      double(hours),
      double(minutes),
      double(seconds),
      double(milliseconds),
      double(microseconds),
      double(nanoseconds),
  };
  MOZ_ASSERT(IsValidDuration(result));
  return result;
}

/**
 * TemporalDurationFromInternal ( internalDuration, largestUnit )
 */
bool js::temporal::TemporalDurationFromInternal(
    JSContext* cx, const TimeDuration& timeDuration, TemporalUnit largestUnit,
    Duration* result) {
  MOZ_ASSERT(IsValidTimeDuration(timeDuration));

  auto [seconds, nanoseconds] = timeDuration.denormalize();

  // Steps 1-3. (Not applicable in our implementation.)
  //
  // We don't need to convert to positive numbers, because integer division
  // truncates and the %-operator has modulo semantics.

  // Steps 4-10.
  switch (largestUnit) {
    // Steps 4-7.
    case TemporalUnit::Year:
    case TemporalUnit::Month:
    case TemporalUnit::Week:
    case TemporalUnit::Day:
    case TemporalUnit::Hour:
    case TemporalUnit::Minute:
    case TemporalUnit::Second:
      *result = ::TemporalDurationFromInternal(timeDuration, largestUnit);
      return true;

    // Step 8.
    case TemporalUnit::Millisecond: {
      // Valid time durations must be below |limit|.
      constexpr auto limit = TimeDuration::max().toMilliseconds() + 1;

      // The largest possible milliseconds value whose double representation
      // doesn't exceed the time duration limit.
      constexpr auto max = int64_t(0x7cff'ffff'ffff'fdff);

      // Assert |max| is the maximum allowed milliseconds value.
      static_assert(double(max) < double(limit));
      static_assert(double(max + 1) >= double(limit));

      static_assert((TimeDuration::max().seconds + 1) *
                            ToMilliseconds(TemporalUnit::Second) <=
                        INT64_MAX,
                    "total number duration milliseconds fits into int64");

      // Step 8.a.
      int64_t microseconds = nanoseconds / 1000;

      // Step 8.b.
      nanoseconds = nanoseconds % 1000;

      // Step 8.c.
      int64_t milliseconds = microseconds / 1000;
      MOZ_ASSERT(std::abs(milliseconds) <= 999);

      // Step 8.d.
      microseconds = microseconds % 1000;

      auto millis =
          (seconds * ToMilliseconds(TemporalUnit::Second)) + milliseconds;
      if (std::abs(millis) > max) {
        JS_ReportErrorNumberASCII(
            cx, GetErrorMessage, nullptr,
            JSMSG_TEMPORAL_DURATION_INVALID_NORMALIZED_TIME);
        return false;
      }

      // Step 11.
      *result = {0,
                 0,
                 0,
                 0,
                 0,
                 0,
                 0,
                 double(millis),
                 double(microseconds),
                 double(nanoseconds)};
      MOZ_ASSERT(IsValidDuration(*result));
      return true;
    }

    // Step 9.
    case TemporalUnit::Microsecond: {
      // Valid time durations must be below |limit|.
      constexpr auto limit =
          Uint128{TimeDuration::max().toMicroseconds()} + Uint128{1};

      // The largest possible microseconds value whose double representation
      // doesn't exceed the time duration limit.
      constexpr auto max =
          (Uint128{0x1e8} << 64) + Uint128{0x47ff'ffff'fff7'ffff};
      static_assert(max < limit);

      // Assert |max| is the maximum allowed microseconds value.
      MOZ_ASSERT(double(max) < double(limit));
      MOZ_ASSERT(double(max + Uint128{1}) >= double(limit));

      // Step 9.a.
      int64_t microseconds = nanoseconds / 1000;
      MOZ_ASSERT(std::abs(microseconds) <= 999'999);

      // Step 9.b.
      nanoseconds = nanoseconds % 1000;

      auto micros =
          (Int128{seconds} * Int128{ToMicroseconds(TemporalUnit::Second)}) +
          Int128{microseconds};
      if (micros.abs() > max) {
        JS_ReportErrorNumberASCII(
            cx, GetErrorMessage, nullptr,
            JSMSG_TEMPORAL_DURATION_INVALID_NORMALIZED_TIME);
        return false;
      }

      // Step 11.
      *result = {0, 0, 0, 0, 0, 0, 0, 0, double(micros), double(nanoseconds)};
      MOZ_ASSERT(IsValidDuration(*result));
      return true;
    }

    // Step 10.
    case TemporalUnit::Nanosecond: {
      // Valid time durations must be below |limit|.
      constexpr auto limit =
          Uint128{TimeDuration::max().toNanoseconds()} + Uint128{1};

      // The largest possible nanoseconds value whose double representation
      // doesn't exceed the time duration limit.
      constexpr auto max =
          (Uint128{0x77359} << 64) + Uint128{0x3fff'ffff'dfff'ffff};
      static_assert(max < limit);

      // Assert |max| is the maximum allowed nanoseconds value.
      MOZ_ASSERT(double(max) < double(limit));
      MOZ_ASSERT(double(max + Uint128{1}) >= double(limit));

      MOZ_ASSERT(std::abs(nanoseconds) <= 999'999'999);

      auto nanos =
          (Int128{seconds} * Int128{ToNanoseconds(TemporalUnit::Second)}) +
          Int128{nanoseconds};
      if (nanos.abs() > max) {
        JS_ReportErrorNumberASCII(
            cx, GetErrorMessage, nullptr,
            JSMSG_TEMPORAL_DURATION_INVALID_NORMALIZED_TIME);
        return false;
      }

      // Step 11.
      *result = {0, 0, 0, 0, 0, 0, 0, 0, 0, double(nanos)};
      MOZ_ASSERT(IsValidDuration(*result));
      return true;
    }

    case TemporalUnit::Auto:
      break;
  }
  MOZ_CRASH("Unexpected temporal unit");
}

/**
 * TemporalDurationFromInternal ( internalDuration, largestUnit )
 */
bool js::temporal::TemporalDurationFromInternal(
    JSContext* cx, const InternalDuration& internalDuration,
    TemporalUnit largestUnit, Duration* result) {
  MOZ_ASSERT(IsValidDuration(internalDuration.date));
  MOZ_ASSERT(IsValidTimeDuration(internalDuration.time));

  // Steps 1-11.
  Duration duration;
  if (!TemporalDurationFromInternal(cx, internalDuration.time, largestUnit,
                                    &duration)) {
    return false;
  }
  MOZ_ASSERT(IsValidDuration(duration));

  // Step 12.
  auto days = mozilla::CheckedInt64(internalDuration.date.days) +
              mozilla::AssertedCast<int64_t>(duration.days);
  MOZ_ASSERT(days.isValid(), "valid duration days can't overflow");

  *result = {
      double(internalDuration.date.years),
      double(internalDuration.date.months),
      double(internalDuration.date.weeks),
      double(days.value()),
      duration.hours,
      duration.minutes,
      duration.seconds,
      duration.milliseconds,
      duration.microseconds,
      duration.nanoseconds,
  };
  return ThrowIfInvalidDuration(cx, *result);
}

/**
 * TimeDurationFromEpochNanosecondsDifference ( one, two )
 */
TimeDuration js::temporal::TimeDurationFromEpochNanosecondsDifference(
    const EpochNanoseconds& one, const EpochNanoseconds& two) {
  MOZ_ASSERT(IsValidEpochNanoseconds(one));
  MOZ_ASSERT(IsValidEpochNanoseconds(two));

  // Step 1.
  auto result = one - two;

  // Step 2.
  MOZ_ASSERT(IsValidEpochDuration(result));

  // Step 3.
  return result.to<TimeDuration>();
}

#ifdef DEBUG
/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
bool js::temporal::IsValidDuration(const Duration& duration) {
  MOZ_ASSERT(IsIntegerOrInfinityDuration(duration));

  const auto& [years, months, weeks, days, hours, minutes, seconds,
               milliseconds, microseconds, nanoseconds] = duration;

  // Step 1.
  int32_t sign = 0;

  // Step 2.
  for (auto v : {years, months, weeks, days, hours, minutes, seconds,
                 milliseconds, microseconds, nanoseconds}) {
    // Step 2.a.
    if (!std::isfinite(v)) {
      return false;
    }

    // Step 2.b.
    if (v < 0) {
      // Step 2.b.i.
      if (sign > 0) {
        return false;
      }

      // Step 2.b.ii.
      sign = -1;
    }

    // Step 2.c.
    else if (v > 0) {
      // Step 2.c.i.
      if (sign < 0) {
        return false;
      }

      // Step 2.c.ii.
      sign = 1;
    }
  }

  // Step 3.
  if (std::abs(years) >= double(int64_t(1) << 32)) {
    return false;
  }

  // Step 4.
  if (std::abs(months) >= double(int64_t(1) << 32)) {
    return false;
  }

  // Step 5.
  if (std::abs(weeks) >= double(int64_t(1) << 32)) {
    return false;
  }

  // Steps 6-8.
  if (!TimeDurationFromDuration(duration)) {
    return false;
  }

  // Step 9.
  return true;
}

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
bool js::temporal::IsValidDuration(const DateDuration& duration) {
  return IsValidDuration(duration.toDuration());
}

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
bool js::temporal::IsValidDuration(const InternalDuration& duration) {
  if (!IsValidTimeDuration(duration.time)) {
    return false;
  }

  auto d = duration.date.toDuration();
  auto [seconds, nanoseconds] = duration.time.denormalize();
  d.seconds = double(seconds);
  d.nanoseconds = double(nanoseconds);

  return IsValidDuration(d);
}
#endif

static bool ThrowInvalidDurationPart(JSContext* cx, double value,
                                     const char* name, unsigned errorNumber) {
  ToCStringBuf cbuf;
  const char* numStr = NumberToCString(&cbuf, value);

  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, errorNumber, name,
                            numStr);
  return false;
}

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds )
 */
bool js::temporal::ThrowIfInvalidDuration(JSContext* cx,
                                          const Duration& duration) {
  MOZ_ASSERT(IsIntegerOrInfinityDuration(duration));

  const auto& [years, months, weeks, days, hours, minutes, seconds,
               milliseconds, microseconds, nanoseconds] = duration;

  // Step 1.
  int32_t sign = DurationSign(duration);

  auto throwIfInvalid = [&](double v, const char* name) {
    // Step 2.a.
    if (!std::isfinite(v)) {
      return ThrowInvalidDurationPart(
          cx, v, name, JSMSG_TEMPORAL_DURATION_INVALID_NON_FINITE);
    }

    // Steps 2.b-c.
    if ((v < 0 && sign > 0) || (v > 0 && sign < 0)) {
      return ThrowInvalidDurationPart(cx, v, name,
                                      JSMSG_TEMPORAL_DURATION_INVALID_SIGN);
    }

    return true;
  };

  auto throwIfTooLarge = [&](double v, const char* name) {
    if (std::abs(v) >= double(int64_t(1) << 32)) {
      return ThrowInvalidDurationPart(
          cx, v, name, JSMSG_TEMPORAL_DURATION_INVALID_NON_FINITE);
    }
    return true;
  };

  // Step 2.
  if (!throwIfInvalid(years, "years")) {
    return false;
  }
  if (!throwIfInvalid(months, "months")) {
    return false;
  }
  if (!throwIfInvalid(weeks, "weeks")) {
    return false;
  }
  if (!throwIfInvalid(days, "days")) {
    return false;
  }
  if (!throwIfInvalid(hours, "hours")) {
    return false;
  }
  if (!throwIfInvalid(minutes, "minutes")) {
    return false;
  }
  if (!throwIfInvalid(seconds, "seconds")) {
    return false;
  }
  if (!throwIfInvalid(milliseconds, "milliseconds")) {
    return false;
  }
  if (!throwIfInvalid(microseconds, "microseconds")) {
    return false;
  }
  if (!throwIfInvalid(nanoseconds, "nanoseconds")) {
    return false;
  }

  // Step 3.
  if (!throwIfTooLarge(years, "years")) {
    return false;
  }

  // Step 4.
  if (!throwIfTooLarge(months, "months")) {
    return false;
  }

  // Step 5.
  if (!throwIfTooLarge(weeks, "weeks")) {
    return false;
  }

  // Steps 6-8.
  if (!TimeDurationFromDuration(duration)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_INVALID_NORMALIZED_TIME);
    return false;
  }

  MOZ_ASSERT(IsValidDuration(duration));

  // Step 9.
  return true;
}

/**
 * DefaultTemporalLargestUnit ( duration )
 */
static TemporalUnit DefaultTemporalLargestUnit(const Duration& duration) {
  MOZ_ASSERT(IsIntegerDuration(duration));

  // Step 1.
  if (duration.years != 0) {
    return TemporalUnit::Year;
  }

  // Step 2.
  if (duration.months != 0) {
    return TemporalUnit::Month;
  }

  // Step 3.
  if (duration.weeks != 0) {
    return TemporalUnit::Week;
  }

  // Step 4.
  if (duration.days != 0) {
    return TemporalUnit::Day;
  }

  // Step 5.
  if (duration.hours != 0) {
    return TemporalUnit::Hour;
  }

  // Step 6.
  if (duration.minutes != 0) {
    return TemporalUnit::Minute;
  }

  // Step 7.
  if (duration.seconds != 0) {
    return TemporalUnit::Second;
  }

  // Step 8.
  if (duration.milliseconds != 0) {
    return TemporalUnit::Millisecond;
  }

  // Step 9.
  if (duration.microseconds != 0) {
    return TemporalUnit::Microsecond;
  }

  // Step 10.
  return TemporalUnit::Nanosecond;
}

/**
 * CreateTemporalDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds [ , newTarget ] )
 */
static DurationObject* CreateTemporalDuration(JSContext* cx,
                                              const CallArgs& args,
                                              const Duration& duration) {
  const auto& [years, months, weeks, days, hours, minutes, seconds,
               milliseconds, microseconds, nanoseconds] = duration;

  // Step 1.
  if (!ThrowIfInvalidDuration(cx, duration)) {
    return nullptr;
  }

  // Steps 2-3.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Duration, &proto)) {
    return nullptr;
  }

  auto* object = NewObjectWithClassProto<DurationObject>(cx, proto);
  if (!object) {
    return nullptr;
  }

  // Steps 4-13.
  // Add zero to convert -0 to +0.
  object->setFixedSlot(DurationObject::YEARS_SLOT, NumberValue(years + (+0.0)));
  object->setFixedSlot(DurationObject::MONTHS_SLOT,
                       NumberValue(months + (+0.0)));
  object->setFixedSlot(DurationObject::WEEKS_SLOT, NumberValue(weeks + (+0.0)));
  object->setFixedSlot(DurationObject::DAYS_SLOT, NumberValue(days + (+0.0)));
  object->setFixedSlot(DurationObject::HOURS_SLOT, NumberValue(hours + (+0.0)));
  object->setFixedSlot(DurationObject::MINUTES_SLOT,
                       NumberValue(minutes + (+0.0)));
  object->setFixedSlot(DurationObject::SECONDS_SLOT,
                       NumberValue(seconds + (+0.0)));
  object->setFixedSlot(DurationObject::MILLISECONDS_SLOT,
                       NumberValue(milliseconds + (+0.0)));
  object->setFixedSlot(DurationObject::MICROSECONDS_SLOT,
                       NumberValue(microseconds + (+0.0)));
  object->setFixedSlot(DurationObject::NANOSECONDS_SLOT,
                       NumberValue(nanoseconds + (+0.0)));

  // Step 14.
  return object;
}

/**
 * CreateTemporalDuration ( years, months, weeks, days, hours, minutes, seconds,
 * milliseconds, microseconds, nanoseconds [ , newTarget ] )
 */
DurationObject* js::temporal::CreateTemporalDuration(JSContext* cx,
                                                     const Duration& duration) {
  const auto& [years, months, weeks, days, hours, minutes, seconds,
               milliseconds, microseconds, nanoseconds] = duration;

  MOZ_ASSERT(IsInteger(years));
  MOZ_ASSERT(IsInteger(months));
  MOZ_ASSERT(IsInteger(weeks));
  MOZ_ASSERT(IsInteger(days));
  MOZ_ASSERT(IsInteger(hours));
  MOZ_ASSERT(IsInteger(minutes));
  MOZ_ASSERT(IsInteger(seconds));
  MOZ_ASSERT(IsInteger(milliseconds));
  MOZ_ASSERT(IsInteger(microseconds));
  MOZ_ASSERT(IsInteger(nanoseconds));

  // Step 1.
  if (!ThrowIfInvalidDuration(cx, duration)) {
    return nullptr;
  }

  // Steps 2-3.
  auto* object = NewBuiltinClassInstance<DurationObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Steps 4-13.
  // Add zero to convert -0 to +0.
  object->setFixedSlot(DurationObject::YEARS_SLOT, NumberValue(years + (+0.0)));
  object->setFixedSlot(DurationObject::MONTHS_SLOT,
                       NumberValue(months + (+0.0)));
  object->setFixedSlot(DurationObject::WEEKS_SLOT, NumberValue(weeks + (+0.0)));
  object->setFixedSlot(DurationObject::DAYS_SLOT, NumberValue(days + (+0.0)));
  object->setFixedSlot(DurationObject::HOURS_SLOT, NumberValue(hours + (+0.0)));
  object->setFixedSlot(DurationObject::MINUTES_SLOT,
                       NumberValue(minutes + (+0.0)));
  object->setFixedSlot(DurationObject::SECONDS_SLOT,
                       NumberValue(seconds + (+0.0)));
  object->setFixedSlot(DurationObject::MILLISECONDS_SLOT,
                       NumberValue(milliseconds + (+0.0)));
  object->setFixedSlot(DurationObject::MICROSECONDS_SLOT,
                       NumberValue(microseconds + (+0.0)));
  object->setFixedSlot(DurationObject::NANOSECONDS_SLOT,
                       NumberValue(nanoseconds + (+0.0)));

  // Step 14.
  return object;
}

/**
 * ToIntegerIfIntegral ( argument )
 */
static bool ToIntegerIfIntegral(JSContext* cx, const char* name,
                                Handle<Value> argument, double* num) {
  // Step 1.
  double d;
  if (!JS::ToNumber(cx, argument, &d)) {
    return false;
  }

  // Step 2.
  if (!js::IsInteger(d)) {
    ToCStringBuf cbuf;
    const char* numStr = NumberToCString(&cbuf, d);

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_NOT_INTEGER, numStr,
                              name);
    return false;
  }

  // Step 3.
  *num = d;
  return true;
}

/**
 * ToIntegerIfIntegral ( argument )
 */
static bool ToIntegerIfIntegral(JSContext* cx, Handle<PropertyName*> name,
                                Handle<Value> argument, double* result) {
  // Step 1.
  double d;
  if (!JS::ToNumber(cx, argument, &d)) {
    return false;
  }

  // Step 2.
  if (!js::IsInteger(d)) {
    if (auto nameStr = js::QuoteString(cx, name)) {
      ToCStringBuf cbuf;
      const char* numStr = NumberToCString(&cbuf, d);

      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_DURATION_NOT_INTEGER, numStr,
                                nameStr.get());
    }
    return false;
  }

  // Step 3.
  *result = d;
  return true;
}

/**
 * ToTemporalPartialDurationRecord ( temporalDurationLike )
 */
static bool ToTemporalPartialDurationRecord(
    JSContext* cx, Handle<JSObject*> temporalDurationLike, Duration* result) {
  // Steps 1-3. (Not applicable in our implementation.)

  Rooted<Value> value(cx);
  bool any = false;

  auto getDurationProperty = [&](Handle<PropertyName*> name, double* num) {
    if (!GetProperty(cx, temporalDurationLike, temporalDurationLike, name,
                     &value)) {
      return false;
    }

    if (!value.isUndefined()) {
      any = true;

      if (!ToIntegerIfIntegral(cx, name, value, num)) {
        return false;
      }
    }
    return true;
  };

  // Steps 4-23.
  if (!getDurationProperty(cx->names().days, &result->days)) {
    return false;
  }
  if (!getDurationProperty(cx->names().hours, &result->hours)) {
    return false;
  }
  if (!getDurationProperty(cx->names().microseconds, &result->microseconds)) {
    return false;
  }
  if (!getDurationProperty(cx->names().milliseconds, &result->milliseconds)) {
    return false;
  }
  if (!getDurationProperty(cx->names().minutes, &result->minutes)) {
    return false;
  }
  if (!getDurationProperty(cx->names().months, &result->months)) {
    return false;
  }
  if (!getDurationProperty(cx->names().nanoseconds, &result->nanoseconds)) {
    return false;
  }
  if (!getDurationProperty(cx->names().seconds, &result->seconds)) {
    return false;
  }
  if (!getDurationProperty(cx->names().weeks, &result->weeks)) {
    return false;
  }
  if (!getDurationProperty(cx->names().years, &result->years)) {
    return false;
  }

  // Step 24.
  if (!any) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_MISSING_UNIT);
    return false;
  }

  // Step 25.
  return true;
}

/**
 * ToTemporalDuration ( item )
 */
bool js::temporal::ToTemporalDuration(JSContext* cx, Handle<Value> item,
                                      Duration* result) {
  // Steps 1 and 3-15.
  if (item.isObject()) {
    Rooted<JSObject*> itemObj(cx, &item.toObject());

    // Step 1.
    if (auto* duration = itemObj->maybeUnwrapIf<DurationObject>()) {
      *result = ToDuration(duration);
      return true;
    }

    // Step 3. (Reordered)
    Duration duration = {};

    // Steps 4-14.
    if (!ToTemporalPartialDurationRecord(cx, itemObj, &duration)) {
      return false;
    }

    // Step 15.
    if (!ThrowIfInvalidDuration(cx, duration)) {
      return false;
    }

    *result = duration;
    return true;
  }

  // Step 2.a.
  if (!item.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, item,
                     nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> string(cx, item.toString());

  // Step 2.b.
  return ParseTemporalDurationString(cx, string, result);
}

/**
 * DateDurationDays ( dateDuration, plainRelativeTo )
 */
static bool DateDurationDays(JSContext* cx, const DateDuration& duration,
                             Handle<PlainDate> plainRelativeTo,
                             int64_t* result) {
  MOZ_ASSERT(IsValidDuration(duration));

  auto [years, months, weeks, days] = duration;

  // Step 1.
  auto yearsMonthsWeeksDuration = DateDuration{years, months, weeks};

  // Step 2.
  if (yearsMonthsWeeksDuration == DateDuration{}) {
    *result = days;
    return true;
  }

  // Moved from caller.
  if (!plainRelativeTo) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_UNCOMPARABLE,
                              "relativeTo");
    return false;
  }

  // Step 3.
  ISODate later;
  if (!CalendarDateAdd(cx, plainRelativeTo.calendar(), plainRelativeTo,
                       yearsMonthsWeeksDuration, TemporalOverflow::Constrain,
                       &later)) {
    return false;
  }

  // Step 4.
  int32_t epochDays1 = MakeDay(plainRelativeTo);
  MOZ_ASSERT(MinEpochDay <= epochDays1 && epochDays1 <= MaxEpochDay);

  // Step 5.
  int32_t epochDays2 = MakeDay(later);
  MOZ_ASSERT(MinEpochDay <= epochDays2 && epochDays2 <= MaxEpochDay);

  // Step 4.
  int32_t yearsMonthsWeeksInDay = epochDays2 - epochDays1;

  // Step 5.
  *result = days + yearsMonthsWeeksInDay;
  return true;
}

static bool NumberToStringBuilder(JSContext* cx, double num,
                                  JSStringBuilder& sb) {
  MOZ_ASSERT(IsInteger(num));
  MOZ_ASSERT(num >= 0);
  MOZ_ASSERT(num < DOUBLE_INTEGRAL_PRECISION_LIMIT);

  ToCStringBuf cbuf;
  size_t length;
  const char* numStr = NumberToCString(&cbuf, num, &length);

  return sb.append(numStr, length);
}

static Duration AbsoluteDuration(const Duration& duration) {
  return {
      std::abs(duration.years),        std::abs(duration.months),
      std::abs(duration.weeks),        std::abs(duration.days),
      std::abs(duration.hours),        std::abs(duration.minutes),
      std::abs(duration.seconds),      std::abs(duration.milliseconds),
      std::abs(duration.microseconds), std::abs(duration.nanoseconds),
  };
}

/**
 * FormatFractionalSeconds ( subSecondNanoseconds, precision )
 */
[[nodiscard]] static bool FormatFractionalSeconds(JSStringBuilder& result,
                                                  int32_t subSecondNanoseconds,
                                                  Precision precision) {
  MOZ_ASSERT(0 <= subSecondNanoseconds && subSecondNanoseconds < 1'000'000'000);
  MOZ_ASSERT(precision != Precision::Minute());

  // Steps 1-2.
  if (precision == Precision::Auto()) {
    // Step 1.a.
    if (subSecondNanoseconds == 0) {
      return true;
    }

    // Step 3. (Reordered)
    if (!result.append('.')) {
      return false;
    }

    // Steps 1.b-c.
    int32_t k = 100'000'000;
    do {
      if (!result.append(char('0' + (subSecondNanoseconds / k)))) {
        return false;
      }
      subSecondNanoseconds %= k;
      k /= 10;
    } while (subSecondNanoseconds);
  } else {
    // Step 2.a.
    uint8_t p = precision.value();
    if (p == 0) {
      return true;
    }

    // Step 3. (Reordered)
    if (!result.append('.')) {
      return false;
    }

    // Steps 2.b-c.
    int32_t k = 100'000'000;
    for (uint8_t i = 0; i < precision.value(); i++) {
      if (!result.append(char('0' + (subSecondNanoseconds / k)))) {
        return false;
      }
      subSecondNanoseconds %= k;
      k /= 10;
    }
  }

  return true;
}

/**
 * TemporalDurationToString ( duration, precision )
 */
static JSString* TemporalDurationToString(JSContext* cx,
                                          const Duration& duration,
                                          Precision precision) {
  MOZ_ASSERT(IsValidDuration(duration));
  MOZ_ASSERT(precision != Precision::Minute());

  // Fast path for zero durations.
  if (duration == Duration{} &&
      (precision == Precision::Auto() || precision.value() == 0)) {
    return NewStringCopyZ<CanGC>(cx, "PT0S");
  }

  // Convert to absolute values up front. This is okay to do, because when the
  // duration is valid, all components have the same sign.
  const auto& [years, months, weeks, days, hours, minutes, seconds,
               milliseconds, microseconds, nanoseconds] =
      AbsoluteDuration(duration);

  // Years to seconds parts are all safe integers for valid durations.
  MOZ_ASSERT(years < DOUBLE_INTEGRAL_PRECISION_LIMIT);
  MOZ_ASSERT(months < DOUBLE_INTEGRAL_PRECISION_LIMIT);
  MOZ_ASSERT(weeks < DOUBLE_INTEGRAL_PRECISION_LIMIT);
  MOZ_ASSERT(days < DOUBLE_INTEGRAL_PRECISION_LIMIT);
  MOZ_ASSERT(hours < DOUBLE_INTEGRAL_PRECISION_LIMIT);
  MOZ_ASSERT(minutes < DOUBLE_INTEGRAL_PRECISION_LIMIT);
  MOZ_ASSERT(seconds < DOUBLE_INTEGRAL_PRECISION_LIMIT);

  // Step 1.
  int32_t sign = DurationSign(duration);

  // Steps 2 and 7.
  JSStringBuilder result(cx);

  // Step 14. (Reordered)
  if (sign < 0) {
    if (!result.append('-')) {
      return nullptr;
    }
  }

  // Step 15. (Reordered)
  if (!result.append('P')) {
    return nullptr;
  }

  // Step 3.
  if (years != 0) {
    if (!NumberToStringBuilder(cx, years, result)) {
      return nullptr;
    }
    if (!result.append('Y')) {
      return nullptr;
    }
  }

  // Step 4.
  if (months != 0) {
    if (!NumberToStringBuilder(cx, months, result)) {
      return nullptr;
    }
    if (!result.append('M')) {
      return nullptr;
    }
  }

  // Step 5.
  if (weeks != 0) {
    if (!NumberToStringBuilder(cx, weeks, result)) {
      return nullptr;
    }
    if (!result.append('W')) {
      return nullptr;
    }
  }

  // Step 6.
  if (days != 0) {
    if (!NumberToStringBuilder(cx, days, result)) {
      return nullptr;
    }
    if (!result.append('D')) {
      return nullptr;
    }
  }

  // Step 7. (Moved above)

  // Steps 10-11. (Reordered)
  bool zeroMinutesAndHigher = years == 0 && months == 0 && weeks == 0 &&
                              days == 0 && hours == 0 && minutes == 0;

  // Step 12.
  auto secondsDuration = TimeDurationFromComponents(
      0.0, 0.0, seconds, milliseconds, microseconds, nanoseconds);

  // Steps 8-9, 13, and 16.
  bool hasSecondsPart = (secondsDuration != TimeDuration{}) ||
                        zeroMinutesAndHigher || precision != Precision::Auto();
  if (hours != 0 || minutes != 0 || hasSecondsPart) {
    // Step 16. (Reordered)
    if (!result.append('T')) {
      return nullptr;
    }

    // Step 8.
    if (hours != 0) {
      if (!NumberToStringBuilder(cx, hours, result)) {
        return nullptr;
      }
      if (!result.append('H')) {
        return nullptr;
      }
    }

    // Step 9.
    if (minutes != 0) {
      if (!NumberToStringBuilder(cx, minutes, result)) {
        return nullptr;
      }
      if (!result.append('M')) {
        return nullptr;
      }
    }

    // Step 13.
    if (hasSecondsPart) {
      // Step 13.a.
      if (!NumberToStringBuilder(cx, double(secondsDuration.seconds), result)) {
        return nullptr;
      }

      // Step 13.b.
      if (!FormatFractionalSeconds(result, secondsDuration.nanoseconds,
                                   precision)) {
        return nullptr;
      }

      // Step 13.c.
      if (!result.append('S')) {
        return nullptr;
      }
    }
  }

  // Steps 14-16. (Moved above)

  // Step 17.
  return result.finishString();
}

/**
 * GetTemporalRelativeToOption ( options )
 */
static bool GetTemporalRelativeToOption(
    JSContext* cx, Handle<JSObject*> options,
    MutableHandle<PlainDate> plainRelativeTo,
    MutableHandle<ZonedDateTime> zonedRelativeTo) {
  // Default initialize both return values.
  plainRelativeTo.set(PlainDate{});
  zonedRelativeTo.set(ZonedDateTime{});

  // Step 1.
  Rooted<Value> value(cx);
  if (!GetProperty(cx, options, options, cx->names().relativeTo, &value)) {
    return false;
  }

  // Step 2.
  if (value.isUndefined()) {
    return true;
  }

  // Step 3.
  auto offsetBehaviour = OffsetBehaviour::Option;

  // Step 4.
  auto matchBehaviour = MatchBehaviour::MatchExactly;

  // Steps 5-6.
  EpochNanoseconds epochNanoseconds;
  Rooted<TimeZoneValue> timeZone(cx);
  Rooted<CalendarValue> calendar(cx);
  if (value.isObject()) {
    Rooted<JSObject*> obj(cx, &value.toObject());

    // Step 5.a.
    if (auto* zonedDateTime = obj->maybeUnwrapIf<ZonedDateTimeObject>()) {
      auto epochNs = zonedDateTime->epochNanoseconds();
      Rooted<TimeZoneValue> timeZone(cx, zonedDateTime->timeZone());
      Rooted<CalendarValue> calendar(cx, zonedDateTime->calendar());

      if (!timeZone.wrap(cx)) {
        return false;
      }
      if (!calendar.wrap(cx)) {
        return false;
      }

      // Step 5.a.i.
      zonedRelativeTo.set(ZonedDateTime{epochNs, timeZone, calendar});
      return true;
    }

    // Step 5.b.
    if (auto* plainDate = obj->maybeUnwrapIf<PlainDateObject>()) {
      auto date = plainDate->date();

      Rooted<CalendarValue> calendar(cx, plainDate->calendar());
      if (!calendar.wrap(cx)) {
        return false;
      }

      // Step 5.b.i.
      plainRelativeTo.set(PlainDate{date, calendar});
      return true;
    }

    // Step 5.c.
    if (auto* dateTime = obj->maybeUnwrapIf<PlainDateTimeObject>()) {
      auto date = dateTime->date();

      Rooted<CalendarValue> calendar(cx, dateTime->calendar());
      if (!calendar.wrap(cx)) {
        return false;
      }

      // Steps 5.c.i-ii.
      plainRelativeTo.set(PlainDate{date, calendar});
      return true;
    }

    // Step 5.d.
    if (!GetTemporalCalendarWithISODefault(cx, obj, &calendar)) {
      return false;
    }

    // Step 5.e.
    Rooted<CalendarFields> fields(cx);
    if (!PrepareCalendarFields(cx, calendar, obj,
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
                               &fields)) {
      return false;
    }

    // Step 5.f.
    ISODateTime dateTime;
    if (!InterpretTemporalDateTimeFields(
            cx, calendar, fields, TemporalOverflow::Constrain, &dateTime)) {
      return false;
    }

    // Step 5.g.
    timeZone = fields.timeZone();

    // Step 5.h.
    auto offset = fields.offset();

    // Step 5.j.
    if (!fields.has(CalendarField::Offset)) {
      offsetBehaviour = OffsetBehaviour::Wall;
    }

    // Step 7.
    if (!timeZone) {
      // Steps 7.a-b.
      return CreateTemporalDate(cx, dateTime.date, calendar, plainRelativeTo);
    }

    // Steps 8-9.
    int64_t offsetNs = 0;
    if (offsetBehaviour == OffsetBehaviour::Option) {
      // Step 8.a.
      offsetNs = int64_t(offset);
    }

    // Step 10.
    if (!InterpretISODateTimeOffset(
            cx, dateTime, offsetBehaviour, offsetNs, timeZone,
            TemporalDisambiguation::Compatible, TemporalOffset::Reject,
            matchBehaviour, &epochNanoseconds)) {
      return false;
    }
  } else {
    // Step 6.a.
    if (!value.isString()) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, value,
                       nullptr, "not a string");
      return false;
    }
    Rooted<JSString*> string(cx, value.toString());

    // Step 6.b.
    Rooted<ParsedZonedDateTime> parsed(cx);
    if (!ParseTemporalRelativeToString(cx, string, &parsed)) {
      return false;
    }

    // Steps 6.c-e. (Not applicable in our implementation.)

    // Step 6.f.
    if (parsed.timeZoneAnnotation()) {
      // Step 6.f.i.
      if (!ToTemporalTimeZone(cx, parsed.timeZoneAnnotation(), &timeZone)) {
        return false;
      }

      // Steps 6.f.ii-iii.
      if (parsed.isUTC()) {
        offsetBehaviour = OffsetBehaviour::Exact;
      } else if (!parsed.hasOffset()) {
        offsetBehaviour = OffsetBehaviour::Wall;
      }

      // Step 6.f.iv.
      matchBehaviour = MatchBehaviour::MatchMinutes;
    } else {
      MOZ_ASSERT(!timeZone);
    }

    // Steps 6.g-i.
    if (parsed.calendar()) {
      if (!CanonicalizeCalendar(cx, parsed.calendar(), &calendar)) {
        return false;
      }
    } else {
      calendar.set(CalendarValue(CalendarId::ISO8601));
    }

    // Step 7.
    if (!timeZone) {
      // Steps 7.a-b.
      return CreateTemporalDate(cx, parsed.dateTime().date, calendar,
                                plainRelativeTo);
    }

    // Steps 8-9.
    int64_t offsetNs;
    if (offsetBehaviour == OffsetBehaviour::Option) {
      MOZ_ASSERT(parsed.hasOffset());

      // Step 8.a.
      offsetNs = parsed.timeZoneOffset();
    } else {
      // Step 9.
      offsetNs = 0;
    }

    // Step 10.
    if (parsed.isStartOfDay()) {
      if (!InterpretISODateTimeOffset(
              cx, parsed.dateTime().date, offsetBehaviour, offsetNs, timeZone,
              TemporalDisambiguation::Compatible, TemporalOffset::Reject,
              matchBehaviour, &epochNanoseconds)) {
        return false;
      }
    } else {
      if (!InterpretISODateTimeOffset(
              cx, parsed.dateTime(), offsetBehaviour, offsetNs, timeZone,
              TemporalDisambiguation::Compatible, TemporalOffset::Reject,
              matchBehaviour, &epochNanoseconds)) {
        return false;
      }
    }
  }
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  // Steps 11-12.
  zonedRelativeTo.set(ZonedDateTime{epochNanoseconds, timeZone, calendar});
  return true;
}

/**
 * RoundTimeDurationToIncrement ( d, increment, roundingMode )
 */
static TimeDuration RoundTimeDurationToIncrement(
    const TimeDuration& duration, const TemporalUnit unit, Increment increment,
    TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidTimeDuration(duration));
  MOZ_ASSERT(unit >= TemporalUnit::Day);
  MOZ_ASSERT_IF(unit >= TemporalUnit::Hour,
                increment <= MaximumTemporalDurationRoundingIncrement(unit));

  auto divisor = Int128{ToNanoseconds(unit)} * Int128{increment.value()};
  MOZ_ASSERT(divisor > Int128{0});
  MOZ_ASSERT_IF(unit >= TemporalUnit::Hour,
                divisor <= Int128{ToNanoseconds(TemporalUnit::Day)});

  auto totalNanoseconds = duration.toNanoseconds();
  auto rounded =
      RoundNumberToIncrement(totalNanoseconds, divisor, roundingMode);
  return TimeDuration::fromNanoseconds(rounded);
}

/**
 * TotalTimeDuration ( timeDuration, unit )
 */
double js::temporal::TotalTimeDuration(const TimeDuration& duration,
                                       TemporalUnit unit) {
  MOZ_ASSERT(IsValidTimeDuration(duration));
  MOZ_ASSERT(unit >= TemporalUnit::Day);

  auto numerator = duration.toNanoseconds();
  auto denominator = Int128{ToNanoseconds(unit)};
  return FractionToDouble(numerator, denominator);
}

/**
 * RoundTimeDuration ( duration, increment, unit, roundingMode )
 */
static bool RoundTimeDuration(JSContext* cx, const TimeDuration& duration,
                              Increment increment, TemporalUnit unit,
                              TemporalRoundingMode roundingMode,
                              TimeDuration* result) {
  MOZ_ASSERT(IsValidTimeDuration(duration));
  MOZ_ASSERT(increment <= Increment::max());
  MOZ_ASSERT(unit > TemporalUnit::Day);

  // Step 1-2.
  auto rounded =
      RoundTimeDurationToIncrement(duration, unit, increment, roundingMode);
  if (!IsValidTimeDuration(rounded)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_INVALID_NORMALIZED_TIME);
    return false;
  }
  *result = rounded;
  return true;
}

/**
 * RoundTimeDuration ( duration, increment, unit, roundingMode )
 */
TimeDuration js::temporal::RoundTimeDuration(
    const TimeDuration& duration, Increment increment, TemporalUnit unit,
    TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidTimeDuration(duration));
  MOZ_ASSERT(increment <= Increment::max());
  MOZ_ASSERT(unit > TemporalUnit::Day);

  auto result =
      RoundTimeDurationToIncrement(duration, unit, increment, roundingMode);
  MOZ_ASSERT(IsValidTimeDuration(result));

  return result;
}

#ifdef DEBUG
/**
 * Return true if the input is within the valid epoch nanoseconds limits with a
 * time zone offset applied, i.e. it's smaller than ±(8.64 × 10^21 + nsPerDay).
 */
static bool IsValidLocalNanoseconds(const EpochNanoseconds& epochNanoseconds) {
  MOZ_ASSERT(0 <= epochNanoseconds.nanoseconds &&
             epochNanoseconds.nanoseconds <= 999'999'999);

  // Time zone offsets can't exceed 24 hours.
  constexpr auto oneDay = EpochDuration::fromDays(1);

  // Exclusive limits.
  constexpr auto min = EpochNanoseconds::min() - oneDay;
  constexpr auto max = EpochNanoseconds::max() + oneDay;

  return min < epochNanoseconds && epochNanoseconds < max;
}
#endif

enum class UnsignedRoundingMode {
  Zero,
  Infinity,
  HalfZero,
  HalfInfinity,
  HalfEven
};

/**
 * GetUnsignedRoundingMode ( roundingMode, sign )
 */
static UnsignedRoundingMode GetUnsignedRoundingMode(
    TemporalRoundingMode roundingMode, bool isNegative) {
  switch (roundingMode) {
    case TemporalRoundingMode::Ceil:
      return isNegative ? UnsignedRoundingMode::Zero
                        : UnsignedRoundingMode::Infinity;
    case TemporalRoundingMode::Floor:
      return isNegative ? UnsignedRoundingMode::Infinity
                        : UnsignedRoundingMode::Zero;
    case TemporalRoundingMode::Expand:
      return UnsignedRoundingMode::Infinity;
    case TemporalRoundingMode::Trunc:
      return UnsignedRoundingMode::Zero;
    case TemporalRoundingMode::HalfCeil:
      return isNegative ? UnsignedRoundingMode::HalfZero
                        : UnsignedRoundingMode::HalfInfinity;
    case TemporalRoundingMode::HalfFloor:
      return isNegative ? UnsignedRoundingMode::HalfInfinity
                        : UnsignedRoundingMode::HalfZero;
    case TemporalRoundingMode::HalfExpand:
      return UnsignedRoundingMode::HalfInfinity;
    case TemporalRoundingMode::HalfTrunc:
      return UnsignedRoundingMode::HalfZero;
    case TemporalRoundingMode::HalfEven:
      return UnsignedRoundingMode::HalfEven;
  }
  MOZ_CRASH("invalid rounding mode");
}

struct DurationNudge {
  InternalDuration duration;
  EpochNanoseconds epochNs;
  double total = 0;
  bool didExpandCalendarUnit = false;
};

/**
 * NudgeToCalendarUnit ( sign, duration, destEpochNs, isoDateTime, timeZone,
 * calendar, increment, unit, roundingMode )
 */
static bool NudgeToCalendarUnit(JSContext* cx, const InternalDuration& duration,
                                const EpochNanoseconds& destEpochNs,
                                const ISODateTime& isoDateTime,
                                Handle<TimeZoneValue> timeZone,
                                Handle<CalendarValue> calendar,
                                Increment increment, TemporalUnit unit,
                                TemporalRoundingMode roundingMode,
                                DurationNudge* result) {
  MOZ_ASSERT(IsValidDuration(duration));
  MOZ_ASSERT_IF(timeZone, IsValidEpochNanoseconds(destEpochNs));
  MOZ_ASSERT_IF(!timeZone, IsValidLocalNanoseconds(destEpochNs));
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime));
  MOZ_ASSERT(unit <= TemporalUnit::Day);

  int32_t sign = InternalDurationSign(duration) < 0 ? -1 : 1;

  // Steps 1-4.
  int64_t r1;
  int64_t r2;
  DateDuration startDuration;
  DateDuration endDuration;
  if (unit == TemporalUnit::Year) {
    // Step 1.a.
    int64_t years = RoundNumberToIncrement(duration.date.years, increment,
                                           TemporalRoundingMode::Trunc);

    // Step 1.b.
    r1 = years;

    // Step 1.c.
    r2 = years + int64_t(increment.value()) * sign;

    // Step 1.d.
    startDuration = {r1};

    // Step 1.e.
    endDuration = {r2};
  } else if (unit == TemporalUnit::Month) {
    // Step 2.a.
    int64_t months = RoundNumberToIncrement(duration.date.months, increment,
                                            TemporalRoundingMode::Trunc);

    // Step 2.b.
    r1 = months;

    // Step 2.c.
    r2 = months + int64_t(increment.value()) * sign;

    // Step 2.d.
    startDuration = {duration.date.years, r1};

    // Step 2.e.
    endDuration = {duration.date.years, r2};
  } else if (unit == TemporalUnit::Week) {
    // Step 3.a.
    auto yearsMonths = DateDuration{duration.date.years, duration.date.months};

    // Step 3.b.
    ISODate weeksStart;
    if (!CalendarDateAdd(cx, calendar, isoDateTime.date, yearsMonths,
                         TemporalOverflow::Constrain, &weeksStart)) {
      return false;
    }
    MOZ_ASSERT(ISODateWithinLimits(weeksStart));

    // Step 3.c.
    ISODate weeksEnd;
    if (!BalanceISODate(cx, weeksStart, duration.date.days, &weeksEnd)) {
      return false;
    }
    MOZ_ASSERT(ISODateWithinLimits(weeksEnd));

    // Step 3.d.
    DateDuration untilResult;
    if (!CalendarDateUntil(cx, calendar, weeksStart, weeksEnd,
                           TemporalUnit::Week, &untilResult)) {
      return false;
    }

    // Step 3.e.
    int64_t weeks =
        RoundNumberToIncrement(duration.date.weeks + untilResult.weeks,
                               increment, TemporalRoundingMode::Trunc);

    // Step 3.f.
    r1 = weeks;

    // Step 3.g.
    r2 = weeks + int64_t(increment.value()) * sign;

    // Step 3.h.
    startDuration = {duration.date.years, duration.date.months, r1};

    // Step 3.i.
    endDuration = {duration.date.years, duration.date.months, r2};
  } else {
    // Step 4.a.
    MOZ_ASSERT(unit == TemporalUnit::Day);

    // Step 4.b.
    int64_t days = RoundNumberToIncrement(duration.date.days, increment,
                                          TemporalRoundingMode::Trunc);

    // Step 4.c.
    r1 = days;

    // Step 4.d.
    r2 = days + int64_t(increment.value()) * sign;

    // Step 4.e.
    startDuration = {duration.date.years, duration.date.months,
                     duration.date.weeks, r1};

    // Step 4.f.
    endDuration = {duration.date.years, duration.date.months,
                   duration.date.weeks, r2};
  }
  MOZ_ASSERT(IsValidDuration(startDuration));
  MOZ_ASSERT(IsValidDuration(endDuration));

  // Step 5.
  MOZ_ASSERT_IF(sign > 0, r1 >= 0 && r1 < r2);

  // Step 6.
  MOZ_ASSERT_IF(sign < 0, r1 <= 0 && r1 > r2);

  // Step 7.
  ISODate start;
  if (!CalendarDateAdd(cx, calendar, isoDateTime.date, startDuration,
                       TemporalOverflow::Constrain, &start)) {
    return false;
  }

  // Step 8.
  ISODate end;
  if (!CalendarDateAdd(cx, calendar, isoDateTime.date, endDuration,
                       TemporalOverflow::Constrain, &end)) {
    return false;
  }

  // Step 9.
  auto startDateTime = ISODateTime{start, isoDateTime.time};
  MOZ_ASSERT(ISODateTimeWithinLimits(startDateTime));

  // Step 10.
  auto endDateTime = ISODateTime{end, isoDateTime.time};
  MOZ_ASSERT(ISODateTimeWithinLimits(endDateTime));

  // Steps 11-12.
  EpochNanoseconds startEpochNs;
  EpochNanoseconds endEpochNs;
  if (!timeZone) {
    // Step 11.a.
    startEpochNs = GetUTCEpochNanoseconds(startDateTime);

    // Step 11.b.
    endEpochNs = GetUTCEpochNanoseconds(endDateTime);
  } else {
    // Step 12.a.
    if (!GetEpochNanosecondsFor(cx, timeZone, startDateTime,
                                TemporalDisambiguation::Compatible,
                                &startEpochNs)) {
      return false;
    }

    // Step 12.b.
    if (!GetEpochNanosecondsFor(cx, timeZone, endDateTime,
                                TemporalDisambiguation::Compatible,
                                &endEpochNs)) {
      return false;
    }
  }

  // Steps 13-14.
  MOZ_ASSERT_IF(sign > 0,
                startEpochNs <= destEpochNs && destEpochNs <= endEpochNs);
  MOZ_ASSERT_IF(sign < 0,
                endEpochNs <= destEpochNs && destEpochNs <= startEpochNs);

  // Step 15.
  MOZ_ASSERT(startEpochNs != endEpochNs);

  // Step 16.
  auto numerator = (destEpochNs - startEpochNs).toNanoseconds();
  auto denominator = (endEpochNs - startEpochNs).toNanoseconds();
  MOZ_ASSERT(denominator != Int128{0});
  MOZ_ASSERT(numerator.abs() <= denominator.abs());
  MOZ_ASSERT_IF(denominator > Int128{0}, numerator >= Int128{0});
  MOZ_ASSERT_IF(denominator < Int128{0}, numerator <= Int128{0});

  // Ensure |numerator| and |denominator| are both non-negative to simplify the
  // following computations.
  if (denominator < Int128{0}) {
    numerator = -numerator;
    denominator = -denominator;
  }

  // Steps 17-19.
  //
  // |total| must only be computed when called from Duration.prototype.total,
  // which always passes "trunc" rounding mode with an increment of one.
  double total = mozilla::UnspecifiedNaN<double>();
  if (roundingMode == TemporalRoundingMode::Trunc &&
      increment == Increment{1}) {
    // total = r1 + progress × increment × sign
    //       = r1 + (numerator / denominator) × increment × sign
    //       = r1 + (numerator × increment × sign) / denominator
    //       = (r1 × denominator + numerator × increment × sign) / denominator
    //
    // Computing `n` can't overflow, because:
    // - For years, months, and weeks, `abs(r1) ≤ 2^32`.
    // - For days, `abs(r1) < ⌈(2^53) / (24 * 60 * 60)⌉`.
    // - `denominator` and `numerator` are below-or-equal `2 × 8.64 × 10^21`.
    // - And finally `increment ≤ 10^9`.
    auto n = Int128{r1} * denominator + numerator * Int128{sign};
    total = FractionToDouble(n, denominator);
  }

  // Steps 20-21.
  auto unsignedRoundingMode = GetUnsignedRoundingMode(roundingMode, sign < 0);

  // Steps 22-23. (Inlined ApplyUnsignedRoundingMode)
  //
  // clang-format off
  //
  // ApplyUnsignedRoundingMode, steps 1-16.
  //
  // `total = r1` iff `progress = 0`. And `progress = 0` iff `numerator = 0`.
  //
  // d1 = total - r1
  //    = (r1 × denominator + numerator × increment × sign) / denominator - r1
  //    = (numerator × increment × sign) / denominator
  //
  // d2 = r2 - total
  //    = r1 + increment - (r1 × denominator + numerator × increment × sign) / denominator
  //    = (increment × denominator - numerator × increment × sign) / denominator
  //
  // d1 < d2
  // ⇔ (numerator × increment × sign) / denominator < (increment × denominator - numerator × increment × sign) / denominator
  // ⇔ (numerator × increment × sign) < (increment × denominator - numerator × increment × sign)
  // ⇔ (numerator × sign) < (denominator - numerator × sign)
  // ⇔ (2 × numerator × sign) < denominator
  //
  // cardinality = (r1 / (r2 – r1)) modulo 2
  //             = (r1 / (r1 + increment - r1)) modulo 2
  //             = (r1 / increment) modulo 2
  //
  // clang-format on
  bool didExpandCalendarUnit;
  if (numerator == denominator) {
    didExpandCalendarUnit = true;
  } else if (numerator == Int128{0}) {
    didExpandCalendarUnit = false;
  } else if (unsignedRoundingMode == UnsignedRoundingMode::Zero) {
    didExpandCalendarUnit = false;
  } else if (unsignedRoundingMode == UnsignedRoundingMode::Infinity) {
    didExpandCalendarUnit = true;
  } else if (numerator + numerator < denominator) {
    didExpandCalendarUnit = false;
  } else if (numerator + numerator > denominator) {
    didExpandCalendarUnit = true;
  } else if (unsignedRoundingMode == UnsignedRoundingMode::HalfZero) {
    didExpandCalendarUnit = false;
  } else if (unsignedRoundingMode == UnsignedRoundingMode::HalfInfinity) {
    didExpandCalendarUnit = true;
  } else if ((r1 / increment.value()) % 2 == 0) {
    didExpandCalendarUnit = false;
  } else {
    didExpandCalendarUnit = true;
  }

  // Steps 24-28.
  auto resultDuration = didExpandCalendarUnit ? endDuration : startDuration;
  auto resultEpochNs = didExpandCalendarUnit ? endEpochNs : startEpochNs;
  *result = {{resultDuration, {}}, resultEpochNs, total, didExpandCalendarUnit};
  return true;
}

#ifdef DEBUG
static bool IsValidTimeFromDateTimeDuration(const TimeDuration& timeDuration) {
  // Time zone adjustment can't exceed 24 hours.
  constexpr auto oneDay = EpochDuration::fromDays(1);

  // Time zone adjusted nsMinInstant and nsMaxInstant.
  constexpr auto min = EpochNanoseconds::min() - oneDay;
  constexpr auto max = EpochNanoseconds::max() + oneDay;

  // Maximum duration between two date-time points.
  constexpr auto maxDuration = (max - min).to<TimeDuration>();
  static_assert(maxDuration == TimeDuration::fromDays(200'000'002));

  // If |timeDuration| is a duration between two date-times within the valid
  // limits, the duration can't exceed the duration between time zone adjusted
  // nsMinInstant and nsMaxInstant.
  return timeDuration.abs() < maxDuration;
}
#endif

/**
 * NudgeToZonedTime ( sign, duration, isoDateTime, timeZone, calendar,
 * increment, unit, roundingMode )
 */
static bool NudgeToZonedTime(JSContext* cx, const InternalDuration& duration,
                             const ISODateTime& isoDateTime,
                             Handle<TimeZoneValue> timeZone,
                             Handle<CalendarValue> calendar,
                             Increment increment, TemporalUnit unit,
                             TemporalRoundingMode roundingMode,
                             DurationNudge* result) {
  MOZ_ASSERT(IsValidDuration(duration));
  MOZ_ASSERT(IsValidTimeFromDateTimeDuration(duration.time));
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime));
  MOZ_ASSERT(unit >= TemporalUnit::Hour);

  int32_t sign = InternalDurationSign(duration) < 0 ? -1 : 1;

  // Step 1.
  ISODate start;
  if (!CalendarDateAdd(cx, calendar, isoDateTime.date, duration.date,
                       TemporalOverflow::Constrain, &start)) {
    return false;
  }

  // Step 2.
  auto startDateTime = ISODateTime{start, isoDateTime.time};
  MOZ_ASSERT(ISODateTimeWithinLimits(startDateTime));

  // Step 3.
  auto end = BalanceISODate(start, sign);

  // Step 4.
  auto endDateTime = ISODateTime{end, isoDateTime.time};
  if (!ISODateTimeWithinLimits(endDateTime)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
    return false;
  }

  // Step 5.
  EpochNanoseconds startEpochNs;
  if (!GetEpochNanosecondsFor(cx, timeZone, startDateTime,
                              TemporalDisambiguation::Compatible,
                              &startEpochNs)) {
    return false;
  }

  // Step 6.
  EpochNanoseconds endEpochNs;
  if (!GetEpochNanosecondsFor(cx, timeZone, endDateTime,
                              TemporalDisambiguation::Compatible,
                              &endEpochNs)) {
    return false;
  }

  // Step 7.
  auto daySpan =
      TimeDurationFromEpochNanosecondsDifference(endEpochNs, startEpochNs);
  MOZ_ASSERT(daySpan.abs() <= TimeDuration::fromDays(2),
             "maximum day length for repeated days");

  // Step 8.
  MOZ_ASSERT(TimeDurationSign(daySpan) == sign);

  // Steps 9-10.
  //
  // RoundTimeDurationToIncrement is infallible |duration.time| is a valid
  // date-time duration.
  auto roundedTime = RoundTimeDurationToIncrement(duration.time, unit,
                                                  increment, roundingMode);
  MOZ_ASSERT(IsValidTimeDuration(roundedTime));

  // Step 11. (Inlined AddTimeDuration)
  auto beyondDaySpan = roundedTime - daySpan;
  MOZ_ASSERT(IsValidTimeDuration(beyondDaySpan));

  // Steps 12-13.
  bool didRoundBeyondDay;
  int32_t dayDelta;
  EpochNanoseconds nudgedEpochNs;
  if (TimeDurationSign(beyondDaySpan) != -sign) {
    // Step 12.a.
    didRoundBeyondDay = true;

    // Step 12.b.
    dayDelta = sign;

    // Step 12.c.
    //
    // This call to RoundTimeDurationToIncrement is also infallible.
    roundedTime = RoundTimeDurationToIncrement(beyondDaySpan, unit, increment,
                                               roundingMode);
    MOZ_ASSERT(IsValidTimeDuration(roundedTime));

    // Step 12.d. (Inlined AddTimeDurationToEpochNanoseconds)
    nudgedEpochNs = endEpochNs + roundedTime.to<EpochDuration>();
  } else {
    // Step 13.a.
    didRoundBeyondDay = false;

    // Step 13.b.
    dayDelta = 0;

    // Step 13.c. (Inlined AddTimeDurationToEpochNanoseconds)
    nudgedEpochNs = startEpochNs + roundedTime.to<EpochDuration>();
  }

  // Step 14.
  auto dateDuration = DateDuration{
      duration.date.years,
      duration.date.months,
      duration.date.weeks,
      duration.date.days + dayDelta,
  };
  MOZ_ASSERT(IsValidDuration(dateDuration));

  // Step 15.
  MOZ_ASSERT(DateDurationSign(dateDuration) * TimeDurationSign(roundedTime) >=
             0);
  auto resultDuration = InternalDuration{dateDuration, roundedTime};

  // Step 16.
  *result = {
      resultDuration,
      nudgedEpochNs,
      mozilla::UnspecifiedNaN<double>(),
      didRoundBeyondDay,
  };
  return true;
}

/**
 * NudgeToDayOrTime ( duration, destEpochNs, largestUnit, increment,
 * smallestUnit, roundingMode )
 */
static DurationNudge NudgeToDayOrTime(const InternalDuration& duration,
                                      const EpochNanoseconds& destEpochNs,
                                      TemporalUnit largestUnit,
                                      Increment increment,
                                      TemporalUnit smallestUnit,
                                      TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidDuration(duration));
  MOZ_ASSERT(IsValidLocalNanoseconds(destEpochNs));
  MOZ_ASSERT(smallestUnit >= TemporalUnit::Day);

  // Step 1. (Inlined Add24HourDaysToTimeDuration)
  auto timeDuration =
      duration.time + TimeDuration::fromDays(duration.date.days);
  MOZ_ASSERT(IsValidTimeDuration(timeDuration));
  MOZ_ASSERT(IsValidTimeFromDateTimeDuration(timeDuration));

  // Steps 2-3.
  //
  // RoundTimeDurationToIncrement is infallible |timeDuration| is a valid
  // date-time duration.
  auto roundedTime = RoundTimeDurationToIncrement(timeDuration, smallestUnit,
                                                  increment, roundingMode);
  MOZ_ASSERT(IsValidTimeDuration(roundedTime));

  // Step 4. (Inlined AddTimeDuration)
  auto diffTime = roundedTime - timeDuration;
  MOZ_ASSERT(IsValidTimeDuration(diffTime));

  // Step 5.
  int64_t wholeDays = timeDuration.toDays();

  // Step 6.
  int64_t roundedWholeDays = roundedTime.toDays();

  // Step 7.
  int64_t dayDelta = roundedWholeDays - wholeDays;

  // Step 8.
  int32_t dayDeltaSign = dayDelta < 0 ? -1 : dayDelta > 0 ? 1 : 0;

  // Step 9.
  bool didExpandDays = dayDeltaSign == TimeDurationSign(timeDuration);

  // Step 10. (Inlined AddTimeDurationToEpochNanoseconds)
  auto nudgedEpochNs = destEpochNs + diffTime.to<EpochDuration>();

  // Step 11.
  int64_t days = 0;

  // Step 12.
  auto remainder = roundedTime;

  // Step 13.
  if (largestUnit <= TemporalUnit::Day) {
    // Step 13.a.
    days = roundedWholeDays;

    // Step 13.b.
    remainder = roundedTime - TimeDuration::fromDays(roundedWholeDays);
    MOZ_ASSERT(IsValidTimeDuration(remainder));
  }

  // Step 14.
  auto dateDuration = DateDuration{
      duration.date.years,
      duration.date.months,
      duration.date.weeks,
      days,
  };
  MOZ_ASSERT(IsValidDuration(dateDuration));

  // Step 15.
  MOZ_ASSERT(DateDurationSign(dateDuration) * TimeDurationSign(remainder) >= 0);
  auto resultDuration = InternalDuration{dateDuration, remainder};

  // Step 16.
  return {resultDuration, nudgedEpochNs, mozilla::UnspecifiedNaN<double>(),
          didExpandDays};
}

/**
 * BubbleRelativeDuration ( sign, duration, nudgedEpochNs, isoDateTime,
 * timeZone, calendar, largestUnit, smallestUnit )
 */
static bool BubbleRelativeDuration(
    JSContext* cx, const InternalDuration& duration, const DurationNudge& nudge,
    const ISODateTime& isoDateTime, Handle<TimeZoneValue> timeZone,
    Handle<CalendarValue> calendar, TemporalUnit largestUnit,
    TemporalUnit smallestUnit, InternalDuration* result) {
  MOZ_ASSERT(IsValidDuration(duration));
  MOZ_ASSERT(IsValidDuration(nudge.duration));
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime));
  MOZ_ASSERT(largestUnit <= TemporalUnit::Day);
  MOZ_ASSERT(smallestUnit <= TemporalUnit::Day);
  MOZ_ASSERT(largestUnit <= smallestUnit);

  int32_t sign = InternalDurationSign(duration) < 0 ? -1 : 1;

  // Step 1.
  if (smallestUnit == largestUnit) {
    *result = nudge.duration;
    return true;
  }
  MOZ_ASSERT(smallestUnit != TemporalUnit::Year);

  // Steps 2-6.
  auto dateDuration = nudge.duration.date;
  auto timeDuration = nudge.duration.time;
  auto unit = smallestUnit;
  while (unit > largestUnit) {
    using TemporalUnitType = std::underlying_type_t<TemporalUnit>;

    static_assert(static_cast<TemporalUnitType>(TemporalUnit::Auto) == 0,
                  "TemporalUnit::Auto has value zero");
    MOZ_ASSERT(unit > TemporalUnit::Auto, "can subtract unit by one");

    // Steps 4, 6.a, and 6.c.
    unit = static_cast<TemporalUnit>(static_cast<TemporalUnitType>(unit) - 1);
    MOZ_ASSERT(TemporalUnit::Year <= unit && unit <= TemporalUnit::Week);

    // Step 6.b.
    if (unit != TemporalUnit::Week || largestUnit == TemporalUnit::Week) {
      // Steps 6.b.i-iii.
      DateDuration endDuration;
      if (unit == TemporalUnit::Year) {
        // Step 6.b.i.1.
        int64_t years = dateDuration.years + sign;

        // Step 6.b.i.2.
        endDuration = {years};
      } else if (unit == TemporalUnit::Month) {
        // Step 6.b.ii.1.
        int64_t months = dateDuration.months + sign;

        // Step 6.b.ii.2.
        endDuration = {dateDuration.years, months};
      } else {
        // Step 6.b.iii.1.
        MOZ_ASSERT(unit == TemporalUnit::Week);

        // Step 6.b.iii.2.
        int64_t weeks = dateDuration.weeks + sign;

        // Step 6.b.iii.3.
        endDuration = {dateDuration.years, dateDuration.months, weeks};
      }
      MOZ_ASSERT(IsValidDuration(endDuration));

      // Steps 6.b.iv.
      ISODate end;
      if (!CalendarDateAdd(cx, calendar, isoDateTime.date, endDuration,
                           TemporalOverflow::Constrain, &end)) {
        return false;
      }

      // Steps 6.b.v.
      auto endDateTime = ISODateTime{end, isoDateTime.time};
      MOZ_ASSERT(ISODateTimeWithinLimits(endDateTime));

      // Steps 6.b.vi-vii.
      EpochNanoseconds endEpochNs;
      if (!timeZone) {
        endEpochNs = GetUTCEpochNanoseconds(endDateTime);
      } else {
        if (!GetEpochNanosecondsFor(cx, timeZone, endDateTime,
                                    TemporalDisambiguation::Compatible,
                                    &endEpochNs)) {
          return false;
        }
      }

      // Step 6.b.viii.
      //
      // NB: |nudge.epochNs| can be outside the valid epoch nanoseconds limits.
      auto beyondEnd = nudge.epochNs - endEpochNs;

      // Step 6.b.ix.
      int32_t beyondEndSign = beyondEnd < EpochDuration{}   ? -1
                              : beyondEnd > EpochDuration{} ? 1
                                                            : 0;

      // Steps 6.b.x-xi.
      if (beyondEndSign != -sign) {
        dateDuration = endDuration;
        timeDuration = {};
      } else {
        break;
      }
    }

    // Step 6.c. (Moved above)
  }

  // Step 7.
  *result = {dateDuration, timeDuration};
  return true;
}

/**
 * RoundRelativeDuration ( duration, destEpochNs, isoDateTime, timeZone,
 * calendar, largestUnit, increment, smallestUnit, roundingMode )
 */
bool js::temporal::RoundRelativeDuration(
    JSContext* cx, const InternalDuration& duration,
    const EpochNanoseconds& destEpochNs, const ISODateTime& isoDateTime,
    Handle<TimeZoneValue> timeZone, Handle<CalendarValue> calendar,
    TemporalUnit largestUnit, Increment increment, TemporalUnit smallestUnit,
    TemporalRoundingMode roundingMode, InternalDuration* result) {
  MOZ_ASSERT(IsValidDuration(duration));
  MOZ_ASSERT_IF(timeZone, IsValidEpochNanoseconds(destEpochNs));
  MOZ_ASSERT_IF(!timeZone, IsValidLocalNanoseconds(destEpochNs));
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime));
  MOZ_ASSERT(largestUnit <= smallestUnit);

  // Steps 1-3.
  bool irregularLengthUnit = (smallestUnit < TemporalUnit::Day) ||
                             (timeZone && smallestUnit == TemporalUnit::Day);

  // Step 4. (Not applicable in our implementation.)

  // Steps 5-7.
  DurationNudge nudge;
  if (irregularLengthUnit) {
    // Step 5.a.
    if (!NudgeToCalendarUnit(cx, duration, destEpochNs, isoDateTime, timeZone,
                             calendar, increment, smallestUnit, roundingMode,
                             &nudge)) {
      return false;
    }
  } else if (timeZone) {
    // Step 6.a.
    if (!NudgeToZonedTime(cx, duration, isoDateTime, timeZone, calendar,
                          increment, smallestUnit, roundingMode, &nudge)) {
      return false;
    }
  } else {
    // Step 7.a.
    nudge = NudgeToDayOrTime(duration, destEpochNs, largestUnit, increment,
                             smallestUnit, roundingMode);
  }

  // Step 8.
  auto nudgedDuration = nudge.duration;

  // Step 9.
  if (nudge.didExpandCalendarUnit && smallestUnit != TemporalUnit::Week) {
    // Step 9.a. (Inlined LargerOfTwoTemporalUnits)
    auto startUnit = std::min(smallestUnit, TemporalUnit::Day);

    // Step 9.b.
    if (!BubbleRelativeDuration(cx, duration, nudge, isoDateTime, timeZone,
                                calendar, largestUnit, startUnit,
                                &nudgedDuration)) {
      return false;
    }
  }

  // Step 10.
  *result = nudgedDuration;
  return true;
}

/**
 * TotalRelativeDuration ( duration, destEpochNs, isoDateTime, timeZone,
 * calendar, unit )
 */
bool js::temporal::TotalRelativeDuration(JSContext* cx,
                                         const InternalDuration& duration,
                                         const EpochNanoseconds& destEpochNs,
                                         const ISODateTime& isoDateTime,
                                         JS::Handle<TimeZoneValue> timeZone,
                                         JS::Handle<CalendarValue> calendar,
                                         TemporalUnit unit, double* result) {
  MOZ_ASSERT(IsValidDuration(duration));
  MOZ_ASSERT_IF(timeZone, IsValidEpochNanoseconds(destEpochNs));
  MOZ_ASSERT_IF(!timeZone, IsValidLocalNanoseconds(destEpochNs));
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime));
  MOZ_ASSERT(unit <= TemporalUnit::Day);
  MOZ_ASSERT_IF(unit == TemporalUnit::Day, timeZone);

  // Steps 1.a-b.
  DurationNudge nudge;
  if (!NudgeToCalendarUnit(cx, duration, destEpochNs, isoDateTime, timeZone,
                           calendar, Increment{1}, unit,
                           TemporalRoundingMode::Trunc, &nudge)) {
    return false;
  }

  // Step 1.c.
  *result = nudge.total;
  return true;
}

/**
 * AddDurations ( operation, duration, other )
 */
static bool AddDurations(JSContext* cx, TemporalAddDuration operation,
                         const CallArgs& args) {
  auto* durationObj = &args.thisv().toObject().as<DurationObject>();
  auto duration = ToDuration(durationObj);

  // Step 1.
  Duration other;
  if (!ToTemporalDuration(cx, args.get(0), &other)) {
    return false;
  }

  // Step 2.
  if (operation == TemporalAddDuration::Subtract) {
    other = other.negate();
  }

  // Step 3.
  auto largestUnit1 = DefaultTemporalLargestUnit(duration);

  // Step 4.
  auto largestUnit2 = DefaultTemporalLargestUnit(other);

  // Step 5.
  auto largestUnit = std::min(largestUnit1, largestUnit2);

  // Step 6.
  if (largestUnit <= TemporalUnit::Week) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_UNCOMPARABLE,
                              "relativeTo");
    return false;
  }

  // Step 7.
  auto d1 = ToInternalDurationRecordWith24HourDays(duration).time;

  // Step 8.
  auto d2 = ToInternalDurationRecordWith24HourDays(other).time;

  // Step 9. (Inline AddTimeDuration)
  auto timeResult = d1 + d2;
  if (!IsValidTimeDuration(timeResult)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_INVALID_NORMALIZED_TIME);
    return false;
  }

  // Steps 10-11.
  Duration resultDuration;
  if (!TemporalDurationFromInternal(cx, timeResult, largestUnit,
                                    &resultDuration)) {
    return false;
  }
  MOZ_ASSERT(IsValidDuration(resultDuration));

  auto* obj = CreateTemporalDuration(cx, resultDuration);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.Duration ( [ years [ , months [ , weeks [ , days [ , hours [ ,
 * minutes [ , seconds [ , milliseconds [ , microseconds [ , nanoseconds ] ] ] ]
 * ] ] ] ] ] ] )
 */
static bool DurationConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Temporal.Duration")) {
    return false;
  }

  // Step 2.
  double years = 0;
  if (args.hasDefined(0) &&
      !ToIntegerIfIntegral(cx, "years", args[0], &years)) {
    return false;
  }

  // Step 3.
  double months = 0;
  if (args.hasDefined(1) &&
      !ToIntegerIfIntegral(cx, "months", args[1], &months)) {
    return false;
  }

  // Step 4.
  double weeks = 0;
  if (args.hasDefined(2) &&
      !ToIntegerIfIntegral(cx, "weeks", args[2], &weeks)) {
    return false;
  }

  // Step 5.
  double days = 0;
  if (args.hasDefined(3) && !ToIntegerIfIntegral(cx, "days", args[3], &days)) {
    return false;
  }

  // Step 6.
  double hours = 0;
  if (args.hasDefined(4) &&
      !ToIntegerIfIntegral(cx, "hours", args[4], &hours)) {
    return false;
  }

  // Step 7.
  double minutes = 0;
  if (args.hasDefined(5) &&
      !ToIntegerIfIntegral(cx, "minutes", args[5], &minutes)) {
    return false;
  }

  // Step 8.
  double seconds = 0;
  if (args.hasDefined(6) &&
      !ToIntegerIfIntegral(cx, "seconds", args[6], &seconds)) {
    return false;
  }

  // Step 9.
  double milliseconds = 0;
  if (args.hasDefined(7) &&
      !ToIntegerIfIntegral(cx, "milliseconds", args[7], &milliseconds)) {
    return false;
  }

  // Step 10.
  double microseconds = 0;
  if (args.hasDefined(8) &&
      !ToIntegerIfIntegral(cx, "microseconds", args[8], &microseconds)) {
    return false;
  }

  // Step 11.
  double nanoseconds = 0;
  if (args.hasDefined(9) &&
      !ToIntegerIfIntegral(cx, "nanoseconds", args[9], &nanoseconds)) {
    return false;
  }

  // Step 12.
  auto* duration = CreateTemporalDuration(
      cx, args,
      {years, months, weeks, days, hours, minutes, seconds, milliseconds,
       microseconds, nanoseconds});
  if (!duration) {
    return false;
  }

  args.rval().setObject(*duration);
  return true;
}

/**
 * Temporal.Duration.from ( item )
 */
static bool Duration_from(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Duration result;
  if (!ToTemporalDuration(cx, args.get(0), &result)) {
    return false;
  }

  auto* obj = CreateTemporalDuration(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.Duration.compare ( one, two [ , options ] )
 */
static bool Duration_compare(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Duration one;
  if (!ToTemporalDuration(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  Duration two;
  if (!ToTemporalDuration(cx, args.get(1), &two)) {
    return false;
  }

  // Steps 3-4.
  Rooted<PlainDate> plainRelativeTo(cx);
  Rooted<ZonedDateTime> zonedRelativeTo(cx);
  if (args.hasDefined(2)) {
    // Step 3.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", "compare", args[2]));
    if (!options) {
      return false;
    }

    // Step 4.
    if (!GetTemporalRelativeToOption(cx, options, &plainRelativeTo,
                                     &zonedRelativeTo)) {
      return false;
    }
    MOZ_ASSERT(!plainRelativeTo || !zonedRelativeTo);
  }

  // Step 5.
  if (one == two) {
    args.rval().setInt32(0);
    return true;
  }

  // Steps 6-9. (Not applicable in our implementation.)

  // Step 10.
  auto duration1 = ToInternalDurationRecord(one);

  // Step 11.
  auto duration2 = ToInternalDurationRecord(two);

  // Step 12.
  if (zonedRelativeTo &&
      (duration1.date != DateDuration{} || duration2.date != DateDuration{})) {
    // Steps 12.a-b. (Not applicable in our implementation.)

    // Step 12.c.
    EpochNanoseconds after1;
    if (!AddZonedDateTime(cx, zonedRelativeTo, duration1, &after1)) {
      return false;
    }

    // Step 12.d.
    EpochNanoseconds after2;
    if (!AddZonedDateTime(cx, zonedRelativeTo, duration2, &after2)) {
      return false;
    }

    // Steps 12.e-g.
    args.rval().setInt32(after1 < after2 ? -1 : after1 > after2 ? 1 : 0);
    return true;
  }

  // Steps 13.a-b and 14.a.
  int64_t days1;
  if (!DateDurationDays(cx, duration1.date, plainRelativeTo, &days1)) {
    return false;
  }

  // Steps 13.a, 13.c, and 14.b.
  int64_t days2;
  if (!DateDurationDays(cx, duration2.date, plainRelativeTo, &days2)) {
    return false;
  }

  // Step 15.
  auto timeDuration1 = duration1.time;
  if (!Add24HourDaysToTimeDuration(cx, duration1.time, days1, &timeDuration1)) {
    return false;
  }

  // Step 16.
  auto timeDuration2 = duration2.time;
  if (!Add24HourDaysToTimeDuration(cx, duration2.time, days2, &timeDuration2)) {
    return false;
  }

  // Step 17.
  args.rval().setInt32(CompareTimeDuration(timeDuration1, timeDuration2));
  return true;
}

/**
 * get Temporal.Duration.prototype.years
 */
static bool Duration_years(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* duration = &args.thisv().toObject().as<DurationObject>();
  args.rval().setNumber(duration->years());
  return true;
}

/**
 * get Temporal.Duration.prototype.years
 */
static bool Duration_years(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_years>(cx, args);
}

/**
 * get Temporal.Duration.prototype.months
 */
static bool Duration_months(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* duration = &args.thisv().toObject().as<DurationObject>();
  args.rval().setNumber(duration->months());
  return true;
}

/**
 * get Temporal.Duration.prototype.months
 */
static bool Duration_months(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_months>(cx, args);
}

/**
 * get Temporal.Duration.prototype.weeks
 */
static bool Duration_weeks(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* duration = &args.thisv().toObject().as<DurationObject>();
  args.rval().setNumber(duration->weeks());
  return true;
}

/**
 * get Temporal.Duration.prototype.weeks
 */
static bool Duration_weeks(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_weeks>(cx, args);
}

/**
 * get Temporal.Duration.prototype.days
 */
static bool Duration_days(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* duration = &args.thisv().toObject().as<DurationObject>();
  args.rval().setNumber(duration->days());
  return true;
}

/**
 * get Temporal.Duration.prototype.days
 */
static bool Duration_days(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_days>(cx, args);
}

/**
 * get Temporal.Duration.prototype.hours
 */
static bool Duration_hours(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* duration = &args.thisv().toObject().as<DurationObject>();
  args.rval().setNumber(duration->hours());
  return true;
}

/**
 * get Temporal.Duration.prototype.hours
 */
static bool Duration_hours(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_hours>(cx, args);
}

/**
 * get Temporal.Duration.prototype.minutes
 */
static bool Duration_minutes(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* duration = &args.thisv().toObject().as<DurationObject>();
  args.rval().setNumber(duration->minutes());
  return true;
}

/**
 * get Temporal.Duration.prototype.minutes
 */
static bool Duration_minutes(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_minutes>(cx, args);
}

/**
 * get Temporal.Duration.prototype.seconds
 */
static bool Duration_seconds(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* duration = &args.thisv().toObject().as<DurationObject>();
  args.rval().setNumber(duration->seconds());
  return true;
}

/**
 * get Temporal.Duration.prototype.seconds
 */
static bool Duration_seconds(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_seconds>(cx, args);
}

/**
 * get Temporal.Duration.prototype.milliseconds
 */
static bool Duration_milliseconds(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* duration = &args.thisv().toObject().as<DurationObject>();
  args.rval().setNumber(duration->milliseconds());
  return true;
}

/**
 * get Temporal.Duration.prototype.milliseconds
 */
static bool Duration_milliseconds(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_milliseconds>(cx, args);
}

/**
 * get Temporal.Duration.prototype.microseconds
 */
static bool Duration_microseconds(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* duration = &args.thisv().toObject().as<DurationObject>();
  args.rval().setNumber(duration->microseconds());
  return true;
}

/**
 * get Temporal.Duration.prototype.microseconds
 */
static bool Duration_microseconds(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_microseconds>(cx, args);
}

/**
 * get Temporal.Duration.prototype.nanoseconds
 */
static bool Duration_nanoseconds(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* duration = &args.thisv().toObject().as<DurationObject>();
  args.rval().setNumber(duration->nanoseconds());
  return true;
}

/**
 * get Temporal.Duration.prototype.nanoseconds
 */
static bool Duration_nanoseconds(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_nanoseconds>(cx, args);
}

/**
 * get Temporal.Duration.prototype.sign
 */
static bool Duration_sign(JSContext* cx, const CallArgs& args) {
  auto duration = ToDuration(&args.thisv().toObject().as<DurationObject>());

  // Step 3.
  args.rval().setInt32(DurationSign(duration));
  return true;
}

/**
 * get Temporal.Duration.prototype.sign
 */
static bool Duration_sign(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_sign>(cx, args);
}

/**
 * get Temporal.Duration.prototype.blank
 */
static bool Duration_blank(JSContext* cx, const CallArgs& args) {
  auto duration = ToDuration(&args.thisv().toObject().as<DurationObject>());

  // Steps 3-4.
  args.rval().setBoolean(duration == Duration{});
  return true;
}

/**
 * get Temporal.Duration.prototype.blank
 */
static bool Duration_blank(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_blank>(cx, args);
}

/**
 * Temporal.Duration.prototype.with ( temporalDurationLike )
 */
static bool Duration_with(JSContext* cx, const CallArgs& args) {
  // Absent values default to the corresponding values of |this| object.
  auto duration = ToDuration(&args.thisv().toObject().as<DurationObject>());

  // Steps 3-23.
  Rooted<JSObject*> temporalDurationLike(
      cx, RequireObjectArg(cx, "temporalDurationLike", "with", args.get(0)));
  if (!temporalDurationLike) {
    return false;
  }
  if (!ToTemporalPartialDurationRecord(cx, temporalDurationLike, &duration)) {
    return false;
  }

  // Step 24.
  auto* result = CreateTemporalDuration(cx, duration);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Duration.prototype.with ( temporalDurationLike )
 */
static bool Duration_with(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_with>(cx, args);
}

/**
 * Temporal.Duration.prototype.negated ( )
 */
static bool Duration_negated(JSContext* cx, const CallArgs& args) {
  auto duration = ToDuration(&args.thisv().toObject().as<DurationObject>());

  // Step 3.
  auto* result = CreateTemporalDuration(cx, duration.negate());
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Duration.prototype.negated ( )
 */
static bool Duration_negated(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_negated>(cx, args);
}

/**
 * Temporal.Duration.prototype.abs ( )
 */
static bool Duration_abs(JSContext* cx, const CallArgs& args) {
  auto duration = ToDuration(&args.thisv().toObject().as<DurationObject>());

  // Step 3.
  auto* result = CreateTemporalDuration(cx, AbsoluteDuration(duration));
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Duration.prototype.abs ( )
 */
static bool Duration_abs(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_abs>(cx, args);
}

/**
 * Temporal.Duration.prototype.add ( other )
 */
static bool Duration_add(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurations(cx, TemporalAddDuration::Add, args);
}

/**
 * Temporal.Duration.prototype.add ( other )
 */
static bool Duration_add(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_add>(cx, args);
}

/**
 * Temporal.Duration.prototype.subtract ( other )
 */
static bool Duration_subtract(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurations(cx, TemporalAddDuration::Subtract, args);
}

/**
 * Temporal.Duration.prototype.subtract ( other )
 */
static bool Duration_subtract(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_subtract>(cx, args);
}

/**
 * Temporal.Duration.prototype.round ( roundTo )
 */
static bool Duration_round(JSContext* cx, const CallArgs& args) {
  auto duration = ToDuration(&args.thisv().toObject().as<DurationObject>());

  // Step 17. (Reordered)
  auto existingLargestUnit = DefaultTemporalLargestUnit(duration);

  // Steps 3-25.
  auto smallestUnit = TemporalUnit::Auto;
  TemporalUnit largestUnit;
  auto roundingMode = TemporalRoundingMode::HalfExpand;
  auto roundingIncrement = Increment{1};
  Rooted<PlainDate> plainRelativeTo(cx);
  Rooted<ZonedDateTime> zonedRelativeTo(cx);
  if (args.get(0).isString()) {
    // Step 4. (Not applicable in our implementation.)

    // Steps 6-14. (Not applicable)

    // Step 15.
    Rooted<JSString*> paramString(cx, args[0].toString());
    if (!GetTemporalUnitValuedOption(
            cx, paramString, TemporalUnitKey::SmallestUnit,
            TemporalUnitGroup::DateTime, &smallestUnit)) {
      return false;
    }

    // Step 16. (Not applicable)

    // Step 17. (Moved above)

    // Step 18.
    auto defaultLargestUnit = std::min(existingLargestUnit, smallestUnit);

    // Step 19. (Not applicable)

    // Step 19.a. (Not applicable)

    // Step 19.b.
    largestUnit = defaultLargestUnit;

    // Steps 20-25. (Not applicable)
  } else {
    // Steps 3 and 5.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "roundTo", "round", args.get(0)));
    if (!options) {
      return false;
    }

    // Step 6.
    bool smallestUnitPresent = true;

    // Step 7.
    bool largestUnitPresent = true;

    // Steps 8-9.
    //
    // Inlined GetTemporalUnitValuedOption and GetOption so we can more easily
    // detect an absent "largestUnit" value.
    Rooted<Value> largestUnitValue(cx);
    if (!GetProperty(cx, options, options, cx->names().largestUnit,
                     &largestUnitValue)) {
      return false;
    }

    if (!largestUnitValue.isUndefined()) {
      Rooted<JSString*> largestUnitStr(cx, JS::ToString(cx, largestUnitValue));
      if (!largestUnitStr) {
        return false;
      }

      largestUnit = TemporalUnit::Auto;
      if (!GetTemporalUnitValuedOption(
              cx, largestUnitStr, TemporalUnitKey::LargestUnit,
              TemporalUnitGroup::DateTime, &largestUnit)) {
        return false;
      }
    }

    // Steps 10-12.
    if (!GetTemporalRelativeToOption(cx, options, &plainRelativeTo,
                                     &zonedRelativeTo)) {
      return false;
    }
    MOZ_ASSERT(!plainRelativeTo || !zonedRelativeTo);

    // Step 13.
    if (!GetRoundingIncrementOption(cx, options, &roundingIncrement)) {
      return false;
    }

    // Step 14.
    if (!GetRoundingModeOption(cx, options, &roundingMode)) {
      return false;
    }

    // Step 15.
    if (!GetTemporalUnitValuedOption(cx, options, TemporalUnitKey::SmallestUnit,
                                     TemporalUnitGroup::DateTime,
                                     &smallestUnit)) {
      return false;
    }

    // Step 16.
    if (smallestUnit == TemporalUnit::Auto) {
      // Step 16.a.
      smallestUnitPresent = false;

      // Step 16.b.
      smallestUnit = TemporalUnit::Nanosecond;
    }

    // Step 17. (Moved above)

    // Step 18.
    auto defaultLargestUnit = std::min(existingLargestUnit, smallestUnit);

    // Steps 19-20.
    if (largestUnitValue.isUndefined()) {
      // Step 19.a.
      largestUnitPresent = false;

      // Step 19.b.
      largestUnit = defaultLargestUnit;
    } else if (largestUnit == TemporalUnit::Auto) {
      // Step 20.a
      largestUnit = defaultLargestUnit;
    }

    // Step 21.
    if (!smallestUnitPresent && !largestUnitPresent) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_DURATION_MISSING_UNIT_SPECIFIER);
      return false;
    }

    // Step 22.
    if (largestUnit > smallestUnit) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INVALID_UNIT_RANGE);
      return false;
    }

    // Steps 23-24.
    if (smallestUnit > TemporalUnit::Day) {
      // Step 23.
      auto maximum = MaximumTemporalDurationRoundingIncrement(smallestUnit);

      // Step 24.
      if (!ValidateTemporalRoundingIncrement(cx, roundingIncrement, maximum,
                                             false)) {
        return false;
      }
    }

    // Step 25.
    if (roundingIncrement > Increment{1} && largestUnit != smallestUnit &&
        smallestUnit <= TemporalUnit::Day) {
      Int32ToCStringBuf cbuf;
      const char* numStr =
          Int32ToCString(&cbuf, int32_t(roundingIncrement.value()));

      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_INVALID_OPTION_VALUE, "roundingIncrement",
                                numStr);
      return false;
    }
  }

  // Step 26.
  if (zonedRelativeTo) {
    // Step 26.a.
    auto internalDuration = ToInternalDurationRecord(duration);

    // Steps 26.b-d. (Not applicable in our implementation.)

    // Step 26.e.
    EpochNanoseconds targetEpochNs;
    if (!AddZonedDateTime(cx, zonedRelativeTo, internalDuration,
                          &targetEpochNs)) {
      return false;
    }

    // Step 26.f.
    if (!DifferenceZonedDateTimeWithRounding(cx, zonedRelativeTo, targetEpochNs,
                                             {
                                                 smallestUnit,
                                                 largestUnit,
                                                 roundingMode,
                                                 roundingIncrement,
                                             },
                                             &internalDuration)) {
      return false;
    }

    // Step 26.g.
    largestUnit = std::max(largestUnit, TemporalUnit::Hour);

    // Step 26.h
    Duration result;
    if (!TemporalDurationFromInternal(cx, internalDuration, largestUnit,
                                      &result)) {
      return false;
    }

    auto* obj = CreateTemporalDuration(cx, result);
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Step 27.
  if (plainRelativeTo) {
    // Step 27.a.
    auto internalDuration = ToInternalDurationRecordWith24HourDays(duration);

    // Step 27.b.
    auto targetTime = AddTime(Time{}, internalDuration.time);

    // Step 27.c.
    auto calendar = plainRelativeTo.calendar();

    // Step 27.d.
    auto dateDuration = DateDuration{
        internalDuration.date.years,
        internalDuration.date.months,
        internalDuration.date.weeks,
        targetTime.days,
    };
    MOZ_ASSERT(IsValidDuration(dateDuration));

    // 27.e.
    ISODate targetDate;
    if (!CalendarDateAdd(cx, calendar, plainRelativeTo, dateDuration,
                         TemporalOverflow::Constrain, &targetDate)) {
      return false;
    }

    // Step 27.f.
    auto isoDateTime = ISODateTime{plainRelativeTo, {}};

    // Step 27.g.
    auto targetDateTime = ISODateTime{targetDate, targetTime.time};

    // DifferencePlainDateTimeWithRounding, step 2.
    if (!ISODateTimeWithinLimits(isoDateTime) ||
        !ISODateTimeWithinLimits(targetDateTime)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
      return false;
    }

    // Step 27.h.
    if (!DifferencePlainDateTimeWithRounding(cx, isoDateTime, targetDateTime,
                                             calendar,
                                             {
                                                 smallestUnit,
                                                 largestUnit,
                                                 roundingMode,
                                                 roundingIncrement,
                                             },
                                             &internalDuration)) {
      return false;
    }

    // Step 27.i
    Duration result;
    if (!TemporalDurationFromInternal(cx, internalDuration, largestUnit,
                                      &result)) {
      return false;
    }

    auto* obj = CreateTemporalDuration(cx, result);
    if (!obj) {
      return false;
    }

    args.rval().setObject(*obj);
    return true;
  }

  // Step 28.
  if (existingLargestUnit < TemporalUnit::Day ||
      largestUnit < TemporalUnit::Day) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_DURATION_UNCOMPARABLE,
                              "relativeTo");
    return false;
  }

  // Step 29.
  MOZ_ASSERT(smallestUnit >= TemporalUnit::Day);

  // Step 30.
  auto internalDuration = ToInternalDurationRecordWith24HourDays(duration);
  MOZ_ASSERT(internalDuration.date == DateDuration{});

  // Steps 31-32.
  if (smallestUnit == TemporalUnit::Day) {
    // Steps 31.a-b.
    constexpr auto nsPerDay = ToNanoseconds(TemporalUnit::Day);
    auto rounded =
        RoundNumberToIncrement(internalDuration.time.toNanoseconds(), nsPerDay,
                               roundingIncrement, roundingMode);
    MOZ_ASSERT(Int128{INT64_MIN} <= rounded && rounded <= Int128{INT64_MAX},
               "rounded days fits in int64");
    auto days = static_cast<int64_t>(rounded);

    // Step 31.c. (Inlined CreateDateDurationRecord)
    if (std::abs(days) > TimeDuration::max().toDays()) {
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr,
          JSMSG_TEMPORAL_DURATION_INVALID_NORMALIZED_TIME);
      return false;
    }
    auto dateDuration = DateDuration{0, 0, 0, days};
    MOZ_ASSERT(IsValidDuration(dateDuration));

    // Step 31.d.
    internalDuration = {dateDuration, {}};
  } else {
    // Step 32.a.
    TimeDuration timeDuration;
    if (!RoundTimeDuration(cx, internalDuration.time, roundingIncrement,
                           smallestUnit, roundingMode, &timeDuration)) {
      return false;
    }

    // Step 32.b.
    internalDuration = {{}, timeDuration};
  }

  // Step 32.
  Duration result;
  if (!TemporalDurationFromInternal(cx, internalDuration, largestUnit,
                                    &result)) {
    return false;
  }

  auto* obj = CreateTemporalDuration(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.Duration.prototype.round ( options )
 */
static bool Duration_round(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_round>(cx, args);
}

/**
 * Temporal.Duration.prototype.total ( totalOf )
 */
static bool Duration_total(JSContext* cx, const CallArgs& args) {
  auto* durationObj = &args.thisv().toObject().as<DurationObject>();
  auto duration = ToDuration(durationObj);

  // Steps 3-10.
  Rooted<PlainDate> plainRelativeTo(cx);
  Rooted<ZonedDateTime> zonedRelativeTo(cx);
  auto unit = TemporalUnit::Auto;
  if (args.get(0).isString()) {
    // Step 4. (Not applicable in our implementation.)

    // Steps 6-9. (Implicit)
    MOZ_ASSERT(!plainRelativeTo && !zonedRelativeTo);

    // Step 10.
    Rooted<JSString*> paramString(cx, args[0].toString());
    if (!GetTemporalUnitValuedOption(cx, paramString, TemporalUnitKey::Unit,
                                     TemporalUnitGroup::DateTime, &unit)) {
      return false;
    }
  } else {
    // Steps 3 and 5.
    Rooted<JSObject*> totalOf(
        cx, RequireObjectArg(cx, "totalOf", "total", args.get(0)));
    if (!totalOf) {
      return false;
    }

    // Steps 6-9.
    if (!GetTemporalRelativeToOption(cx, totalOf, &plainRelativeTo,
                                     &zonedRelativeTo)) {
      return false;
    }
    MOZ_ASSERT(!plainRelativeTo || !zonedRelativeTo);

    // Step 10.
    if (!GetTemporalUnitValuedOption(cx, totalOf, TemporalUnitKey::Unit,
                                     TemporalUnitGroup::DateTime, &unit)) {
      return false;
    }

    if (unit == TemporalUnit::Auto) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_MISSING_OPTION, "unit");
      return false;
    }
  }

  // Steps 11-13.
  double total;
  if (zonedRelativeTo) {
    // Step 11.a.
    auto internalDuration = ToInternalDurationRecord(duration);

    // Steps 11.b-d. (Not applicable in our implementation.)

    // Step 11.e.
    EpochNanoseconds targetEpochNs;
    if (!AddZonedDateTime(cx, zonedRelativeTo, internalDuration,
                          &targetEpochNs)) {
      return false;
    }

    // Step 11.f.
    if (!DifferenceZonedDateTimeWithTotal(cx, zonedRelativeTo, targetEpochNs,
                                          unit, &total)) {
      return false;
    }
  } else if (plainRelativeTo) {
    // Step 12.a.
    auto internalDuration = ToInternalDurationRecordWith24HourDays(duration);

    // Step 12.b.
    auto targetTime = AddTime(Time{}, internalDuration.time);

    // Step 12.c.
    auto calendar = plainRelativeTo.calendar();

    // Step 12.d.
    auto dateDuration = DateDuration{
        internalDuration.date.years,
        internalDuration.date.months,
        internalDuration.date.weeks,
        targetTime.days,
    };
    MOZ_ASSERT(IsValidDuration(dateDuration));

    // Step 12.e.
    ISODate targetDate;
    if (!CalendarDateAdd(cx, calendar, plainRelativeTo, dateDuration,
                         TemporalOverflow::Constrain, &targetDate)) {
      return false;
    }

    // Step 12.f.
    auto isoDateTime = ISODateTime{plainRelativeTo, {}};

    // Step 12.g.
    auto targetDateTime = ISODateTime{targetDate, targetTime.time};

    // DifferencePlainDateTimeWithTotal, step 2.
    if (!ISODateTimeWithinLimits(isoDateTime) ||
        !ISODateTimeWithinLimits(targetDateTime)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_PLAIN_DATE_TIME_INVALID);
      return false;
    }

    // Step 12.h.
    if (!DifferencePlainDateTimeWithTotal(cx, isoDateTime, targetDateTime,
                                          calendar, unit, &total)) {
      return false;
    }
  } else {
    // Steps 13.a-b.
    if (duration.years != 0 || duration.months != 0 || duration.weeks != 0 ||
        unit < TemporalUnit::Day) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_DURATION_UNCOMPARABLE,
                                "relativeTo");
      return false;
    }

    // Step 13.c.
    auto internalDuration = ToInternalDurationRecordWith24HourDays(duration);

    // Step 13.d.
    total = TotalTimeDuration(internalDuration.time, unit);
  }

  // Step 14.
  args.rval().setNumber(total);
  return true;
}

/**
 * Temporal.Duration.prototype.total ( totalOf )
 */
static bool Duration_total(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_total>(cx, args);
}

/**
 * Temporal.Duration.prototype.toString ( [ options ] )
 */
static bool Duration_toString(JSContext* cx, const CallArgs& args) {
  auto duration = ToDuration(&args.thisv().toObject().as<DurationObject>());

  // Steps 3-9.
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
    if (smallestUnit == TemporalUnit::Hour ||
        smallestUnit == TemporalUnit::Minute) {
      const char* smallestUnitStr =
          smallestUnit == TemporalUnit::Hour ? "hour" : "minute";
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INVALID_UNIT_OPTION,
                                smallestUnitStr, "smallestUnit");
      return false;
    }

    // Step 9.
    precision = ToSecondsStringPrecision(smallestUnit, digits);
  }
  MOZ_ASSERT(precision.unit >= TemporalUnit::Minute);

  // Steps 10-16.
  auto roundedDuration = duration;
  if (precision.unit != TemporalUnit::Nanosecond ||
      precision.increment != Increment{1}) {
    // Step 11.
    auto largestUnit = DefaultTemporalLargestUnit(duration);

    // Step 12.
    auto internalDuration = ToInternalDurationRecord(duration);

    // Step 13.
    TimeDuration timeDuration;
    if (!RoundTimeDuration(cx, internalDuration.time, precision.increment,
                           precision.unit, roundingMode, &timeDuration)) {
      return false;
    }

    // Step 14.
    internalDuration = {internalDuration.date, timeDuration};

    // Step 15.
    auto roundedLargestUnit = std::min(largestUnit, TemporalUnit::Second);

    // Step 16.
    if (!TemporalDurationFromInternal(cx, internalDuration, roundedLargestUnit,
                                      &roundedDuration)) {
      return false;
    }
    MOZ_ASSERT(IsValidDuration(roundedDuration));
  }

  // Steps 10.a. and 17.
  JSString* str =
      TemporalDurationToString(cx, roundedDuration, precision.precision);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.Duration.prototype.toString ( [ options ] )
 */
static bool Duration_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_toString>(cx, args);
}

/**
 * Temporal.Duration.prototype.toJSON ( )
 */
static bool Duration_toJSON(JSContext* cx, const CallArgs& args) {
  auto duration = ToDuration(&args.thisv().toObject().as<DurationObject>());

  // Step 3.
  JSString* str = TemporalDurationToString(cx, duration, Precision::Auto());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.Duration.prototype.toJSON ( )
 */
static bool Duration_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_toJSON>(cx, args);
}

/**
 * Temporal.Duration.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool Duration_toLocaleString(JSContext* cx, const CallArgs& args) {
  // Steps 3-7.
  return TemporalDurationToLocaleString(cx, args);
}

/**
 * Temporal.Duration.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool Duration_toLocaleString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsDuration, Duration_toLocaleString>(cx, args);
}

/**
 * Temporal.Duration.prototype.valueOf ( )
 */
static bool Duration_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                            "Duration", "primitive type");
  return false;
}

const JSClass DurationObject::class_ = {
    "Temporal.Duration",
    JSCLASS_HAS_RESERVED_SLOTS(DurationObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Duration),
    JS_NULL_CLASS_OPS,
    &DurationObject::classSpec_,
};

const JSClass& DurationObject::protoClass_ = PlainObject::class_;

static const JSFunctionSpec Duration_methods[] = {
    JS_FN("from", Duration_from, 1, 0),
    JS_FN("compare", Duration_compare, 2, 0),
    JS_FS_END,
};

static const JSFunctionSpec Duration_prototype_methods[] = {
    JS_FN("with", Duration_with, 1, 0),
    JS_FN("negated", Duration_negated, 0, 0),
    JS_FN("abs", Duration_abs, 0, 0),
    JS_FN("add", Duration_add, 1, 0),
    JS_FN("subtract", Duration_subtract, 1, 0),
    JS_FN("round", Duration_round, 1, 0),
    JS_FN("total", Duration_total, 1, 0),
    JS_FN("toString", Duration_toString, 0, 0),
    JS_FN("toJSON", Duration_toJSON, 0, 0),
    JS_FN("toLocaleString", Duration_toLocaleString, 0, 0),
    JS_FN("valueOf", Duration_valueOf, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec Duration_prototype_properties[] = {
    JS_PSG("years", Duration_years, 0),
    JS_PSG("months", Duration_months, 0),
    JS_PSG("weeks", Duration_weeks, 0),
    JS_PSG("days", Duration_days, 0),
    JS_PSG("hours", Duration_hours, 0),
    JS_PSG("minutes", Duration_minutes, 0),
    JS_PSG("seconds", Duration_seconds, 0),
    JS_PSG("milliseconds", Duration_milliseconds, 0),
    JS_PSG("microseconds", Duration_microseconds, 0),
    JS_PSG("nanoseconds", Duration_nanoseconds, 0),
    JS_PSG("sign", Duration_sign, 0),
    JS_PSG("blank", Duration_blank, 0),
    JS_STRING_SYM_PS(toStringTag, "Temporal.Duration", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec DurationObject::classSpec_ = {
    GenericCreateConstructor<DurationConstructor, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<DurationObject>,
    Duration_methods,
    nullptr,
    Duration_prototype_methods,
    Duration_prototype_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};
