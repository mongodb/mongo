/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript date/time computation and creation functions. */

#ifndef js_Date_h
#define js_Date_h

/*
 * Dates in JavaScript are defined by IEEE-754 double precision numbers from
 * the set:
 *
 *   { t ∈ ℕ : -8.64e15 ≤ t ≤ +8.64e15 } ∪ { NaN }
 *
 * The single NaN value represents any invalid-date value.  All other values
 * represent idealized durations in milliseconds since the UTC epoch.  (Leap
 * seconds are ignored; leap days are not.)  +0 is the only zero in this set.
 * The limit represented by 8.64e15 milliseconds is 100 million days either
 * side of 00:00 January 1, 1970 UTC.
 *
 * Dates in the above set are represented by the |ClippedTime| class.  The
 * double type is a superset of the above set, so it *may* (but need not)
 * represent a date.  Use ECMAScript's |TimeClip| method to produce a date from
 * a double.
 *
 * Date *objects* are simply wrappers around |TimeClip|'d numbers, with a bunch
 * of accessor methods to the various aspects of the represented date.
 */

#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"

#include "js/Conversions.h"
#include "js/Value.h"

namespace JS {

/**
 * Re-query the system to determine the current time zone adjustment from UTC,
 * including any component due to DST.  If the time zone has changed, this will
 * cause all Date object non-UTC methods and formatting functions to produce
 * appropriately adjusted results.
 *
 * Left to its own devices, SpiderMonkey itself may occasionally call this
 * method to attempt to keep up with system time changes.  However, no
 * particular frequency of checking is guaranteed.  Embedders unable to accept
 * occasional inaccuracies should call this method in response to system time
 * changes, or immediately before operations requiring instantaneous
 * correctness, to guarantee correct behavior.
 */
extern JS_PUBLIC_API(void)
ResetTimeZone();

class ClippedTime;
inline ClippedTime TimeClip(double time);

/*
 * |ClippedTime| represents the limited subset of dates/times described above.
 *
 * An invalid date/time may be created through the |ClippedTime::invalid|
 * method.  Otherwise, a |ClippedTime| may be created using the |TimeClip|
 * method.
 *
 * In typical use, the user might wish to manipulate a timestamp.  The user
 * performs a series of operations on it, but the final value might not be a
 * date as defined above -- it could have overflowed, acquired a fractional
 * component, &c.  So as a *final* step, the user passes that value through
 * |TimeClip| to produce a number restricted to JavaScript's date range.
 *
 * APIs that accept a JavaScript date value thus accept a |ClippedTime|, not a
 * double.  This ensures that date/time APIs will only ever receive acceptable
 * JavaScript dates.  This also forces users to perform any desired clipping,
 * as only the user knows what behavior is desired when clipping occurs.
 */
class ClippedTime
{
    double t;

    explicit ClippedTime(double time) : t(time) {}
    friend ClippedTime TimeClip(double time);

  public:
    // Create an invalid date.
    ClippedTime() : t(mozilla::UnspecifiedNaN<double>()) {}

    // Create an invalid date/time, more explicitly; prefer this to the default
    // constructor.
    static ClippedTime invalid() { return ClippedTime(); }

    double toDouble() const { return t; }

    bool isValid() const { return !mozilla::IsNaN(t); }
};

// ES6 20.3.1.15.
//
// Clip a double to JavaScript's date range (or to an invalid date) using the
// ECMAScript TimeClip algorithm.
inline ClippedTime
TimeClip(double time)
{
    // Steps 1-2.
    const double MaxTimeMagnitude = 8.64e15;
    if (!mozilla::IsFinite(time) || mozilla::Abs(time) > MaxTimeMagnitude)
        return ClippedTime(mozilla::UnspecifiedNaN<double>());

    // Step 3.
    return ClippedTime(ToInteger(time) + (+0.0));
}

// Produce a double Value from the given time.  Because times may be NaN,
// prefer using this to manual canonicalization.
inline Value
TimeValue(ClippedTime time)
{
    return DoubleValue(JS::CanonicalizeNaN(time.toDouble()));
}

// Create a new Date object whose [[DateValue]] internal slot contains the
// clipped |time|.  (Users who must represent times outside that range must use
// another representation.)
extern JS_PUBLIC_API(JSObject*)
NewDateObject(JSContext* cx, ClippedTime time);

// Year is a year, month is 0-11, day is 1-based.  The return value is a number
// of milliseconds since the epoch.
//
// Consistent with the MakeDate algorithm defined in ECMAScript, this value is
// *not* clipped!  Use JS::TimeClip if you need a clipped date.
JS_PUBLIC_API(double)
MakeDate(double year, unsigned month, unsigned day);

// Year is a year, month is 0-11, day is 1-based, and time is in milliseconds.
// The return value is a number of milliseconds since the epoch.
//
// Consistent with the MakeDate algorithm defined in ECMAScript, this value is
// *not* clipped!  Use JS::TimeClip if you need a clipped date.
JS_PUBLIC_API(double)
MakeDate(double year, unsigned month, unsigned day, double time);

// Takes an integer number of milliseconds since the epoch and returns the
// year.  Can return NaN, and will do so if NaN is passed in.
JS_PUBLIC_API(double)
YearFromTime(double time);

// Takes an integer number of milliseconds since the epoch and returns the
// month (0-11).  Can return NaN, and will do so if NaN is passed in.
JS_PUBLIC_API(double)
MonthFromTime(double time);

// Takes an integer number of milliseconds since the epoch and returns the
// day (1-based).  Can return NaN, and will do so if NaN is passed in.
JS_PUBLIC_API(double)
DayFromTime(double time);

// Takes an integer year and returns the number of days from epoch to the given
// year.
// NOTE: The calculation performed by this function is literally that given in
// the ECMAScript specification.  Nonfinite years, years containing fractional
// components, and years outside ECMAScript's date range are not handled with
// any particular intelligence.  Garbage in, garbage out.
JS_PUBLIC_API(double)
DayFromYear(double year);

// Takes an integer number of milliseconds since the epoch and an integer year,
// returns the number of days in that year. If |time| is nonfinite, returns NaN.
// Otherwise |time| *must* correspond to a time within the valid year |year|.
// This should usually be ensured by computing |year| as |JS::DayFromYear(time)|.
JS_PUBLIC_API(double)
DayWithinYear(double time, double year);

// The callback will be a wrapper function that accepts a single double (the time
// to clamp and jitter.) Inside the JS Engine, other parameters that may be needed
// are all constant, so they are handled inside the wrapper function
using ReduceMicrosecondTimePrecisionCallback = double(*)(double);

// Set a callback into the toolkit/components/resistfingerprinting function that
// will centralize time resolution and jitter into one place.
JS_PUBLIC_API(void)
SetReduceMicrosecondTimePrecisionCallback(ReduceMicrosecondTimePrecisionCallback callback);

// Sets the time resolution for fingerprinting protection, and whether jitter
// should occur. If resolution is set to zero, then no rounding or jitter will
// occur. This is used if the callback above is not specified.
JS_PUBLIC_API(void)
SetTimeResolutionUsec(uint32_t resolution, bool jitter);

} // namespace JS

#endif /* js_Date_h */
