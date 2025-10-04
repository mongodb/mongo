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
#include "mozilla/Sprintf.h"
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

#ifdef JS_HAS_TEMPORAL_API
#  include "builtin/temporal/Instant.h"
#endif
#include "js/CallAndConstruct.h"  // JS::IsCallable
#include "js/Conversions.h"
#include "js/Date.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/LocaleSensitive.h"
#include "js/Object.h"  // JS::GetBuiltinClass
#include "js/PropertySpec.h"
#include "js/Wrapper.h"
#include "util/DifferentialTesting.h"
#include "util/StringBuffer.h"
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

/*
 * The JS 'Date' object is patterned after the Java 'Date' object.
 * Here is a script:
 *
 *    today = new Date();
 *
 *    print(today.toLocaleString());
 *
 *    weekDay = today.getDay();
 *
 *
 * These Java (and ECMA-262) methods are supported:
 *
 *     UTC
 *     getDate (getUTCDate)
 *     getDay (getUTCDay)
 *     getHours (getUTCHours)
 *     getMinutes (getUTCMinutes)
 *     getMonth (getUTCMonth)
 *     getSeconds (getUTCSeconds)
 *     getMilliseconds (getUTCMilliseconds)
 *     getTime
 *     getTimezoneOffset
 *     getYear
 *     getFullYear (getUTCFullYear)
 *     parse
 *     setDate (setUTCDate)
 *     setHours (setUTCHours)
 *     setMinutes (setUTCMinutes)
 *     setMonth (setUTCMonth)
 *     setSeconds (setUTCSeconds)
 *     setMilliseconds (setUTCMilliseconds)
 *     setTime
 *     setYear (setFullYear, setUTCFullYear)
 *     toGMTString (toUTCString)
 *     toLocaleString
 *     toString
 *
 *
 * These Java methods are not supported
 *
 *     setDay
 *     before
 *     after
 *     equals
 *     hashCode
 */

namespace {

class DateTimeHelper {
 private:
#if JS_HAS_INTL_API
  static double localTZA(DateTimeInfo::ForceUTC forceUTC, double t,
                         DateTimeInfo::TimeZoneOffset offset);
#else
  static int equivalentYearForDST(int year);
  static bool isRepresentableAsTime32(double t);
  static double daylightSavingTA(DateTimeInfo::ForceUTC forceUTC, double t);
  static double adjustTime(DateTimeInfo::ForceUTC forceUTC, double date);
  static PRMJTime toPRMJTime(DateTimeInfo::ForceUTC forceUTC, double localTime,
                             double utcTime);
#endif

 public:
  static double localTime(DateTimeInfo::ForceUTC forceUTC, double t);
  static double UTC(DateTimeInfo::ForceUTC forceUTC, double t);
  static JSString* timeZoneComment(JSContext* cx,
                                   DateTimeInfo::ForceUTC forceUTC,
                                   const char* locale, double utcTime,
                                   double localTime);
#if !JS_HAS_INTL_API
  static size_t formatTime(DateTimeInfo::ForceUTC forceUTC, char* buf,
                           size_t buflen, const char* fmt, double utcTime,
                           double localTime);
#endif
};

}  // namespace

static DateTimeInfo::ForceUTC ForceUTC(const Realm* realm) {
  return realm->creationOptions().forceUTC() ? DateTimeInfo::ForceUTC::Yes
                                             : DateTimeInfo::ForceUTC::No;
}

// ES2019 draft rev 0ceb728a1adbffe42b26972a6541fd7f398b1557
// 5.2.5 Mathematical Operations
static inline double PositiveModulo(double dividend, double divisor) {
  MOZ_ASSERT(divisor > 0);
  MOZ_ASSERT(std::isfinite(divisor));

  double result = fmod(dividend, divisor);
  if (result < 0) {
    result += divisor;
  }
  return result + (+0.0);
}

static inline double Day(double t) { return floor(t / msPerDay); }

static double TimeWithinDay(double t) { return PositiveModulo(t, msPerDay); }

/* ES5 15.9.1.3. */
static inline bool IsLeapYear(double year) {
  MOZ_ASSERT(ToInteger(year) == year);
  return fmod(year, 4) == 0 && (fmod(year, 100) != 0 || fmod(year, 400) == 0);
}

static inline double DayFromYear(double y) {
  return 365 * (y - 1970) + floor((y - 1969) / 4.0) -
         floor((y - 1901) / 100.0) + floor((y - 1601) / 400.0);
}

static inline double TimeFromYear(double y) {
  return ::DayFromYear(y) * msPerDay;
}

namespace {
struct YearMonthDay {
  int32_t year;
  uint32_t month;
  uint32_t day;
};
}  // namespace

/*
 * This function returns the year, month and day corresponding to a given
 * time value. The implementation closely follows (w.r.t. types and variable
 * names) the algorithm shown in Figure 12 of [1].
 *
 * A key point of the algorithm is that it works on the so called
 * Computational calendar where years run from March to February -- this
 * largely avoids complications with leap years. The algorithm finds the
 * date in the Computation calendar and then maps it to the Gregorian
 * calendar.
 *
 * [1] Neri C, Schneider L., "Euclidean affine functions and their
 * application to calendar algorithms."
 * Softw Pract Exper. 2023;53(4):937-970. doi: 10.1002/spe.3172
 * https://onlinelibrary.wiley.com/doi/full/10.1002/spe.3172
 */
static YearMonthDay ToYearMonthDay(double t) {
  MOZ_ASSERT(ToInteger(t) == t);

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
  MOZ_ASSERT(double(minTime) <= t && t <= double(maxTime));
  const int64_t time = int64_t(t);

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
  const uint32_t M_G = J ? M - 12 : M;
  const uint32_t D_G = D + 1;

  return {Y_G, M_G, D_G};
}

static double YearFromTime(double t) {
  if (!std::isfinite(t)) {
    return GenericNaN();
  }
  auto const year = ToYearMonthDay(t).year;
  return double(year);
}

/* ES5 15.9.1.4. */
static double DayWithinYear(double t, double year) {
  MOZ_ASSERT_IF(std::isfinite(t), ::YearFromTime(t) == year);
  return Day(t) - ::DayFromYear(year);
}

static double MonthFromTime(double t) {
  if (!std::isfinite(t)) {
    return GenericNaN();
  }
  const auto month = ToYearMonthDay(t).month;
  return double(month);
}

/* ES5 15.9.1.5. */
static double DateFromTime(double t) {
  if (!std::isfinite(t)) {
    return GenericNaN();
  }
  const auto day = ToYearMonthDay(t).day;
  return double(day);
}

/* ES5 15.9.1.6. */
static int WeekDay(double t) {
  /*
   * We can't assert TimeClip(t) == t because we call this function with
   * local times, which can be offset outside TimeClip's permitted range.
   */
  MOZ_ASSERT(ToInteger(t) == t);
  int result = (int(Day(t)) + 4) % 7;
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

/* ES5 15.9.1.12 (out of order to accommodate DaylightSavingTA). */
static double MakeDay(double year, double month, double date) {
  /* Step 1. */
  if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date)) {
    return GenericNaN();
  }

  /* Steps 2-4. */
  double y = ToInteger(year);
  double m = ToInteger(month);
  double dt = ToInteger(date);

  /* Step 5. */
  double ym = y + floor(m / 12);

  /* Step 6. */
  int mn = int(PositiveModulo(m, 12));

  /* Steps 7-8. */
  bool leap = IsLeapYear(ym);

  double yearday = floor(TimeFromYear(ym) / msPerDay);
  double monthday = DayFromMonth(mn, leap);

  return yearday + monthday + dt - 1;
}

/* ES5 15.9.1.13 (out of order to accommodate DaylightSavingTA). */
static inline double MakeDate(double day, double time) {
  /* Step 1. */
  if (!std::isfinite(day) || !std::isfinite(time)) {
    return GenericNaN();
  }

  /* Step 2. */
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
  return ::YearFromTime(clipped.toDouble());
}

JS_PUBLIC_API double JS::MonthFromTime(double time) {
  const auto clipped = TimeClip(time);
  if (!clipped.isValid()) {
    return GenericNaN();
  }
  return ::MonthFromTime(clipped.toDouble());
}

JS_PUBLIC_API double JS::DayFromTime(double time) {
  const auto clipped = TimeClip(time);
  if (!clipped.isValid()) {
    return GenericNaN();
  }
  return DateFromTime(clipped.toDouble());
}

JS_PUBLIC_API double JS::DayFromYear(double year) {
  return ::DayFromYear(year);
}

JS_PUBLIC_API double JS::DayWithinYear(double time, double year) {
  const auto clipped = TimeClip(time);
  if (!clipped.isValid()) {
    return GenericNaN();
  }
  return ::DayWithinYear(clipped.toDouble(), year);
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
// ES2019 draft rev 0ceb728a1adbffe42b26972a6541fd7f398b1557
// 20.3.1.7 LocalTZA ( t, isUTC )
double DateTimeHelper::localTZA(DateTimeInfo::ForceUTC forceUTC, double t,
                                DateTimeInfo::TimeZoneOffset offset) {
  MOZ_ASSERT(std::isfinite(t));

  int64_t milliseconds = static_cast<int64_t>(t);
  int32_t offsetMilliseconds =
      DateTimeInfo::getOffsetMilliseconds(forceUTC, milliseconds, offset);
  return static_cast<double>(offsetMilliseconds);
}

// ES2019 draft rev 0ceb728a1adbffe42b26972a6541fd7f398b1557
// 20.3.1.8 LocalTime ( t )
double DateTimeHelper::localTime(DateTimeInfo::ForceUTC forceUTC, double t) {
  if (!std::isfinite(t)) {
    return GenericNaN();
  }

  MOZ_ASSERT(StartOfTime <= t && t <= EndOfTime);
  return t + localTZA(forceUTC, t, DateTimeInfo::TimeZoneOffset::UTC);
}

// ES2019 draft rev 0ceb728a1adbffe42b26972a6541fd7f398b1557
// 20.3.1.9 UTC ( t )
double DateTimeHelper::UTC(DateTimeInfo::ForceUTC forceUTC, double t) {
  if (!std::isfinite(t)) {
    return GenericNaN();
  }

  if (t < (StartOfTime - msPerDay) || t > (EndOfTime + msPerDay)) {
    return GenericNaN();
  }

  return t - localTZA(forceUTC, t, DateTimeInfo::TimeZoneOffset::Local);
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
bool DateTimeHelper::isRepresentableAsTime32(double t) {
  return 0.0 <= t && t < 2145916800000.0;
}

/* ES5 15.9.1.8. */
double DateTimeHelper::daylightSavingTA(DateTimeInfo::ForceUTC forceUTC,
                                        double t) {
  if (!std::isfinite(t)) {
    return GenericNaN();
  }

  /*
   * If earlier than 1970 or after 2038, potentially beyond the ken of
   * many OSes, map it to an equivalent year before asking.
   */
  if (!isRepresentableAsTime32(t)) {
    int year = equivalentYearForDST(int(::YearFromTime(t)));
    double day = MakeDay(year, ::MonthFromTime(t), DateFromTime(t));
    t = MakeDate(day, TimeWithinDay(t));
  }

  int64_t utcMilliseconds = static_cast<int64_t>(t);
  int32_t offsetMilliseconds =
      DateTimeInfo::getDSTOffsetMilliseconds(forceUTC, utcMilliseconds);
  return static_cast<double>(offsetMilliseconds);
}

double DateTimeHelper::adjustTime(DateTimeInfo::ForceUTC forceUTC,
                                  double date) {
  double localTZA = DateTimeInfo::localTZA(forceUTC);
  double t = daylightSavingTA(forceUTC, date) + localTZA;
  t = (localTZA >= 0) ? fmod(t, msPerDay) : -fmod(msPerDay - t, msPerDay);
  return t;
}

/* ES5 15.9.1.9. */
double DateTimeHelper::localTime(DateTimeInfo::ForceUTC forceUTC, double t) {
  return t + adjustTime(forceUTC, t);
}

double DateTimeHelper::UTC(DateTimeInfo::ForceUTC forceUTC, double t) {
  // Following the ES2017 specification creates undesirable results at DST
  // transitions. For example when transitioning from PST to PDT,
  // |new Date(2016,2,13,2,0,0).toTimeString()| returns the string value
  // "01:00:00 GMT-0800 (PST)" instead of "03:00:00 GMT-0700 (PDT)". Follow
  // V8 and subtract one hour before computing the offset.
  // Spec bug: https://bugs.ecmascript.org/show_bug.cgi?id=4007

  return t -
         adjustTime(forceUTC, t - DateTimeInfo::localTZA(forceUTC) - msPerHour);
}
#endif /* JS_HAS_INTL_API */

static double LocalTime(DateTimeInfo::ForceUTC forceUTC, double t) {
  return DateTimeHelper::localTime(forceUTC, t);
}

static double UTC(DateTimeInfo::ForceUTC forceUTC, double t) {
  return DateTimeHelper::UTC(forceUTC, t);
}

/* ES5 15.9.1.10. */
static double HourFromTime(double t) {
  return PositiveModulo(floor(t / msPerHour), HoursPerDay);
}

static double MinFromTime(double t) {
  return PositiveModulo(floor(t / msPerMinute), MinutesPerHour);
}

static double SecFromTime(double t) {
  return PositiveModulo(floor(t / msPerSecond), SecondsPerMinute);
}

static double msFromTime(double t) { return PositiveModulo(t, msPerSecond); }

/* ES5 15.9.1.11. */
static double MakeTime(double hour, double min, double sec, double ms) {
  /* Step 1. */
  if (!std::isfinite(hour) || !std::isfinite(min) || !std::isfinite(sec) ||
      !std::isfinite(ms)) {
    return GenericNaN();
  }

  /* Step 2. */
  double h = ToInteger(hour);

  /* Step 3. */
  double m = ToInteger(min);

  /* Step 4. */
  double s = ToInteger(sec);

  /* Step 5. */
  double milli = ToInteger(ms);

  /* Steps 6-7. */
  return h * msPerHour + m * msPerMinute + s * msPerSecond + milli;
}

/**
 * end of ECMA 'support' functions
 */

// ES2017 draft rev (TODO: Add git hash when PR 642 is merged.)
// 20.3.3.4
// Date.UTC(year [, month [, date [, hours [, minutes [, seconds [, ms]]]]]])
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
  double yr = y;
  if (!std::isnan(y)) {
    double yint = ToInteger(y);
    if (0 <= yint && yint <= 99) {
      yr = 1900 + yint;
    }
  }

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

  double date = MakeDate(MakeDay(dateMul * double(year), month, day),
                         MakeTime(hour, min, sec, msec));

  if (isLocalTime) {
    date = UTC(forceUTC, date);
  } else {
    date -= tzMul * (tzHour * msPerHour + tzMin * msPerMinute);
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
int FixupYear(int year) {
  if (year < 50) {
    year += 2000;
  } else if (year >= 50 && year < 100) {
    year += 1900;
  }
  return year;
}

template <typename CharT>
bool MatchesKeyword(const CharT* s, size_t len, const char* keyword) {
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
bool StartsWithMonthPrefix(const CharT* s, const char* prefix) {
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
bool IsMonthName(const CharT* s, size_t len, int* mon) {
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
constexpr size_t MinKeywordLength(const CharsAndAction (&keywords)[N]) {
  size_t min = size_t(-1);
  for (const CharsAndAction& keyword : keywords) {
    min = std::min(min, std::char_traits<char>::length(keyword.chars));
  }
  return min;
}

template <typename CharT>
static bool ParseDate(DateTimeInfo::ForceUTC forceUTC, const CharT* s,
                      size_t length, ClippedTime* result) {
  if (length == 0) {
    return false;
  }

  if (ParseISOStyleDate(forceUTC, s, length, result)) {
    return true;
  }

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
    date += tzOffset * msPerMinute;
  }

  *result = TimeClip(date);
  return true;
}

static bool ParseDate(DateTimeInfo::ForceUTC forceUTC, JSLinearString* s,
                      ClippedTime* result) {
  AutoCheckCannotGC nogc;
  return s->hasLatin1Chars()
             ? ParseDate(forceUTC, s->latin1Chars(nogc), s->length(), result)
             : ParseDate(forceUTC, s->twoByteChars(nogc), s->length(), result);
}

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
  if (!ParseDate(ForceUTC(cx->realm()), linearStr, &result)) {
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

bool js::date_now(JSContext* cx, unsigned argc, Value* vp) {
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

  double utcTime = UTCTime().toNumber();

  if (!std::isfinite(utcTime)) {
    for (size_t ind = COMPONENTS_START_SLOT; ind < RESERVED_SLOTS; ind++) {
      setReservedSlot(ind, DoubleValue(utcTime));
    }
    return;
  }

  double localTime = LocalTime(forceUTC(), utcTime);

  setReservedSlot(LOCAL_TIME_SLOT, DoubleValue(localTime));

  const auto [year, month, day] = ToYearMonthDay(localTime);

  setReservedSlot(LOCAL_YEAR_SLOT, Int32Value(year));
  setReservedSlot(LOCAL_MONTH_SLOT, Int32Value(int32_t(month)));
  setReservedSlot(LOCAL_DATE_SLOT, Int32Value(int32_t(day)));

  int weekday = WeekDay(localTime);
  setReservedSlot(LOCAL_DAY_SLOT, Int32Value(weekday));

  double yearStartTime = TimeFromYear(year);
  uint64_t yearTime = uint64_t(localTime - yearStartTime);
  int32_t yearSeconds = int32_t(yearTime / 1000);
  setReservedSlot(LOCAL_SECONDS_INTO_YEAR_SLOT, Int32Value(yearSeconds));
}

MOZ_ALWAYS_INLINE bool IsDate(HandleValue v) {
  return v.isObject() && v.toObject().is<DateObject>();
}

/*
 * See ECMA 15.9.5.4 thru 15.9.5.23
 */

static bool date_getTime(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getTime");
  if (!unwrapped) {
    return false;
  }

  args.rval().set(unwrapped->UTCTime());
  return true;
}

static bool date_getYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getYear");
  if (!unwrapped) {
    return false;
  }

  unwrapped->fillLocalTimeSlots();

  Value yearVal = unwrapped->localYear();
  if (yearVal.isInt32()) {
    /* Follow ECMA-262 to the letter, contrary to IE JScript. */
    int year = yearVal.toInt32() - 1900;
    args.rval().setInt32(year);
  } else {
    args.rval().set(yearVal);
  }
  return true;
}

static bool date_getFullYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getFullYear");
  if (!unwrapped) {
    return false;
  }

  unwrapped->fillLocalTimeSlots();
  args.rval().set(unwrapped->localYear());
  return true;
}

static bool date_getUTCFullYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCFullYear");
  if (!unwrapped) {
    return false;
  }

  double result = unwrapped->UTCTime().toNumber();
  if (std::isfinite(result)) {
    result = ::YearFromTime(result);
  }

  args.rval().setNumber(result);
  return true;
}

static bool date_getMonth(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getMonth");
  if (!unwrapped) {
    return false;
  }

  unwrapped->fillLocalTimeSlots();
  args.rval().set(unwrapped->localMonth());
  return true;
}

static bool date_getUTCMonth(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCMonth");
  if (!unwrapped) {
    return false;
  }

  double d = unwrapped->UTCTime().toNumber();
  args.rval().setNumber(::MonthFromTime(d));
  return true;
}

static bool date_getDate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getDate");
  if (!unwrapped) {
    return false;
  }

  unwrapped->fillLocalTimeSlots();

  args.rval().set(unwrapped->localDate());
  return true;
}

static bool date_getUTCDate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCDate");
  if (!unwrapped) {
    return false;
  }

  double result = unwrapped->UTCTime().toNumber();
  if (std::isfinite(result)) {
    result = DateFromTime(result);
  }

  args.rval().setNumber(result);
  return true;
}

static bool date_getDay(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getDay");
  if (!unwrapped) {
    return false;
  }

  unwrapped->fillLocalTimeSlots();
  args.rval().set(unwrapped->localDay());
  return true;
}

static bool date_getUTCDay(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCDay");
  if (!unwrapped) {
    return false;
  }

  double result = unwrapped->UTCTime().toNumber();
  if (std::isfinite(result)) {
    result = WeekDay(result);
  }

  args.rval().setNumber(result);
  return true;
}

static bool date_getHours(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getHours");
  if (!unwrapped) {
    return false;
  }

  unwrapped->fillLocalTimeSlots();

  // Note: localSecondsIntoYear is guaranteed to return an
  // int32 or NaN after the call to fillLocalTimeSlots.
  Value yearSeconds = unwrapped->localSecondsIntoYear();
  if (yearSeconds.isDouble()) {
    MOZ_ASSERT(std::isnan(yearSeconds.toDouble()));
    args.rval().set(yearSeconds);
  } else {
    args.rval().setInt32((yearSeconds.toInt32() / int(SecondsPerHour)) %
                         int(HoursPerDay));
  }
  return true;
}

static bool date_getUTCHours(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCHours");
  if (!unwrapped) {
    return false;
  }

  double result = unwrapped->UTCTime().toNumber();
  if (std::isfinite(result)) {
    result = HourFromTime(result);
  }

  args.rval().setNumber(result);
  return true;
}

static bool date_getMinutes(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getMinutes");
  if (!unwrapped) {
    return false;
  }

  unwrapped->fillLocalTimeSlots();

  // Note: localSecondsIntoYear is guaranteed to return an
  // int32 or NaN after the call to fillLocalTimeSlots.
  Value yearSeconds = unwrapped->localSecondsIntoYear();
  if (yearSeconds.isDouble()) {
    MOZ_ASSERT(std::isnan(yearSeconds.toDouble()));
    args.rval().set(yearSeconds);
  } else {
    args.rval().setInt32((yearSeconds.toInt32() / int(SecondsPerMinute)) %
                         int(MinutesPerHour));
  }
  return true;
}

static bool date_getUTCMinutes(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCMinutes");
  if (!unwrapped) {
    return false;
  }

  double result = unwrapped->UTCTime().toNumber();
  if (std::isfinite(result)) {
    result = MinFromTime(result);
  }

  args.rval().setNumber(result);
  return true;
}

static bool date_getSeconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "getSeconds");
  if (!unwrapped) {
    return false;
  }

  unwrapped->fillLocalTimeSlots();

  // Note: localSecondsIntoYear is guaranteed to return an
  // int32 or NaN after the call to fillLocalTimeSlots.
  Value yearSeconds = unwrapped->localSecondsIntoYear();
  if (yearSeconds.isDouble()) {
    MOZ_ASSERT(std::isnan(yearSeconds.toDouble()));
    args.rval().set(yearSeconds);
  } else {
    args.rval().setInt32(yearSeconds.toInt32() % int(SecondsPerMinute));
  }
  return true;
}

static bool date_getUTCSeconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "getUTCSeconds");
  if (!unwrapped) {
    return false;
  }

  double result = unwrapped->UTCTime().toNumber();
  if (std::isfinite(result)) {
    result = SecFromTime(result);
  }

  args.rval().setNumber(result);
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

static bool getMilliseconds(JSContext* cx, unsigned argc, Value* vp,
                            const char* methodName) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, methodName);
  if (!unwrapped) {
    return false;
  }

  double result = unwrapped->UTCTime().toNumber();
  if (std::isfinite(result)) {
    result = msFromTime(result);
  }

  args.rval().setNumber(result);
  return true;
}

static bool date_getMilliseconds(JSContext* cx, unsigned argc, Value* vp) {
  return getMilliseconds(cx, argc, vp, "getMilliseconds");
}

static bool date_getUTCMilliseconds(JSContext* cx, unsigned argc, Value* vp) {
  return getMilliseconds(cx, argc, vp, "getUTCMilliseconds");
}

static bool date_getTimezoneOffset(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "getTimezoneOffset");
  if (!unwrapped) {
    return false;
  }

  unwrapped->fillLocalTimeSlots();

  double utctime = unwrapped->UTCTime().toNumber();
  double localtime = unwrapped->localTime().toDouble();

  /*
   * Return the time zone offset in minutes for the current locale that is
   * appropriate for this time. This value would be a constant except for
   * daylight savings time.
   */
  double result = (utctime - localtime) / msPerMinute;
  args.rval().setNumber(result);
  return true;
}

static bool date_setTime(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setTime"));
  if (!unwrapped) {
    return false;
  }

  if (args.length() == 0) {
    unwrapped->setUTCTime(ClippedTime::invalid(), args.rval());
    return true;
  }

  double result;
  if (!ToNumber(cx, args[0], &result)) {
    return false;
  }

  unwrapped->setUTCTime(TimeClip(result), args.rval());
  return true;
}

static bool GetMsecsOrDefault(JSContext* cx, const CallArgs& args, unsigned i,
                              double t, double* millis) {
  if (args.length() <= i) {
    *millis = msFromTime(t);
    return true;
  }
  return ToNumber(cx, args[i], millis);
}

static bool GetSecsOrDefault(JSContext* cx, const CallArgs& args, unsigned i,
                             double t, double* sec) {
  if (args.length() <= i) {
    *sec = SecFromTime(t);
    return true;
  }
  return ToNumber(cx, args[i], sec);
}

static bool GetMinsOrDefault(JSContext* cx, const CallArgs& args, unsigned i,
                             double t, double* mins) {
  if (args.length() <= i) {
    *mins = MinFromTime(t);
    return true;
  }
  return ToNumber(cx, args[i], mins);
}

/* ES6 20.3.4.23. */
static bool date_setMilliseconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setMilliseconds"));
  if (!unwrapped) {
    return false;
  }
  double t = LocalTime(unwrapped->forceUTC(), unwrapped->UTCTime().toNumber());

  // Step 2.
  double ms;
  if (!ToNumber(cx, args.get(0), &ms)) {
    return false;
  }

  // Step 3.
  double time = MakeTime(HourFromTime(t), MinFromTime(t), SecFromTime(t), ms);

  // Step 4.
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), MakeDate(Day(t), time)));

  // Steps 5-6.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/* ES5 15.9.5.29. */
static bool date_setUTCMilliseconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCMilliseconds"));
  if (!unwrapped) {
    return false;
  }

  /* Step 1. */
  double t = unwrapped->UTCTime().toNumber();

  /* Step 2. */
  double milli;
  if (!ToNumber(cx, args.get(0), &milli)) {
    return false;
  }
  double time =
      MakeTime(HourFromTime(t), MinFromTime(t), SecFromTime(t), milli);

  /* Step 3. */
  ClippedTime v = TimeClip(MakeDate(Day(t), time));

  /* Steps 4-5. */
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/* ES6 20.3.4.26. */
static bool date_setSeconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setSeconds"));
  if (!unwrapped) {
    return false;
  }

  // Steps 1-2.
  double t = LocalTime(unwrapped->forceUTC(), unwrapped->UTCTime().toNumber());

  // Steps 3-4.
  double s;
  if (!ToNumber(cx, args.get(0), &s)) {
    return false;
  }

  // Steps 5-6.
  double milli;
  if (!GetMsecsOrDefault(cx, args, 1, t, &milli)) {
    return false;
  }

  // Step 7.
  double date =
      MakeDate(Day(t), MakeTime(HourFromTime(t), MinFromTime(t), s, milli));

  // Step 8.
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), date));

  // Step 9.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/* ES5 15.9.5.32. */
static bool date_setUTCSeconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCSeconds"));
  if (!unwrapped) {
    return false;
  }

  /* Step 1. */
  double t = unwrapped->UTCTime().toNumber();

  /* Step 2. */
  double s;
  if (!ToNumber(cx, args.get(0), &s)) {
    return false;
  }

  /* Step 3. */
  double milli;
  if (!GetMsecsOrDefault(cx, args, 1, t, &milli)) {
    return false;
  }

  /* Step 4. */
  double date =
      MakeDate(Day(t), MakeTime(HourFromTime(t), MinFromTime(t), s, milli));

  /* Step 5. */
  ClippedTime v = TimeClip(date);

  /* Steps 6-7. */
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/* ES6 20.3.4.24. */
static bool date_setMinutes(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setMinutes"));
  if (!unwrapped) {
    return false;
  }

  // Steps 1-2.
  double t = LocalTime(unwrapped->forceUTC(), unwrapped->UTCTime().toNumber());

  // Steps 3-4.
  double m;
  if (!ToNumber(cx, args.get(0), &m)) {
    return false;
  }

  // Steps 5-6.
  double s;
  if (!GetSecsOrDefault(cx, args, 1, t, &s)) {
    return false;
  }

  // Steps 7-8.
  double milli;
  if (!GetMsecsOrDefault(cx, args, 2, t, &milli)) {
    return false;
  }

  // Step 9.
  double date = MakeDate(Day(t), MakeTime(HourFromTime(t), m, s, milli));

  // Step 10.
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), date));

  // Steps 11-12.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/* ES5 15.9.5.34. */
static bool date_setUTCMinutes(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCMinutes"));
  if (!unwrapped) {
    return false;
  }

  /* Step 1. */
  double t = unwrapped->UTCTime().toNumber();

  /* Step 2. */
  double m;
  if (!ToNumber(cx, args.get(0), &m)) {
    return false;
  }

  /* Step 3. */
  double s;
  if (!GetSecsOrDefault(cx, args, 1, t, &s)) {
    return false;
  }

  /* Step 4. */
  double milli;
  if (!GetMsecsOrDefault(cx, args, 2, t, &milli)) {
    return false;
  }

  /* Step 5. */
  double date = MakeDate(Day(t), MakeTime(HourFromTime(t), m, s, milli));

  /* Step 6. */
  ClippedTime v = TimeClip(date);

  /* Steps 7-8. */
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/* ES5 15.9.5.35. */
static bool date_setHours(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setHours"));
  if (!unwrapped) {
    return false;
  }

  // Steps 1-2.
  double t = LocalTime(unwrapped->forceUTC(), unwrapped->UTCTime().toNumber());

  // Steps 3-4.
  double h;
  if (!ToNumber(cx, args.get(0), &h)) {
    return false;
  }

  // Steps 5-6.
  double m;
  if (!GetMinsOrDefault(cx, args, 1, t, &m)) {
    return false;
  }

  // Steps 7-8.
  double s;
  if (!GetSecsOrDefault(cx, args, 2, t, &s)) {
    return false;
  }

  // Steps 9-10.
  double milli;
  if (!GetMsecsOrDefault(cx, args, 3, t, &milli)) {
    return false;
  }

  // Step 11.
  double date = MakeDate(Day(t), MakeTime(h, m, s, milli));

  // Step 12.
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), date));

  // Steps 13-14.
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/* ES5 15.9.5.36. */
static bool date_setUTCHours(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCHours"));
  if (!unwrapped) {
    return false;
  }

  /* Step 1. */
  double t = unwrapped->UTCTime().toNumber();

  /* Step 2. */
  double h;
  if (!ToNumber(cx, args.get(0), &h)) {
    return false;
  }

  /* Step 3. */
  double m;
  if (!GetMinsOrDefault(cx, args, 1, t, &m)) {
    return false;
  }

  /* Step 4. */
  double s;
  if (!GetSecsOrDefault(cx, args, 2, t, &s)) {
    return false;
  }

  /* Step 5. */
  double milli;
  if (!GetMsecsOrDefault(cx, args, 3, t, &milli)) {
    return false;
  }

  /* Step 6. */
  double newDate = MakeDate(Day(t), MakeTime(h, m, s, milli));

  /* Step 7. */
  ClippedTime v = TimeClip(newDate);

  /* Steps 8-9. */
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/* ES5 15.9.5.37. */
static bool date_setDate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setDate"));
  if (!unwrapped) {
    return false;
  }

  /* Step 1. */
  double t = LocalTime(unwrapped->forceUTC(), unwrapped->UTCTime().toNumber());

  /* Step 2. */
  double date;
  if (!ToNumber(cx, args.get(0), &date)) {
    return false;
  }

  /* Step 3. */
  double newDate = MakeDate(
      MakeDay(::YearFromTime(t), ::MonthFromTime(t), date), TimeWithinDay(t));

  /* Step 4. */
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), newDate));

  /* Steps 5-6. */
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

static bool date_setUTCDate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCDate"));
  if (!unwrapped) {
    return false;
  }

  /* Step 1. */
  double t = unwrapped->UTCTime().toNumber();

  /* Step 2. */
  double date;
  if (!ToNumber(cx, args.get(0), &date)) {
    return false;
  }

  /* Step 3. */
  double newDate = MakeDate(
      MakeDay(::YearFromTime(t), ::MonthFromTime(t), date), TimeWithinDay(t));

  /* Step 4. */
  ClippedTime v = TimeClip(newDate);

  /* Steps 5-6. */
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

static bool GetDateOrDefault(JSContext* cx, const CallArgs& args, unsigned i,
                             double t, double* date) {
  if (args.length() <= i) {
    *date = DateFromTime(t);
    return true;
  }
  return ToNumber(cx, args[i], date);
}

static bool GetMonthOrDefault(JSContext* cx, const CallArgs& args, unsigned i,
                              double t, double* month) {
  if (args.length() <= i) {
    *month = ::MonthFromTime(t);
    return true;
  }
  return ToNumber(cx, args[i], month);
}

/* ES5 15.9.5.38. */
static bool date_setMonth(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setMonth"));
  if (!unwrapped) {
    return false;
  }

  /* Step 1. */
  double t = LocalTime(unwrapped->forceUTC(), unwrapped->UTCTime().toNumber());

  /* Step 2. */
  double m;
  if (!ToNumber(cx, args.get(0), &m)) {
    return false;
  }

  /* Step 3. */
  double date;
  if (!GetDateOrDefault(cx, args, 1, t, &date)) {
    return false;
  }

  /* Step 4. */
  double newDate =
      MakeDate(MakeDay(::YearFromTime(t), m, date), TimeWithinDay(t));

  /* Step 5. */
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), newDate));

  /* Steps 6-7. */
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/* ES5 15.9.5.39. */
static bool date_setUTCMonth(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCMonth"));
  if (!unwrapped) {
    return false;
  }

  /* Step 1. */
  double t = unwrapped->UTCTime().toNumber();

  /* Step 2. */
  double m;
  if (!ToNumber(cx, args.get(0), &m)) {
    return false;
  }

  /* Step 3. */
  double date;
  if (!GetDateOrDefault(cx, args, 1, t, &date)) {
    return false;
  }

  /* Step 4. */
  double newDate =
      MakeDate(MakeDay(::YearFromTime(t), m, date), TimeWithinDay(t));

  /* Step 5. */
  ClippedTime v = TimeClip(newDate);

  /* Steps 6-7. */
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

static double ThisLocalTimeOrZero(DateTimeInfo::ForceUTC forceUTC,
                                  Handle<DateObject*> dateObj) {
  double t = dateObj->UTCTime().toNumber();
  if (std::isnan(t)) {
    return +0;
  }
  return LocalTime(forceUTC, t);
}

static double ThisUTCTimeOrZero(Handle<DateObject*> dateObj) {
  double t = dateObj->as<DateObject>().UTCTime().toNumber();
  return std::isnan(t) ? +0 : t;
}

/* ES5 15.9.5.40. */
static bool date_setFullYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setFullYear"));
  if (!unwrapped) {
    return false;
  }

  /* Step 1. */
  double t = ThisLocalTimeOrZero(unwrapped->forceUTC(), unwrapped);

  /* Step 2. */
  double y;
  if (!ToNumber(cx, args.get(0), &y)) {
    return false;
  }

  /* Step 3. */
  double m;
  if (!GetMonthOrDefault(cx, args, 1, t, &m)) {
    return false;
  }

  /* Step 4. */
  double date;
  if (!GetDateOrDefault(cx, args, 2, t, &date)) {
    return false;
  }

  /* Step 5. */
  double newDate = MakeDate(MakeDay(y, m, date), TimeWithinDay(t));

  /* Step 6. */
  ClippedTime u = TimeClip(UTC(unwrapped->forceUTC(), newDate));

  /* Steps 7-8. */
  unwrapped->setUTCTime(u, args.rval());
  return true;
}

/* ES5 15.9.5.41. */
static bool date_setUTCFullYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setUTCFullYear"));
  if (!unwrapped) {
    return false;
  }

  /* Step 1. */
  double t = ThisUTCTimeOrZero(unwrapped);

  /* Step 2. */
  double y;
  if (!ToNumber(cx, args.get(0), &y)) {
    return false;
  }

  /* Step 3. */
  double m;
  if (!GetMonthOrDefault(cx, args, 1, t, &m)) {
    return false;
  }

  /* Step 4. */
  double date;
  if (!GetDateOrDefault(cx, args, 2, t, &date)) {
    return false;
  }

  /* Step 5. */
  double newDate = MakeDate(MakeDay(y, m, date), TimeWithinDay(t));

  /* Step 6. */
  ClippedTime v = TimeClip(newDate);

  /* Steps 7-8. */
  unwrapped->setUTCTime(v, args.rval());
  return true;
}

/* ES5 Annex B.2.5. */
static bool date_setYear(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DateObject*> unwrapped(
      cx, UnwrapAndTypeCheckThis<DateObject>(cx, args, "setYear"));
  if (!unwrapped) {
    return false;
  }

  /* Step 1. */
  double t = ThisLocalTimeOrZero(unwrapped->forceUTC(), unwrapped);

  /* Step 2. */
  double y;
  if (!ToNumber(cx, args.get(0), &y)) {
    return false;
  }

  /* Step 3. */
  if (std::isnan(y)) {
    unwrapped->setUTCTime(ClippedTime::invalid(), args.rval());
    return true;
  }

  /* Step 4. */
  double yint = ToInteger(y);
  if (0 <= yint && yint <= 99) {
    yint += 1900;
  }

  /* Step 5. */
  double day = MakeDay(yint, ::MonthFromTime(t), DateFromTime(t));

  /* Step 6. */
  double u = UTC(unwrapped->forceUTC(), MakeDate(day, TimeWithinDay(t)));

  /* Steps 7-8. */
  unwrapped->setUTCTime(TimeClip(u), args.rval());
  return true;
}

/* constants for toString, toUTCString */
static const char* const days[] = {"Sun", "Mon", "Tue", "Wed",
                                   "Thu", "Fri", "Sat"};
static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* ES5 B.2.6. */
static bool date_toUTCString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toUTCString");
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "toUTCString");
  if (!unwrapped) {
    return false;
  }

  double utctime = unwrapped->UTCTime().toNumber();
  if (!std::isfinite(utctime)) {
    args.rval().setString(cx->names().Invalid_Date_);
    return true;
  }

  char buf[100];
  SprintfLiteral(buf, "%s, %.2d %s %.4d %.2d:%.2d:%.2d GMT",
                 days[int(WeekDay(utctime))], int(DateFromTime(utctime)),
                 months[int(::MonthFromTime(utctime))],
                 int(::YearFromTime(utctime)), int(HourFromTime(utctime)),
                 int(MinFromTime(utctime)), int(SecFromTime(utctime)));

  JSString* str = NewStringCopyZ<CanGC>(cx, buf);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/* ES6 draft 2015-01-15 20.3.4.36. */
static bool date_toISOString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toISOString");
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "toISOString");
  if (!unwrapped) {
    return false;
  }

  double utctime = unwrapped->UTCTime().toNumber();
  if (!std::isfinite(utctime)) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_INVALID_DATE);
    return false;
  }

  char buf[100];
  int year = int(::YearFromTime(utctime));
  if (year < 0 || year > 9999) {
    SprintfLiteral(buf, "%+.6d-%.2d-%.2dT%.2d:%.2d:%.2d.%.3dZ",
                   int(::YearFromTime(utctime)),
                   int(::MonthFromTime(utctime)) + 1,
                   int(DateFromTime(utctime)), int(HourFromTime(utctime)),
                   int(MinFromTime(utctime)), int(SecFromTime(utctime)),
                   int(msFromTime(utctime)));
  } else {
    SprintfLiteral(buf, "%.4d-%.2d-%.2dT%.2d:%.2d:%.2d.%.3dZ",
                   int(::YearFromTime(utctime)),
                   int(::MonthFromTime(utctime)) + 1,
                   int(DateFromTime(utctime)), int(HourFromTime(utctime)),
                   int(MinFromTime(utctime)), int(SecFromTime(utctime)),
                   int(msFromTime(utctime)));
  }

  JSString* str = NewStringCopyZ<CanGC>(cx, buf);
  if (!str) {
    return false;
  }
  args.rval().setString(str);
  return true;
}

/* ES5 15.9.5.44. */
static bool date_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toJSON");
  CallArgs args = CallArgsFromVp(argc, vp);

  /* Step 1. */
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  /* Step 2. */
  RootedValue tv(cx, ObjectValue(*obj));
  if (!ToPrimitive(cx, JSTYPE_NUMBER, &tv)) {
    return false;
  }

  /* Step 3. */
  if (tv.isDouble() && !std::isfinite(tv.toDouble())) {
    args.rval().setNull();
    return true;
  }

  /* Step 4. */
  RootedValue toISO(cx);
  if (!GetProperty(cx, obj, obj, cx->names().toISOString, &toISO)) {
    return false;
  }

  /* Step 5. */
  if (!IsCallable(toISO)) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_BAD_TOISOSTRING_PROP);
    return false;
  }

  /* Step 6. */
  return Call(cx, toISO, obj, args.rval());
}

#if JS_HAS_INTL_API
JSString* DateTimeHelper::timeZoneComment(JSContext* cx,
                                          DateTimeInfo::ForceUTC forceUTC,
                                          const char* locale, double utcTime,
                                          double localTime) {
  char16_t tzbuf[100];
  tzbuf[0] = ' ';
  tzbuf[1] = '(';

  char16_t* timeZoneStart = tzbuf + 2;
  constexpr size_t remainingSpace =
      std::size(tzbuf) - 2 - 1;  // for the trailing ')'

  int64_t utcMilliseconds = static_cast<int64_t>(utcTime);
  if (!DateTimeInfo::timeZoneDisplayName(
          forceUTC, timeZoneStart, remainingSpace, utcMilliseconds, locale)) {
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
                                    double localTime, double utcTime) {
  double year = ::YearFromTime(localTime);

  PRMJTime prtm;
  prtm.tm_usec = int32_t(msFromTime(localTime)) * 1000;
  prtm.tm_sec = int8_t(SecFromTime(localTime));
  prtm.tm_min = int8_t(MinFromTime(localTime));
  prtm.tm_hour = int8_t(HourFromTime(localTime));
  prtm.tm_mday = int8_t(DateFromTime(localTime));
  prtm.tm_mon = int8_t(::MonthFromTime(localTime));
  prtm.tm_wday = int8_t(WeekDay(localTime));
  prtm.tm_year = year;
  prtm.tm_yday = int16_t(::DayWithinYear(localTime, year));
  prtm.tm_isdst = (daylightSavingTA(forceUTC, utcTime) != 0);

  return prtm;
}

size_t DateTimeHelper::formatTime(DateTimeInfo::ForceUTC forceUTC, char* buf,
                                  size_t buflen, const char* fmt,
                                  double utcTime, double localTime) {
  PRMJTime prtm = toPRMJTime(forceUTC, localTime, utcTime);

  // If an equivalent year was used to compute the date/time components, use
  // the same equivalent year to determine the time zone name and offset in
  // PRMJ_FormatTime(...).
  int timeZoneYear = isRepresentableAsTime32(utcTime)
                         ? prtm.tm_year
                         : equivalentYearForDST(prtm.tm_year);
  int offsetInSeconds = (int)floor((localTime - utcTime) / msPerSecond);

  return PRMJ_FormatTime(buf, buflen, fmt, &prtm, timeZoneYear,
                         offsetInSeconds);
}

JSString* DateTimeHelper::timeZoneComment(JSContext* cx,
                                          DateTimeInfo::ForceUTC forceUTC,
                                          const char* locale, double utcTime,
                                          double localTime) {
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
  if (!std::isfinite(utcTime)) {
    rval.setString(cx->names().Invalid_Date_);
    return true;
  }

  MOZ_ASSERT(NumbersAreIdentical(TimeClip(utcTime).toDouble(), utcTime));

  double localTime = LocalTime(forceUTC, utcTime);

  int offset = 0;
  RootedString timeZoneComment(cx);
  if (format == FormatSpec::DateTime || format == FormatSpec::Time) {
    // Offset from GMT in minutes. The offset includes daylight savings,
    // if it applies.
    int minutes = (int)trunc((localTime - utcTime) / msPerMinute);

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
    timeZoneComment = DateTimeHelper::timeZoneComment(cx, forceUTC, locale,
                                                      utcTime, localTime);
    if (!timeZoneComment) {
      return false;
    }
  }

  char buf[100];
  switch (format) {
    case FormatSpec::DateTime:
      /* Tue Oct 31 2000 09:41:40 GMT-0800 */
      SprintfLiteral(
          buf, "%s %s %.2d %.4d %.2d:%.2d:%.2d GMT%+.4d",
          days[int(WeekDay(localTime))],
          months[int(::MonthFromTime(localTime))], int(DateFromTime(localTime)),
          int(::YearFromTime(localTime)), int(HourFromTime(localTime)),
          int(MinFromTime(localTime)), int(SecFromTime(localTime)), offset);
      break;
    case FormatSpec::Date:
      /* Tue Oct 31 2000 */
      SprintfLiteral(buf, "%s %s %.2d %.4d", days[int(WeekDay(localTime))],
                     months[int(::MonthFromTime(localTime))],
                     int(DateFromTime(localTime)),
                     int(::YearFromTime(localTime)));
      break;
    case FormatSpec::Time:
      /* 09:41:40 GMT-0800 */
      SprintfLiteral(buf, "%.2d:%.2d:%.2d GMT%+.4d",
                     int(HourFromTime(localTime)), int(MinFromTime(localTime)),
                     int(SecFromTime(localTime)), offset);
      break;
  }

  RootedString str(cx, NewStringCopyZ<CanGC>(cx, buf));
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
  double utcTime = unwrapped->UTCTime().toNumber();

  const char* locale = unwrapped->realm()->getLocale();
  if (!locale) {
    return false;
  }

  char buf[100];
  if (!std::isfinite(utcTime)) {
    strcpy(buf, "InvalidDate");
  } else {
    double localTime = LocalTime(forceUTC, utcTime);

    /* Let PRMJTime format it. */
    size_t result_len = DateTimeHelper::formatTime(forceUTC, buf, sizeof buf,
                                                   format, utcTime, localTime);

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

/* ES5 15.9.5.5. */
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

static bool date_toTimeString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toTimeString");
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "toTimeString");
  if (!unwrapped) {
    return false;
  }

  const char* locale = unwrapped->realm()->getLocale();
  if (!locale) {
    return false;
  }
  return FormatDate(cx, unwrapped->forceUTC(), locale,
                    unwrapped->UTCTime().toNumber(), FormatSpec::Time,
                    args.rval());
}

static bool date_toDateString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toDateString");
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "toDateString");
  if (!unwrapped) {
    return false;
  }

  const char* locale = unwrapped->realm()->getLocale();
  if (!locale) {
    return false;
  }
  return FormatDate(cx, unwrapped->forceUTC(), locale,
                    unwrapped->UTCTime().toNumber(), FormatSpec::Date,
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
      !NumberValueToStringBuffer(unwrapped->UTCTime(), sb) ||
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

bool date_toString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Date.prototype", "toString");
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "toString");
  if (!unwrapped) {
    return false;
  }

  const char* locale = unwrapped->realm()->getLocale();
  if (!locale) {
    return false;
  }
  return FormatDate(cx, unwrapped->forceUTC(), locale,
                    unwrapped->UTCTime().toNumber(), FormatSpec::DateTime,
                    args.rval());
}

bool js::date_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, "valueOf");
  if (!unwrapped) {
    return false;
  }

  args.rval().set(unwrapped->UTCTime());
  return true;
}

// ES6 20.3.4.45 Date.prototype[@@toPrimitive]
static bool date_toPrimitive(JSContext* cx, unsigned argc, Value* vp) {
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

  args.rval().set(args.thisv());
  RootedObject obj(cx, &args.thisv().toObject());
  return OrdinaryToPrimitive(cx, obj, hint, args.rval());
}

#if JS_HAS_TEMPORAL_API
/**
 * Date.prototype.toTemporalInstant ( )
 */
static bool date_toTemporalInstant(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  auto* unwrapped =
      UnwrapAndTypeCheckThis<DateObject>(cx, args, "toTemporalInstant");
  if (!unwrapped) {
    return false;
  }

  // Step 2.
  double utctime = unwrapped->UTCTime().toNumber();
  if (!std::isfinite(utctime)) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_INVALID_DATE);
    return false;
  }
  MOZ_ASSERT(IsInteger(utctime));

  auto instant = temporal::Instant::fromMilliseconds(int64_t(utctime));
  MOZ_ASSERT(temporal::IsValidEpochInstant(instant));

  // Step 3.
  auto* result = temporal::CreateTemporalInstant(cx, instant);
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}
#endif /* JS_HAS_TEMPORAL_API */

static const JSFunctionSpec date_static_methods[] = {
    JS_FN("UTC", date_UTC, 7, 0), JS_FN("parse", date_parse, 1, 0),
    JS_FN("now", date_now, 0, 0), JS_FS_END};

static const JSFunctionSpec date_methods[] = {
    JS_FN("getTime", date_getTime, 0, 0),
    JS_FN("getTimezoneOffset", date_getTimezoneOffset, 0, 0),
    JS_FN("getYear", date_getYear, 0, 0),
    JS_FN("getFullYear", date_getFullYear, 0, 0),
    JS_FN("getUTCFullYear", date_getUTCFullYear, 0, 0),
    JS_FN("getMonth", date_getMonth, 0, 0),
    JS_FN("getUTCMonth", date_getUTCMonth, 0, 0),
    JS_FN("getDate", date_getDate, 0, 0),
    JS_FN("getUTCDate", date_getUTCDate, 0, 0),
    JS_FN("getDay", date_getDay, 0, 0),
    JS_FN("getUTCDay", date_getUTCDay, 0, 0),
    JS_FN("getHours", date_getHours, 0, 0),
    JS_FN("getUTCHours", date_getUTCHours, 0, 0),
    JS_FN("getMinutes", date_getMinutes, 0, 0),
    JS_FN("getUTCMinutes", date_getUTCMinutes, 0, 0),
    JS_FN("getSeconds", date_getSeconds, 0, 0),
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
#if JS_HAS_TEMPORAL_API
    JS_FN("toTemporalInstant", date_toTemporalInstant, 0, 0),
#endif
#if JS_HAS_INTL_API
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
    JS_FN("valueOf", date_valueOf, 0, 0),
    JS_SYM_FN(toPrimitive, date_toPrimitive, 1, JSPROP_READONLY),
    JS_FS_END};

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

static bool ToDateString(JSContext* cx, const CallArgs& args, ClippedTime t) {
  const char* locale = cx->realm()->getLocale();
  if (!locale) {
    return false;
  }
  return FormatDate(cx, ForceUTC(cx->realm()), locale, t.toDouble(),
                    FormatSpec::DateTime, args.rval());
}

static bool DateNoArguments(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(args.length() == 0);

  ClippedTime now = NowAsMillis(cx);

  if (args.isConstructing()) {
    return NewDateObject(cx, args, now);
  }

  return ToDateString(cx, args, now);
}

static bool DateOneArgument(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(args.length() == 1);

  if (args.isConstructing()) {
    if (args[0].isObject()) {
      RootedObject obj(cx, &args[0].toObject());

      ESClass cls;
      if (!GetBuiltinClass(cx, obj, &cls)) {
        return false;
      }

      if (cls == ESClass::Date) {
        RootedValue unboxed(cx);
        if (!Unbox(cx, obj, &unboxed)) {
          return false;
        }

        return NewDateObject(cx, args, TimeClip(unboxed.toNumber()));
      }
    }

    if (!ToPrimitive(cx, args[0])) {
      return false;
    }

    ClippedTime t;
    if (args[0].isString()) {
      JSLinearString* linearStr = args[0].toString()->ensureLinear(cx);
      if (!linearStr) {
        return false;
      }

      if (!ParseDate(ForceUTC(cx->realm()), linearStr, &t)) {
        t = ClippedTime::invalid();
      }
    } else {
      double d;
      if (!ToNumber(cx, args[0], &d)) {
        return false;
      }
      t = TimeClip(d);
    }

    return NewDateObject(cx, args, t);
  }

  return ToDateString(cx, args, NowAsMillis(cx));
}

static bool DateMultipleArguments(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(args.length() >= 2);

  // Step 3.
  if (args.isConstructing()) {
    // Steps 3a-b.
    double y;
    if (!ToNumber(cx, args[0], &y)) {
      return false;
    }

    // Steps 3c-d.
    double m;
    if (!ToNumber(cx, args[1], &m)) {
      return false;
    }

    // Steps 3e-f.
    double dt;
    if (args.length() >= 3) {
      if (!ToNumber(cx, args[2], &dt)) {
        return false;
      }
    } else {
      dt = 1;
    }

    // Steps 3g-h.
    double h;
    if (args.length() >= 4) {
      if (!ToNumber(cx, args[3], &h)) {
        return false;
      }
    } else {
      h = 0;
    }

    // Steps 3i-j.
    double min;
    if (args.length() >= 5) {
      if (!ToNumber(cx, args[4], &min)) {
        return false;
      }
    } else {
      min = 0;
    }

    // Steps 3k-l.
    double s;
    if (args.length() >= 6) {
      if (!ToNumber(cx, args[5], &s)) {
        return false;
      }
    } else {
      s = 0;
    }

    // Steps 3m-n.
    double milli;
    if (args.length() >= 7) {
      if (!ToNumber(cx, args[6], &milli)) {
        return false;
      }
    } else {
      milli = 0;
    }

    // Step 3o.
    double yr = y;
    if (!std::isnan(y)) {
      double yint = ToInteger(y);
      if (0 <= yint && yint <= 99) {
        yr = 1900 + yint;
      }
    }

    // Step 3p.
    double finalDate = MakeDate(MakeDay(yr, m, dt), MakeTime(h, min, s, milli));

    // Steps 3q-t.
    return NewDateObject(cx, args,
                         TimeClip(UTC(ForceUTC(cx->realm()), finalDate)));
  }

  return ToDateString(cx, args, NowAsMillis(cx));
}

static bool DateConstructor(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSConstructorProfilerEntry pseudoFrame(cx, "Date");
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() == 0) {
    return DateNoArguments(cx, args);
  }

  if (args.length() == 1) {
    return DateOneArgument(cx, args);
  }

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
    FinishDateClassInit};

const JSClass DateObject::class_ = {"Date",
                                    JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS) |
                                        JSCLASS_HAS_CACHED_PROTO(JSProto_Date),
                                    JS_NULL_CLASS_OPS, &DateObjectClassSpec};

const JSClass DateObject::protoClass_ = {
    "Date.prototype", JSCLASS_HAS_CACHED_PROTO(JSProto_Date), JS_NULL_CLASS_OPS,
    &DateObjectClassSpec};

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
