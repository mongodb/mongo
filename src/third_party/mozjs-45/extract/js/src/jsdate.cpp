/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

#include "mozilla/ArrayUtils.h"
#include "mozilla/FloatingPoint.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "jsapi.h"
#include "jscntxt.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsprf.h"
#include "jsstr.h"
#include "jstypes.h"
#include "jsutil.h"
#include "jswrapper.h"

#include "js/Conversions.h"
#include "js/Date.h"
#include "vm/DateTime.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/String.h"
#include "vm/StringBuffer.h"
#include "vm/Time.h"

#include "jsobjinlines.h"

using namespace js;

using mozilla::ArrayLength;
using mozilla::IsFinite;
using mozilla::IsNaN;
using mozilla::NumbersAreIdentical;

using JS::AutoCheckCannotGC;
using JS::ClippedTime;
using JS::GenericNaN;
using JS::TimeClip;
using JS::ToInteger;

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

static inline double
Day(double t)
{
    return floor(t / msPerDay);
}

static double
TimeWithinDay(double t)
{
    double result = fmod(t, msPerDay);
    if (result < 0)
        result += msPerDay;
    return result;
}

/* ES5 15.9.1.3. */
static inline bool
IsLeapYear(double year)
{
    MOZ_ASSERT(ToInteger(year) == year);
    return fmod(year, 4) == 0 && (fmod(year, 100) != 0 || fmod(year, 400) == 0);
}

static inline double
DaysInYear(double year)
{
    if (!IsFinite(year))
        return GenericNaN();
    return IsLeapYear(year) ? 366 : 365;
}

static inline double
DayFromYear(double y)
{
    return 365 * (y - 1970) +
           floor((y - 1969) / 4.0) -
           floor((y - 1901) / 100.0) +
           floor((y - 1601) / 400.0);
}

static inline double
TimeFromYear(double y)
{
    return DayFromYear(y) * msPerDay;
}

static double
YearFromTime(double t)
{
    if (!IsFinite(t))
        return GenericNaN();

    MOZ_ASSERT(ToInteger(t) == t);

    double y = floor(t / (msPerDay * 365.2425)) + 1970;
    double t2 = TimeFromYear(y);

    /*
     * Adjust the year if the approximation was wrong.  Since the year was
     * computed using the average number of ms per year, it will usually
     * be wrong for dates within several hours of a year transition.
     */
    if (t2 > t) {
        y--;
    } else {
        if (t2 + msPerDay * DaysInYear(y) <= t)
            y++;
    }
    return y;
}

static inline int
DaysInFebruary(double year)
{
    return IsLeapYear(year) ? 29 : 28;
}

/* ES5 15.9.1.4. */
static inline double
DayWithinYear(double t, double year)
{
    MOZ_ASSERT_IF(IsFinite(t), YearFromTime(t) == year);
    return Day(t) - DayFromYear(year);
}

static double
MonthFromTime(double t)
{
    if (!IsFinite(t))
        return GenericNaN();

    double year = YearFromTime(t);
    double d = DayWithinYear(t, year);

    int step;
    if (d < (step = 31))
        return 0;
    if (d < (step += DaysInFebruary(year)))
        return 1;
    if (d < (step += 31))
        return 2;
    if (d < (step += 30))
        return 3;
    if (d < (step += 31))
        return 4;
    if (d < (step += 30))
        return 5;
    if (d < (step += 31))
        return 6;
    if (d < (step += 31))
        return 7;
    if (d < (step += 30))
        return 8;
    if (d < (step += 31))
        return 9;
    if (d < (step += 30))
        return 10;
    return 11;
}

/* ES5 15.9.1.5. */
static double
DateFromTime(double t)
{
    if (!IsFinite(t))
        return GenericNaN();

    double year = YearFromTime(t);
    double d = DayWithinYear(t, year);

    int next;
    if (d <= (next = 30))
        return d + 1;
    int step = next;
    if (d <= (next += DaysInFebruary(year)))
        return d - step;
    step = next;
    if (d <= (next += 31))
        return d - step;
    step = next;
    if (d <= (next += 30))
        return d - step;
    step = next;
    if (d <= (next += 31))
        return d - step;
    step = next;
    if (d <= (next += 30))
        return d - step;
    step = next;
    if (d <= (next += 31))
        return d - step;
    step = next;
    if (d <= (next += 31))
        return d - step;
    step = next;
    if (d <= (next += 30))
        return d - step;
    step = next;
    if (d <= (next += 31))
        return d - step;
    step = next;
    if (d <= (next += 30))
        return d - step;
    step = next;
    return d - step;
}

/* ES5 15.9.1.6. */
static int
WeekDay(double t)
{
    /*
     * We can't assert TimeClip(t) == t because we call this function with
     * local times, which can be offset outside TimeClip's permitted range.
     */
    MOZ_ASSERT(ToInteger(t) == t);
    int result = (int(Day(t)) + 4) % 7;
    if (result < 0)
        result += 7;
    return result;
}

static inline int
DayFromMonth(int month, bool isLeapYear)
{
    /*
     * The following array contains the day of year for the first day of
     * each month, where index 0 is January, and day 0 is January 1.
     */
    static const int firstDayOfMonth[2][13] = {
        {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
        {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}
    };

    MOZ_ASSERT(0 <= month && month <= 12);
    return firstDayOfMonth[isLeapYear][month];
}

template<typename T>
static inline int
DayFromMonth(T month, bool isLeapYear) = delete;

/* ES5 15.9.1.12 (out of order to accommodate DaylightSavingTA). */
static double
MakeDay(double year, double month, double date)
{
    /* Step 1. */
    if (!IsFinite(year) || !IsFinite(month) || !IsFinite(date))
        return GenericNaN();

    /* Steps 2-4. */
    double y = ToInteger(year);
    double m = ToInteger(month);
    double dt = ToInteger(date);

    /* Step 5. */
    double ym = y + floor(m / 12);

    /* Step 6. */
    int mn = int(fmod(m, 12.0));
    if (mn < 0)
        mn += 12;

    /* Steps 7-8. */
    bool leap = IsLeapYear(ym);

    double yearday = floor(TimeFromYear(ym) / msPerDay);
    double monthday = DayFromMonth(mn, leap);

    return yearday + monthday + dt - 1;
}

/* ES5 15.9.1.13 (out of order to accommodate DaylightSavingTA). */
static inline double
MakeDate(double day, double time)
{
    /* Step 1. */
    if (!IsFinite(day) || !IsFinite(time))
        return GenericNaN();

    /* Step 2. */
    return day * msPerDay + time;
}

JS_PUBLIC_API(double)
JS::MakeDate(double year, unsigned month, unsigned day)
{
    return ::MakeDate(MakeDay(year, month, day), 0);
}

JS_PUBLIC_API(double)
JS::YearFromTime(double time)
{
    return ::YearFromTime(time);
}

JS_PUBLIC_API(double)
JS::MonthFromTime(double time)
{
    return ::MonthFromTime(time);
}

JS_PUBLIC_API(double)
JS::DayFromTime(double time)
{
    return DateFromTime(time);
}

/*
 * Find a year for which any given date will fall on the same weekday.
 *
 * This function should be used with caution when used other than
 * for determining DST; it hasn't been proven not to produce an
 * incorrect year for times near year boundaries.
 */
static int
EquivalentYearForDST(int year)
{
    /*
     * Years and leap years on which Jan 1 is a Sunday, Monday, etc.
     *
     * yearStartingWith[0][i] is an example non-leap year where
     * Jan 1 appears on Sunday (i == 0), Monday (i == 1), etc.
     *
     * yearStartingWith[1][i] is an example leap year where
     * Jan 1 appears on Sunday (i == 0), Monday (i == 1), etc.
     */
    static const int yearStartingWith[2][7] = {
        {1978, 1973, 1974, 1975, 1981, 1971, 1977},
        {1984, 1996, 1980, 1992, 1976, 1988, 1972}
    };

    int day = int(DayFromYear(year) + 4) % 7;
    if (day < 0)
        day += 7;

    return yearStartingWith[IsLeapYear(year)][day];
}

/* ES5 15.9.1.8. */
static double
DaylightSavingTA(double t)
{
    if (!IsFinite(t))
        return GenericNaN();

    /*
     * If earlier than 1970 or after 2038, potentially beyond the ken of
     * many OSes, map it to an equivalent year before asking.
     */
    if (t < 0.0 || t > 2145916800000.0) {
        int year = EquivalentYearForDST(int(YearFromTime(t)));
        double day = MakeDay(year, MonthFromTime(t), DateFromTime(t));
        t = MakeDate(day, TimeWithinDay(t));
    }

    int64_t utcMilliseconds = static_cast<int64_t>(t);
    int64_t offsetMilliseconds = DateTimeInfo::getDSTOffsetMilliseconds(utcMilliseconds);
    return static_cast<double>(offsetMilliseconds);
}

static double
AdjustTime(double date)
{
    double localTZA = DateTimeInfo::localTZA();
    double t = DaylightSavingTA(date) + localTZA;
    t = (localTZA >= 0) ? fmod(t, msPerDay) : -fmod(msPerDay - t, msPerDay);
    return t;
}

/* ES5 15.9.1.9. */
static double
LocalTime(double t)
{
    return t + AdjustTime(t);
}

static double
UTC(double t)
{
    return t - AdjustTime(t - DateTimeInfo::localTZA());
}

/* ES5 15.9.1.10. */
static double
HourFromTime(double t)
{
    double result = fmod(floor(t/msPerHour), HoursPerDay);
    if (result < 0)
        result += HoursPerDay;
    return result;
}

static double
MinFromTime(double t)
{
    double result = fmod(floor(t / msPerMinute), MinutesPerHour);
    if (result < 0)
        result += MinutesPerHour;
    return result;
}

static double
SecFromTime(double t)
{
    double result = fmod(floor(t / msPerSecond), SecondsPerMinute);
    if (result < 0)
        result += SecondsPerMinute;
    return result;
}

static double
msFromTime(double t)
{
    double result = fmod(t, msPerSecond);
    if (result < 0)
        result += msPerSecond;
    return result;
}

/* ES5 15.9.1.11. */
static double
MakeTime(double hour, double min, double sec, double ms)
{
    /* Step 1. */
    if (!IsFinite(hour) ||
        !IsFinite(min) ||
        !IsFinite(sec) ||
        !IsFinite(ms))
    {
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

/* for use by date_parse */

static const char* const wtb[] = {
    "am", "pm",
    "monday", "tuesday", "wednesday", "thursday", "friday",
    "saturday", "sunday",
    "january", "february", "march", "april", "may", "june",
    "july", "august", "september", "october", "november", "december",
    "gmt", "ut", "utc",
    "est", "edt",
    "cst", "cdt",
    "mst", "mdt",
    "pst", "pdt"
    /* time zone table needs to be expanded */
};

static const int ttb[] = {
    -1, -2, 0, 0, 0, 0, 0, 0, 0,       /* AM/PM */
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    10000 + 0, 10000 + 0, 10000 + 0,   /* GMT/UT/UTC */
    10000 + 5 * 60, 10000 + 4 * 60,    /* EST/EDT */
    10000 + 6 * 60, 10000 + 5 * 60,    /* CST/CDT */
    10000 + 7 * 60, 10000 + 6 * 60,    /* MST/MDT */
    10000 + 8 * 60, 10000 + 7 * 60     /* PST/PDT */
};

template <typename CharT>
static bool
RegionMatches(const char* s1, int s1off, const CharT* s2, int s2off, int count)
{
    while (count > 0 && s1[s1off] && s2[s2off]) {
        if (unicode::ToLowerCase(s1[s1off]) != unicode::ToLowerCase(s2[s2off]))
            break;

        s1off++;
        s2off++;
        count--;
    }

    return count == 0;
}

/* ES6 20.3.3.4. */
static bool
date_UTC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Steps 1-2.
    double y;
    if (!ToNumber(cx, args.get(0), &y))
        return false;

    // Steps 3-4.
    double m;
    if (!ToNumber(cx, args.get(1), &m))
        return false;

    // Steps 5-6.
    double dt;
    if (args.length() >= 3) {
        if (!ToNumber(cx, args[2], &dt))
            return false;
    } else {
        dt = 1;
    }

    // Steps 7-8.
    double h;
    if (args.length() >= 4) {
        if (!ToNumber(cx, args[3], &h))
            return false;
    } else {
        h = 0;
    }

    // Steps 9-10.
    double min;
    if (args.length() >= 5) {
        if (!ToNumber(cx, args[4], &min))
            return false;
    } else {
        min = 0;
    }

    // Steps 11-12.
    double s;
    if (args.length() >= 6) {
        if (!ToNumber(cx, args[5], &s))
            return false;
    } else {
        s = 0;
    }

    // Steps 13-14.
    double milli;
    if (args.length() >= 7) {
        if (!ToNumber(cx, args[6], &milli))
            return false;
    } else {
        milli = 0;
    }

    // Step 15.
    double yr = y;
    if (!IsNaN(y)) {
        double yint = ToInteger(y);
        if (0 <= yint && yint <= 99)
            yr = 1900 + yint;
    }

    // Step 16.
    ClippedTime time = TimeClip(MakeDate(MakeDay(yr, m, dt), MakeTime(h, min, s, milli)));
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
static bool
ParseDigits(size_t* result, const CharT* s, size_t* i, size_t limit)
{
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
 * while *i < limit.
 *
 * Succeed if any digits are converted. Advance *i only
 * as digits are consumed.
 */
template <typename CharT>
static bool
ParseFractional(double* result, const CharT* s, size_t* i, size_t limit)
{
    double factor = 0.1;
    size_t init = *i;
    *result = 0.0;
    while (*i < limit && ('0' <= s[*i] && s[*i] <= '9')) {
        *result += (s[*i] - '0') * factor;
        factor *= 0.1;
        ++(*i);
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
static bool
ParseDigitsN(size_t n, size_t* result, const CharT* s, size_t* i, size_t limit)
{
    size_t init = *i;

    if (ParseDigits(result, s, i, Min(limit, init + n)))
        return (*i - init) == n;

    *i = init;
    return false;
}

static int
DaysInMonth(int year, int month)
{
    bool leap = IsLeapYear(year);
    int result = int(DayFromMonth(month, leap) - DayFromMonth(month - 1, leap));
    return result;
}

/*
 * Parse a string in one of the date-time formats given by the W3C
 * "NOTE-datetime" specification. These formats make up a restricted
 * profile of the ISO 8601 format. Quoted here:
 *
 *   The formats are as follows. Exactly the components shown here
 *   must be present, with exactly this punctuation. Note that the "T"
 *   appears literally in the string, to indicate the beginning of the
 *   time element, as specified in ISO 8601.
 *
 *   Any combination of the date formats with the time formats is
 *   allowed, and also either the date or the time can be missing.
 *
 *   The specification is silent on the meaning when fields are
 *   ommitted so the interpretations are a guess, but hopefully a
 *   reasonable one. We default the month to January, the day to the
 *   1st, and hours minutes and seconds all to 0. If the date is
 *   missing entirely then we assume 1970-01-01 so that the time can
 *   be aded to a date later. If the time is missing then we assume
 *   00:00 UTC.  If the time is present but the time zone field is
 *   missing then we use local time.
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
 *     Thh:mm:ss.sTZD (eg T19:20:30.45+01:00)
 *
 * where:
 *
 *   YYYY = four-digit year or six digit year as +YYYYYY or -YYYYYY
 *   MM   = two-digit month (01=January, etc.)
 *   DD   = two-digit day of month (01 through 31)
 *   hh   = two digits of hour (00 through 23) (am/pm NOT allowed)
 *   mm   = two digits of minute (00 through 59)
 *   ss   = two digits of second (00 through 59)
 *   s    = one or more digits representing a decimal fraction of a second
 *   TZD  = time zone designator (Z or +hh:mm or -hh:mm or missing for local)
 */
template <typename CharT>
static bool
ParseISODate(const CharT* s, size_t length, ClippedTime* result)
{
    size_t i = 0;
    int tzMul = 1;
    int dateMul = 1;
    size_t year = 1970;
    size_t month = 1;
    size_t day = 1;
    size_t hour = 0;
    size_t min = 0;
    size_t sec = 0;
    double frac = 0;
    bool isLocalTime = false;
    size_t tzHour = 0;
    size_t tzMin = 0;

#define PEEK(ch) (i < length && s[i] == ch)

#define NEED(ch)                                                               \
    if (i >= length || s[i] != ch) { return false; } else { ++i; }

#define DONE_DATE_UNLESS(ch)                                                   \
    if (i >= length || s[i] != ch) { goto done_date; } else { ++i; }

#define DONE_UNLESS(ch)                                                        \
    if (i >= length || s[i] != ch) { goto done; } else { ++i; }

#define NEED_NDIGITS(n, field)                                                 \
    if (!ParseDigitsN(n, &field, s, &i, length)) { return false; }

    if (PEEK('+') || PEEK('-')) {
        if (PEEK('-'))
            dateMul = -1;
        ++i;
        NEED_NDIGITS(6, year);
    } else if (!PEEK('T')) {
        NEED_NDIGITS(4, year);
    }
    DONE_DATE_UNLESS('-');
    NEED_NDIGITS(2, month);
    DONE_DATE_UNLESS('-');
    NEED_NDIGITS(2, day);

 done_date:
    DONE_UNLESS('T');
    NEED_NDIGITS(2, hour);
    NEED(':');
    NEED_NDIGITS(2, min);

    if (PEEK(':')) {
        ++i;
        NEED_NDIGITS(2, sec);
        if (PEEK('.')) {
            ++i;
            if (!ParseFractional(&frac, s, &i, length))
                return false;
        }
    }

    if (PEEK('Z')) {
        ++i;
    } else if (PEEK('+') || PEEK('-')) {
        if (PEEK('-'))
            tzMul = -1;
        ++i;
        NEED_NDIGITS(2, tzHour);
        /*
         * Non-standard extension to the ISO date format (permitted by ES5):
         * allow "-0700" as a time zone offset, not just "-07:00".
         */
        if (PEEK(':'))
            ++i;
        NEED_NDIGITS(2, tzMin);
    } else {
        isLocalTime = true;
    }

 done:
    if (year > 275943 // ceil(1e8/365) + 1970
        || (month == 0 || month > 12)
        || (day == 0 || day > size_t(DaysInMonth(year,month)))
        || hour > 24
        || ((hour == 24) && (min > 0 || sec > 0))
        || min > 59
        || sec > 59
        || tzHour > 23
        || tzMin > 59)
    {
        return false;
    }

    if (i != length)
        return false;

    month -= 1; /* convert month to 0-based */

    double msec = MakeDate(MakeDay(dateMul * double(year), month, day),
                           MakeTime(hour, min, sec, frac * 1000.0));

    if (isLocalTime)
        msec = UTC(msec);
    else
        msec -= tzMul * (tzHour * msPerHour + tzMin * msPerMinute);

    *result = TimeClip(msec);
    return NumbersAreIdentical(msec, result->toDouble());

#undef PEEK
#undef NEED
#undef DONE_UNLESS
#undef NEED_NDIGITS
}

template <typename CharT>
static bool
ParseDate(const CharT* s, size_t length, ClippedTime* result)
{
    if (ParseISODate(s, length, result))
        return true;

    if (length == 0)
        return false;

    int year = -1;
    int mon = -1;
    int mday = -1;
    int hour = -1;
    int min = -1;
    int sec = -1;
    int tzOffset = -1;

    int prevc = 0;

    bool seenPlusMinus = false;
    bool seenMonthName = false;

    size_t i = 0;
    while (i < length) {
        int c = s[i];
        i++;
        if (c <= ' ' || c == ',' || c == '-') {
            if (c == '-' && '0' <= s[i] && s[i] <= '9')
                prevc = c;
            continue;
        }
        if (c == '(') { /* comments) */
            int depth = 1;
            while (i < length) {
                c = s[i];
                i++;
                if (c == '(') {
                    depth++;
                } else if (c == ')') {
                    if (--depth <= 0)
                        break;
                }
            }
            continue;
        }
        if ('0' <= c && c <= '9') {
            int n = c - '0';
            while (i < length && '0' <= (c = s[i]) && c <= '9') {
                n = n * 10 + c - '0';
                i++;
            }

            /*
             * Allow TZA before the year, so 'Wed Nov 05 21:49:11 GMT-0800 1997'
             * works.
             *
             * Uses of seenPlusMinus allow ':' in TZA, so Java no-timezone style
             * of GMT+4:30 works.
             */

            if ((prevc == '+' || prevc == '-')/*  && year>=0 */) {
                /* Make ':' case below change tzOffset. */
                seenPlusMinus = true;

                /* offset */
                if (n < 24)
                    n = n * 60; /* EG. "GMT-3" */
                else
                    n = n % 100 + n / 100 * 60; /* eg "GMT-0430" */

                if (prevc == '+')       /* plus means east of GMT */
                    n = -n;

                if (tzOffset != 0 && tzOffset != -1)
                    return false;

                tzOffset = n;
            } else if (prevc == '/' && mon >= 0 && mday >= 0 && year < 0) {
                if (c <= ' ' || c == ',' || c == '/' || i >= length)
                    year = n;
                else
                    return false;
            } else if (c == ':') {
                if (hour < 0)
                    hour = /*byte*/ n;
                else if (min < 0)
                    min = /*byte*/ n;
                else
                    return false;
            } else if (c == '/') {
                /*
                 * Until it is determined that mon is the actual month, keep
                 * it as 1-based rather than 0-based.
                 */
                if (mon < 0)
                    mon = /*byte*/ n;
                else if (mday < 0)
                    mday = /*byte*/ n;
                else
                    return false;
            } else if (i < length && c != ',' && c > ' ' && c != '-' && c != '(') {
                return false;
            } else if (seenPlusMinus && n < 60) {  /* handle GMT-3:30 */
                if (tzOffset < 0)
                    tzOffset -= n;
                else
                    tzOffset += n;
            } else if (hour >= 0 && min < 0) {
                min = /*byte*/ n;
            } else if (prevc == ':' && min >= 0 && sec < 0) {
                sec = /*byte*/ n;
            } else if (mon < 0) {
                mon = /*byte*/n;
            } else if (mon >= 0 && mday < 0) {
                mday = /*byte*/ n;
            } else if (mon >= 0 && mday >= 0 && year < 0) {
                year = n;
            } else {
                return false;
            }
            prevc = 0;
        } else if (c == '/' || c == ':' || c == '+' || c == '-') {
            prevc = c;
        } else {
            size_t st = i - 1;
            int k;
            while (i < length) {
                c = s[i];
                if (!(('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z')))
                    break;
                i++;
            }

            if (i <= st + 1)
                return false;

            for (k = ArrayLength(wtb); --k >= 0;) {
                if (RegionMatches(wtb[k], 0, s, st, i - st)) {
                    int action = ttb[k];
                    if (action != 0) {
                        if (action < 0) {
                            /*
                             * AM/PM. Count 12:30 AM as 00:30, 12:30 PM as
                             * 12:30, instead of blindly adding 12 if PM.
                             */
                            MOZ_ASSERT(action == -1 || action == -2);
                            if (hour > 12 || hour < 0)
                                return false;

                            if (action == -1 && hour == 12) /* am */
                                hour = 0;
                            else if (action == -2 && hour != 12) /* pm */
                                hour += 12;
                        } else if (action <= 13) { /* month! */
                            /*
                             * Adjust mon to be 1-based until the final values
                             * for mon, mday and year are adjusted below.
                             */
                            if (seenMonthName)
                                return false;

                            seenMonthName = true;
                            int temp = /*byte*/ (action - 2) + 1;

                            if (mon < 0) {
                                mon = temp;
                            } else if (mday < 0) {
                                mday = mon;
                                mon = temp;
                            } else if (year < 0) {
                                year = mon;
                                mon = temp;
                            } else {
                                return false;
                            }
                        } else {
                            tzOffset = action - 10000;
                        }
                    }
                    break;
                }
            }

            if (k < 0)
                return false;

            prevc = 0;
        }
    }

    if (year < 0 || mon < 0 || mday < 0)
        return false;

    /*
     * Case 1. The input string contains an English month name.
     *         The form of the string can be month f l, or f month l, or
     *         f l month which each evaluate to the same date.
     *         If f and l are both greater than or equal to 70, or
     *         both less than 70, the date is invalid.
     *         The year is taken to be the greater of the values f, l.
     *         If the year is greater than or equal to 70 and less than 100,
     *         it is considered to be the number of years after 1900.
     * Case 2. The input string is of the form "f/m/l" where f, m and l are
     *         integers, e.g. 7/16/45.
     *         Adjust the mon, mday and year values to achieve 100% MSIE
     *         compatibility.
     *         a. If 0 <= f < 70, f/m/l is interpreted as month/day/year.
     *            i.  If year < 100, it is the number of years after 1900
     *            ii. If year >= 100, it is the number of years after 0.
     *         b. If 70 <= f < 100
     *            i.  If m < 70, f/m/l is interpreted as
     *                year/month/day where year is the number of years after
     *                1900.
     *            ii. If m >= 70, the date is invalid.
     *         c. If f >= 100
     *            i.  If m < 70, f/m/l is interpreted as
     *                year/month/day where year is the number of years after 0.
     *            ii. If m >= 70, the date is invalid.
     */
    if (seenMonthName) {
        if ((mday >= 70 && year >= 70) || (mday < 70 && year < 70))
            return false;

        if (mday > year) {
            int temp = year;
            year = mday;
            mday = temp;
        }
        if (year >= 70 && year < 100) {
            year += 1900;
        }
    } else if (mon < 70) { /* (a) month/day/year */
        if (year < 100) {
            year += 1900;
        }
    } else if (mon < 100) { /* (b) year/month/day */
        if (mday < 70) {
            int temp = year;
            year = mon + 1900;
            mon = mday;
            mday = temp;
        } else {
            return false;
        }
    } else { /* (c) year/month/day */
        if (mday < 70) {
            int temp = year;
            year = mon;
            mon = mday;
            mday = temp;
        } else {
            return false;
        }
    }

    mon -= 1; /* convert month to 0-based */
    if (sec < 0)
        sec = 0;
    if (min < 0)
        min = 0;
    if (hour < 0)
        hour = 0;

    double msec = MakeDate(MakeDay(year, mon, mday), MakeTime(hour, min, sec, 0));

    if (tzOffset == -1) /* no time zone specified, have to use local */
        msec = UTC(msec);
    else
        msec += tzOffset * msPerMinute;

    *result = TimeClip(msec);
    return true;
}

static bool
ParseDate(JSLinearString* s, ClippedTime* result)
{
    AutoCheckCannotGC nogc;
    return s->hasLatin1Chars()
           ? ParseDate(s->latin1Chars(nogc), s->length(), result)
           : ParseDate(s->twoByteChars(nogc), s->length(), result);
}

static bool
date_parse(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() == 0) {
        args.rval().setNaN();
        return true;
    }

    JSString* str = ToString<CanGC>(cx, args[0]);
    if (!str)
        return false;

    JSLinearString* linearStr = str->ensureLinear(cx);
    if (!linearStr)
        return false;

    ClippedTime result;
    if (!ParseDate(linearStr, &result)) {
        args.rval().setNaN();
        return true;
    }

    args.rval().set(TimeValue(result));
    return true;
}

static ClippedTime
NowAsMillis()
{
    return TimeClip(static_cast<double>(PRMJ_Now()) / PRMJ_USEC_PER_MSEC);
}

bool
js::date_now(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().set(TimeValue(NowAsMillis()));
    return true;
}

void
DateObject::setUTCTime(ClippedTime t)
{
    for (size_t ind = COMPONENTS_START_SLOT; ind < RESERVED_SLOTS; ind++)
        setReservedSlot(ind, UndefinedValue());

    setFixedSlot(UTC_TIME_SLOT, TimeValue(t));
}

void
DateObject::setUTCTime(ClippedTime t, MutableHandleValue vp)
{
    setUTCTime(t);
    vp.set(TimeValue(t));
}

void
DateObject::fillLocalTimeSlots()
{
    /* Check if the cache is already populated. */
    if (!getReservedSlot(LOCAL_TIME_SLOT).isUndefined() &&
        getReservedSlot(TZA_SLOT).toDouble() == DateTimeInfo::localTZA())
    {
        return;
    }

    /* Remember timezone used to generate the local cache. */
    setReservedSlot(TZA_SLOT, DoubleValue(DateTimeInfo::localTZA()));

    double utcTime = UTCTime().toNumber();

    if (!IsFinite(utcTime)) {
        for (size_t ind = COMPONENTS_START_SLOT; ind < RESERVED_SLOTS; ind++)
            setReservedSlot(ind, DoubleValue(utcTime));
        return;
    }

    double localTime = LocalTime(utcTime);

    setReservedSlot(LOCAL_TIME_SLOT, DoubleValue(localTime));

    int year = (int) floor(localTime /(msPerDay * 365.2425)) + 1970;
    double yearStartTime = TimeFromYear(year);

    /* Adjust the year in case the approximation was wrong, as in YearFromTime. */
    int yearDays;
    if (yearStartTime > localTime) {
        year--;
        yearStartTime -= (msPerDay * DaysInYear(year));
        yearDays = DaysInYear(year);
    } else {
        yearDays = DaysInYear(year);
        double nextStart = yearStartTime + (msPerDay * yearDays);
        if (nextStart <= localTime) {
            year++;
            yearStartTime = nextStart;
            yearDays = DaysInYear(year);
        }
    }

    setReservedSlot(LOCAL_YEAR_SLOT, Int32Value(year));

    uint64_t yearTime = uint64_t(localTime - yearStartTime);
    int yearSeconds = uint32_t(yearTime / 1000);

    int day = yearSeconds / int(SecondsPerDay);

    int step = -1, next = 30;
    int month;

    do {
        if (day <= next) {
            month = 0;
            break;
        }
        step = next;
        next += ((yearDays == 366) ? 29 : 28);
        if (day <= next) {
            month = 1;
            break;
        }
        step = next;
        if (day <= (next += 31)) {
            month = 2;
            break;
        }
        step = next;
        if (day <= (next += 30)) {
            month = 3;
            break;
        }
        step = next;
        if (day <= (next += 31)) {
            month = 4;
            break;
        }
        step = next;
        if (day <= (next += 30)) {
            month = 5;
            break;
        }
        step = next;
        if (day <= (next += 31)) {
            month = 6;
            break;
        }
        step = next;
        if (day <= (next += 31)) {
            month = 7;
            break;
        }
        step = next;
        if (day <= (next += 30)) {
            month = 8;
            break;
        }
        step = next;
        if (day <= (next += 31)) {
            month = 9;
            break;
        }
        step = next;
        if (day <= (next += 30)) {
            month = 10;
            break;
        }
        step = next;
        month = 11;
    } while (0);

    setReservedSlot(LOCAL_MONTH_SLOT, Int32Value(month));
    setReservedSlot(LOCAL_DATE_SLOT, Int32Value(day - step));

    int weekday = WeekDay(localTime);
    setReservedSlot(LOCAL_DAY_SLOT, Int32Value(weekday));

    int seconds = yearSeconds % 60;
    setReservedSlot(LOCAL_SECONDS_SLOT, Int32Value(seconds));

    int minutes = (yearSeconds / 60) % 60;
    setReservedSlot(LOCAL_MINUTES_SLOT, Int32Value(minutes));

    int hours = (yearSeconds / (60 * 60)) % 24;
    setReservedSlot(LOCAL_HOURS_SLOT, Int32Value(hours));
}

inline double
DateObject::cachedLocalTime()
{
    fillLocalTimeSlots();
    return getReservedSlot(LOCAL_TIME_SLOT).toDouble();
}

MOZ_ALWAYS_INLINE bool
IsDate(HandleValue v)
{
    return v.isObject() && v.toObject().is<DateObject>();
}

/*
 * See ECMA 15.9.5.4 thru 15.9.5.23
 */
/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getTime_impl(JSContext* cx, const CallArgs& args)
{
    args.rval().set(args.thisv().toObject().as<DateObject>().UTCTime());
    return true;
}

static bool
date_getTime(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getTime_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getYear_impl(JSContext* cx, const CallArgs& args)
{
    DateObject* dateObj = &args.thisv().toObject().as<DateObject>();
    dateObj->fillLocalTimeSlots();

    Value yearVal = dateObj->getReservedSlot(LOCAL_YEAR_SLOT);
    if (yearVal.isInt32()) {
        /* Follow ECMA-262 to the letter, contrary to IE JScript. */
        int year = yearVal.toInt32() - 1900;
        args.rval().setInt32(year);
    } else {
        args.rval().set(yearVal);
    }

    return true;
}

static bool
date_getYear(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getYear_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getFullYear_impl(JSContext* cx, const CallArgs& args)
{
    DateObject* dateObj = &args.thisv().toObject().as<DateObject>();
    dateObj->fillLocalTimeSlots();

    args.rval().set(dateObj->getReservedSlot(LOCAL_YEAR_SLOT));
    return true;
}

static bool
date_getFullYear(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getFullYear_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getUTCFullYear_impl(JSContext* cx, const CallArgs& args)
{
    double result = args.thisv().toObject().as<DateObject>().UTCTime().toNumber();
    if (IsFinite(result))
        result = YearFromTime(result);

    args.rval().setNumber(result);
    return true;
}

static bool
date_getUTCFullYear(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getUTCFullYear_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getMonth_impl(JSContext* cx, const CallArgs& args)
{
    DateObject* dateObj = &args.thisv().toObject().as<DateObject>();
    dateObj->fillLocalTimeSlots();

    args.rval().set(dateObj->getReservedSlot(LOCAL_MONTH_SLOT));
    return true;
}

static bool
date_getMonth(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getMonth_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getUTCMonth_impl(JSContext* cx, const CallArgs& args)
{
    double d = args.thisv().toObject().as<DateObject>().UTCTime().toNumber();
    args.rval().setNumber(MonthFromTime(d));
    return true;
}

static bool
date_getUTCMonth(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getUTCMonth_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getDate_impl(JSContext* cx, const CallArgs& args)
{
    DateObject* dateObj = &args.thisv().toObject().as<DateObject>();
    dateObj->fillLocalTimeSlots();

    args.rval().set(dateObj->getReservedSlot(LOCAL_DATE_SLOT));
    return true;
}

static bool
date_getDate(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getDate_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getUTCDate_impl(JSContext* cx, const CallArgs& args)
{
    double result = args.thisv().toObject().as<DateObject>().UTCTime().toNumber();
    if (IsFinite(result))
        result = DateFromTime(result);

    args.rval().setNumber(result);
    return true;
}

static bool
date_getUTCDate(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getUTCDate_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getDay_impl(JSContext* cx, const CallArgs& args)
{
    DateObject* dateObj = &args.thisv().toObject().as<DateObject>();
    dateObj->fillLocalTimeSlots();

    args.rval().set(dateObj->getReservedSlot(LOCAL_DAY_SLOT));
    return true;
}

static bool
date_getDay(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getDay_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getUTCDay_impl(JSContext* cx, const CallArgs& args)
{
    double result = args.thisv().toObject().as<DateObject>().UTCTime().toNumber();
    if (IsFinite(result))
        result = WeekDay(result);

    args.rval().setNumber(result);
    return true;
}

static bool
date_getUTCDay(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getUTCDay_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getHours_impl(JSContext* cx, const CallArgs& args)
{
    DateObject* dateObj = &args.thisv().toObject().as<DateObject>();
    dateObj->fillLocalTimeSlots();

    args.rval().set(dateObj->getReservedSlot(LOCAL_HOURS_SLOT));
    return true;
}

static bool
date_getHours(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getHours_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getUTCHours_impl(JSContext* cx, const CallArgs& args)
{
    double result = args.thisv().toObject().as<DateObject>().UTCTime().toNumber();
    if (IsFinite(result))
        result = HourFromTime(result);

    args.rval().setNumber(result);
    return true;
}

static bool
date_getUTCHours(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getUTCHours_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getMinutes_impl(JSContext* cx, const CallArgs& args)
{
    DateObject* dateObj = &args.thisv().toObject().as<DateObject>();
    dateObj->fillLocalTimeSlots();

    args.rval().set(dateObj->getReservedSlot(LOCAL_MINUTES_SLOT));
    return true;
}

static bool
date_getMinutes(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getMinutes_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getUTCMinutes_impl(JSContext* cx, const CallArgs& args)
{
    double result = args.thisv().toObject().as<DateObject>().UTCTime().toNumber();
    if (IsFinite(result))
        result = MinFromTime(result);

    args.rval().setNumber(result);
    return true;
}

static bool
date_getUTCMinutes(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getUTCMinutes_impl>(cx, args);
}

/* Date.getSeconds is mapped to getUTCSeconds */

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getUTCSeconds_impl(JSContext* cx, const CallArgs& args)
{
    DateObject* dateObj = &args.thisv().toObject().as<DateObject>();
    dateObj->fillLocalTimeSlots();

    args.rval().set(dateObj->getReservedSlot(LOCAL_SECONDS_SLOT));
    return true;
}

static bool
date_getUTCSeconds(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getUTCSeconds_impl>(cx, args);
}

/* Date.getMilliseconds is mapped to getUTCMilliseconds */

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getUTCMilliseconds_impl(JSContext* cx, const CallArgs& args)
{
    double result = args.thisv().toObject().as<DateObject>().UTCTime().toNumber();
    if (IsFinite(result))
        result = msFromTime(result);

    args.rval().setNumber(result);
    return true;
}

static bool
date_getUTCMilliseconds(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getUTCMilliseconds_impl>(cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool
DateObject::getTimezoneOffset_impl(JSContext* cx, const CallArgs& args)
{
    DateObject* dateObj = &args.thisv().toObject().as<DateObject>();
    double utctime = dateObj->UTCTime().toNumber();
    double localtime = dateObj->cachedLocalTime();

    /*
     * Return the time zone offset in minutes for the current locale that is
     * appropriate for this time. This value would be a constant except for
     * daylight savings time.
     */
    double result = (utctime - localtime) / msPerMinute;
    args.rval().setNumber(result);
    return true;
}

static bool
date_getTimezoneOffset(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, DateObject::getTimezoneOffset_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
date_setTime_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());
    if (args.length() == 0) {
        dateObj->setUTCTime(ClippedTime::invalid(), args.rval());
        return true;
    }

    double result;
    if (!ToNumber(cx, args[0], &result))
        return false;

    dateObj->setUTCTime(TimeClip(result), args.rval());
    return true;
}

static bool
date_setTime(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setTime_impl>(cx, args);
}

static bool
GetMsecsOrDefault(JSContext* cx, const CallArgs& args, unsigned i, double t, double* millis)
{
    if (args.length() <= i) {
        *millis = msFromTime(t);
        return true;
    }
    return ToNumber(cx, args[i], millis);
}

static bool
GetSecsOrDefault(JSContext* cx, const CallArgs& args, unsigned i, double t, double* sec)
{
    if (args.length() <= i) {
        *sec = SecFromTime(t);
        return true;
    }
    return ToNumber(cx, args[i], sec);
}

static bool
GetMinsOrDefault(JSContext* cx, const CallArgs& args, unsigned i, double t, double* mins)
{
    if (args.length() <= i) {
        *mins = MinFromTime(t);
        return true;
    }
    return ToNumber(cx, args[i], mins);
}

/* ES6 20.3.4.23. */
MOZ_ALWAYS_INLINE bool
date_setMilliseconds_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    // Steps 1-2.
    double t = LocalTime(dateObj->UTCTime().toNumber());

    // Steps 3-4.
    double ms;
    if (!ToNumber(cx, args.get(0), &ms))
        return false;

    // Step 5.
    double time = MakeTime(HourFromTime(t), MinFromTime(t), SecFromTime(t), ms);

    // Step 6.
    ClippedTime u = TimeClip(UTC(MakeDate(Day(t), time)));

    // Steps 7-8.
    dateObj->setUTCTime(u, args.rval());
    return true;
}

static bool
date_setMilliseconds(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setMilliseconds_impl>(cx, args);
}

/* ES5 15.9.5.29. */
MOZ_ALWAYS_INLINE bool
date_setUTCMilliseconds_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    /* Step 1. */
    double t = dateObj->UTCTime().toNumber();

    /* Step 2. */
    double milli;
    if (!ToNumber(cx, args.get(0), &milli))
        return false;
    double time = MakeTime(HourFromTime(t), MinFromTime(t), SecFromTime(t), milli);

    /* Step 3. */
    ClippedTime v = TimeClip(MakeDate(Day(t), time));

    /* Steps 4-5. */
    dateObj->setUTCTime(v, args.rval());
    return true;
}

static bool
date_setUTCMilliseconds(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setUTCMilliseconds_impl>(cx, args);
}

/* ES5 15.9.5.30. */
MOZ_ALWAYS_INLINE bool
date_setSeconds_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    // Steps 1-2.
    double t = LocalTime(dateObj->UTCTime().toNumber());

    // Steps 3-4.
    double s;
    if (!ToNumber(cx, args.get(0), &s))
        return false;

    // Steps 5-6.
    double milli;
    if (!GetMsecsOrDefault(cx, args, 1, t, &milli))
        return false;

    // Step 7.
    double date = MakeDate(Day(t), MakeTime(HourFromTime(t), MinFromTime(t), s, milli));

    // Step 8.
    ClippedTime u = TimeClip(UTC(date));

    // Step 9.
    dateObj->setUTCTime(u, args.rval());
    return true;
}

/* ES6 20.3.4.26. */
static bool
date_setSeconds(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setSeconds_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
date_setUTCSeconds_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    /* Step 1. */
    double t = dateObj->UTCTime().toNumber();

    /* Step 2. */
    double s;
    if (!ToNumber(cx, args.get(0), &s))
        return false;

    /* Step 3. */
    double milli;
    if (!GetMsecsOrDefault(cx, args, 1, t, &milli))
        return false;

    /* Step 4. */
    double date = MakeDate(Day(t), MakeTime(HourFromTime(t), MinFromTime(t), s, milli));

    /* Step 5. */
    ClippedTime v = TimeClip(date);

    /* Steps 6-7. */
    dateObj->setUTCTime(v, args.rval());
    return true;
}

/* ES5 15.9.5.32. */
static bool
date_setUTCSeconds(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setUTCSeconds_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
date_setMinutes_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    // Steps 1-2.
    double t = LocalTime(dateObj->UTCTime().toNumber());

    // Steps 3-4.
    double m;
    if (!ToNumber(cx, args.get(0), &m))
        return false;

    // Steps 5-6.
    double s;
    if (!GetSecsOrDefault(cx, args, 1, t, &s))
        return false;

    // Steps 7-8.
    double milli;
    if (!GetMsecsOrDefault(cx, args, 2, t, &milli))
        return false;

    // Step 9.
    double date = MakeDate(Day(t), MakeTime(HourFromTime(t), m, s, milli));

    // Step 10.
    ClippedTime u = TimeClip(UTC(date));

    // Steps 11-12.
    dateObj->setUTCTime(u, args.rval());
    return true;
}

/* ES6 20.3.4.24. */
static bool
date_setMinutes(JSContext* cx, unsigned argc, Value* vp)
{
    // Steps 1-2 (the effectful parts).
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setMinutes_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
date_setUTCMinutes_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    /* Step 1. */
    double t = dateObj->UTCTime().toNumber();

    /* Step 2. */
    double m;
    if (!ToNumber(cx, args.get(0), &m))
        return false;

    /* Step 3. */
    double s;
    if (!GetSecsOrDefault(cx, args, 1, t, &s))
        return false;

    /* Step 4. */
    double milli;
    if (!GetMsecsOrDefault(cx, args, 2, t, &milli))
        return false;

    /* Step 5. */
    double date = MakeDate(Day(t), MakeTime(HourFromTime(t), m, s, milli));

    /* Step 6. */
    ClippedTime v = TimeClip(date);

    /* Steps 7-8. */
    dateObj->setUTCTime(v, args.rval());
    return true;
}

/* ES5 15.9.5.34. */
static bool
date_setUTCMinutes(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setUTCMinutes_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
date_setHours_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    // Steps 1-2.
    double t = LocalTime(dateObj->UTCTime().toNumber());

    // Steps 3-4.
    double h;
    if (!ToNumber(cx, args.get(0), &h))
        return false;

    // Steps 5-6.
    double m;
    if (!GetMinsOrDefault(cx, args, 1, t, &m))
        return false;

    // Steps 7-8.
    double s;
    if (!GetSecsOrDefault(cx, args, 2, t, &s))
        return false;

    // Steps 9-10.
    double milli;
    if (!GetMsecsOrDefault(cx, args, 3, t, &milli))
        return false;

    // Step 11.
    double date = MakeDate(Day(t), MakeTime(h, m, s, milli));

    // Step 12.
    ClippedTime u = TimeClip(UTC(date));

    // Steps 13-14.
    dateObj->setUTCTime(u, args.rval());
    return true;
}

/* ES5 15.9.5.35. */
static bool
date_setHours(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setHours_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
date_setUTCHours_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    /* Step 1. */
    double t = dateObj->UTCTime().toNumber();

    /* Step 2. */
    double h;
    if (!ToNumber(cx, args.get(0), &h))
        return false;

    /* Step 3. */
    double m;
    if (!GetMinsOrDefault(cx, args, 1, t, &m))
        return false;

    /* Step 4. */
    double s;
    if (!GetSecsOrDefault(cx, args, 2, t, &s))
        return false;

    /* Step 5. */
    double milli;
    if (!GetMsecsOrDefault(cx, args, 3, t, &milli))
        return false;

    /* Step 6. */
    double newDate = MakeDate(Day(t), MakeTime(h, m, s, milli));

    /* Step 7. */
    ClippedTime v = TimeClip(newDate);

    /* Steps 8-9. */
    dateObj->setUTCTime(v, args.rval());
    return true;
}

/* ES5 15.9.5.36. */
static bool
date_setUTCHours(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setUTCHours_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
date_setDate_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    /* Step 1. */
    double t = LocalTime(dateObj->UTCTime().toNumber());

    /* Step 2. */
    double date;
    if (!ToNumber(cx, args.get(0), &date))
        return false;

    /* Step 3. */
    double newDate = MakeDate(MakeDay(YearFromTime(t), MonthFromTime(t), date), TimeWithinDay(t));

    /* Step 4. */
    ClippedTime u = TimeClip(UTC(newDate));

    /* Steps 5-6. */
    dateObj->setUTCTime(u, args.rval());
    return true;
}

/* ES5 15.9.5.37. */
static bool
date_setDate(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setDate_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
date_setUTCDate_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    /* Step 1. */
    double t = dateObj->UTCTime().toNumber();

    /* Step 2. */
    double date;
    if (!ToNumber(cx, args.get(0), &date))
        return false;

    /* Step 3. */
    double newDate = MakeDate(MakeDay(YearFromTime(t), MonthFromTime(t), date), TimeWithinDay(t));

    /* Step 4. */
    ClippedTime v = TimeClip(newDate);

    /* Steps 5-6. */
    dateObj->setUTCTime(v, args.rval());
    return true;
}

static bool
date_setUTCDate(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setUTCDate_impl>(cx, args);
}

static bool
GetDateOrDefault(JSContext* cx, const CallArgs& args, unsigned i, double t, double* date)
{
    if (args.length() <= i) {
        *date = DateFromTime(t);
        return true;
    }
    return ToNumber(cx, args[i], date);
}

static bool
GetMonthOrDefault(JSContext* cx, const CallArgs& args, unsigned i, double t, double* month)
{
    if (args.length() <= i) {
        *month = MonthFromTime(t);
        return true;
    }
    return ToNumber(cx, args[i], month);
}

/* ES5 15.9.5.38. */
MOZ_ALWAYS_INLINE bool
date_setMonth_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    /* Step 1. */
    double t = LocalTime(dateObj->UTCTime().toNumber());

    /* Step 2. */
    double m;
    if (!ToNumber(cx, args.get(0), &m))
        return false;

    /* Step 3. */
    double date;
    if (!GetDateOrDefault(cx, args, 1, t, &date))
        return false;

    /* Step 4. */
    double newDate = MakeDate(MakeDay(YearFromTime(t), m, date), TimeWithinDay(t));

    /* Step 5. */
    ClippedTime u = TimeClip(UTC(newDate));

    /* Steps 6-7. */
    dateObj->setUTCTime(u, args.rval());
    return true;
}

static bool
date_setMonth(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setMonth_impl>(cx, args);
}

/* ES5 15.9.5.39. */
MOZ_ALWAYS_INLINE bool
date_setUTCMonth_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    /* Step 1. */
    double t = dateObj->UTCTime().toNumber();

    /* Step 2. */
    double m;
    if (!ToNumber(cx, args.get(0), &m))
        return false;

    /* Step 3. */
    double date;
    if (!GetDateOrDefault(cx, args, 1, t, &date))
        return false;

    /* Step 4. */
    double newDate = MakeDate(MakeDay(YearFromTime(t), m, date), TimeWithinDay(t));

    /* Step 5. */
    ClippedTime v = TimeClip(newDate);

    /* Steps 6-7. */
    dateObj->setUTCTime(v, args.rval());
    return true;
}

static bool
date_setUTCMonth(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setUTCMonth_impl>(cx, args);
}

static double
ThisLocalTimeOrZero(Handle<DateObject*> dateObj)
{
    double t = dateObj->UTCTime().toNumber();
    if (IsNaN(t))
        return +0;
    return LocalTime(t);
}

static double
ThisUTCTimeOrZero(Handle<DateObject*> dateObj)
{
    double t = dateObj->as<DateObject>().UTCTime().toNumber();
    return IsNaN(t) ? +0 : t;
}

/* ES5 15.9.5.40. */
MOZ_ALWAYS_INLINE bool
date_setFullYear_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    /* Step 1. */
    double t = ThisLocalTimeOrZero(dateObj);

    /* Step 2. */
    double y;
    if (!ToNumber(cx, args.get(0), &y))
        return false;

    /* Step 3. */
    double m;
    if (!GetMonthOrDefault(cx, args, 1, t, &m))
        return false;

    /* Step 4. */
    double date;
    if (!GetDateOrDefault(cx, args, 2, t, &date))
        return false;

    /* Step 5. */
    double newDate = MakeDate(MakeDay(y, m, date), TimeWithinDay(t));

    /* Step 6. */
    ClippedTime u = TimeClip(UTC(newDate));

    /* Steps 7-8. */
    dateObj->setUTCTime(u, args.rval());
    return true;
}

static bool
date_setFullYear(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setFullYear_impl>(cx, args);
}

/* ES5 15.9.5.41. */
MOZ_ALWAYS_INLINE bool
date_setUTCFullYear_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    /* Step 1. */
    double t = ThisUTCTimeOrZero(dateObj);

    /* Step 2. */
    double y;
    if (!ToNumber(cx, args.get(0), &y))
        return false;

    /* Step 3. */
    double m;
    if (!GetMonthOrDefault(cx, args, 1, t, &m))
        return false;

    /* Step 4. */
    double date;
    if (!GetDateOrDefault(cx, args, 2, t, &date))
        return false;

    /* Step 5. */
    double newDate = MakeDate(MakeDay(y, m, date), TimeWithinDay(t));

    /* Step 6. */
    ClippedTime v = TimeClip(newDate);

    /* Steps 7-8. */
    dateObj->setUTCTime(v, args.rval());
    return true;
}

static bool
date_setUTCFullYear(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setUTCFullYear_impl>(cx, args);
}

/* ES5 Annex B.2.5. */
MOZ_ALWAYS_INLINE bool
date_setYear_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    /* Step 1. */
    double t = ThisLocalTimeOrZero(dateObj);

    /* Step 2. */
    double y;
    if (!ToNumber(cx, args.get(0), &y))
        return false;

    /* Step 3. */
    if (IsNaN(y)) {
        dateObj->setUTCTime(ClippedTime::invalid(), args.rval());
        return true;
    }

    /* Step 4. */
    double yint = ToInteger(y);
    if (0 <= yint && yint <= 99)
        yint += 1900;

    /* Step 5. */
    double day = MakeDay(yint, MonthFromTime(t), DateFromTime(t));

    /* Step 6. */
    double u = UTC(MakeDate(day, TimeWithinDay(t)));

    /* Steps 7-8. */
    dateObj->setUTCTime(TimeClip(u), args.rval());
    return true;
}

static bool
date_setYear(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_setYear_impl>(cx, args);
}

/* constants for toString, toUTCString */
static const char js_NaN_date_str[] = "Invalid Date";
static const char * const days[] =
{
   "Sun","Mon","Tue","Wed","Thu","Fri","Sat"
};
static const char * const months[] =
{
   "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};


// Avoid dependence on PRMJ_FormatTimeUSEnglish, because it
// requires a PRMJTime... which only has 16-bit years.  Sub-ECMA.
static void
print_gmt_string(char* buf, size_t size, double utctime)
{
    MOZ_ASSERT(NumbersAreIdentical(TimeClip(utctime).toDouble(), utctime));
    JS_snprintf(buf, size, "%s, %.2d %s %.4d %.2d:%.2d:%.2d GMT",
                days[int(WeekDay(utctime))],
                int(DateFromTime(utctime)),
                months[int(MonthFromTime(utctime))],
                int(YearFromTime(utctime)),
                int(HourFromTime(utctime)),
                int(MinFromTime(utctime)),
                int(SecFromTime(utctime)));
}

static void
print_iso_string(char* buf, size_t size, double utctime)
{
    MOZ_ASSERT(NumbersAreIdentical(TimeClip(utctime).toDouble(), utctime));
    JS_snprintf(buf, size, "%.4d-%.2d-%.2dT%.2d:%.2d:%.2d.%.3dZ",
                int(YearFromTime(utctime)),
                int(MonthFromTime(utctime)) + 1,
                int(DateFromTime(utctime)),
                int(HourFromTime(utctime)),
                int(MinFromTime(utctime)),
                int(SecFromTime(utctime)),
                int(msFromTime(utctime)));
}

static void
print_iso_extended_string(char* buf, size_t size, double utctime)
{
    MOZ_ASSERT(NumbersAreIdentical(TimeClip(utctime).toDouble(), utctime));
    JS_snprintf(buf, size, "%+.6d-%.2d-%.2dT%.2d:%.2d:%.2d.%.3dZ",
                int(YearFromTime(utctime)),
                int(MonthFromTime(utctime)) + 1,
                int(DateFromTime(utctime)),
                int(HourFromTime(utctime)),
                int(MinFromTime(utctime)),
                int(SecFromTime(utctime)),
                int(msFromTime(utctime)));
}

/* ES5 B.2.6. */
MOZ_ALWAYS_INLINE bool
date_toGMTString_impl(JSContext* cx, const CallArgs& args)
{
    double utctime = args.thisv().toObject().as<DateObject>().UTCTime().toNumber();

    char buf[100];
    if (!IsFinite(utctime))
        JS_snprintf(buf, sizeof buf, js_NaN_date_str);
    else
        print_gmt_string(buf, sizeof buf, utctime);

    JSString* str = JS_NewStringCopyZ(cx, buf);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

static bool
date_toGMTString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_toGMTString_impl>(cx, args);
}

/* ES6 draft 2015-01-15 20.3.4.36. */
MOZ_ALWAYS_INLINE bool
date_toISOString_impl(JSContext* cx, const CallArgs& args)
{
    double utctime = args.thisv().toObject().as<DateObject>().UTCTime().toNumber();
    if (!IsFinite(utctime)) {
        JS_ReportErrorNumber(cx, js::GetErrorMessage, nullptr, JSMSG_INVALID_DATE);
        return false;
    }

    char buf[100];
    int year = int(YearFromTime(utctime));
    if (year < 0 || year > 9999)
        print_iso_extended_string(buf, sizeof buf, utctime);
    else
        print_iso_string(buf, sizeof buf, utctime);

    JSString* str = JS_NewStringCopyZ(cx, buf);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;

}

static bool
date_toISOString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_toISOString_impl>(cx, args);
}

/* ES5 15.9.5.44. */
static bool
date_toJSON(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Step 2. */
    RootedValue tv(cx, ObjectValue(*obj));
    if (!ToPrimitive(cx, JSTYPE_NUMBER, &tv))
        return false;

    /* Step 3. */
    if (tv.isDouble() && !IsFinite(tv.toDouble())) {
        args.rval().setNull();
        return true;
    }

    /* Step 4. */
    RootedValue toISO(cx);
    if (!GetProperty(cx, obj, obj, cx->names().toISOString, &toISO))
        return false;

    /* Step 5. */
    if (!IsCallable(toISO)) {
        JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, js::GetErrorMessage, nullptr,
                                     JSMSG_BAD_TOISOSTRING_PROP);
        return false;
    }

    /* Step 6. */
    InvokeArgs args2(cx);
    if (!args2.init(0))
        return false;

    args2.setCallee(toISO);
    args2.setThis(ObjectValue(*obj));

    if (!Invoke(cx, args2))
        return false;
    args.rval().set(args2.rval());
    return true;
}

/* for Date.toLocaleFormat; interface to PRMJTime date struct.
 */
static void
new_explode(double timeval, PRMJTime* split)
{
    double year = YearFromTime(timeval);

    split->tm_usec = int32_t(msFromTime(timeval)) * 1000;
    split->tm_sec = int8_t(SecFromTime(timeval));
    split->tm_min = int8_t(MinFromTime(timeval));
    split->tm_hour = int8_t(HourFromTime(timeval));
    split->tm_mday = int8_t(DateFromTime(timeval));
    split->tm_mon = int8_t(MonthFromTime(timeval));
    split->tm_wday = int8_t(WeekDay(timeval));
    split->tm_year = year;
    split->tm_yday = int16_t(DayWithinYear(timeval, year));

    /* not sure how this affects things, but it doesn't seem
       to matter. */
    split->tm_isdst = (DaylightSavingTA(timeval) != 0);
}

typedef enum formatspec {
    FORMATSPEC_FULL, FORMATSPEC_DATE, FORMATSPEC_TIME
} formatspec;

/* helper function */
static bool
date_format(JSContext* cx, double date, formatspec format, MutableHandleValue rval)
{
    char buf[100];
    char tzbuf[100];
    bool usetz;
    size_t i, tzlen;
    PRMJTime split;

    if (!IsFinite(date)) {
        JS_snprintf(buf, sizeof buf, js_NaN_date_str);
    } else {
        MOZ_ASSERT(NumbersAreIdentical(TimeClip(date).toDouble(), date));

        double local = LocalTime(date);

        /* offset from GMT in minutes.  The offset includes daylight savings,
           if it applies. */
        int minutes = (int) floor(AdjustTime(date) / msPerMinute);

        /* map 510 minutes to 0830 hours */
        int offset = (minutes / 60) * 100 + minutes % 60;

        /* print as "Wed Nov 05 19:38:03 GMT-0800 (PST) 1997" The TZA is
         * printed as 'GMT-0800' rather than as 'PST' to avoid
         * operating-system dependence on strftime (which
         * PRMJ_FormatTimeUSEnglish calls, for %Z only.)  win32 prints
         * PST as 'Pacific Standard Time.'  This way we always know
         * what we're getting, and can parse it if we produce it.
         * The OS TZA string is included as a comment.
         */

        /* get a timezone string from the OS to include as a
           comment. */
        new_explode(date, &split);
        if (PRMJ_FormatTime(tzbuf, sizeof tzbuf, "(%Z)", &split) != 0) {

            /* Decide whether to use the resulting timezone string.
             *
             * Reject it if it contains any non-ASCII, non-alphanumeric
             * characters.  It's then likely in some other character
             * encoding, and we probably won't display it correctly.
             */
            usetz = true;
            tzlen = strlen(tzbuf);
            if (tzlen > 100) {
                usetz = false;
            } else {
                for (i = 0; i < tzlen; i++) {
                    char16_t c = tzbuf[i];
                    if (c > 127 ||
                        !(isalpha(c) || isdigit(c) ||
                          c == ' ' || c == '(' || c == ')' || c == '.')) {
                        usetz = false;
                    }
                }
            }

            /* Also reject it if it's not parenthesized or if it's '()'. */
            if (tzbuf[0] != '(' || tzbuf[1] == ')')
                usetz = false;
        } else
            usetz = false;

        switch (format) {
          case FORMATSPEC_FULL:
            /*
             * Avoid dependence on PRMJ_FormatTimeUSEnglish, because it
             * requires a PRMJTime... which only has 16-bit years.  Sub-ECMA.
             */
            /* Tue Oct 31 2000 09:41:40 GMT-0800 (PST) */
            JS_snprintf(buf, sizeof buf,
                        "%s %s %.2d %.4d %.2d:%.2d:%.2d GMT%+.4d%s%s",
                        days[int(WeekDay(local))],
                        months[int(MonthFromTime(local))],
                        int(DateFromTime(local)),
                        int(YearFromTime(local)),
                        int(HourFromTime(local)),
                        int(MinFromTime(local)),
                        int(SecFromTime(local)),
                        offset,
                        usetz ? " " : "",
                        usetz ? tzbuf : "");
            break;
          case FORMATSPEC_DATE:
            /* Tue Oct 31 2000 */
            JS_snprintf(buf, sizeof buf,
                        "%s %s %.2d %.4d",
                        days[int(WeekDay(local))],
                        months[int(MonthFromTime(local))],
                        int(DateFromTime(local)),
                        int(YearFromTime(local)));
            break;
          case FORMATSPEC_TIME:
            /* 09:41:40 GMT-0800 (PST) */
            JS_snprintf(buf, sizeof buf,
                        "%.2d:%.2d:%.2d GMT%+.4d%s%s",
                        int(HourFromTime(local)),
                        int(MinFromTime(local)),
                        int(SecFromTime(local)),
                        offset,
                        usetz ? " " : "",
                        usetz ? tzbuf : "");
            break;
        }
    }

    JSString* str = JS_NewStringCopyZ(cx, buf);
    if (!str)
        return false;
    rval.setString(str);
    return true;
}

static bool
ToLocaleFormatHelper(JSContext* cx, HandleObject obj, const char* format, MutableHandleValue rval)
{
    double utctime = obj->as<DateObject>().UTCTime().toNumber();

    char buf[100];
    if (!IsFinite(utctime)) {
        JS_snprintf(buf, sizeof buf, js_NaN_date_str);
    } else {
        int result_len;
        double local = LocalTime(utctime);
        PRMJTime split;
        new_explode(local, &split);

        /* Let PRMJTime format it. */
        result_len = PRMJ_FormatTime(buf, sizeof buf, format, &split);

        /* If it failed, default to toString. */
        if (result_len == 0)
            return date_format(cx, utctime, FORMATSPEC_FULL, rval);

        /* Hacked check against undesired 2-digit year 00/00/00 form. */
        if (strcmp(format, "%x") == 0 && result_len >= 6 &&
            /* Format %x means use OS settings, which may have 2-digit yr, so
               hack end of 3/11/22 or 11.03.22 or 11Mar22 to use 4-digit yr...*/
            !isdigit(buf[result_len - 3]) &&
            isdigit(buf[result_len - 2]) && isdigit(buf[result_len - 1]) &&
            /* ...but not if starts with 4-digit year, like 2022/3/11. */
            !(isdigit(buf[0]) && isdigit(buf[1]) &&
              isdigit(buf[2]) && isdigit(buf[3]))) {
            double localtime = obj->as<DateObject>().cachedLocalTime();
            int year = IsNaN(localtime) ? 0 : (int) YearFromTime(localtime);
            JS_snprintf(buf + (result_len - 2), (sizeof buf) - (result_len - 2),
                        "%d", year);
        }

    }

    if (cx->runtime()->localeCallbacks && cx->runtime()->localeCallbacks->localeToUnicode)
        return cx->runtime()->localeCallbacks->localeToUnicode(cx, buf, rval);

    JSString* str = JS_NewStringCopyZ(cx, buf);
    if (!str)
        return false;
    rval.setString(str);
    return true;
}

#if !EXPOSE_INTL_API
static bool
ToLocaleStringHelper(JSContext* cx, Handle<DateObject*> dateObj, MutableHandleValue rval)
{
    /*
     * Use '%#c' for windows, because '%c' is backward-compatible and non-y2k
     * with msvc; '%#c' requests that a full year be used in the result string.
     */
    return ToLocaleFormatHelper(cx, dateObj,
#if defined(_WIN32) && !defined(__MWERKS__)
                          "%#c"
#else
                          "%c"
#endif
                         , rval);
}

/* ES5 15.9.5.5. */
MOZ_ALWAYS_INLINE bool
date_toLocaleString_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());
    return ToLocaleStringHelper(cx, dateObj, args.rval());
}

static bool
date_toLocaleString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_toLocaleString_impl>(cx, args);
}

/* ES5 15.9.5.6. */
MOZ_ALWAYS_INLINE bool
date_toLocaleDateString_impl(JSContext* cx, const CallArgs& args)
{
    /*
     * Use '%#x' for windows, because '%x' is backward-compatible and non-y2k
     * with msvc; '%#x' requests that a full year be used in the result string.
     */
    static const char format[] =
#if defined(_WIN32) && !defined(__MWERKS__)
                                   "%#x"
#else
                                   "%x"
#endif
                                   ;

    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());
    return ToLocaleFormatHelper(cx, dateObj, format, args.rval());
}

static bool
date_toLocaleDateString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_toLocaleDateString_impl>(cx, args);
}

/* ES5 15.9.5.7. */
MOZ_ALWAYS_INLINE bool
date_toLocaleTimeString_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());
    return ToLocaleFormatHelper(cx, dateObj, "%X", args.rval());
}

static bool
date_toLocaleTimeString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_toLocaleTimeString_impl>(cx, args);
}
#endif /* !EXPOSE_INTL_API */

MOZ_ALWAYS_INLINE bool
date_toLocaleFormat_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());

    if (args.length() == 0) {
        /*
         * Use '%#c' for windows, because '%c' is backward-compatible and non-y2k
         * with msvc; '%#c' requests that a full year be used in the result string.
         */
        return ToLocaleFormatHelper(cx, dateObj,
#if defined(_WIN32) && !defined(__MWERKS__)
                              "%#c"
#else
                              "%c"
#endif
                             , args.rval());
    }

    RootedString fmt(cx, ToString<CanGC>(cx, args[0]));
    if (!fmt)
        return false;

    JSAutoByteString fmtbytes(cx, fmt);
    if (!fmtbytes)
        return false;

    return ToLocaleFormatHelper(cx, dateObj, fmtbytes.ptr(), args.rval());
}

static bool
date_toLocaleFormat(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_toLocaleFormat_impl>(cx, args);
}

/* ES5 15.9.5.4. */
MOZ_ALWAYS_INLINE bool
date_toTimeString_impl(JSContext* cx, const CallArgs& args)
{
    return date_format(cx, args.thisv().toObject().as<DateObject>().UTCTime().toNumber(),
                       FORMATSPEC_TIME, args.rval());
}

static bool
date_toTimeString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_toTimeString_impl>(cx, args);
}

/* ES5 15.9.5.3. */
MOZ_ALWAYS_INLINE bool
date_toDateString_impl(JSContext* cx, const CallArgs& args)
{
    return date_format(cx, args.thisv().toObject().as<DateObject>().UTCTime().toNumber(),
                       FORMATSPEC_DATE, args.rval());
}

static bool
date_toDateString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_toDateString_impl>(cx, args);
}

#if JS_HAS_TOSOURCE
MOZ_ALWAYS_INLINE bool
date_toSource_impl(JSContext* cx, const CallArgs& args)
{
    StringBuffer sb(cx);
    if (!sb.append("(new Date(") ||
        !NumberValueToStringBuffer(cx, args.thisv().toObject().as<DateObject>().UTCTime(), sb) ||
        !sb.append("))"))
    {
        return false;
    }

    JSString* str = sb.finishString();
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

static bool
date_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_toSource_impl>(cx, args);
}
#endif

MOZ_ALWAYS_INLINE bool
IsObject(HandleValue v)
{
    return v.isObject();
}

// ES6 20.3.4.41.
MOZ_ALWAYS_INLINE bool
date_toString_impl(JSContext* cx, const CallArgs& args)
{
    // Step 1.
    RootedObject obj(cx, &args.thisv().toObject());

    // Step 2.
    ESClassValue cls;
    if (!GetBuiltinClass(cx, obj, &cls))
        return false;

    double tv;
    if (cls != ESClass_Date) {
        // Step 2.
        tv = GenericNaN();
    } else {
        // Step 3.
        RootedValue unboxed(cx);
        if (!Unbox(cx, obj, &unboxed))
            return false;

        tv = unboxed.toNumber();
    }

    // Step 4.
    return date_format(cx, tv, FORMATSPEC_FULL, args.rval());
}

bool
date_toString(JSContext* cx, unsigned argc, Value* vp)
{
    // Step 1.
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsObject, date_toString_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool
date_valueOf_impl(JSContext* cx, const CallArgs& args)
{
    Rooted<DateObject*> dateObj(cx, &args.thisv().toObject().as<DateObject>());
    args.rval().set(dateObj->UTCTime());
    return true;
}

bool
js::date_valueOf(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsDate, date_valueOf_impl>(cx, args);
}

// ES6 20.3.4.45 Date.prototype[@@toPrimitive]
static bool
date_toPrimitive(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Steps 1-2.
    if (!args.thisv().isObject()) {
        ReportIncompatible(cx, args);
        return false;
    }

    // Steps 3-5.
    JSType hint;
    if (!GetFirstArgumentAsTypeHint(cx, args, &hint))
        return false;
    if (hint == JSTYPE_VOID)
        hint = JSTYPE_STRING;

    args.rval().set(args.thisv());
    RootedObject obj(cx, &args.thisv().toObject());
    return OrdinaryToPrimitive(cx, obj, hint, args.rval());
}

static const JSFunctionSpec date_static_methods[] = {
    JS_FN("UTC",                 date_UTC,                7,0),
    JS_FN("parse",               date_parse,              1,0),
    JS_FN("now",                 date_now,                0,0),
    JS_FS_END
};

static const JSFunctionSpec date_methods[] = {
    JS_FN("getTime",             date_getTime,            0,0),
    JS_FN("getTimezoneOffset",   date_getTimezoneOffset,  0,0),
    JS_FN("getYear",             date_getYear,            0,0),
    JS_FN("getFullYear",         date_getFullYear,        0,0),
    JS_FN("getUTCFullYear",      date_getUTCFullYear,     0,0),
    JS_FN("getMonth",            date_getMonth,           0,0),
    JS_FN("getUTCMonth",         date_getUTCMonth,        0,0),
    JS_FN("getDate",             date_getDate,            0,0),
    JS_FN("getUTCDate",          date_getUTCDate,         0,0),
    JS_FN("getDay",              date_getDay,             0,0),
    JS_FN("getUTCDay",           date_getUTCDay,          0,0),
    JS_FN("getHours",            date_getHours,           0,0),
    JS_FN("getUTCHours",         date_getUTCHours,        0,0),
    JS_FN("getMinutes",          date_getMinutes,         0,0),
    JS_FN("getUTCMinutes",       date_getUTCMinutes,      0,0),
    JS_FN("getSeconds",          date_getUTCSeconds,      0,0),
    JS_FN("getUTCSeconds",       date_getUTCSeconds,      0,0),
    JS_FN("getMilliseconds",     date_getUTCMilliseconds, 0,0),
    JS_FN("getUTCMilliseconds",  date_getUTCMilliseconds, 0,0),
    JS_FN("setTime",             date_setTime,            1,0),
    JS_FN("setYear",             date_setYear,            1,0),
    JS_FN("setFullYear",         date_setFullYear,        3,0),
    JS_FN("setUTCFullYear",      date_setUTCFullYear,     3,0),
    JS_FN("setMonth",            date_setMonth,           2,0),
    JS_FN("setUTCMonth",         date_setUTCMonth,        2,0),
    JS_FN("setDate",             date_setDate,            1,0),
    JS_FN("setUTCDate",          date_setUTCDate,         1,0),
    JS_FN("setHours",            date_setHours,           4,0),
    JS_FN("setUTCHours",         date_setUTCHours,        4,0),
    JS_FN("setMinutes",          date_setMinutes,         3,0),
    JS_FN("setUTCMinutes",       date_setUTCMinutes,      3,0),
    JS_FN("setSeconds",          date_setSeconds,         2,0),
    JS_FN("setUTCSeconds",       date_setUTCSeconds,      2,0),
    JS_FN("setMilliseconds",     date_setMilliseconds,    1,0),
    JS_FN("setUTCMilliseconds",  date_setUTCMilliseconds, 1,0),
    JS_FN("toUTCString",         date_toGMTString,        0,0),
    JS_FN("toLocaleFormat",      date_toLocaleFormat,     0,0),
#if EXPOSE_INTL_API
    JS_SELF_HOSTED_FN(js_toLocaleString_str, "Date_toLocaleString", 0,0),
    JS_SELF_HOSTED_FN("toLocaleDateString", "Date_toLocaleDateString", 0,0),
    JS_SELF_HOSTED_FN("toLocaleTimeString", "Date_toLocaleTimeString", 0,0),
#else
    JS_FN(js_toLocaleString_str, date_toLocaleString,     0,0),
    JS_FN("toLocaleDateString",  date_toLocaleDateString, 0,0),
    JS_FN("toLocaleTimeString",  date_toLocaleTimeString, 0,0),
#endif
    JS_FN("toDateString",        date_toDateString,       0,0),
    JS_FN("toTimeString",        date_toTimeString,       0,0),
    JS_FN("toISOString",         date_toISOString,        0,0),
    JS_FN(js_toJSON_str,         date_toJSON,             1,0),
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,       date_toSource,           0,0),
#endif
    JS_FN(js_toString_str,       date_toString,           0,0),
    JS_FN(js_valueOf_str,        date_valueOf,            0,0),
    JS_SYM_FN(toPrimitive,       date_toPrimitive,        1,JSPROP_READONLY),
    JS_FS_END
};

static bool
NewDateObject(JSContext* cx, const CallArgs& args, ClippedTime t)
{
    MOZ_ASSERT(args.isConstructing());

    RootedObject proto(cx);
    RootedObject newTarget(cx, &args.newTarget().toObject());
    if (!GetPrototypeFromConstructor(cx, newTarget, &proto))
        return false;

    JSObject* obj = NewDateObjectMsec(cx, t, proto);
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

static bool
ToDateString(JSContext* cx, const CallArgs& args, ClippedTime t)
{
    return date_format(cx, t.toDouble(), FORMATSPEC_FULL, args.rval());
}

static bool
DateNoArguments(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(args.length() == 0);

    ClippedTime now = NowAsMillis();

    if (args.isConstructing())
        return NewDateObject(cx, args, now);

    return ToDateString(cx, args, now);
}

static bool
DateOneArgument(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(args.length() == 1);

    if (args.isConstructing()) {
        if (args[0].isObject()) {
            RootedObject obj(cx, &args[0].toObject());

            ESClassValue cls;
            if (!GetBuiltinClass(cx, obj, &cls))
                return false;

            if (cls == ESClass_Date) {
                RootedValue unboxed(cx);
                if (!Unbox(cx, obj, &unboxed))
                    return false;

                return NewDateObject(cx, args, TimeClip(unboxed.toNumber()));
            }
        }

        if (!ToPrimitive(cx, args[0]))
            return false;

        ClippedTime t;
        if (args[0].isString()) {
            JSLinearString* linearStr = args[0].toString()->ensureLinear(cx);
            if (!linearStr)
                return false;

            if (!ParseDate(linearStr, &t))
                t = ClippedTime::invalid();
        } else {
            double d;
            if (!ToNumber(cx, args[0], &d))
                return false;
            t = TimeClip(d);
        }

        return NewDateObject(cx, args, t);
    }

    return ToDateString(cx, args, NowAsMillis());
}

static bool
DateMultipleArguments(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(args.length() >= 2);

    // Step 3.
    if (args.isConstructing()) {
        // Steps 3a-b.
        double y;
        if (!ToNumber(cx, args[0], &y))
            return false;

        // Steps 3c-d.
        double m;
        if (!ToNumber(cx, args[1], &m))
            return false;

        // Steps 3e-f.
        double dt;
        if (args.length() >= 3) {
            if (!ToNumber(cx, args[2], &dt))
                return false;
        } else {
            dt = 1;
        }

        // Steps 3g-h.
        double h;
        if (args.length() >= 4) {
            if (!ToNumber(cx, args[3], &h))
                return false;
        } else {
            h = 0;
        }

        // Steps 3i-j.
        double min;
        if (args.length() >= 5) {
            if (!ToNumber(cx, args[4], &min))
                return false;
        } else {
            min = 0;
        }

        // Steps 3k-l.
        double s;
        if (args.length() >= 6) {
            if (!ToNumber(cx, args[5], &s))
                return false;
        } else {
            s = 0;
        }

        // Steps 3m-n.
        double milli;
        if (args.length() >= 7) {
            if (!ToNumber(cx, args[6], &milli))
                return false;
        } else {
            milli = 0;
        }

        // Step 3o.
        double yr = y;
        if (!IsNaN(y)) {
            double yint = ToInteger(y);
            if (0 <= yint && yint <= 99)
                yr = 1900 + yint;
        }

        // Step 3p.
        double finalDate = MakeDate(MakeDay(yr, m, dt), MakeTime(h, min, s, milli));

        // Steps 3q-t.
        return NewDateObject(cx, args, TimeClip(UTC(finalDate)));
    }

    return ToDateString(cx, args, NowAsMillis());
}

bool
js::DateConstructor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0)
        return DateNoArguments(cx, args);

    if (args.length() == 1)
        return DateOneArgument(cx, args);

    return DateMultipleArguments(cx, args);
}

// ES6 final draft 20.3.4.
static JSObject*
CreateDatePrototype(JSContext* cx, JSProtoKey key)
{
    return cx->global()->createBlankPrototype(cx, &DateObject::protoClass_);
}

static bool
FinishDateClassInit(JSContext* cx, HandleObject ctor, HandleObject proto)
{
    /*
     * Date.prototype.toGMTString has the same initial value as
     * Date.prototype.toUTCString.
     */
    RootedValue toUTCStringFun(cx);
    RootedId toUTCStringId(cx, NameToId(cx->names().toUTCString));
    RootedId toGMTStringId(cx, NameToId(cx->names().toGMTString));
    return NativeGetProperty(cx, proto.as<NativeObject>(), toUTCStringId, &toUTCStringFun) &&
           NativeDefineProperty(cx, proto.as<NativeObject>(), toGMTStringId, toUTCStringFun,
                                nullptr, nullptr, 0);
}

const Class DateObject::class_ = {
    js_Date_str,
    JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Date),
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    nullptr, /* finalize */
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    nullptr, /* trace */
    {
        GenericCreateConstructor<DateConstructor, 7, gc::AllocKind::FUNCTION>,
        CreateDatePrototype,
        date_static_methods,
        nullptr,
        date_methods,
        nullptr,
        FinishDateClassInit
    }
};

const Class DateObject::protoClass_ = {
    js_Object_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Date),
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    nullptr, /* finalize */
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    nullptr, /* trace  */
    {
        DELEGATED_CLASSSPEC(&DateObject::class_.spec),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        ClassSpec::IsDelegated
    }
};

JSObject*
js::NewDateObjectMsec(JSContext* cx, ClippedTime t, HandleObject proto /* = nullptr */)
{
    JSObject* obj = NewObjectWithClassProto(cx, &DateObject::class_, proto);
    if (!obj)
        return nullptr;
    obj->as<DateObject>().setUTCTime(t);
    return obj;
}

JS_FRIEND_API(JSObject*)
js::NewDateObject(JSContext* cx, int year, int mon, int mday,
                  int hour, int min, int sec)
{
    MOZ_ASSERT(mon < 12);
    double msec_time = MakeDate(MakeDay(year, mon, mday), MakeTime(hour, min, sec, 0.0));
    return NewDateObjectMsec(cx, TimeClip(UTC(msec_time)));
}

JS_FRIEND_API(bool)
js::DateIsValid(JSContext* cx, HandleObject obj, bool* isValid)
{
    ESClassValue cls;
    if (!GetBuiltinClass(cx, obj, &cls))
        return false;

    if (cls != ESClass_Date) {
        *isValid = false;
        return true;
    }

    RootedValue unboxed(cx);
    if (!Unbox(cx, obj, &unboxed))
        return false;

    *isValid = !IsNaN(unboxed.toNumber());
    return true;
}

JS_FRIEND_API(bool)
js::DateGetMsecSinceEpoch(JSContext* cx, HandleObject obj, double* msecsSinceEpoch)
{
    ESClassValue cls;
    if (!GetBuiltinClass(cx, obj, &cls))
        return false;

    if (cls != ESClass_Date) {
        *msecsSinceEpoch = 0;
        return true;
    }

    RootedValue unboxed(cx);
    if (!Unbox(cx, obj, &unboxed))
        return false;

    *msecsSinceEpoch = unboxed.toNumber();
    return true;
}
