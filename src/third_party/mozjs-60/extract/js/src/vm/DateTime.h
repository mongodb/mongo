/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_DateTime_h
#define vm_DateTime_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"

#include <stdint.h>

#include "js/Conversions.h"
#include "js/Date.h"
#include "js/Initialization.h"
#include "js/Value.h"
#include "threading/ExclusiveData.h"

namespace js {

/* Constants defined by ES5 15.9.1.10. */
const double HoursPerDay = 24;
const double MinutesPerHour = 60;
const double SecondsPerMinute = 60;
const double msPerSecond = 1000;
const double msPerMinute = msPerSecond * SecondsPerMinute;
const double msPerHour = msPerMinute * MinutesPerHour;

/* ES5 15.9.1.2. */
const double msPerDay = msPerHour * HoursPerDay;

/*
 * Additional quantities not mentioned in the spec.  Be careful using these!
 * They aren't doubles (and aren't defined in terms of all the other constants
 * so that they can be used in constexpr scenarios; if you need constants that
 * trigger floating point semantics, you'll have to manually cast to get it.
 */
const unsigned SecondsPerHour = 60 * 60;
const unsigned SecondsPerDay = SecondsPerHour * 24;

const double StartOfTime = -8.64e15;
const double EndOfTime = 8.64e15;

extern bool
InitDateTimeState();

extern void
FinishDateTimeState();

/*
 * Stores date/time information, particularly concerning the current local
 * time zone, and implements a small cache for daylight saving time offset
 * computation.
 *
 * The basic idea is premised upon this fact: the DST offset never changes more
 * than once in any thirty-day period.  If we know the offset at t_0 is o_0,
 * the offset at [t_1, t_2] is also o_0, where t_1 + 3_0 days == t_2,
 * t_1 <= t_0, and t0 <= t2.  (In other words, t_0 is always somewhere within a
 * thirty-day range where the DST offset is constant: DST changes never occur
 * more than once in any thirty-day period.)  Therefore, if we intelligently
 * retain knowledge of the offset for a range of dates (which may vary over
 * time), and if requests are usually for dates within that range, we can often
 * provide a response without repeated offset calculation.
 *
 * Our caching strategy is as follows: on the first request at date t_0 compute
 * the requested offset o_0.  Save { start: t_0, end: t_0, offset: o_0 } as the
 * cache's state.  Subsequent requests within that range are straightforwardly
 * handled.  If a request for t_i is far outside the range (more than thirty
 * days), compute o_i = dstOffset(t_i) and save { start: t_i, end: t_i,
 * offset: t_i }.  Otherwise attempt to *overextend* the range to either
 * [start - 30d, end] or [start, end + 30d] as appropriate to encompass
 * t_i.  If the offset o_i30 is the same as the cached offset, extend the
 * range.  Otherwise the over-guess crossed a DST change -- compute
 * o_i = dstOffset(t_i) and either extend the original range (if o_i == offset)
 * or start a new one beneath/above the current one with o_i30 as the offset.
 *
 * This cache strategy results in 0 to 2 DST offset computations.  The naive
 * always-compute strategy is 1 computation, and since cache maintenance is a
 * handful of integer arithmetic instructions the speed difference between
 * always-1 and 1-with-cache is negligible.  Caching loses if two computations
 * happen: when the date is within 30 days of the cached range and when that
 * 30-day range crosses a DST change.  This is relatively uncommon.  Further,
 * instances of such are often dominated by in-range hits, so caching is an
 * overall slight win.
 *
 * Why 30 days?  For correctness the duration must be smaller than any possible
 * duration between DST changes.  Past that, note that 1) a large duration
 * increases the likelihood of crossing a DST change while reducing the number
 * of cache misses, and 2) a small duration decreases the size of the cached
 * range while producing more misses.  Using a month as the interval change is
 * a balance between these two that tries to optimize for the calendar month at
 * a time that a site might display.  (One could imagine an adaptive duration
 * that accommodates near-DST-change dates better; we don't believe the
 * potential win from better caching offsets the loss from extra complexity.)
 */
class DateTimeInfo
{
    static ExclusiveData<DateTimeInfo>* instance;
    friend class ExclusiveData<DateTimeInfo>;

    friend bool InitDateTimeState();
    friend void FinishDateTimeState();

    DateTimeInfo();

  public:
    // The spec implicitly assumes DST and time zone adjustment information
    // never change in the course of a function -- sometimes even across
    // reentrancy.  So make critical sections as narrow as possible.

    /*
     * Get the DST offset in milliseconds at a UTC time.  This is usually
     * either 0 or |msPerSecond * SecondsPerHour|, but at least one exotic time
     * zone (Lord Howe Island, Australia) has a fractional-hour offset, just to
     * keep things interesting.
     */
    static int64_t getDSTOffsetMilliseconds(int64_t utcMilliseconds) {
        auto guard = instance->lock();
        return guard->internalGetDSTOffsetMilliseconds(utcMilliseconds);
    }

    /* ES5 15.9.1.7. */
    static double localTZA() {
        auto guard = instance->lock();
        return guard->localTZA_;
    }

  private:
    // We don't want anyone accidentally calling *only*
    // DateTimeInfo::updateTimeZoneAdjustment() to respond to a system time
    // zone change (missing the necessary poking of ICU as well), so ensure
    // only JS::ResetTimeZone() can call this via access restrictions.
    friend void JS::ResetTimeZone();

    static void updateTimeZoneAdjustment() {
        auto guard = instance->lock();
        guard->internalUpdateTimeZoneAdjustment();
    }

    /*
     * The current local time zone adjustment, cached because retrieving this
     * dynamically is Slow, and a certain venerable benchmark which shall not
     * be named depends on it being fast.
     *
     * SpiderMonkey occasionally and arbitrarily updates this value from the
     * system time zone to attempt to keep this reasonably up-to-date.  If
     * temporary inaccuracy can't be tolerated, JSAPI clients may call
     * JS::ResetTimeZone to forcibly sync this with the system time zone.
     */
    double localTZA_;

    /*
     * Compute the DST offset at the given UTC time in seconds from the epoch.
     * (getDSTOffsetMilliseconds attempts to return a cached value, but in case
     * of a cache miss it calls this method.  The cache is represented through
     * the offset* and *{Start,End}Seconds fields below.)
     */
    int64_t computeDSTOffsetMilliseconds(int64_t utcSeconds);

    int64_t offsetMilliseconds;
    int64_t rangeStartSeconds, rangeEndSeconds; // UTC-based

    int64_t oldOffsetMilliseconds;
    int64_t oldRangeStartSeconds, oldRangeEndSeconds; // UTC-based

    /*
     * Cached offset in seconds from the current UTC time to the current
     * local standard time (i.e. not including any offset due to DST).
     */
    int32_t utcToLocalStandardOffsetSeconds;

    static const int64_t MaxUnixTimeT = 2145859200; /* time_t 12/31/2037 */

    static const int64_t RangeExpansionAmount = 30 * SecondsPerDay;

    int64_t internalGetDSTOffsetMilliseconds(int64_t utcMilliseconds);
    void internalUpdateTimeZoneAdjustment();

    void sanityCheck();
};

enum class IcuTimeZoneStatus { Valid, NeedsUpdate };

extern ExclusiveData<IcuTimeZoneStatus>*
IcuTimeZoneState;

/**
 * ICU's default time zone, used for various date/time formatting operations
 * that include the local time in the representation, is allowed to go stale
 * for unfortunate performance reasons.  Call this function when an up-to-date
 * default time zone is required, to resync ICU's default time zone with
 * reality.
 */
extern void
ResyncICUDefaultTimeZone();

}  /* namespace js */

#endif /* vm_DateTime_h */
