/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS date methods.
 *
 * "For example, OS/360 devotes 26 bytes of the permanently
 *  resident date-turnover routine to the proper handling of
 *  December 31 on leap years (when it is Day 366).  That
 *  might have been left to the operator."
 *
 * Frederick Brooks, 'The Second-System Effect'.
 */

#include "jsdate.h"

#include "mozilla/Atomics.h"
#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/TextUtils.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <math.h>
#include <string.h>

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jsnum.h"
#include "jstypes.h"

#ifdef JS_HAS_INTL_API
#  include "builtin/temporal/Instant.h"
#endif
#include "jit/InlinableNatives.h"
#include "js/CallAndConstruct.h"  // JS::IsCallable
#include "js/Conversions.h"
#include "js/Date.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/LocaleSensitive.h"
#include "js/Object.h"  // JS::GetBuiltinClass
#include "js/PropertySpec.h"
#include "js/Wrapper.h"
#include "util/DifferentialTesting.h"
#include "util/StringBuilder.h"
#include "util/Text.h"
#include "vm/DateObject.h"
#include "vm/DateTime.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/StringType.h"
#include "vm/Time.h"

#include "vm/Compartment-inl.h"  // For js::UnwrapAndTypeCheckThis
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;

using mozilla::Atomic;
using mozilla::BitwiseCast;
using mozilla::IsAsciiAlpha;
using mozilla::IsAsciiDigit;
using mozilla::IsAsciiLowercaseAlpha;
using mozilla::NumbersAreIdentical;
using mozilla::Relaxed;

using JS::AutoCheckCannotGC;
using JS::ClippedTime;
using JS::GenericNaN;
using JS::GetBuiltinClass;
using JS::TimeClip;
using JS::ToInteger;

// When this value is non-zero, we'll round the time by this resolution.
static Atomic<uint32_t, Relaxed> sResolutionUsec;
// This is not implemented yet, but we will use this to know to jitter the time
// in the JS shell
static Atomic<bool, Relaxed> sJitter;
// The callback we will use for the Gecko implementation of Timer
// Clamping/Jittering
static Atomic<JS::ReduceMicrosecondTimePrecisionCallback, Relaxed>
    sReduceMicrosecondTimePrecisionCallback;

namespace {

class DateTimeHelper {
 private:
#if !JS_HAS_INTL_API
  static int equivalentYearForDST(int year);
  static bool isRepresentableAsTime32(int64_t t);
  static int32_t daylightSavingTA(DateTimeInfo::ForceUTC forceUTC, int64_t t);
  static int32_t adjustTime(DateTimeInfo::ForceUTC forceUTC, int64_t date);
  static PRMJTime toPRMJTime(DateTimeInfo::ForceUTC forceUTC, int64_t localTime,
                             int64_t utcTime);
#endif

 public:
  static int32_t getTimeZoneOffset(DateTimeInfo::ForceUTC forceUTC,
                                   int64_t epochMilliseconds,
                                   DateTimeInfo::TimeZoneOffset offset);

  static JSString* timeZoneComment(JSContext* cx,
                                   DateTimeInfo::ForceUTC forceUTC,
                                   const char* locale, int64_t utcTime,
                                   int64_t localTime);
#if !JS_HAS_INTL_API
  static size_t formatTime(DateTimeInfo::ForceUTC forceUTC, char* buf,
                           size_t buflen, const char* fmt, int64_t utcTime,
                           int64_t localTime);
#endif
};

}  // namespace

static DateTimeInfo::ForceUTC ForceUTC(const Realm* realm) {
  return realm->creationOptions().forceUTC() ? DateTimeInfo::ForceUTC::Yes
                                             : DateTimeInfo::ForceUTC::No;
}

/**
 * 5.2.5 Mathematical Operations
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static inline double PositiveModulo(double dividend, double divisor) {
  MOZ_ASSERT(divisor > 0);
  MOZ_ASSERT(std::isfinite(divisor));

  double result = fmod(dividend, divisor);
  if (result < 0) {
    result += divisor;
  }
  return result + (+0.0);
}

template <typename T>
static inline std::enable_if_t<std::is_integral_v<T>, int32_t> PositiveModulo(
    T dividend, int32_t divisor) {
  MOZ_ASSERT(divisor > 0);

  int32_t result = dividend % divisor;
  if (result < 0) {
    result += divisor;
  }
  return result;
}

template <typename T>
static constexpr T FloorDiv(T dividend, int32_t divisor) {
  MOZ_ASSERT(divisor > 0);

  T quotient = dividend / divisor;
  T remainder = dividend % divisor;
  if (remainder < 0) {
    quotient -= 1;
  }
  return quotient;
}

#ifdef DEBUG
/**
 * 21.4.1.1 Time Values and Time Range
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static inline bool IsTimeValue(double t) {
  if (std::isnan(t)) {
    return true;
  }
  return IsInteger(t) && StartOfTime <= t && t <= EndOfTime;
}
#endif

/**
 * Time value with local time zone offset applied.
 */
static inline bool IsLocalTimeValue(double t) {
  if (std::isnan(t)) {
    return true;
  }
  return IsInteger(t) && (StartOfTime - msPerDay) < t &&
         t < (EndOfTime + msPerDay);
}

/**
 * 21.4.1.3 Day ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static inline int32_t Day(int64_t t) {
  MOZ_ASSERT(IsLocalTimeValue(t));
  return int32_t(FloorDiv(t, msPerDay));
}

/**
 * 21.4.1.4 TimeWithinDay ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static int32_t TimeWithinDay(int64_t t) {
  MOZ_ASSERT(IsLocalTimeValue(t));
  return PositiveModulo(t, msPerDay);
}

/**
 * 21.4.1.5 DaysInYear ( y )
 * 21.4.1.10 InLeapYear ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static inline bool IsLeapYear(double year) {
  MOZ_ASSERT(IsInteger(year));
  return fmod(year, 4) == 0 && (fmod(year, 100) != 0 || fmod(year, 400) == 0);
}

static constexpr bool IsLeapYear(int32_t year) {
  MOZ_ASSERT(mozilla::Abs(year) <= 2'000'000);

  // From: https://www.youtube.com/watch?v=0s9F4QWAl-E&t=1790s
  int32_t d = (year % 100 != 0) ? 4 : 16;
  return (year & (d - 1)) == 0;
}

/**
 * 21.4.1.6 DayFromYear ( y )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static inline double DayFromYear(double y) {
  // Steps 1-7.
  return 365 * (y - 1970) + floor((y - 1969) / 4.0) -
         floor((y - 1901) / 100.0) + floor((y - 1601) / 400.0);
}

static constexpr int32_t DayFromYear(int32_t y) {
  MOZ_ASSERT(mozilla::Abs(y) <= 2'000'000);

  // Steps 1-7.
  return 365 * (y - 1970) + FloorDiv((y - 1969), 4) -
         FloorDiv((y - 1901), 100) + FloorDiv((y - 1601), 400);
}

/**
 * 21.4.1.7 TimeFromYear ( y )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static inline double TimeFromYear(double y) {
  return ::DayFromYear(y) * msPerDay;
}

static inline int64_t TimeFromYear(int32_t y) {
  return ::DayFromYear(y) * int64_t(msPerDay);
}

/*
 * This function returns the year, month and day corresponding to a given
 * time value. The implementation closely follows (w.r.t. types and variable
 * names) the algorithm shown in Figure 12 of [1].
 *
 * A key point of the algorithm is that it works on the so called
 * Computational calendar where years run from March to February -- this
 * largely avoids complications with leap years. The algorithm finds the
 * date in the Computational calendar and then maps it to the Gregorian
 * calendar.
 *
 * [1] Neri C, Schneider L., "Euclidean affine functions and their
 * application to calendar algorithms."
 * Softw Pract Exper. 2023;53(4):937-970. doi: 10.1002/spe.3172
 * https://onlinelibrary.wiley.com/doi/full/10.1002/spe.3172
 */
YearMonthDay js::ToYearMonthDay(int64_t time) {
  // Calendar cycles repeat every 400 years in the Gregorian calendar: a
  // leap day is added every 4 years, removed every 100 years and added
  // every 400 years. The number of days in 400 years is cycleInDays.
  constexpr uint32_t cycleInYears = 400;
  constexpr uint32_t cycleInDays = cycleInYears * 365 + (cycleInYears / 4) -
                                   (cycleInYears / 100) + (cycleInYears / 400);
  static_assert(cycleInDays == 146097, "Wrong calculation of cycleInDays.");

  // The natural epoch for the Computational calendar is 0000/Mar/01 and
  // there are rataDie1970Jan1 = 719468 days from this date to 1970/Jan/01,
  // the epoch used by ES2024, 21.4.1.1.
  constexpr uint32_t rataDie1970Jan1 = 719468;

  constexpr uint32_t maxU32 = std::numeric_limits<uint32_t>::max();

  // Let N_U be the number of days since the 1970/Jan/01. This function sets
  // N = N_U + K, where K = rataDie1970Jan1 + s * cycleInDays and s is an
  // integer number (to be chosen). Then, it evaluates 4 * N + 3 on uint32_t
  // operands so that N must be positive and, to prevent overflow,
  //   4 * N + 3 <= maxU32 <=> N <= (maxU32 - 3) / 4.
  // Therefore, we must have  0 <= N_U + K <= (maxU32 - 3) / 4 or, in other
  // words, N_U must be in [minDays, maxDays] = [-K, (maxU32 - 3) / 4 - K].
  // Notice that this interval moves cycleInDays positions to the left when
  // s is incremented. We chose s to get the interval's mid-point as close
  // as possible to 0. For this, we wish to have:
  //   K ~= (maxU32 - 3) / 4 - K <=> 2 * K ~= (maxU32 - 3) / 4 <=>
  //   K ~= (maxU32 - 3) / 8 <=>
  //   rataDie1970Jan1 + s * cycleInDays ~= (maxU32 - 3) / 8 <=>
  //   s ~= ((maxU32 - 3) / 8 - rataDie1970Jan1) / cycleInDays ~= 3669.8.
  // Therefore, we chose s = 3670. The shift and correction constants
  // (see [1]) are then:
  constexpr uint32_t s = 3670;
  constexpr uint32_t K = rataDie1970Jan1 + s * cycleInDays;
  constexpr uint32_t L = s * cycleInYears;

  // [minDays, maxDays] correspond to a date range from -1'468'000/Mar/01 to
  // 1'471'805/Jun/05.
  constexpr int32_t minDays = -int32_t(K);
  constexpr int32_t maxDays = (maxU32 - 3) / 4 - K;
  static_assert(minDays == -536'895'458, "Wrong calculation of minDays or K.");
  static_assert(maxDays == 536'846'365, "Wrong calculation of maxDays or K.");

  // These are hard limits for the algorithm and far greater than the
  // range [-8.64e15, 8.64e15] required by ES2024 21.4.1.1. Callers must
  // ensure this function is not called out of the hard limits and,
  // preferably, not outside the ES2024 limits.
  constexpr int64_t minTime = minDays * int64_t(msPerDay);
  [[maybe_unused]] constexpr int64_t maxTime = maxDays * int64_t(msPerDay);
  MOZ_ASSERT(minTime <= time && time <= maxTime);

  // Since time is the number of milliseconds since the epoch, 1970/Jan/01,
  // one might expect N_U = time / uint64_t(msPerDay) is the number of days
  // since epoch. There's a catch tough. Consider, for instance, half day
  // before the epoch, that is, t = -0.5 * msPerDay. This falls on
  // 1969/Dec/31 and should correspond to N_U = -1 but the above gives
  // N_U = 0. Indeed, t / msPerDay = -0.5 but integer division truncates
  // towards 0 (C++ [expr.mul]/4) and not towards -infinity as needed, so
  // that time / uint64_t(msPerDay) = 0. To workaround this issue we perform
  // the division on positive operands so that truncations towards 0 and
  // -infinity are equivalent. For this, set u = time - minTime, which is
  // positive as asserted above. Then, perform the division u / msPerDay and
  // to the result add minTime / msPerDay = minDays to cancel the
  // subtraction of minTime.
  const uint64_t u = uint64_t(time - minTime);
  const int32_t N_U = int32_t(u / uint64_t(msPerDay)) + minDays;
  MOZ_ASSERT(minDays <= N_U && N_U <= maxDays);

  const uint32_t N = uint32_t(N_U) + K;

  // Some magic numbers have been explained above but, unfortunately,
  // others with no precise interpretation do appear. They mostly come
  // from numerical approximations of Euclidean affine functions (see [1])
  // which are faster for the CPU to calculate. Unfortunately, no compiler
  // can do these optimizations.

  // Century C and year of the century N_C:
  const uint32_t N_1 = 4 * N + 3;
  const uint32_t C = N_1 / 146097;
  const uint32_t N_C = N_1 % 146097 / 4;

  // Year of the century Z and day of the year N_Y:
  const uint32_t N_2 = 4 * N_C + 3;
  const uint64_t P_2 = uint64_t(2939745) * N_2;
  const uint32_t Z = uint32_t(P_2 / 4294967296);
  const uint32_t N_Y = uint32_t(P_2 % 4294967296) / 2939745 / 4;

  // Year Y:
  const uint32_t Y = 100 * C + Z;

  // Month M and day D.
  // The expression for N_3 has been adapted to account for the difference
  // between month numbers in ES5 15.9.1.4 (from 0 to 11) and [1] (from 1
  // to 12). This is done by subtracting 65536 from the original
  // expression so that M decreases by 1 and so does M_G further down.
  const uint32_t N_3 = 2141 * N_Y + 132377;  // 132377 = 197913 - 65536
  const uint32_t M = N_3 / 65536;
  const uint32_t D = N_3 % 65536 / 2141;

  // Map from Computational to Gregorian calendar. Notice also the year
  // correction and the type change and that Jan/01 is day 306 of the
  // Computational calendar, cf. Table 1. [1]
  constexpr uint32_t daysFromMar01ToJan01 = 306;
  const uint32_t J = N_Y >= daysFromMar01ToJan01;
  const int32_t Y_G = int32_t((Y - L) + J);
  const int32_t M_G = int32_t(J ? M - 12 : M);
  const int32_t D_G = int32_t(D + 1);

  return {Y_G, M_G, D_G};
}

/**
 * 21.4.1.8 YearFromTime ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static int32_t YearFromTime(int64_t t) {
  MOZ_ASSERT(IsLocalTimeValue(t));
  return ToYearMonthDay(t).year;
}

/**
 * 21.4.1.9 DayWithinYear ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static double DayWithinYear(int64_t t, double year) {
  MOZ_ASSERT(::YearFromTime(t) == year);
  return Day(t) - ::DayFromYear(year);
}

/**
 * 21.4.1.11 MonthFromTime ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static int32_t MonthFromTime(int64_t t) {
  MOZ_ASSERT(IsLocalTimeValue(t));
  return ToYearMonthDay(t).month;
}

/**
 * 21.4.1.12 DateFromTime ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static int32_t DateFromTime(int64_t t) {
  MOZ_ASSERT(IsLocalTimeValue(t));
  return ToYearMonthDay(t).day;
}

/**
 * 21.4.1.13 WeekDay ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static int32_t WeekDay(int64_t t) {
  MOZ_ASSERT(IsLocalTimeValue(t));

  int32_t result = (Day(t) + 4) % 7;
  if (result < 0) {
    result += 7;
  }
  return result;
}

static inline int DayFromMonth(int month, bool isLeapYear) {
  /*
   * The following array contains the day of year for the first day of
   * each month, where index 0 is January, and day 0 is January 1.
   */
  static const int firstDayOfMonth[2][13] = {
      {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
      {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};

  MOZ_ASSERT(0 <= month && month <= 12);
  return firstDayOfMonth[isLeapYear][month];
}

template <typename T>
static inline int DayFromMonth(T month, bool isLeapYear) = delete;

/**
 * 21.4.1.28 MakeDay ( year, month, date )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static double MakeDay(double year, double month, double date) {
  // Step 1.
  if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date)) {
    return GenericNaN();
  }

  // Steps 2-4.
  double y = ToInteger(year);
  double m = ToInteger(month);
  double dt = ToInteger(date);

  static constexpr int32_t maxYears = 1'000'000;
  static constexpr int32_t maxMonths = 1'000'000 * 12;
  static constexpr int32_t maxDate = 100'000'000;

  // Use integer math if possible, because it avoids some notoriously slow
  // functions like `fmod`.
  if (MOZ_LIKELY(std::abs(y) <= maxYears && std::abs(m) <= maxMonths &&
                 std::abs(dt) <= maxDate)) {
    int32_t year = mozilla::AssertedCast<int32_t>(y);
    int32_t month = mozilla::AssertedCast<int32_t>(m);
    int32_t date = mozilla::AssertedCast<int32_t>(dt);

    static_assert(maxMonths % 12 == 0,
                  "maxYearMonths expects maxMonths is divisible by 12");

    static constexpr int32_t maxYearMonths = maxYears + (maxMonths / 12);
    static constexpr int32_t maxYearDay = DayFromYear(maxYearMonths);
    static constexpr int32_t minYearDay = DayFromYear(-maxYearMonths);
    static constexpr int32_t daysInLeapYear = 366;
    static constexpr int32_t maxDay = maxYearDay + daysInLeapYear + maxDate;
    static constexpr int32_t minDay = minYearDay + daysInLeapYear - maxDate;

    static_assert(maxYearMonths == 2'000'000);
    static_assert(maxYearDay == 729'765'472);
    static_assert(minYearDay == -731'204'528);
    static_assert(maxDay == maxYearDay + daysInLeapYear + maxDate);
    static_assert(minDay == minYearDay + daysInLeapYear - maxDate);

    // Step 5.
    int32_t ym = year + FloorDiv(month, 12);
    MOZ_ASSERT(std::abs(ym) <= maxYearMonths);

    // Step 6. (Implicit)

    // Step 7.
    int32_t mn = PositiveModulo(month, 12);

    // Step 8.
    bool leap = IsLeapYear(ym);
    int32_t yearday = ::DayFromYear(ym);
    int32_t monthday = DayFromMonth(mn, leap);
    MOZ_ASSERT(minYearDay <= yearday && yearday <= maxYearDay);

    // Step 9.
    int32_t day = yearday + monthday + date - 1;
    MOZ_ASSERT(minDay <= day && day <= maxDay);
    return day;
  }

  // Step 5.
  double ym = y + floor(m / 12);

  // Step 6.
  if (!std::isfinite(ym)) {
    return GenericNaN();
  }

  // Step 7.
  int mn = int(PositiveModulo(m, 12));

  // Step 8.
  bool leap = IsLeapYear(ym);
  double yearday = floor(TimeFromYear(ym) / msPerDay);
  double monthday = DayFromMonth(mn, leap);

  // Step 9.
  return yearday + monthday + dt - 1;
}

/**
 * 21.4.1.29 MakeDate ( day, time )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static inline double MakeDate(double day, double time) {
  // Step 1.
  if (!std::isfinite(day) || !std::isfinite(time)) {
    return GenericNaN();
  }

  // Steps 2-4.
  return day * msPerDay + time;
}

JS_PUBLIC_API double JS::MakeDate(double year, unsigned month, unsigned day) {
  MOZ_ASSERT(month <= 11);
  MOZ_ASSERT(day >= 1 && day <= 31);

  return ::MakeDate(MakeDay(year, month, day), 0);
}

JS_PUBLIC_API double JS::MakeDate(double year, unsigned month, unsigned day,
                                  double time) {
  MOZ_ASSERT(month <= 11);
  MOZ_ASSERT(day >= 1 && day <= 31);

  return ::MakeDate(MakeDay(year, month, day), time);
}

JS_PUBLIC_API double JS::YearFromTime(double time) {
  const auto clipped = TimeClip(time);
  if (!clipped.isValid()) {
    return GenericNaN();
  }
  int64_t tv;
  MOZ_ALWAYS_TRUE(mozilla::NumberEqualsInt64(clipped.toDouble(), &tv));
  return ::YearFromTime(tv);
}

JS_PUBLIC_API double JS::MonthFromTime(double time) {
  const auto clipped = TimeClip(time);
  if (!clipped.isValid()) {
    return GenericNaN();
  }
  int64_t tv;
  MOZ_ALWAYS_TRUE(mozilla::NumberEqualsInt64(clipped.toDouble(), &tv));
  return ::MonthFromTime(tv);
}

JS_PUBLIC_API double JS::DayFromTime(double time) {
  const auto clipped = TimeClip(time);
  if (!clipped.isValid()) {
    return GenericNaN();
  }
  int64_t tv;
  MOZ_ALWAYS_TRUE(mozilla::NumberEqualsInt64(clipped.toDouble(), &tv));
  return DateFromTime(tv);
}

JS_PUBLIC_API double JS::DayFromYear(double year) {
  return ::DayFromYear(year);
}

JS_PUBLIC_API double JS::DayWithinYear(double time, double year) {
  const auto clipped = TimeClip(time);
  if (!clipped.isValid()) {
    return GenericNaN();
  }
  int64_t tv;
  MOZ_ALWAYS_TRUE(mozilla::NumberEqualsInt64(clipped.toDouble(), &tv));
  return ::DayWithinYear(tv, year);
}

JS_PUBLIC_API void JS::SetReduceMicrosecondTimePrecisionCallback(
    JS::ReduceMicrosecondTimePrecisionCallback callback) {
  sReduceMicrosecondTimePrecisionCallback = callback;
}

JS_PUBLIC_API JS::ReduceMicrosecondTimePrecisionCallback
JS::GetReduceMicrosecondTimePrecisionCallback() {
  return sReduceMicrosecondTimePrecisionCallback;
}

JS_PUBLIC_API void JS::SetTimeResolutionUsec(uint32_t resolution, bool jitter) {
  sResolutionUsec = resolution;
  sJitter = jitter;
}

#if JS_HAS_INTL_API
int32_t DateTimeHelper::getTimeZoneOffset(DateTimeInfo::ForceUTC forceUTC,
                                          int64_t epochMilliseconds,
                                          DateTimeInfo::TimeZoneOffset offset) {
  MOZ_ASSERT_IF(offset == DateTimeInfo::TimeZoneOffset::UTC,
                IsTimeValue(epochMilliseconds));
  MOZ_ASSERT_IF(offset == DateTimeInfo::TimeZoneOffset::Local,
                IsLocalTimeValue(epochMilliseconds));

  return DateTimeInfo::getOffsetMilliseconds(forceUTC, epochMilliseconds,
                                             offset);
}
#else
/*
 * Find a year for which any given date will fall on the same weekday.
 *
 * This function should be used with caution when used other than
 * for determining DST; it hasn't been proven not to produce an
 * incorrect year for times near year boundaries.
 */
int DateTimeHelper::equivalentYearForDST(int year) {
  /*
   * Years and leap years on which Jan 1 is a Sunday, Monday, etc.
   *
   * yearStartingWith[0][i] is an example non-leap year where
   * Jan 1 appears on Sunday (i == 0), Monday (i == 1), etc.
   *
   * yearStartingWith[1][i] is an example leap year where
   * Jan 1 appears on Sunday (i == 0), Monday (i == 1), etc.
   *
   * Keep two different mappings, one for past years (< 1970), and a
   * different one for future years (> 2037).
   */
  static const int pastYearStartingWith[2][7] = {
      {1978, 1973, 1974, 1975, 1981, 1971, 1977},
      {1984, 1996, 1980, 1992, 1976, 1988, 1972}};
  static const int futureYearStartingWith[2][7] = {
      {2034, 2035, 2030, 2031, 2037, 2027, 2033},
      {2012, 2024, 2036, 2020, 2032, 2016, 2028}};

  int day = int(::DayFromYear(year) + 4) % 7;
  if (day < 0) {
    day += 7;
  }

  const auto& yearStartingWith =
      year < 1970 ? pastYearStartingWith : futureYearStartingWith;
  return yearStartingWith[IsLeapYear(year)][day];
}

// Return true if |t| is representable as a 32-bit time_t variable, that means
// the year is in [1970, 2038).
bool DateTimeHelper::isRepresentableAsTime32(int64_t t) {
  return 0 <= t && t < 2145916800000;
}

/* ES5 15.9.1.8. */
int32_t DateTimeHelper::daylightSavingTA(DateTimeInfo::ForceUTC forceUTC,
                                         int64_t t) {
  /*
   * If earlier than 1970 or after 2038, potentially beyond the ken of
   * many OSes, map it to an equivalent year before asking.
   */
  if (!isRepresentableAsTime32(t)) {
    auto [year, month, day] = ToYearMonthDay(t);

    int equivalentYear = equivalentYearForDST(year);
    double equivalentDay = MakeDay(equivalentYear, month, day);
    double equivalentDate = MakeDate(equivalentDay, TimeWithinDay(t));

    MOZ_ALWAYS_TRUE(mozilla::NumberEqualsInt64(equivalentDate, &t));
  }

  return DateTimeInfo::getDSTOffsetMilliseconds(forceUTC, t);
}

int32_t DateTimeHelper::adjustTime(DateTimeInfo::ForceUTC forceUTC,
                                   int64_t date) {
  int32_t localTZA = DateTimeInfo::localTZA(forceUTC);
  int32_t t = daylightSavingTA(forceUTC, date) + localTZA;
  return (localTZA >= 0) ? (t % msPerDay) : -((msPerDay - t) % msPerDay);
}

int32_t DateTimeHelper::getTimeZoneOffset(DateTimeInfo::ForceUTC forceUTC,
                                          int64_t epochMilliseconds,
                                          DateTimeInfo::TimeZoneOffset offset) {
  MOZ_ASSERT_IF(offset == DateTimeInfo::TimeZoneOffset::UTC,
                IsTimeValue(epochMilliseconds));
  MOZ_ASSERT_IF(offset == DateTimeInfo::TimeZoneOffset::Local,
                IsLocalTimeValue(epochMilliseconds));

  if (offset == DateTimeInfo::TimeZoneOffset::UTC) {
    return adjustTime(forceUTC, epochMilliseconds);
  }

  // Following the ES2017 specification creates undesirable results at DST
  // transitions. For example when transitioning from PST to PDT,
  // |new Date(2016,2,13,2,0,0).toTimeString()| returns the string value
  // "01:00:00 GMT-0800 (PST)" instead of "03:00:00 GMT-0700 (PDT)". Follow
  // V8 and subtract one hour before computing the offset.
  // Spec bug: https://bugs.ecmascript.org/show_bug.cgi?id=4007

  return adjustTime(forceUTC, epochMilliseconds -
                                  int64_t(DateTimeInfo::localTZA(forceUTC)) -
                                  int64_t(msPerHour));
}
#endif /* JS_HAS_INTL_API */

/**
 * 21.4.1.25 LocalTime ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static int64_t LocalTime(DateTimeInfo::ForceUTC forceUTC, double t) {
  MOZ_ASSERT(std::isfinite(t));
  MOZ_ASSERT(IsTimeValue(t));

  // Steps 1-4.
  int32_t offsetMs = DateTimeHelper::getTimeZoneOffset(
      forceUTC, static_cast<int64_t>(t), DateTimeInfo::TimeZoneOffset::UTC);
  MOZ_ASSERT(std::abs(offsetMs) < msPerDay);

  // Step 5.
  return static_cast<int64_t>(t) + offsetMs;
}

/**
 * 21.4.1.26 UTC ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static double UTC(DateTimeInfo::ForceUTC forceUTC, double t) {
  // Step 1.
  if (!std::isfinite(t)) {
    return GenericNaN();
  }

  // Return early when |t| is outside the valid local time value limits.
  if (!IsLocalTimeValue(t)) {
    return GenericNaN();
  }

  // Steps 2-5.
  int32_t offsetMs = DateTimeHelper::getTimeZoneOffset(
      forceUTC, static_cast<int64_t>(t), DateTimeInfo::TimeZoneOffset::Local);
  MOZ_ASSERT(std::abs(offsetMs) < msPerDay);

  // Step 6.
  return static_cast<double>(static_cast<int64_t>(t) - offsetMs);
}

/**
 * 21.4.1.14 HourFromTime ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static int32_t HourFromTime(int64_t t) {
  MOZ_ASSERT(IsLocalTimeValue(t));
  return PositiveModulo(FloorDiv(t, msPerHour), HoursPerDay);
}

/**
 * 21.4.1.15 MinFromTime ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static int32_t MinFromTime(int64_t t) {
  MOZ_ASSERT(IsLocalTimeValue(t));
  return PositiveModulo(FloorDiv(t, msPerMinute), MinutesPerHour);
}

/**
 * 21.4.1.16 SecFromTime ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static int32_t SecFromTime(int64_t t) {
  MOZ_ASSERT(IsLocalTimeValue(t));
  return PositiveModulo(FloorDiv(t, msPerSecond), SecondsPerMinute);
}

/**
 * 21.4.1.17 msFromTime ( t )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static int32_t msFromTime(int64_t t) {
  MOZ_ASSERT(IsLocalTimeValue(t));
  return PositiveModulo(t, msPerSecond);
}

HourMinuteSecond js::ToHourMinuteSecond(int64_t epochMilliseconds) {
  MOZ_ASSERT(IsLocalTimeValue(epochMilliseconds));

  int32_t hour = HourFromTime(epochMilliseconds);
  MOZ_ASSERT(0 <= hour && hour < HoursPerDay);

  int32_t minute = MinFromTime(epochMilliseconds);
  MOZ_ASSERT(0 <= minute && minute < MinutesPerHour);

  int32_t second = SecFromTime(epochMilliseconds);
  MOZ_ASSERT(0 <= minute && minute < SecondsPerMinute);

  return {hour, minute, second};
}

/**
 * 21.4.1.27 MakeTime ( hour, min, sec, ms )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static double MakeTime(double hour, double min, double sec, double ms) {
  // Step 1.
  if (!std::isfinite(hour) || !std::isfinite(min) || !std::isfinite(sec) ||
      !std::isfinite(ms)) {
    return GenericNaN();
  }

  // Step 2.
  double h = ToInteger(hour);

  // Step 3.
  double m = ToInteger(min);

  // Step 4.
  double s = ToInteger(sec);

  // Step 5.
  double milli = ToInteger(ms);

  // Step 6.
  return h * msPerHour + m * msPerMinute + s * msPerSecond + milli;
}

/**
 * 21.4.1.30 MakeFullYear ( year )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static double MakeFullYear(double year) {
  // Step 1.
  if (std::isnan(year)) {
    return year;
  }

  // Step 2.
  double truncated = ToInteger(year);

  // Step 3.
  if (0 <= truncated && truncated <= 99) {
    return 1900 + truncated;
  }

  // Step 4.
  return truncated;
}

/**
 * end of ECMA 'support' functions
 */

/**
 * 21.4.3.4 Date.UTC ( year [ , month [ , date [ , hours [ , minutes [ , seconds
 * [ , ms ] ] ] ] ] ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_UTC(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date", "UTC");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  double y;
  if (!ToNumber(cx, args.get(0), &y)) {
    return false;
  }

  // Step 2.
  double m;
  if (args.length() >= 2) {
    if (!ToNumber(cx, args[1], &m)) {
      return false;
    }
  } else {
    m = 0;
  }

  // Step 3.
  double dt;
  if (args.length() >= 3) {
    if (!ToNumber(cx, args[2], &dt)) {
      return false;
    }
  } else {
    dt = 1;
  }

  // Step 4.
  double h;
  if (args.length() >= 4) {
    if (!ToNumber(cx, args[3], &h)) {
      return false;
    }
  } else {
    h = 0;
  }

  // Step 5.
  double min;
  if (args.length() >= 5) {
    if (!ToNumber(cx, args[4], &min)) {
      return false;
    }
  } else {
    min = 0;
  }

  // Step 6.
  double s;
  if (args.length() >= 6) {
    if (!ToNumber(cx, args[5], &s)) {
      return false;
    }
  } else {
    s = 0;
  }

  // Step 7.
  double milli;
  if (args.length() >= 7) {
    if (!ToNumber(cx, args[6], &milli)) {
      return false;
    }
  } else {
    milli = 0;
  }

  // Step 8.
  double yr = MakeFullYear(y);

  // Step 9.
  ClippedTime time =
      TimeClip(MakeDate(MakeDay(yr, m, dt), MakeTime(h, min, s, milli)));
  args.rval().set(TimeValue(time));
  return true;
}

/*
 * Read and convert decimal digits from s[*i] into *result
 * while *i < limit.
 *
 * Succeed if any digits are converted. Advance *i only
 * as digits are consumed.
 */
template <typename CharT>
static bool ParseDigits(size_t* result, const CharT* s, size_t* i,
                        size_t limit) {
  size_t init = *i;
  *result = 0;
  while (*i < limit && ('0' <= s[*i] && s[*i] <= '9')) {
    *result *= 10;
    *result += (s[*i] - '0');
    ++(*i);
  }
  return *i != init;
}

/*
 * Read and convert decimal digits to the right of a decimal point,
 * representing a fractional integer, from s[*i] into *result
 * while *i < limit, up to 3 digits. Consumes any digits beyond 3
 * without affecting the result.
 *
 * Succeed if any digits are converted. Advance *i only
 * as digits are consumed.
 */
template <typename CharT>
static bool ParseFractional(int* result, const CharT* s, size_t* i,
                            size_t limit) {
  int factor = 100;
  size_t init = *i;
  *result = 0;
  for (; *i < limit && ('0' <= s[*i] && s[*i] <= '9'); ++(*i)) {
    if (*i - init >= 3) {
      // If we're past 3 digits, do nothing with it, but continue to
      // consume the remainder of the digits
      continue;
    }
    *result += (s[*i] - '0') * factor;
    factor /= 10;
  }
  return *i != init;
}

/*
 * Read and convert exactly n decimal digits from s[*i]
 * to s[min(*i+n,limit)] into *result.
 *
 * Succeed if exactly n digits are converted. Advance *i only
 * on success.
 */
template <typename CharT>
static bool ParseDigitsN(size_t n, size_t* result, const CharT* s, size_t* i,
                         size_t limit) {
  size_t init = *i;

  if (ParseDigits(result, s, i, std::min(limit, init + n))) {
    return (*i - init) == n;
  }

  *i = init;
  return false;
}

/*
 * Read and convert n or less decimal digits from s[*i]
 * to s[min(*i+n,limit)] into *result.
 *
 * Succeed only if greater than zero but less than or equal to n digits are
 * converted. Advance *i only on success.
 */
template <typename CharT>
static bool ParseDigitsNOrLess(size_t n, size_t* result, const CharT* s,
                               size_t* i, size_t limit) {
  size_t init = *i;

  if (ParseDigits(result, s, i, std::min(limit, init + n))) {
    return ((*i - init) > 0) && ((*i - init) <= n);
  }

  *i = init;
  return false;
}

/*
 * Parse a string according to the formats specified in the standard:
 *
 * https://tc39.es/ecma262/#sec-date-time-string-format
 * https://tc39.es/ecma262/#sec-expanded-years
 *
 * These formats are based upon a simplification of the ISO 8601 Extended
 * Format. As per the spec omitted month and day values are defaulted to '01',
 * omitted HH:mm:ss values are defaulted to '00' and an omitted sss field is
 * defaulted to '000'.
 *
 * For cross compatibility we allow the following extensions.
 *
 * These are:
 *
 *   One or more decimal digits for milliseconds:
 *     The specification requires exactly three decimal digits for
 *     the fractional part but we allow for one or more digits.
 *
 *   Time zone specifier without ':':
 *     We allow the time zone to be specified without a ':' character.
 *     E.g. "T19:00:00+0700" is equivalent to "T19:00:00+07:00".
 *
 * Date part:
 *
 *  Year:
 *     YYYY (eg 1997)
 *
 *  Year and month:
 *     YYYY-MM (eg 1997-07)
 *
 *  Complete date:
 *     YYYY-MM-DD (eg 1997-07-16)
 *
 * Time part:
 *
 *  Hours and minutes:
 *     Thh:mmTZD (eg T19:20+01:00)
 *
 *  Hours, minutes and seconds:
 *     Thh:mm:ssTZD (eg T19:20:30+01:00)
 *
 *  Hours, minutes, seconds and a decimal fraction of a second:
 *     Thh:mm:ss.sssTZD (eg T19:20:30.45+01:00)
 *
 * where:
 *
 *   YYYY = four-digit year or six digit year as +YYYYYY or -YYYYYY
 *   MM   = two-digit month (01=January, etc.)
 *   DD   = two-digit day of month (01 through 31)
 *   hh   = two digits of hour (00 through 24) (am/pm NOT allowed)
 *   mm   = two digits of minute (00 through 59)
 *   ss   = two digits of second (00 through 59)
 *   sss  = one or more digits representing a decimal fraction of a second
 *   TZD  = time zone designator (Z or +hh:mm or -hh:mm or missing for local)
 */
template <typename CharT>
static bool ParseISOStyleDate(DateTimeInfo::ForceUTC forceUTC, const CharT* s,
                              size_t length, ClippedTime* result) {
  size_t i = 0;
  int tzMul = 1;
  int dateMul = 1;
  size_t year = 1970;
  size_t month = 1;
  size_t day = 1;
  size_t hour = 0;
  size_t min = 0;
  size_t sec = 0;
  int msec = 0;
  bool isLocalTime = false;
  size_t tzHour = 0;
  size_t tzMin = 0;

#define PEEK(ch) (i < length && s[i] == ch)

#define NEED(ch)                   \
  if (i >= length || s[i] != ch) { \
    return false;                  \
  } else {                         \
    ++i;                           \
  }

#define DONE_DATE_UNLESS(ch)       \
  if (i >= length || s[i] != ch) { \
    goto done_date;                \
  } else {                         \
    ++i;                           \
  }

#define DONE_UNLESS(ch)            \
  if (i >= length || s[i] != ch) { \
    goto done;                     \
  } else {                         \
    ++i;                           \
  }

#define NEED_NDIGITS(n, field)                   \
  if (!ParseDigitsN(n, &field, s, &i, length)) { \
    return false;                                \
  }

  if (PEEK('+') || PEEK('-')) {
    if (PEEK('-')) {
      dateMul = -1;
    }
    ++i;
    NEED_NDIGITS(6, year);

    // https://tc39.es/ecma262/#sec-expanded-years
    // -000000 is not a valid expanded year.
    if (year == 0 && dateMul == -1) {
      return false;
    }
  } else {
    NEED_NDIGITS(4, year);
  }
  DONE_DATE_UNLESS('-');
  NEED_NDIGITS(2, month);
  DONE_DATE_UNLESS('-');
  NEED_NDIGITS(2, day);

done_date:
  if (PEEK('T')) {
    ++i;
  } else {
    goto done;
  }

  NEED_NDIGITS(2, hour);
  NEED(':');
  NEED_NDIGITS(2, min);

  if (PEEK(':')) {
    ++i;
    NEED_NDIGITS(2, sec);
    if (PEEK('.')) {
      ++i;
      if (!ParseFractional(&msec, s, &i, length)) {
        return false;
      }
    }
  }

  if (PEEK('Z')) {
    ++i;
  } else if (PEEK('+') || PEEK('-')) {
    if (PEEK('-')) {
      tzMul = -1;
    }
    ++i;
    NEED_NDIGITS(2, tzHour);
    /*
     * Non-standard extension to the ISO date format (permitted by ES5):
     * allow "-0700" as a time zone offset, not just "-07:00".
     */
    if (PEEK(':')) {
      ++i;
    }
    NEED_NDIGITS(2, tzMin);
  } else {
    isLocalTime = true;
  }

done:
  if (year > 275943  // ceil(1e8/365) + 1970
      || month == 0 || month > 12 || day == 0 || day > 31 || hour > 24 ||
      (hour == 24 && (min > 0 || sec > 0 || msec > 0)) || min > 59 ||
      sec > 59 || tzHour > 23 || tzMin > 59) {
    return false;
  }

  if (i != length) {
    return false;
  }

  month -= 1; /* convert month to 0-based */

  double date = MakeDate(MakeDay(dateMul * int32_t(year), month, day),
                         MakeTime(hour, min, sec, msec));

  if (isLocalTime) {
    date = UTC(forceUTC, date);
  } else {
    date -=
        tzMul * (int32_t(tzHour) * msPerHour + int32_t(tzMin) * msPerMinute);
  }

  *result = TimeClip(date);
  return NumbersAreIdentical(date, result->toDouble());

#undef PEEK
#undef NEED
#undef DONE_UNLESS
#undef NEED_NDIGITS
}

/**
 * Non-ISO years < 100 get fixed up, to allow 2-digit year formats.
 * year < 50 becomes 2000-2049, 50-99 becomes 1950-1999.
 */
static int FixupYear(int year) {
  if (year < 50) {
    year += 2000;
  } else if (year >= 50 && year < 100) {
    year += 1900;
  }
  return year;
}

template <typename CharT>
static bool MatchesKeyword(const CharT* s, size_t len, const char* keyword) {
  while (len > 0) {
    MOZ_ASSERT(IsAsciiAlpha(*s));
    MOZ_ASSERT(IsAsciiLowercaseAlpha(*keyword) || *keyword == '\0');

    if (unicode::ToLowerCase(static_cast<Latin1Char>(*s)) != *keyword) {
      return false;
    }

    ++s, ++keyword;
    --len;
  }

  return *keyword == '\0';
}

static constexpr const char* const month_prefixes[] = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec",
};

/**
 * Given a string s of length >= 3, checks if it begins,
 * case-insensitive, with the given lower case prefix.
 */
template <typename CharT>
static bool StartsWithMonthPrefix(const CharT* s, const char* prefix) {
  MOZ_ASSERT(strlen(prefix) == 3);

  for (size_t i = 0; i < 3; ++i) {
    MOZ_ASSERT(IsAsciiAlpha(*s));
    MOZ_ASSERT(IsAsciiLowercaseAlpha(*prefix));

    if (unicode::ToLowerCase(static_cast<Latin1Char>(*s)) != *prefix) {
      return false;
    }

    ++s, ++prefix;
  }

  return true;
}

template <typename CharT>
static bool IsMonthName(const CharT* s, size_t len, int* mon) {
  // Month abbreviations < 3 chars are not accepted.
  if (len < 3) {
    return false;
  }

  for (size_t m = 0; m < std::size(month_prefixes); ++m) {
    if (StartsWithMonthPrefix(s, month_prefixes[m])) {
      // Use numeric value.
      *mon = m + 1;
      return true;
    }
  }

  return false;
}

/*
 * Try to parse the following date formats:
 *   dd-MMM-yyyy
 *   dd-MMM-yy
 *   MMM-dd-yyyy
 *   MMM-dd-yy
 *   yyyy-MMM-dd
 *   yy-MMM-dd
 *
 * Returns true and fills all out parameters when successfully parsed
 * dashed-date.  Otherwise returns false and leaves out parameters untouched.
 */
template <typename CharT>
static bool TryParseDashedDatePrefix(const CharT* s, size_t length,
                                     size_t* indexOut, int* yearOut,
                                     int* monOut, int* mdayOut) {
  size_t i = *indexOut;

  size_t pre = i;
  size_t mday;
  if (!ParseDigitsNOrLess(6, &mday, s, &i, length)) {
    return false;
  }
  size_t mdayDigits = i - pre;

  if (i >= length || s[i] != '-') {
    return false;
  }
  ++i;

  int mon = 0;
  if (*monOut == -1) {
    // If month wasn't already set by ParseDate, it must be in the middle of
    // this format, let's look for it
    size_t start = i;
    for (; i < length; i++) {
      if (!IsAsciiAlpha(s[i])) {
        break;
      }
    }

    if (!IsMonthName(s + start, i - start, &mon)) {
      return false;
    }

    if (i >= length || s[i] != '-') {
      return false;
    }
    ++i;
  }

  pre = i;
  size_t year;
  if (!ParseDigitsNOrLess(6, &year, s, &i, length)) {
    return false;
  }
  size_t yearDigits = i - pre;

  if (i < length && IsAsciiDigit(s[i])) {
    return false;
  }

  // Swap the mday and year if the year wasn't specified in full.
  if (mday > 31 && year <= 31 && yearDigits < 4) {
    std::swap(mday, year);
    std::swap(mdayDigits, yearDigits);
  }

  if (mday > 31 || mdayDigits > 2) {
    return false;
  }

  year = FixupYear(year);

  *indexOut = i;
  *yearOut = year;
  if (*monOut == -1) {
    *monOut = mon;
  }
  *mdayOut = mday;
  return true;
}

/*
 * Try to parse dates in the style of YYYY-MM-DD which do not conform to
 * the formal standard from ParseISOStyleDate. This includes cases such as
 *
 *  - Year does not have 4 digits
 *  - Month or mday has 1 digit
 *  - Space in between date and time, rather than a 'T'
 *
 * Regarding the last case, this function only parses out the date, returning
 * to ParseDate to finish parsing the time and timezone, if present.
 *
 * Returns true and fills all out parameters when successfully parsed
 * dashed-date.  Otherwise returns false and leaves out parameters untouched.
 */
template <typename CharT>
static bool TryParseDashedNumericDatePrefix(const CharT* s, size_t length,
                                            size_t* indexOut, int* yearOut,
                                            int* monOut, int* mdayOut) {
  size_t i = *indexOut;

  size_t first;
  if (!ParseDigitsNOrLess(6, &first, s, &i, length)) {
    return false;
  }

  if (i >= length || s[i] != '-') {
    return false;
  }
  ++i;

  size_t second;
  if (!ParseDigitsNOrLess(2, &second, s, &i, length)) {
    return false;
  }

  if (i >= length || s[i] != '-') {
    return false;
  }
  ++i;

  size_t third;
  if (!ParseDigitsNOrLess(6, &third, s, &i, length)) {
    return false;
  }

  int year;
  int mon = -1;
  int mday = -1;

  // 1 or 2 digits for the first number is tricky; 1-12 means it's a month, 0 or
  // >31 means it's a year, and 13-31 is invalid due to ambiguity.
  if (first >= 1 && first <= 12) {
    mon = first;
  } else if (first == 0 || first > 31) {
    year = first;
  } else {
    return false;
  }

  if (mon < 0) {
    // If month hasn't been set yet, it's definitely the 2nd number
    mon = second;
  } else {
    // If it has, the next number is the mday
    mday = second;
  }

  if (mday < 0) {
    // The third number is probably the mday...
    mday = third;
  } else {
    // But otherwise, it's the year
    year = third;
  }

  if (mon < 1 || mon > 12 || mday < 1 || mday > 31) {
    return false;
  }

  year = FixupYear(year);

  *indexOut = i;
  *yearOut = year;
  *monOut = mon;
  *mdayOut = mday;
  return true;
}

struct CharsAndAction {
  const char* chars;
  int action;
};

static constexpr CharsAndAction keywords[] = {
    // clang-format off
  // AM/PM
  { "am", -1 },
  { "pm", -2 },
  // Time zone abbreviations.
  { "gmt", 10000 + 0 },
  { "z", 10000 + 0 },
  { "ut", 10000 + 0 },
  { "utc", 10000 + 0 },
  { "est", 10000 + 5 * 60 },
  { "edt", 10000 + 4 * 60 },
  { "cst", 10000 + 6 * 60 },
  { "cdt", 10000 + 5 * 60 },
  { "mst", 10000 + 7 * 60 },
  { "mdt", 10000 + 6 * 60 },
  { "pst", 10000 + 8 * 60 },
  { "pdt", 10000 + 7 * 60 },
    // clang-format on
};

template <size_t N>
static constexpr size_t MinKeywordLength(const CharsAndAction (&keywords)[N]) {
  size_t min = size_t(-1);
  for (const CharsAndAction& keyword : keywords) {
    min = std::min(min, std::char_traits<char>::length(keyword.chars));
  }
  return min;
}

template <typename CharT>
static bool ParseDate(JSContext* cx, DateTimeInfo::ForceUTC forceUTC,
                      const CharT* s, size_t length, ClippedTime* result) {
  if (length == 0) {
    return false;
  }

  if (ParseISOStyleDate(forceUTC, s, length, result)) {
    return true;
  }

  // Collect telemetry on how often Date.parse enters implementation defined
  // code. This can be removed in the future, see Bug 1944630.
  cx->runtime()->setUseCounter(cx->global(), JSUseCounter::DATEPARSE_IMPL_DEF);

  size_t index = 0;
  int mon = -1;
  bool seenMonthName = false;

  // Before we begin, we need to scrub any words from the beginning of the
  // string up to the first number, recording the month if we encounter it
  for (; index < length; index++) {
    int c = s[index];

    if (strchr(" ,.-/", c)) {
      continue;
    }
    if (!IsAsciiAlpha(c)) {
      break;
    }

    size_t start = index;
    index++;
    for (; index < length; index++) {
      if (!IsAsciiAlpha(s[index])) {
        break;
      }
    }

    if (index >= length) {
      return false;
    }

    if (IsMonthName(s + start, index - start, &mon)) {
      seenMonthName = true;
      // If the next digit is a number, we need to break so it
      // gets parsed as mday
      if (IsAsciiDigit(s[index])) {
        break;
      }
    } else if (!strchr(" ,.-/", s[index])) {
      // We're only allowing the above delimiters after the day of
      // week to prevent things such as "foo_1" from being parsed
      // as a date, which may break software which uses this function
      // to determine whether or not something is a date.
      return false;
    }
  }

  int year = -1;
  int mday = -1;
  int hour = -1;
  int min = -1;
  int sec = -1;
  int msec = 0;
  int tzOffset = -1;

  // One of '+', '-', ':', '/', or 0 (the default value).
  int prevc = 0;

  bool seenPlusMinus = false;
  bool seenFullYear = false;
  bool negativeYear = false;
  // Includes "GMT", "UTC", "UT", and "Z" timezone keywords
  bool seenGmtAbbr = false;

  // Try parsing the leading dashed-date.
  //
  // If successfully parsed, index is updated to the end of the date part,
  // and year, mon, mday are set to the date.
  // Continue parsing optional time + tzOffset parts.
  //
  // Otherwise, this is no-op.
  bool isDashedDate =
      TryParseDashedDatePrefix(s, length, &index, &year, &mon, &mday) ||
      TryParseDashedNumericDatePrefix(s, length, &index, &year, &mon, &mday);

  if (isDashedDate && index < length && strchr("T:+", s[index])) {
    return false;
  }

  while (index < length) {
    int c = s[index];
    index++;

    // Normalize U+202F (NARROW NO-BREAK SPACE). This character appears between
    // the AM/PM markers for |date.toLocaleString("en")|. We have to normalize
    // it for backward compatibility reasons.
    if (c == 0x202F) {
      c = ' ';
    }

    if ((c == '+' || c == '-') &&
        // Reject + or - after timezone (still allowing for negative year)
        ((seenPlusMinus && year != -1) ||
         // Reject timezones like "1995-09-26 -04:30" (if the - is right up
         // against the previous number, it will get parsed as a time,
         // see the other comment below)
         (year != -1 && hour == -1 && !seenGmtAbbr &&
          !IsAsciiDigit(s[index - 2])))) {
      return false;
    }

    // Spaces, ASCII control characters, periods, and commas are simply ignored.
    if (c <= ' ' || c == '.' || c == ',') {
      continue;
    }

    // Parse delimiter characters.  Save them to the side for future use.
    if (c == '/' || c == ':' || c == '+') {
      prevc = c;
      continue;
    }

    // Dashes are delimiters if they're immediately followed by a number field.
    // If they're not followed by a number field, they're simply ignored.
    if (c == '-') {
      if (index < length && IsAsciiDigit(s[index])) {
        prevc = c;
      }
      continue;
    }

    // Skip over comments -- text inside matching parentheses.  (Comments
    // themselves may contain comments as long as all the parentheses properly
    // match up.  And apparently comments, including nested ones, may validly be
    // terminated by end of input...)
    if (c == '(') {
      int depth = 1;
      while (index < length) {
        c = s[index];
        index++;
        if (c == '(') {
          depth++;
        } else if (c == ')') {
          if (--depth <= 0) {
            break;
          }
        }
      }
      continue;
    }

    // Parse a number field.
    if (IsAsciiDigit(c)) {
      size_t partStart = index - 1;
      uint32_t u = c - '0';
      while (index < length) {
        c = s[index];
        if (!IsAsciiDigit(c)) {
          break;
        }
        u = u * 10 + (c - '0');
        index++;
      }
      size_t partLength = index - partStart;

      // See above for why we have to normalize U+202F.
      if (c == 0x202F) {
        c = ' ';
      }

      int n = int(u);

      /*
       * Allow TZA before the year, so 'Wed Nov 05 21:49:11 GMT-0800 1997'
       * works.
       *
       * Uses of seenPlusMinus allow ':' in TZA, so Java no-timezone style
       * of GMT+4:30 works.
       */

      if (prevc == '-' && (tzOffset != 0 || seenPlusMinus) && partLength >= 4 &&
          year < 0) {
        // Parse as a negative, possibly zero-padded year if
        // 1. the preceding character is '-',
        // 2. the TZA is not 'GMT' (tested by |tzOffset != 0|),
        // 3. or a TZA was already parsed |seenPlusMinus == true|,
        // 4. the part length is at least 4 (to parse '-08' as a TZA),
        // 5. and we did not already parse a year |year < 0|.
        year = n;
        seenFullYear = true;
        negativeYear = true;
      } else if ((prevc == '+' || prevc == '-') &&
                 // "1995-09-26-04:30" needs to be parsed as a time,
                 // not a time zone
                 (seenGmtAbbr || hour != -1)) {
        /* Make ':' case below change tzOffset. */
        seenPlusMinus = true;

        /* offset */
        if (n < 24 && partLength <= 2) {
          n = n * 60; /* EG. "GMT-3" */
        } else {
          n = n % 100 + n / 100 * 60; /* eg "GMT-0430" */
        }

        if (prevc == '+') /* plus means east of GMT */
          n = -n;

        // Reject if not preceded by 'GMT' or if a time zone offset
        // was already parsed.
        if (tzOffset != 0 && tzOffset != -1) {
          return false;
        }

        tzOffset = n;
      } else if (prevc == '/' && mon >= 0 && mday >= 0 && year < 0) {
        if (c <= ' ' || c == ',' || c == '/' || index >= length) {
          year = n;
        } else {
          return false;
        }
      } else if (c == ':') {
        if (hour < 0) {
          hour = /*byte*/ n;
        } else if (min < 0) {
          min = /*byte*/ n;
        } else {
          return false;
        }
      } else if (c == '/') {
        /*
         * Until it is determined that mon is the actual month, keep
         * it as 1-based rather than 0-based.
         */
        if (mon < 0) {
          mon = /*byte*/ n;
        } else if (mday < 0) {
          mday = /*byte*/ n;
        } else {
          return false;
        }
      } else if (index < length && c != ',' && c > ' ' && c != '-' &&
                 c != '(' &&
                 // Allow '.' as a delimiter until seconds have been parsed
                 // (this allows the decimal for milliseconds)
                 (c != '.' || sec != -1) &&
                 // Allow zulu time e.g. "09/26/1995 16:00Z", or
                 // '+' directly after time e.g. 00:00+0500
                 !(hour != -1 && strchr("Zz+", c)) &&
                 // Allow month or AM/PM directly after a number
                 (!IsAsciiAlpha(c) ||
                  (mon != -1 && !(strchr("AaPp", c) && index < length - 1 &&
                                  strchr("Mm", s[index + 1]))))) {
        return false;
      } else if (seenPlusMinus && n < 60) { /* handle GMT-3:30 */
        if (tzOffset < 0) {
          tzOffset -= n;
        } else {
          tzOffset += n;
        }
      } else if (hour >= 0 && min < 0) {
        min = /*byte*/ n;
      } else if (prevc == ':' && min >= 0 && sec < 0) {
        sec = /*byte*/ n;
        if (c == '.') {
          index++;
          if (!ParseFractional(&msec, s, &index, length)) {
            return false;
          }
        }
      } else if (mon < 0) {
        mon = /*byte*/ n;
      } else if (mon >= 0 && mday < 0) {
        mday = /*byte*/ n;
      } else if (mon >= 0 && mday >= 0 && year < 0) {
        year = n;
        seenFullYear = partLength >= 4;
      } else {
        return false;
      }

      prevc = 0;
      continue;
    }

    // Parse fields that are words: ASCII letters spelling out in English AM/PM,
    // day of week, month, or an extremely limited set of legacy time zone
    // abbreviations.
    if (IsAsciiAlpha(c)) {
      size_t start = index - 1;
      while (index < length) {
        c = s[index];
        if (!IsAsciiAlpha(c)) {
          break;
        }
        index++;
      }

      // There must be at least as many letters as in the shortest keyword.
      constexpr size_t MinLength = MinKeywordLength(keywords);
      if (index - start < MinLength) {
        return false;
      }

      // Record a month if it is a month name. Note that some numbers are
      // initially treated as months; if a numeric field has already been
      // interpreted as a month, store that value to the actually appropriate
      // date component and set the month here.
      int tryMonth;
      if (IsMonthName(s + start, index - start, &tryMonth)) {
        if (seenMonthName) {
          // Overwrite the previous month name
          mon = tryMonth;
          prevc = 0;
          continue;
        }

        seenMonthName = true;

        if (mon < 0) {
          mon = tryMonth;
        } else if (mday < 0) {
          mday = mon;
          mon = tryMonth;
        } else if (year < 0) {
          if (mday > 0) {
            // If the date is of the form f l month, then when month is
            // reached we have f in mon and l in mday. In order to be
            // consistent with the f month l and month f l forms, we need to
            // swap so that f is in mday and l is in year.
            year = mday;
            mday = mon;
          } else {
            year = mon;
          }
          mon = tryMonth;
        } else {
          return false;
        }

        prevc = 0;
        continue;
      }

      size_t k = std::size(keywords);
      while (k-- > 0) {
        const CharsAndAction& keyword = keywords[k];

        // If the field doesn't match the keyword, try the next one.
        if (!MatchesKeyword(s + start, index - start, keyword.chars)) {
          continue;
        }

        int action = keyword.action;

        if (action == 10000) {
          seenGmtAbbr = true;
        }

        // Perform action tests from smallest action values to largest.

        // Adjust a previously-specified hour for AM/PM accordingly (taking care
        // to treat 12:xx AM as 00:xx, 12:xx PM as 12:xx).
        if (action < 0) {
          MOZ_ASSERT(action == -1 || action == -2);
          if (hour > 12 || hour < 0) {
            return false;
          }

          if (action == -1 && hour == 12) {
            hour = 0;
          } else if (action == -2 && hour != 12) {
            hour += 12;
          }

          break;
        }

        // Finally, record a time zone offset.
        MOZ_ASSERT(action >= 10000);
        tzOffset = action - 10000;
        break;
      }

      if (k == size_t(-1)) {
        return false;
      }

      prevc = 0;
      continue;
    }

    // Any other character fails to parse.
    return false;
  }

  // Handle cases where the input is a single number. Single numbers >= 1000
  // are handled by the spec (ParseISOStyleDate), so we don't need to account
  // for that here.
  if (mon != -1 && year < 0 && mday < 0) {
    // Reject 13-31 for Chrome parity
    if (mon >= 13 && mon <= 31) {
      return false;
    }

    mday = 1;
    if (mon >= 1 && mon <= 12) {
      // 1-12 is parsed as a month with the year defaulted to 2001
      // (again, for Chrome parity)
      year = 2001;
    } else {
      year = FixupYear(mon);
      mon = 1;
    }
  }

  if (year < 0 || mon < 0 || mday < 0) {
    return false;
  }

  if (!isDashedDate) {
    // NOTE: TryParseDashedDatePrefix already handles the following fixup.

    /*
     * Case 1. The input string contains an English month name.
     *         The form of the string can be month f l, or f month l, or
     *         f l month which each evaluate to the same date.
     *         If f and l are both greater than or equal to 100 the date
     *         is invalid.
     *
     *         The year is taken to be either l, f if f > 31, or whichever
     *         is set to zero.
     *
     * Case 2. The input string is of the form "f/m/l" where f, m and l are
     *         integers, e.g. 7/16/45. mon, mday and year values are adjusted
     *         to achieve Chrome compatibility.
     *
     *         a. If 0 < f <= 12 and 0 < l <= 31, f/m/l is interpreted as
     *         month/day/year.
     *         b. If 31 < f and 0 < m <= 12 and 0 < l <= 31 f/m/l is
     *         interpreted as year/month/day
     */
    if (seenMonthName) {
      if (mday >= 100 && mon >= 100) {
        return false;
      }

      if (year > 0 && (mday == 0 || mday > 31) && !seenFullYear) {
        int temp = year;
        year = mday;
        mday = temp;
      }

      if (mday <= 0 || mday > 31) {
        return false;
      }

    } else if (0 < mon && mon <= 12 && 0 < mday && mday <= 31) {
      /* (a) month/day/year */
    } else {
      /* (b) year/month/day */
      if (mon > 31 && mday <= 12 && year <= 31 && !seenFullYear) {
        int temp = year;
        year = mon;
        mon = mday;
        mday = temp;
      } else {
        return false;
      }
    }

    year = FixupYear(year);

    if (negativeYear) {
      year = -year;
    }
  }

  mon -= 1; /* convert month to 0-based */
  if (sec < 0) {
    sec = 0;
  }
  if (min < 0) {
    min = 0;
  }
  if (hour < 0) {
    hour = 0;
  }

  double date =
      MakeDate(MakeDay(year, mon, mday), MakeTime(hour, min, sec, msec));

  if (tzOffset == -1) { /* no time zone specified, have to use local */
    date = UTC(forceUTC, date);
  } else {
    date += double(tzOffset) * msPerMinute;
  }

  *result = TimeClip(date);
  return true;
}

static bool ParseDate(JSContext* cx, DateTimeInfo::ForceUTC forceUTC,
                      const JSLinearString* s, ClippedTime* result) {
  AutoCheckCannotGC nogc;
  // Collect telemetry on how often Date.parse is being used.
  // This can be removed in the future, see Bug 1944630.
  cx->runtime()->setUseCounter(cx->global(), JSUseCounter::DATEPARSE);
  return s->hasLatin1Chars() ? ParseDate(cx, forceUTC, s->latin1Chars(nogc),
                                         s->length(), result)
                             : ParseDate(cx, forceUTC, s->twoByteChars(nogc),
                                         s->length(), result);
}

/**
 * 21.4.3.2 Date.parse ( string )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_parse(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date", "parse");

  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  JSString* str = ToString<CanGC>(cx, args[0]);
  if (!str) {
    return false;
  }

  JSLinearString* linearStr = str->ensureLinear(cx);
  if (!linearStr) {
    return false;
  }

  ClippedTime result;
  if (!ParseDate(cx, ForceUTC(cx->realm()), linearStr, &result)) {
    args.rval().setNaN();
    return true;
  }

  args.rval().set(TimeValue(result));
  return true;
}

static ClippedTime NowAsMillis(JSContext* cx) {
  if (js::SupportDifferentialTesting()) {
    return TimeClip(0);
  }

  double now = PRMJ_Now();
  bool clampAndJitter = cx->realm()->behaviors().clampAndJitterTime();
  if (clampAndJitter && sReduceMicrosecondTimePrecisionCallback) {
    now = sReduceMicrosecondTimePrecisionCallback(
        now, cx->realm()->behaviors().reduceTimerPrecisionCallerType().value(),
        cx);
  } else if (clampAndJitter && sResolutionUsec) {
    double clamped = floor(now / sResolutionUsec) * sResolutionUsec;

    if (sJitter) {
      // Calculate a random midpoint for jittering. In the browser, we are
      // adversarial: Web Content may try to calculate the midpoint themselves
      // and use that to bypass it's security. In the JS Shell, we are not
      // adversarial, we want to jitter the time to recreate the operating
      // environment, but we do not concern ourselves with trying to prevent an
      // attacker from calculating the midpoint themselves. So we use a very
      // simple, very fast CRC with a hardcoded seed.

      uint64_t midpoint = BitwiseCast<uint64_t>(clamped);
      midpoint ^= 0x0F00DD1E2BAD2DED;  // XOR in a 'secret'
      // MurmurHash3 internal component from
      //   https://searchfox.org/mozilla-central/rev/61d400da1c692453c2dc2c1cf37b616ce13dea5b/dom/canvas/MurmurHash3.cpp#85
      midpoint ^= midpoint >> 33;
      midpoint *= uint64_t{0xFF51AFD7ED558CCD};
      midpoint ^= midpoint >> 33;
      midpoint *= uint64_t{0xC4CEB9FE1A85EC53};
      midpoint ^= midpoint >> 33;
      midpoint %= sResolutionUsec;

      if (now > clamped + midpoint) {  // We're jittering up to the next step
        now = clamped + sResolutionUsec;
      } else {  // We're staying at the clamped value
        now = clamped;
      }
    } else {  // No jitter, only clamping
      now = clamped;
    }
  }

  return TimeClip(now / PRMJ_USEC_PER_MSEC);
}

JS::ClippedTime js::DateNow(JSContext* cx) { return NowAsMillis(cx); }

static bool date_now(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date", "now");
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().set(TimeValue(NowAsMillis(cx)));
  return true;
}

DateTimeInfo::ForceUTC DateObject::forceUTC() const {
  return ForceUTC(realm());
}

void DateObject::setUTCTime(ClippedTime t) {
  for (size_t ind = COMPONENTS_START_SLOT; ind < RESERVED_SLOTS; ind++) {
    setReservedSlot(ind, UndefinedValue());
  }

  setFixedSlot(UTC_TIME_SLOT, TimeValue(t));
}

void DateObject::setUTCTime(ClippedTime t, MutableHandleValue vp) {
  setUTCTime(t);
  vp.set(TimeValue(t));
}

void DateObject::fillLocalTimeSlots() {
  const int32_t utcTZOffset =
      DateTimeInfo::utcToLocalStandardOffsetSeconds(forceUTC());

  /* Check if the cache is already populated. */
  if (!getReservedSlot(LOCAL_TIME_SLOT).isUndefined() &&
      getReservedSlot(UTC_TIME_ZONE_OFFSET_SLOT).toInt32() == utcTZOffset) {
    return;
  }

  /* Remember time zone used to generate the local cache. */
  setReservedSlot(UTC_TIME_ZONE_OFFSET_SLOT, Int32Value(utcTZOffset));

  double utcTime = UTCTime().toDouble();

  if (!std::isfinite(utcTime)) {
    for (size_t ind = COMPONENTS_START_SLOT; ind < RESERVED_SLOTS; ind++) {
      setReservedSlot(ind, DoubleValue(utcTime));
    }
    return;
  }

  int64_t localTime = LocalTime(forceUTC(), utcTime);

  setReservedSlot(LOCAL_TIME_SLOT, DoubleValue(localTime));

  const auto [year, month, day] = ToYearMonthDay(localTime);

  setReservedSlot(LOCAL_YEAR_SLOT, Int32Value(year));
  setReservedSlot(LOCAL_MONTH_SLOT, Int32Value(month));
  setReservedSlot(LOCAL_DATE_SLOT, Int32Value(day));

  int weekday = WeekDay(localTime);
  setReservedSlot(LOCAL_DAY_SLOT, Int32Value(weekday));

  int64_t yearStartTime = TimeFromYear(year);
  uint64_t yearTime = uint64_t(localTime - yearStartTime);
  int32_t yearSeconds = int32_t(yearTime / msPerSecond);
  setReservedSlot(LOCAL_SECONDS_INTO_YEAR_SLOT, Int32Value(yearSeconds));
}

/**
 * 21.4.4.10 Date.prototype.getTime ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getTime(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getTime");
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  args.rval().set(unwrapped->UTCTime());
  return true;
}

/**
 * B.2.3.1 Date.prototype.getYear ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getYear");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-5.
  unwrapped->fillLocalTimeSlots();

  Value yearVal = unwrapped->localYear();
  if (yearVal.isInt32()) {
    int year = yearVal.toInt32() - 1900;
    args.rval().setInt32(year);
  } else {
    args.rval().set(yearVal);
  }
  return true;
}

/**
 * 21.4.4.4 Date.prototype.getFullYear ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getFullYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getFullYear");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-5.
  unwrapped->fillLocalTimeSlots();

  args.rval().set(unwrapped->localYear());
  return true;
}

/**
 * 21.4.4.18 Date.prototype.getUTCMonth ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getUTCFullYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCFullYear");
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 5.
  args.rval().setInt32(::YearFromTime(tv));
  return true;
}

/**
 * 21.4.4.8 Date.prototype.getMonth ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getMonth(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getMonth");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-5.
  unwrapped->fillLocalTimeSlots();

  args.rval().set(unwrapped->localMonth());
  return true;
}

/**
 * 21.4.4.18 Date.prototype.getUTCMonth ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getUTCMonth(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCMonth");
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 5.
  args.rval().setInt32(::MonthFromTime(tv));
  return true;
}

/**
 * 21.4.4.2 Date.prototype.getDate ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getDate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getDate");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-5.
  unwrapped->fillLocalTimeSlots();

  args.rval().set(unwrapped->localDate());
  return true;
}

/**
 * 21.4.4.12 Date.prototype.getUTCDate ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getUTCDate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCDate");
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 5.
  args.rval().setInt32(DateFromTime(tv));
  return true;
}

/**
 * 21.4.4.3 Date.prototype.getDay ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getDay(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getDay");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-5.
  unwrapped->fillLocalTimeSlots();

  args.rval().set(unwrapped->localDay());
  return true;
}

/**
 * 21.4.4.13 Date.prototype.getUTCDay ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getUTCDay(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCDay");
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 5.
  args.rval().setInt32(WeekDay(tv));
  return true;
}

/**
 * 21.4.4.5 Date.prototype.getHours ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getHours(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getHours");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-5.
  unwrapped->fillLocalTimeSlots();

  // Note: localSecondsIntoYear is guaranteed to return an
  // int32 or NaN after the call to fillLocalTimeSlots.
  Value yearSeconds = unwrapped->localSecondsIntoYear();
  if (yearSeconds.isDouble()) {
    MOZ_ASSERT(std::isnan(yearSeconds.toDouble()));
    args.rval().set(yearSeconds);
  } else {
    args.rval().setInt32((yearSeconds.toInt32() / SecondsPerHour) %
                         HoursPerDay);
  }
  return true;
}

/**
 * 21.4.4.15 Date.prototype.getUTCHours ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getUTCHours(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCHours");
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 5.
  args.rval().setInt32(HourFromTime(tv));
  return true;
}

/**
 * 21.4.4.7 Date.prototype.getMinutes ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getMinutes(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getMinutes");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-5.
  unwrapped->fillLocalTimeSlots();

  // Note: localSecondsIntoYear is guaranteed to return an
  // int32 or NaN after the call to fillLocalTimeSlots.
  Value yearSeconds = unwrapped->localSecondsIntoYear();
  if (yearSeconds.isDouble()) {
    MOZ_ASSERT(std::isnan(yearSeconds.toDouble()));
    args.rval().set(yearSeconds);
  } else {
    args.rval().setInt32((yearSeconds.toInt32() / SecondsPerMinute) %
                         MinutesPerHour);
  }
  return true;
}

/**
 * 21.4.4.17 Date.prototype.getUTCMinutes ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getUTCMinutes(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCMinutes");
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 5.
  args.rval().setInt32(MinFromTime(tv));
  return true;
}

/**
 * 21.4.4.9 Date.prototype.getSeconds ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getSeconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getSeconds");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-5.
  unwrapped->fillLocalTimeSlots();

  // Note: localSecondsIntoYear is guaranteed to return an
  // int32 or NaN after the call to fillLocalTimeSlots.
  Value yearSeconds = unwrapped->localSecondsIntoYear();
  if (yearSeconds.isDouble()) {
    MOZ_ASSERT(std::isnan(yearSeconds.toDouble()));
    args.rval().set(yearSeconds);
  } else {
    args.rval().setInt32(yearSeconds.toInt32() % SecondsPerMinute);
  }
  return true;
}

/**
 * 21.4.4.19 Date.prototype.getUTCSeconds ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getUTCSeconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCSeconds");
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 5.
  args.rval().setInt32(SecFromTime(tv));
  return true;
}

/*
 * Date.getMilliseconds is mapped to getUTCMilliseconds. As long as no
 * supported time zone has a fractional-second component, the differences in
 * their specifications aren't observable.
 *
 * The 'tz' database explicitly does not support fractional-second time zones.
 * For example the Netherlands observed Amsterdam Mean Time, estimated to be
 * UT +00:19:32.13, from 1909 to 1937, but in tzdata AMT is defined as exactly
 * UT +00:19:32.
 */

/**
 * 21.4.4.6 Date.prototype.getMilliseconds ( )
 * 21.4.4.16 Date.prototype.getUTCMilliseconds ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool getMilliseconds(JSContext* cx, unsigned argc, Value* vp,
                            const char* methodName) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, methodName);
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 5.
  args.rval().setInt32(msFromTime(tv));
  return true;
}

static bool date_getMilliseconds(JSContext* cx, unsigned argc, Value* vp) {
  return getMilliseconds(cx, argc, vp, "getMilliseconds");
}

static bool date_getUTCMilliseconds(JSContext* cx, unsigned argc, Value* vp) {
  return getMilliseconds(cx, argc, vp, "getUTCMilliseconds");
}

/**
 * 21.4.4.11 Date.prototype.getTimezoneOffset ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_getTimezoneOffset(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "getTimezoneOffset");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-5.
  unwrapped->fillLocalTimeSlots();

  double utctime = unwrapped->UTCTime().toDouble();
  double localtime = unwrapped->localTime().toDouble();

  /*
   * Return the time zone offset in minutes for the current locale that is
   * appropriate for this time. This value would be a constant except for
   * daylight savings time.
   */
  double result = (utctime - localtime) / double(msPerMinute);
  args.rval().setNumber(result);
  return true;
}

/**
 * 21.4.4.27 Date.prototype.setTime ( time )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setTime(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setTime"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double result;
  if (!ToNumber(cx, args.get(0), &result)) {
    return false;
  }

  // Steps 4-6.
  unwrapped->setUTCTime(TimeClip(result), args.rval());
  return true;
}

/**
 * 21.4.4.23 Date.prototype.setMilliseconds ( ms )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setMilliseconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setMilliseconds"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double ms;
  if (!ToNumber(cx, args.get(0), &ms)) {
    return false;
  }

  // Step 5.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }

  // Step 6.
  int64_t tv = LocalTime(unwrapped->forceUTC(), t);

  // Step 7.
  double time =
      MakeTime(HourFromTime(tv), MinFromTime(tv), SecFromTime(tv), ms);

  // Step 8.
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), MakeDate(Day(tv), time)));

  // Steps 9-10.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/**
 * 21.4.4.31 Date.prototype.setUTCMilliseconds ( ms )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setUTCMilliseconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCMilliseconds"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double milli;
  if (!ToNumber(cx, args.get(0), &milli)) {
    return false;
  }

  // Step 5.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 6.
  double time =
      MakeTime(HourFromTime(tv), MinFromTime(tv), SecFromTime(tv), milli);

  // Step 7.
  ClippedTime v = TimeClip(MakeDate(Day(tv), time));

  // Steps 8-9.
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/**
 * 21.4.4.26 Date.prototype.setSeconds ( sec [ , ms ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setSeconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setSeconds"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double s;
  if (!ToNumber(cx, args.get(0), &s)) {
    return false;
  }

  // Step 5.
  double milli;
  if (args.length() > 1 && !ToNumber(cx, args[1], &milli)) {
    return false;
  }

  // Step 6.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }

  // Step 7.
  int64_t tv = LocalTime(unwrapped->forceUTC(), t);

  // Step 8.
  if (args.length() <= 1) {
    milli = msFromTime(tv);
  }

  // Step 9.
  double date =
      MakeDate(Day(tv), MakeTime(HourFromTime(tv), MinFromTime(tv), s, milli));

  // Step 10.
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), date));

  // Steps 11-12.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/**
 * 21.4.4.34 Date.prototype.setUTCSeconds ( sec [ , ms ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setUTCSeconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCSeconds"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double s;
  if (!ToNumber(cx, args.get(0), &s)) {
    return false;
  }

  // Step 5.
  double milli;
  if (args.length() > 1 && !ToNumber(cx, args[1], &milli)) {
    return false;
  }

  // Step 6.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 7.
  if (args.length() <= 1) {
    milli = msFromTime(tv);
  }

  // Step 8.
  double date =
      MakeDate(Day(tv), MakeTime(HourFromTime(tv), MinFromTime(tv), s, milli));

  // Step 9.
  ClippedTime v = TimeClip(date);

  // Steps 10-11.
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/**
 * 21.4.4.24 Date.prototype.setMinutes ( min [ , sec [ , ms ] ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setMinutes(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setMinutes"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double m;
  if (!ToNumber(cx, args.get(0), &m)) {
    return false;
  }

  // Step 5.
  double s;
  if (args.length() > 1 && !ToNumber(cx, args[1], &s)) {
    return false;
  }

  // Step 6.
  double milli;
  if (args.length() > 2 && !ToNumber(cx, args[2], &milli)) {
    return false;
  }

  // Step 7.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }

  // Step 8.
  int64_t tv = LocalTime(unwrapped->forceUTC(), t);

  // Step 9.
  if (args.length() <= 1) {
    s = SecFromTime(tv);
  }

  // Step 10.
  if (args.length() <= 2) {
    milli = msFromTime(tv);
  }

  // Step 11.
  double date = MakeDate(Day(tv), MakeTime(HourFromTime(tv), m, s, milli));

  // Step 12.
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), date));

  // Steps 13-14.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/**
 * 21.4.4.32 Date.prototype.setUTCMinutes ( min [ , sec [ , ms ] ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setUTCMinutes(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCMinutes"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double m;
  if (!ToNumber(cx, args.get(0), &m)) {
    return false;
  }

  // Step 5.
  double s;
  if (args.length() > 1 && !ToNumber(cx, args[1], &s)) {
    return false;
  }

  // Step 6.
  double milli;
  if (args.length() > 2 && !ToNumber(cx, args[2], &milli)) {
    return false;
  }

  // Step 7.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 8.
  if (args.length() <= 1) {
    s = SecFromTime(tv);
  }

  // Step 9.
  if (args.length() <= 2) {
    milli = msFromTime(tv);
  }

  // Step 10.
  double date = MakeDate(Day(tv), MakeTime(HourFromTime(tv), m, s, milli));

  // Step 11.
  ClippedTime v = TimeClip(date);

  // Steps 12-13.
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/**
 * 21.4.4.22 Date.prototype.setHours ( hour [ , min [ , sec [ , ms ] ] ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setHours(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setHours"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double h;
  if (!ToNumber(cx, args.get(0), &h)) {
    return false;
  }

  // Step 5.
  double m;
  if (args.length() > 1 && !ToNumber(cx, args[1], &m)) {
    return false;
  }

  // Step 6.
  double s;
  if (args.length() > 2 && !ToNumber(cx, args[2], &s)) {
    return false;
  }

  // Step 7.
  double milli;
  if (args.length() > 3 && !ToNumber(cx, args[3], &milli)) {
    return false;
  }

  // Step 8.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }

  // Step 9.
  int64_t tv = LocalTime(unwrapped->forceUTC(), t);

  // Step 10.
  if (args.length() <= 1) {
    m = MinFromTime(tv);
  }

  // Step 11.
  if (args.length() <= 2) {
    s = SecFromTime(tv);
  }

  // Step 12.
  if (args.length() <= 3) {
    milli = msFromTime(tv);
  }

  // Step 13.
  double date = MakeDate(Day(tv), MakeTime(h, m, s, milli));

  // Step 14.
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), date));

  // Steps 15-16.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/**
 * 21.4.4.30 Date.prototype.setUTCHours ( hour [ , min [ , sec [ , ms ] ] ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setUTCHours(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCHours"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double h;
  if (!ToNumber(cx, args.get(0), &h)) {
    return false;
  }

  // Step 5.
  double m;
  if (args.length() > 1 && !ToNumber(cx, args[1], &m)) {
    return false;
  }

  // Step 6.
  double s;
  if (args.length() > 2 && !ToNumber(cx, args[2], &s)) {
    return false;
  }

  // Step 7.
  double milli;
  if (args.length() > 3 && !ToNumber(cx, args[3], &milli)) {
    return false;
  }

  // Step 8.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 9.
  if (args.length() <= 1) {
    m = MinFromTime(tv);
  }

  // Step 10.
  if (args.length() <= 2) {
    s = SecFromTime(tv);
  }

  // Step 11.
  if (args.length() <= 3) {
    milli = msFromTime(tv);
  }

  // Step 12.
  double date = MakeDate(Day(tv), MakeTime(h, m, s, milli));

  // Step 13.
  ClippedTime v = TimeClip(date);

  // Steps 14-15.
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/**
 * 21.4.4.20 Date.prototype.setDate ( date )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setDate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setDate"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double dt;
  if (!ToNumber(cx, args.get(0), &dt)) {
    return false;
  }

  // Step 5.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }

  // Step 6.
  int64_t tv = LocalTime(unwrapped->forceUTC(), t);

  // Step 7.
  double newDate = MakeDate(
      MakeDay(::YearFromTime(tv), ::MonthFromTime(tv), dt), TimeWithinDay(tv));

  // Step 8.
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), newDate));

  // Steps 9-10.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/**
 * 21.4.4.28 Date.prototype.setUTCDate ( date )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setUTCDate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCDate"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double dt;
  if (!ToNumber(cx, args.get(0), &dt)) {
    return false;
  }

  // Step 5.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 6.
  double newDate = MakeDate(
      MakeDay(::YearFromTime(tv), ::MonthFromTime(tv), dt), TimeWithinDay(tv));

  // Step 7.
  ClippedTime v = TimeClip(newDate);

  // Steps 8-9.
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/**
 * 21.4.4.25 Date.prototype.setMonth ( month [ , date ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setMonth(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setMonth"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double m;
  if (!ToNumber(cx, args.get(0), &m)) {
    return false;
  }

  // Step 5.
  double dt;
  if (args.length() > 1 && !ToNumber(cx, args[1], &dt)) {
    return false;
  }

  // Step 6.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }

  // Step 7.
  int64_t tv = LocalTime(unwrapped->forceUTC(), t);

  // Step 8.
  if (args.length() <= 1) {
    dt = DateFromTime(tv);
  }

  // Step 9.
  double newDate =
      MakeDate(MakeDay(::YearFromTime(tv), m, dt), TimeWithinDay(tv));

  // Step 10
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), newDate));

  // Steps 11-12.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/**
 * 21.4.4.33 Date.prototype.setUTCMonth ( month [ , date ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setUTCMonth(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCMonth"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double m;
  if (!ToNumber(cx, args.get(0), &m)) {
    return false;
  }

  // Step 5.
  double dt;
  if (args.length() > 1 && !ToNumber(cx, args[1], &dt)) {
    return false;
  }

  // Step 6.
  if (std::isnan(t)) {
    args.rval().setNaN();
    return true;
  }
  int64_t tv = static_cast<int64_t>(t);

  // Step 7.
  if (args.length() <= 1) {
    dt = DateFromTime(tv);
  }

  // Step 8.
  double newDate =
      MakeDate(MakeDay(::YearFromTime(tv), m, dt), TimeWithinDay(tv));

  // Step 9.
  ClippedTime v = TimeClip(newDate);

  // Steps 10-11.
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/**
 * 21.4.4.21 Date.prototype.setFullYear ( year [ , month [ , date ] ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setFullYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setFullYear"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double y;
  if (!ToNumber(cx, args.get(0), &y)) {
    return false;
  }

  // Step 5.
  int64_t tv;
  if (std::isnan(t)) {
    tv = 0;
  } else {
    tv = LocalTime(unwrapped->forceUTC(), t);
  }

  // Step 6.
  double m;
  if (args.length() <= 1) {
    m = MonthFromTime(tv);
  } else {
    if (!ToNumber(cx, args[1], &m)) {
      return false;
    }
  }

  // Step 7.
  double dt;
  if (args.length() <= 2) {
    dt = DateFromTime(tv);
  } else {
    if (!ToNumber(cx, args[2], &dt)) {
      return false;
    }
  }

  // Step 8.
  double newDate = MakeDate(MakeDay(y, m, dt), TimeWithinDay(tv));

  // Step 9.
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), newDate));

  // Steps 10-11.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/**
 * 21.4.4.29 Date.prototype.setUTCFullYear ( year [ , month [ , date ] ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setUTCFullYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCFullYear"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  int64_t tv;
  if (std::isnan(t)) {
    tv = 0;
  } else {
    tv = static_cast<int64_t>(t);
  }

  // Step 5.
  double y;
  if (!ToNumber(cx, args.get(0), &y)) {
    return false;
  }

  // Step 6.
  double m;
  if (args.length() <= 1) {
    m = MonthFromTime(tv);
  } else {
    if (!ToNumber(cx, args[1], &m)) {
      return false;
    }
  }

  // Step 7.
  double dt;
  if (args.length() <= 2) {
    dt = DateFromTime(tv);
  } else {
    if (!ToNumber(cx, args[2], &dt)) {
      return false;
    }
  }

  // Step 8.
  double newDate = MakeDate(MakeDay(y, m, dt), TimeWithinDay(tv));

  // Step 9.
  ClippedTime v = TimeClip(newDate);

  // Steps 10-11.
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/**
 * B.2.3.2 Date.prototype.setYear ( year )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_setYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setYear"));
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  double y;
  if (!ToNumber(cx, args.get(0), &y)) {
    return false;
  }

  // Step 5.
  int64_t tv;
  if (std::isnan(t)) {
    tv = 0;
  } else {
    tv = LocalTime(unwrapped->forceUTC(), t);
  }

  // Step 6.
  double yyyy = MakeFullYear(y);

  // Step 7.
  double day = MakeDay(yyyy, ::MonthFromTime(tv), DateFromTime(tv));

  // Step 8.
  double date = MakeDate(day, TimeWithinDay(tv));

  // Step 9.
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), date));

  // Steps 10-11.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/**
 * Simple date formatter for toISOString, toUTCString, and FormatDate.
 */
class DateFormatter {
  // Longest possible string is 36 characters. Round up to the next multiple of
  // sixteen.
  static constexpr size_t BufferLength = 48;

  char buffer_[BufferLength] = {};
  char* ptr_ = buffer_;

  size_t written() const { return size_t(ptr_ - buffer_); }

  static constexpr uint32_t powerOfTen(uint32_t exp) {
    uint32_t result = 1;
    while (exp--) {
      result *= 10;
    }
    return result;
  }

  // Add |N| digits, padded with zeroes.
  template <uint32_t N>
  void digits(uint32_t value) {
    static_assert(1 <= N && N <= 6);
    MOZ_ASSERT(written() + N <= BufferLength);

    constexpr uint32_t divisor = powerOfTen(N - 1);
    MOZ_ASSERT(value < divisor * 10);

    uint32_t quot = value / divisor;
    [[maybe_unused]] uint32_t rem = value % divisor;

    *ptr_++ = char('0' + quot);
    if constexpr (N > 1) {
      digits<N - 1>(rem);
    }
  }

  // Constants for toString and toUTCString.
  static constexpr char const days[][4] = {
      "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
  };
  static constexpr char const months[][4] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };

 public:
  std::string_view string() const { return {buffer_, written()}; }

  auto& literal(char ch) {
    MOZ_ASSERT(written() + 1 <= BufferLength);
    *ptr_++ = ch;
    return *this;
  }

  template <size_t N>
  auto& literal(const char (&str)[N]) {
    static_assert(N > 0);
    size_t length = N - 1;
    MOZ_ASSERT(written() + length <= BufferLength);
    std::memcpy(ptr_, str, length);
    ptr_ += length;
    return *this;
  }

  auto& year(int32_t value) {
    MOZ_ASSERT(-999'999 <= value && value <= 999'999);
    if (value < 0) {
      literal('-');
      value = std::abs(value);
    }
    if (value <= 9999) {
      digits<4>(value);
    } else if (value <= 99999) {
      digits<5>(value);
    } else {
      digits<6>(value);
    }
    return *this;
  }

  auto& isoYear(int32_t value) {
    MOZ_ASSERT(-999'999 <= value && value <= 999'999);
    if (0 <= value && value <= 9999) {
      digits<4>(value);
    } else {
      literal(value < 0 ? '-' : '+');
      digits<6>(std::abs(value));
    }
    return *this;
  }

  auto& month(int32_t value) {
    MOZ_ASSERT(1 <= value && value <= 12);
    digits<2>(value);
    return *this;
  }

  auto& day(int32_t value) {
    MOZ_ASSERT(1 <= value && value <= 31);
    digits<2>(value);
    return *this;
  }

  auto& hour(int32_t value) {
    MOZ_ASSERT(0 <= value && value <= 23);
    digits<2>(value);
    return *this;
  }

  auto& minute(int32_t value) {
    MOZ_ASSERT(0 <= value && value <= 59);
    digits<2>(value);
    return *this;
  }

  auto& second(int32_t value) {
    MOZ_ASSERT(0 <= value && value <= 59);
    digits<2>(value);
    return *this;
  }

  auto& time(int32_t h, int32_t m, int32_t s) {
    return hour(h).literal(':').minute(m).literal(':').second(s);
  }

  auto& millisecond(int32_t value) {
    MOZ_ASSERT(0 <= value && value <= 999);
    digits<3>(value);
    return *this;
  }

  auto& monthName(int32_t value) {
    MOZ_ASSERT(0 <= value && value < 12);
    return literal(months[value]);
  }

  auto& weekDay(int32_t value) {
    MOZ_ASSERT(0 <= value && value < 7);
    return literal(days[value]);
  }

  auto& timeZoneOffset(int32_t value) {
    MOZ_ASSERT(-2400 < value && value < 2400);
    literal(value < 0 ? '-' : '+');
    digits<4>(std::abs(value));
    return *this;
  }
};

/**
 * 21.4.4.43 Date.prototype.toUTCString ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_toUTCString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toUTCString");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "toUTCString");
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double utctime = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(utctime));

  // Step 4.
  if (std::isnan(utctime)) {
    args.rval().setString(cx->names().Invalid_Date_);
    return true;
  }
  int64_t epochMilliseconds = static_cast<int64_t>(utctime);

  // Steps 5-11.
  auto [year, month, day] = ToYearMonthDay(epochMilliseconds);
  auto [hour, minute, second] = ToHourMinuteSecond(epochMilliseconds);

  DateFormatter fmt{};
  fmt.weekDay(WeekDay(epochMilliseconds))
      .literal(", ")
      .day(day)
      .literal(' ')
      .monthName(month)
      .literal(' ')
      .year(year)
      .literal(' ')
      .time(hour, minute, second)
      .literal(" GMT");

  JSString* str = NewStringCopy<CanGC>(cx, fmt.string());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * 21.4.4.36 Date.prototype.toISOString ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_toISOString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toISOString");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "toISOString");
  if (!unwrapped) {
    return false;
  }

  // Steps 3 and 5.
  double utctime = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(utctime));

  // Step 4.
  if (!std::isfinite(utctime)) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_INVALID_DATE);
    return false;
  }
  int64_t epochMilliseconds = static_cast<int64_t>(utctime);

  // Step 6. (Not applicable in our implementation.)

  // Step 7.
  auto [year, month, day] = ToYearMonthDay(epochMilliseconds);
  auto [hour, minute, second] = ToHourMinuteSecond(epochMilliseconds);

  DateFormatter fmt{};
  fmt.isoYear(year)
      .literal('-')
      .month(month + 1)
      .literal('-')
      .day(day)
      .literal('T')
      .time(hour, minute, second)
      .literal('.')
      .millisecond(msFromTime(epochMilliseconds))
      .literal('Z');

  JSString* str = NewStringCopy<CanGC>(cx, fmt.string());
  if (!str) {
    return false;
  }
  args.rval().setString(str);
  return true;
}

/**
 * 21.4.4.37 Date.prototype.toJSON ( key )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toJSON");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  // Step 2.
  RootedValue tv(cx, ObjectValue(*obj));
  if (!ToPrimitive(cx, JSTYPE_NUMBER, &tv)) {
    return false;
  }

  // Step 3.
  if (tv.isDouble() && !std::isfinite(tv.toDouble())) {
    args.rval().setNull();
    return true;
  }

  // Step 4.
  RootedValue toISO(cx);
  if (!GetProperty(cx, obj, obj, cx->names().toISOString, &toISO)) {
    return false;
  }

  if (!IsCallable(toISO)) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_BAD_TOISOSTRING_PROP);
    return false;
  }

  return Call(cx, toISO, obj, args.rval());
}

#if JS_HAS_INTL_API
JSString* DateTimeHelper::timeZoneComment(JSContext* cx,
                                          DateTimeInfo::ForceUTC forceUTC,
                                          const char* locale, int64_t utcTime,
                                          int64_t localTime) {
  MOZ_ASSERT(IsTimeValue(utcTime));
  MOZ_ASSERT(IsLocalTimeValue(localTime));

  char16_t tzbuf[100];
  tzbuf[0] = ' ';
  tzbuf[1] = '(';

  char16_t* timeZoneStart = tzbuf + 2;
  constexpr size_t remainingSpace =
      std::size(tzbuf) - 2 - 1;  // for the trailing ')'

  if (!DateTimeInfo::timeZoneDisplayName(forceUTC, timeZoneStart,
                                         remainingSpace, utcTime, locale)) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }

  // Reject if the result string is empty.
  size_t len = js_strlen(timeZoneStart);
  if (len == 0) {
    return cx->names().empty_;
  }

  // Parenthesize the returned display name.
  timeZoneStart[len] = ')';

  return NewStringCopyN<CanGC>(cx, tzbuf, 2 + len + 1);
}
#else
/* Interface to PRMJTime date struct. */
PRMJTime DateTimeHelper::toPRMJTime(DateTimeInfo::ForceUTC forceUTC,
                                    int64_t localTime, int64_t utcTime) {
  auto [year, month, day] = ToYearMonthDay(localTime);
  auto [hour, minute, second] = ToHourMinuteSecond(localTime);

  PRMJTime prtm;
  prtm.tm_usec = int32_t(msFromTime(localTime)) * 1000;
  prtm.tm_sec = int8_t(second);
  prtm.tm_min = int8_t(minute);
  prtm.tm_hour = int8_t(hour);
  prtm.tm_mday = int8_t(day);
  prtm.tm_mon = int8_t(month);
  prtm.tm_wday = int8_t(WeekDay(localTime));
  prtm.tm_year = year;
  prtm.tm_yday = int16_t(::DayWithinYear(localTime, year));
  prtm.tm_isdst = (daylightSavingTA(forceUTC, utcTime) != 0);

  return prtm;
}

size_t DateTimeHelper::formatTime(DateTimeInfo::ForceUTC forceUTC, char* buf,
                                  size_t buflen, const char* fmt,
                                  int64_t utcTime, int64_t localTime) {
  PRMJTime prtm = toPRMJTime(forceUTC, localTime, utcTime);

  // If an equivalent year was used to compute the date/time components, use
  // the same equivalent year to determine the time zone name and offset in
  // PRMJ_FormatTime(...).
  int timeZoneYear = isRepresentableAsTime32(utcTime)
                         ? prtm.tm_year
                         : equivalentYearForDST(prtm.tm_year);

  int32_t offsetInSeconds = FloorDiv(localTime - utcTime, msPerSecond);

  return PRMJ_FormatTime(buf, buflen, fmt, &prtm, timeZoneYear,
                         offsetInSeconds);
}

JSString* DateTimeHelper::timeZoneComment(JSContext* cx,
                                          DateTimeInfo::ForceUTC forceUTC,
                                          const char* locale, int64_t utcTime,
                                          int64_t localTime) {
  char tzbuf[100];

  size_t tzlen =
      formatTime(forceUTC, tzbuf, sizeof tzbuf, " (%Z)", utcTime, localTime);
  if (tzlen != 0) {
    // Decide whether to use the resulting time zone string.
    //
    // Reject it if it contains any non-ASCII or non-printable characters.
    // It's then likely in some other character encoding, and we probably
    // won't display it correctly.
    bool usetz = true;
    for (size_t i = 0; i < tzlen; i++) {
      char16_t c = tzbuf[i];
      if (!IsAsciiPrintable(c)) {
        usetz = false;
        break;
      }
    }

    // Also reject it if it's not parenthesized or if it's ' ()'.
    if (tzbuf[0] != ' ' || tzbuf[1] != '(' || tzbuf[2] == ')') {
      usetz = false;
    }

    if (usetz) {
      return NewStringCopyN<CanGC>(cx, tzbuf, tzlen);
    }
  }

  return cx->names().empty_;
}
#endif /* JS_HAS_INTL_API */

enum class FormatSpec { DateTime, Date, Time };

static bool FormatDate(JSContext* cx, DateTimeInfo::ForceUTC forceUTC,
                       const char* locale, double utcTime, FormatSpec format,
                       MutableHandleValue rval) {
  MOZ_ASSERT(IsTimeValue(utcTime));

  if (!std::isfinite(utcTime)) {
    rval.setString(cx->names().Invalid_Date_);
    return true;
  }

  int64_t epochMilliseconds = static_cast<int64_t>(utcTime);
  int64_t localTime = LocalTime(forceUTC, utcTime);

  int offset = 0;
  RootedString timeZoneComment(cx);
  if (format == FormatSpec::DateTime || format == FormatSpec::Time) {
    // Offset from GMT in minutes. The offset includes daylight savings,
    // if it applies.
    int32_t minutes = int32_t(localTime - epochMilliseconds) / msPerMinute;

    // Map 510 minutes to 0830 hours.
    offset = (minutes / 60) * 100 + minutes % 60;

    // Print as "Wed Nov 05 1997 19:38:03 GMT-0800 (PST)".
    //
    // The TZA is printed as 'GMT-0800' rather than as 'PST' to avoid
    // operating-system dependence on strftime (which PRMJ_FormatTime
    // calls, for %Z only.) win32 prints PST as 'Pacific Standard Time.'
    // This way we always know what we're getting, and can parse it if
    // we produce it. The OS time zone string is included as a comment.
    //
    // When ICU is used to retrieve the time zone string, the localized
    // 'long' name format from CLDR is used. For example when the default
    // locale is "en-US", PST is displayed as 'Pacific Standard Time', but
    // when it is "ru", 'Тихоокеанское стандартное время' is used. This
    // also means the time zone string may not fit into Latin-1.

    // Get a time zone string from the OS or ICU to include as a comment.
    timeZoneComment = DateTimeHelper::timeZoneComment(
        cx, forceUTC, locale, epochMilliseconds, localTime);
    if (!timeZoneComment) {
      return false;
    }
  }

  DateFormatter fmt{};
  switch (format) {
    case FormatSpec::DateTime: {
      /* Tue Oct 31 2000 09:41:40 GMT-0800 */
      auto [year, month, day] = ToYearMonthDay(localTime);
      auto [hour, minute, second] = ToHourMinuteSecond(localTime);

      fmt.weekDay(WeekDay(localTime))
          .literal(' ')
          .monthName(month)
          .literal(' ')
          .day(day)
          .literal(' ')
          .year(year)
          .literal(' ')
          .time(hour, minute, second)
          .literal(" GMT")
          .timeZoneOffset(offset);
      break;
    }
    case FormatSpec::Date: {
      /* Tue Oct 31 2000 */
      auto [year, month, day] = ToYearMonthDay(localTime);

      fmt.weekDay(WeekDay(localTime))
          .literal(' ')
          .monthName(month)
          .literal(' ')
          .day(day)
          .literal(' ')
          .year(year);
      break;
    }
    case FormatSpec::Time:
      /* 09:41:40 GMT-0800 */
      auto [hour, minute, second] = ToHourMinuteSecond(localTime);
      fmt.time(hour, minute, second).literal(" GMT").timeZoneOffset(offset);
      break;
  }

  RootedString str(cx, NewStringCopy<CanGC>(cx, fmt.string()));
  if (!str) {
    return false;
  }

  // Append the time zone string if present.
  if (timeZoneComment && !timeZoneComment->empty()) {
    str = js::ConcatStrings<CanGC>(cx, str, timeZoneComment);
    if (!str) {
      return false;
    }
  }

  rval.setString(str);
  return true;
}

#if !JS_HAS_INTL_API
static bool ToLocaleFormatHelper(JSContext* cx, DateObject* unwrapped,
                                 const char* format, MutableHandleValue rval) {
  DateTimeInfo::ForceUTC forceUTC = unwrapped->forceUTC();
  double utcTime = unwrapped->UTCTime().toDouble();

  const char* locale = unwrapped->realm()->getLocale();
  if (!locale) {
    return false;
  }

  char buf[100];
  if (!std::isfinite(utcTime)) {
    strcpy(buf, "InvalidDate");
  } else {
    MOZ_ASSERT(IsTimeValue(utcTime));

    int64_t epochMilliseconds = static_cast<int64_t>(utcTime);
    int64_t localTime = static_cast<int64_t>(LocalTime(forceUTC, utcTime));

    /* Let PRMJTime format it. */
    size_t result_len = DateTimeHelper::formatTime(
        forceUTC, buf, sizeof buf, format, epochMilliseconds, localTime);

    /* If it failed, default to toString. */
    if (result_len == 0) {
      return FormatDate(cx, forceUTC, locale, utcTime, FormatSpec::DateTime,
                        rval);
    }

    /* Hacked check against undesired 2-digit year 00/00/00 form. */
    if (strcmp(format, "%x") == 0 && result_len >= 6 &&
        /* Format %x means use OS settings, which may have 2-digit yr, so
           hack end of 3/11/22 or 11.03.22 or 11Mar22 to use 4-digit yr...*/
        !IsAsciiDigit(buf[result_len - 3]) &&
        IsAsciiDigit(buf[result_len - 2]) &&
        IsAsciiDigit(buf[result_len - 1]) &&
        /* ...but not if starts with 4-digit year, like 2022/3/11. */
        !(IsAsciiDigit(buf[0]) && IsAsciiDigit(buf[1]) &&
          IsAsciiDigit(buf[2]) && IsAsciiDigit(buf[3]))) {
      int year = int(::YearFromTime(localTime));
      snprintf(buf + (result_len - 2), (sizeof buf) - (result_len - 2), "%d",
               year);
    }
  }

  if (cx->runtime()->localeCallbacks &&
      cx->runtime()->localeCallbacks->localeToUnicode) {
    return cx->runtime()->localeCallbacks->localeToUnicode(cx, buf, rval);
  }

  JSString* str = NewStringCopyZ<CanGC>(cx, buf);
  if (!str) {
    return false;
  }
  rval.setString(str);
  return true;
}

/**
 * 21.4.4.39 Date.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_toLocaleString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toLocaleString");
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "toLocaleString");
  if (!unwrapped) {
    return false;
  }

  /*
   * Use '%#c' for windows, because '%c' is backward-compatible and non-y2k
   * with msvc; '%#c' requests that a full year be used in the result string.
   */
  static const char format[] =
#  if defined(_WIN32)
      "%#c"
#  else
      "%c"
#  endif
      ;

  return ToLocaleFormatHelper(cx, unwrapped, format, args.rval());
}

/**
 * 21.4.4.38 Date.prototype.toLocaleDateString ( [ reserved1 [ , reserved2 ] ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_toLocaleDateString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype",
                                        "toLocaleDateString");
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "toLocaleDateString");
  if (!unwrapped) {
    return false;
  }

  /*
   * Use '%#x' for windows, because '%x' is backward-compatible and non-y2k
   * with msvc; '%#x' requests that a full year be used in the result string.
   */
  static const char format[] =
#  if defined(_WIN32)
      "%#x"
#  else
      "%x"
#  endif
      ;

  return ToLocaleFormatHelper(cx, unwrapped, format, args.rval());
}

/**
 * 21.4.4.40 Date.prototype.toLocaleTimeString ( [ reserved1 [ , reserved2 ] ] )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_toLocaleTimeString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype",
                                        "toLocaleTimeString");
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "toLocaleTimeString");
  if (!unwrapped) {
    return false;
  }

  return ToLocaleFormatHelper(cx, unwrapped, "%X", args.rval());
}
#endif /* !JS_HAS_INTL_API */

/**
 * 21.4.4.42 Date.prototype.toTimeString ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_toTimeString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toTimeString");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "toTimeString");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-6.
  const char* locale = unwrapped->realm()->getLocale();
  if (!locale) {
    return false;
  }
  return FormatDate(cx, unwrapped->forceUTC(), locale,
                    unwrapped->UTCTime().toDouble(), FormatSpec::Time,
                    args.rval());
}

/**
 * 21.4.4.35 Date.prototype.toDateString ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_toDateString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toDateString");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "toDateString");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-6.
  const char* locale = unwrapped->realm()->getLocale();
  if (!locale) {
    return false;
  }
  return FormatDate(cx, unwrapped->forceUTC(), locale,
                    unwrapped->UTCTime().toDouble(), FormatSpec::Date,
                    args.rval());
}

static bool date_toSource(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toSource");
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "toSource");
  if (!unwrapped) {
    return false;
  }

  JSStringBuilder sb(cx);
  if (!sb.append("(new Date(") ||
      !NumberValueToStringBuilder(unwrapped->UTCTime(), sb) ||
      !sb.append("))")) {
    return false;
  }

  JSString* str = sb.finishString();
  if (!str) {
    return false;
  }
  args.rval().setString(str);
  return true;
}

/**
 * 21.4.4.41 Date.prototype.toString ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool date_toString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toString");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "toString");
  if (!unwrapped) {
    return false;
  }

  // Steps 3-4.
  const char* locale = unwrapped->realm()->getLocale();
  if (!locale) {
    return false;
  }
  return FormatDate(cx, unwrapped->forceUTC(), locale,
                    unwrapped->UTCTime().toDouble(), FormatSpec::DateTime,
                    args.rval());
}

/**
 * 21.4.4.44 Date.prototype.valueOf ( )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
bool js::date_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "valueOf");
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  args.rval().set(unwrapped->UTCTime());
  return true;
}

/**
 * 21.4.4.45 Date.prototype [ %Symbol.toPrimitive% ] ( hint )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
bool js::date_toPrimitive(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  if (!args.thisv().isObject()) {
    ReportIncompatible(cx, args);
    return false;
  }

  // Steps 3-5.
  JSType hint;
  if (!GetFirstArgumentAsTypeHint(cx, args, &hint)) {
    return false;
  }
  if (hint == JSTYPE_UNDEFINED) {
    hint = JSTYPE_STRING;
  }

  // Step 6.
  RootedObject obj(cx, &args.thisv().toObject());
  return OrdinaryToPrimitive(cx, obj, hint, args.rval());
}

#if JS_HAS_INTL_API
/**
 * Date.prototype.toTemporalInstant ( )
 */
static bool date_toTemporalInstant(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "toTemporalInstant");
  if (!unwrapped) {
    return false;
  }

  // Step 3.
  double t = unwrapped->UTCTime().toDouble();
  MOZ_ASSERT(IsTimeValue(t));

  // Step 4.
  if (std::isnan(t)) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_INVALID_DATE);
    return false;
  }
  int64_t tv = static_cast<int64_t>(t);

  auto epochNs = temporal::EpochNanoseconds::fromMilliseconds(tv);
  MOZ_ASSERT(temporal::IsValidEpochNanoseconds(epochNs));

  // Step 5.
  auto* result = temporal::CreateTemporalInstant(cx, epochNs);
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}
#endif /* JS_HAS_INTL_API */

static const JSFunctionSpec date_static_methods[] = {
    JS_FN("UTC", date_UTC, 7, 0),
    JS_FN("parse", date_parse, 1, 0),
    JS_FN("now", date_now, 0, 0),
    JS_FS_END,
};

static const JSFunctionSpec date_methods[] = {
    JS_INLINABLE_FN("getTime", date_getTime, 0, 0, DateGetTime),
    JS_FN("getTimezoneOffset", date_getTimezoneOffset, 0, 0),
    JS_FN("getYear", date_getYear, 0, 0),
    JS_INLINABLE_FN("getFullYear", date_getFullYear, 0, 0, DateGetFullYear),
    JS_FN("getUTCFullYear", date_getUTCFullYear, 0, 0),
    JS_INLINABLE_FN("getMonth", date_getMonth, 0, 0, DateGetMonth),
    JS_FN("getUTCMonth", date_getUTCMonth, 0, 0),
    JS_INLINABLE_FN("getDate", date_getDate, 0, 0, DateGetDate),
    JS_FN("getUTCDate", date_getUTCDate, 0, 0),
    JS_INLINABLE_FN("getDay", date_getDay, 0, 0, DateGetDay),
    JS_FN("getUTCDay", date_getUTCDay, 0, 0),
    JS_INLINABLE_FN("getHours", date_getHours, 0, 0, DateGetHours),
    JS_FN("getUTCHours", date_getUTCHours, 0, 0),
    JS_INLINABLE_FN("getMinutes", date_getMinutes, 0, 0, DateGetMinutes),
    JS_FN("getUTCMinutes", date_getUTCMinutes, 0, 0),
    JS_INLINABLE_FN("getSeconds", date_getSeconds, 0, 0, DateGetSeconds),
    JS_FN("getUTCSeconds", date_getUTCSeconds, 0, 0),
    JS_FN("getMilliseconds", date_getMilliseconds, 0, 0),
    JS_FN("getUTCMilliseconds", date_getUTCMilliseconds, 0, 0),
    JS_FN("setTime", date_setTime, 1, 0),
    JS_FN("setYear", date_setYear, 1, 0),
    JS_FN("setFullYear", date_setFullYear, 3, 0),
    JS_FN("setUTCFullYear", date_setUTCFullYear, 3, 0),
    JS_FN("setMonth", date_setMonth, 2, 0),
    JS_FN("setUTCMonth", date_setUTCMonth, 2, 0),
    JS_FN("setDate", date_setDate, 1, 0),
    JS_FN("setUTCDate", date_setUTCDate, 1, 0),
    JS_FN("setHours", date_setHours, 4, 0),
    JS_FN("setUTCHours", date_setUTCHours, 4, 0),
    JS_FN("setMinutes", date_setMinutes, 3, 0),
    JS_FN("setUTCMinutes", date_setUTCMinutes, 3, 0),
    JS_FN("setSeconds", date_setSeconds, 2, 0),
    JS_FN("setUTCSeconds", date_setUTCSeconds, 2, 0),
    JS_FN("setMilliseconds", date_setMilliseconds, 1, 0),
    JS_FN("setUTCMilliseconds", date_setUTCMilliseconds, 1, 0),
    JS_FN("toUTCString", date_toUTCString, 0, 0),
#if JS_HAS_INTL_API
    JS_FN("toTemporalInstant", date_toTemporalInstant, 0, 0),
    JS_SELF_HOSTED_FN("toLocaleString", "Date_toLocaleString", 0, 0),
    JS_SELF_HOSTED_FN("toLocaleDateString", "Date_toLocaleDateString", 0, 0),
    JS_SELF_HOSTED_FN("toLocaleTimeString", "Date_toLocaleTimeString", 0, 0),
#else
    JS_FN("toLocaleString", date_toLocaleString, 0, 0),
    JS_FN("toLocaleDateString", date_toLocaleDateString, 0, 0),
    JS_FN("toLocaleTimeString", date_toLocaleTimeString, 0, 0),
#endif
    JS_FN("toDateString", date_toDateString, 0, 0),
    JS_FN("toTimeString", date_toTimeString, 0, 0),
    JS_FN("toISOString", date_toISOString, 0, 0),
    JS_FN("toJSON", date_toJSON, 1, 0),
    JS_FN("toSource", date_toSource, 0, 0),
    JS_FN("toString", date_toString, 0, 0),
    JS_INLINABLE_FN("valueOf", date_valueOf, 0, 0, DateGetTime),
    JS_SYM_FN(toPrimitive, date_toPrimitive, 1, JSPROP_READONLY),
    JS_FS_END,
};

static bool NewDateObject(JSContext* cx, const CallArgs& args, ClippedTime t) {
  MOZ_ASSERT(args.isConstructing());

  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Date, &proto)) {
    return false;
  }

  JSObject* obj = NewDateObjectMsec(cx, t, proto);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * 21.4.4.41.4 ToDateString ( tv )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool ToDateString(JSContext* cx, const CallArgs& args, ClippedTime t) {
  const char* locale = cx->realm()->getLocale();
  if (!locale) {
    return false;
  }
  return FormatDate(cx, ForceUTC(cx->realm()), locale, t.toDouble(),
                    FormatSpec::DateTime, args.rval());
}

/**
 * 21.4.2.1 Date ( ...values )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool DateNoArguments(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(args.isConstructing());
  MOZ_ASSERT(args.length() == 0);

  // Step 3.
  ClippedTime now = NowAsMillis(cx);

  // Steps 6-8.
  return NewDateObject(cx, args, now);
}

/**
 * 21.4.2.1 Date ( ...values )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool DateOneArgument(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(args.isConstructing());
  MOZ_ASSERT(args.length() == 1);

  // Step 4.a.
  MutableHandle<Value> value = args[0];

  // Step 4.b.
  if (value.isObject()) {
    RootedObject obj(cx, &value.toObject());

    ESClass cls;
    if (!GetBuiltinClass(cx, obj, &cls)) {
      return false;
    }

    if (cls == ESClass::Date) {
      RootedValue unboxed(cx);
      if (!Unbox(cx, obj, &unboxed)) {
        return false;
      }

      // Steps 6-8.
      return NewDateObject(cx, args, TimeClip(unboxed.toNumber()));
    }
  }

  // Step 4.c.i.
  if (!ToPrimitive(cx, value)) {
    return false;
  }

  // Steps 4.c.ii-iii.
  ClippedTime t;
  if (value.isString()) {
    JSLinearString* linearStr = value.toString()->ensureLinear(cx);
    if (!linearStr) {
      return false;
    }

    if (!ParseDate(cx, ForceUTC(cx->realm()), linearStr, &t)) {
      t = ClippedTime::invalid();
    }
  } else {
    double d;
    if (!ToNumber(cx, value, &d)) {
      return false;
    }
    t = TimeClip(d);
  }

  // Steps 6-8.
  return NewDateObject(cx, args, t);
}

/**
 * 21.4.2.1 Date ( ...values )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool DateMultipleArguments(JSContext* cx, const CallArgs& args) {
  // Step 5.a.
  MOZ_ASSERT(args.isConstructing());
  MOZ_ASSERT(args.length() >= 2);

  // Step 5.b.
  double y;
  if (!ToNumber(cx, args[0], &y)) {
    return false;
  }

  // Step 5.c.
  double m;
  if (!ToNumber(cx, args[1], &m)) {
    return false;
  }

  // Step 5.d.
  double dt;
  if (args.length() >= 3) {
    if (!ToNumber(cx, args[2], &dt)) {
      return false;
    }
  } else {
    dt = 1;
  }

  // Step 5.e.
  double h;
  if (args.length() >= 4) {
    if (!ToNumber(cx, args[3], &h)) {
      return false;
    }
  } else {
    h = 0;
  }

  // Step 5.f.
  double min;
  if (args.length() >= 5) {
    if (!ToNumber(cx, args[4], &min)) {
      return false;
    }
  } else {
    min = 0;
  }

  // Step 5.g.
  double s;
  if (args.length() >= 6) {
    if (!ToNumber(cx, args[5], &s)) {
      return false;
    }
  } else {
    s = 0;
  }

  // Step 5.h.
  double milli;
  if (args.length() >= 7) {
    if (!ToNumber(cx, args[6], &milli)) {
      return false;
    }
  } else {
    milli = 0;
  }

  // Step 5.i.
  double yr = MakeFullYear(y);

  // Step 5.j.
  double finalDate = MakeDate(MakeDay(yr, m, dt), MakeTime(h, min, s, milli));

  // Steps 5.k and 6-8.
  return NewDateObject(cx, args,
                       TimeClip(UTC(ForceUTC(cx->realm()), finalDate)));
}

/**
 * 21.4.2.1 Date ( ...values )
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
static bool DateConstructor(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSConstructorProfilerEntry pseudoFrame(cx, "Date");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!args.isConstructing()) {
    return ToDateString(cx, args, NowAsMillis(cx));
  }

  // Step 2.
  unsigned numberOfArgs = args.length();

  // Steps 3 and 6-8.
  if (numberOfArgs == 0) {
    return DateNoArguments(cx, args);
  }

  // Steps 4 and 6-8.
  if (numberOfArgs == 1) {
    return DateOneArgument(cx, args);
  }

  // Steps 5-8.
  return DateMultipleArguments(cx, args);
}

static bool FinishDateClassInit(JSContext* cx, HandleObject ctor,
                                HandleObject proto) {
  /*
   * Date.prototype.toGMTString has the same initial value as
   * Date.prototype.toUTCString.
   */
  RootedValue toUTCStringFun(cx);
  RootedId toUTCStringId(cx, NameToId(cx->names().toUTCString));
  RootedId toGMTStringId(cx, NameToId(cx->names().toGMTString));
  return NativeGetProperty(cx, proto.as<NativeObject>(), toUTCStringId,
                           &toUTCStringFun) &&
         NativeDefineDataProperty(cx, proto.as<NativeObject>(), toGMTStringId,
                                  toUTCStringFun, 0);
}

static const ClassSpec DateObjectClassSpec = {
    GenericCreateConstructor<DateConstructor, 7, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<DateObject>,
    date_static_methods,
    nullptr,
    date_methods,
    nullptr,
    FinishDateClassInit,
};

const JSClass DateObject::class_ = {
    "Date",
    JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Date),
    JS_NULL_CLASS_OPS,
    &DateObjectClassSpec,
};

const JSClass DateObject::protoClass_ = {
    "Date.prototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Date),
    JS_NULL_CLASS_OPS,
    &DateObjectClassSpec,
};

JSObject* js::NewDateObjectMsec(JSContext* cx, ClippedTime t,
                                HandleObject proto /* = nullptr */) {
  DateObject* obj = NewObjectWithClassProto<DateObject>(cx, proto);
  if (!obj) {
    return nullptr;
  }
  obj->setUTCTime(t);
  return obj;
}

JS_PUBLIC_API JSObject* JS::NewDateObject(JSContext* cx, ClippedTime time) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return NewDateObjectMsec(cx, time);
}

JS_PUBLIC_API JSObject* js::NewDateObject(JSContext* cx, int year, int mon,
                                          int mday, int hour, int min,
                                          int sec) {
  MOZ_ASSERT(mon < 12);
  double msec_time =
      MakeDate(MakeDay(year, mon, mday), MakeTime(hour, min, sec, 0.0));
  return NewDateObjectMsec(cx, TimeClip(UTC(ForceUTC(cx->realm()), msec_time)));
}

JS_PUBLIC_API bool js::DateIsValid(JSContext* cx, HandleObject obj,
                                   bool* isValid) {
  ESClass cls;
  if (!GetBuiltinClass(cx, obj, &cls)) {
    return false;
  }

  if (cls != ESClass::Date) {
    *isValid = false;
    return true;
  }

  RootedValue unboxed(cx);
  if (!Unbox(cx, obj, &unboxed)) {
    return false;
  }

  *isValid = !std::isnan(unboxed.toNumber());
  return true;
}

JS_PUBLIC_API JSObject* JS::NewDateObject(JSContext* cx, int year, int mon,
                                          int mday, int hour, int min,
                                          int sec) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return js::NewDateObject(cx, year, mon, mday, hour, min, sec);
}

JS_PUBLIC_API bool JS::ObjectIsDate(JSContext* cx, Handle<JSObject*> obj,
                                    bool* isDate) {
  cx->check(obj);

  ESClass cls;
  if (!GetBuiltinClass(cx, obj, &cls)) {
    return false;
  }

  *isDate = cls == ESClass::Date;
  return true;
}

JS_PUBLIC_API bool js::DateGetMsecSinceEpoch(JSContext* cx, HandleObject obj,
                                             double* msecsSinceEpoch) {
  ESClass cls;
  if (!GetBuiltinClass(cx, obj, &cls)) {
    return false;
  }

  if (cls != ESClass::Date) {
    *msecsSinceEpoch = 0;
    return true;
  }

  RootedValue unboxed(cx);
  if (!Unbox(cx, obj, &unboxed)) {
    return false;
  }

  *msecsSinceEpoch = unboxed.toNumber();
  return true;
}

JS_PUBLIC_API bool JS::IsISOStyleDate(JSContext* cx,
                                      const JS::Latin1Chars& str) {
  ClippedTime result;
  return ParseISOStyleDate(ForceUTC(cx->realm()), str.begin().get(),
                           str.length(), &result);
}
