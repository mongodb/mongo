/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/DateTime.h"

#include "mozilla/Atomics.h"

#include <time.h>

#include "jsutil.h"

#include "js/Date.h"
#if ENABLE_INTL_API
#include "unicode/timezone.h"
#endif

using mozilla::Atomic;
using mozilla::ReleaseAcquire;
using mozilla::UnspecifiedNaN;

/* static */ js::DateTimeInfo
js::DateTimeInfo::instance;

/* static */ mozilla::Atomic<bool, mozilla::ReleaseAcquire>
js::DateTimeInfo::AcquireLock::spinLock;

static bool
ComputeLocalTime(time_t local, struct tm* ptm)
{
#if defined(_WIN32)
    return localtime_s(ptm, &local) == 0;
#elif defined(HAVE_LOCALTIME_R)
    return localtime_r(&local, ptm);
#else
    struct tm* otm = localtime(&local);
    if (!otm)
        return false;
    *ptm = *otm;
    return true;
#endif
}

static bool
ComputeUTCTime(time_t t, struct tm* ptm)
{
#if defined(_WIN32)
    return gmtime_s(ptm, &t) == 0;
#elif defined(HAVE_GMTIME_R)
    return gmtime_r(&t, ptm);
#else
    struct tm* otm = gmtime(&t);
    if (!otm)
        return false;
    *ptm = *otm;
    return true;
#endif
}

/*
 * Compute the offset in seconds from the current UTC time to the current local
 * standard time (i.e. not including any offset due to DST).
 *
 * Examples:
 *
 * Suppose we are in California, USA on January 1, 2013 at 04:00 PST (UTC-8, no
 * DST in effect), corresponding to 12:00 UTC.  This function would then return
 * -8 * SecondsPerHour, or -28800.
 *
 * Or suppose we are in Berlin, Germany on July 1, 2013 at 17:00 CEST (UTC+2,
 * DST in effect), corresponding to 15:00 UTC.  This function would then return
 * +1 * SecondsPerHour, or +3600.
 */
static int32_t
UTCToLocalStandardOffsetSeconds()
{
    using js::SecondsPerDay;
    using js::SecondsPerHour;
    using js::SecondsPerMinute;

#if defined(XP_WIN)
    // Windows doesn't follow POSIX: updates to the TZ environment variable are
    // not reflected immediately on that platform as they are on other systems
    // without this call.
    _tzset();
#endif

    // Get the current time.
    time_t currentMaybeWithDST = time(nullptr);
    if (currentMaybeWithDST == time_t(-1))
        return 0;

    // Break down the current time into its (locally-valued, maybe with DST)
    // components.
    struct tm local;
    if (!ComputeLocalTime(currentMaybeWithDST, &local))
        return 0;

    // Compute a |time_t| corresponding to |local| interpreted without DST.
    time_t currentNoDST;
    if (local.tm_isdst == 0) {
        // If |local| wasn't DST, we can use the same time.
        currentNoDST = currentMaybeWithDST;
    } else {
        // If |local| respected DST, we need a time broken down into components
        // ignoring DST.  Turn off DST in the broken-down time.
        local.tm_isdst = 0;

        // Compute a |time_t t| corresponding to the broken-down time with DST
        // off.  This has boundary-condition issues (for about the duration of
        // a DST offset) near the time a location moves to a different time
        // zone.  But 1) errors will be transient; 2) locations rarely change
        // time zone; and 3) in the absence of an API that provides the time
        // zone offset directly, this may be the best we can do.
        currentNoDST = mktime(&local);
        if (currentNoDST == time_t(-1))
            return 0;
    }

    // Break down the time corresponding to the no-DST |local| into UTC-based
    // components.
    struct tm utc;
    if (!ComputeUTCTime(currentNoDST, &utc))
        return 0;

    // Finally, compare the seconds-based components of the local non-DST
    // representation and the UTC representation to determine the actual
    // difference.
    int utc_secs = utc.tm_hour * SecondsPerHour + utc.tm_min * SecondsPerMinute;
    int local_secs = local.tm_hour * SecondsPerHour + local.tm_min * SecondsPerMinute;

    // Same-day?  Just subtract the seconds counts.
    if (utc.tm_mday == local.tm_mday)
        return local_secs - utc_secs;

    // If we have more UTC seconds, move local seconds into the UTC seconds'
    // frame of reference and then subtract.
    if (utc_secs > local_secs)
        return (SecondsPerDay + local_secs) - utc_secs;

    // Otherwise we have more local seconds, so move the UTC seconds into the
    // local seconds' frame of reference and then subtract.
    return local_secs - (utc_secs + SecondsPerDay);
}

void
js::DateTimeInfo::internalUpdateTimeZoneAdjustment()
{
    /*
     * The difference between local standard time and UTC will never change for
     * a given time zone.
     */
    utcToLocalStandardOffsetSeconds = UTCToLocalStandardOffsetSeconds();

    double newTZA = utcToLocalStandardOffsetSeconds * msPerSecond;
    if (newTZA == localTZA_)
        return;

    localTZA_ = newTZA;

    /*
     * The initial range values are carefully chosen to result in a cache miss
     * on first use given the range of possible values.  Be careful to keep
     * these values and the caching algorithm in sync!
     */
    offsetMilliseconds = 0;
    rangeStartSeconds = rangeEndSeconds = INT64_MIN;
    oldOffsetMilliseconds = 0;
    oldRangeStartSeconds = oldRangeEndSeconds = INT64_MIN;

    sanityCheck();
}

/*
 * Since getDSTOffsetMilliseconds guarantees that all times seen will be
 * positive, we can initialize the range at construction time with large
 * negative numbers to ensure the first computation is always a cache miss and
 * doesn't return a bogus offset.
 */
/* static */ void
js::DateTimeInfo::init()
{
    DateTimeInfo* dtInfo = &DateTimeInfo::instance;

    MOZ_ASSERT(dtInfo->localTZA_ == 0,
               "we should be initializing only once, and the static instance "
               "should have started out zeroed");

    // Set to a totally impossible TZA so that the comparison above will fail
    // and all fields will be properly initialized.
    dtInfo->localTZA_ = UnspecifiedNaN<double>();
    dtInfo->internalUpdateTimeZoneAdjustment();
}

int64_t
js::DateTimeInfo::computeDSTOffsetMilliseconds(int64_t utcSeconds)
{
    MOZ_ASSERT(utcSeconds >= 0);
    MOZ_ASSERT(utcSeconds <= MaxUnixTimeT);

#if defined(XP_WIN)
    // Windows does not follow POSIX. Updates to the TZ environment variable
    // are not reflected immediately on that platform as they are on UNIX
    // systems without this call.
    _tzset();
#endif

    struct tm tm;
    if (!ComputeLocalTime(static_cast<time_t>(utcSeconds), &tm))
        return 0;

    int32_t dayoff = int32_t((utcSeconds + utcToLocalStandardOffsetSeconds) % SecondsPerDay);
    int32_t tmoff = tm.tm_sec + (tm.tm_min * SecondsPerMinute) + (tm.tm_hour * SecondsPerHour);

    int32_t diff = tmoff - dayoff;

    if (diff < 0)
        diff += SecondsPerDay;

    return diff * msPerSecond;
}

int64_t
js::DateTimeInfo::internalGetDSTOffsetMilliseconds(int64_t utcMilliseconds)
{
    sanityCheck();

    int64_t utcSeconds = utcMilliseconds / msPerSecond;

    if (utcSeconds > MaxUnixTimeT) {
        utcSeconds = MaxUnixTimeT;
    } else if (utcSeconds < 0) {
        /* Go ahead a day to make localtime work (does not work with 0). */
        utcSeconds = SecondsPerDay;
    }

    /*
     * NB: Be aware of the initial range values when making changes to this
     *     code: the first call to this method, with those initial range
     *     values, must result in a cache miss.
     */

    if (rangeStartSeconds <= utcSeconds && utcSeconds <= rangeEndSeconds)
        return offsetMilliseconds;

    if (oldRangeStartSeconds <= utcSeconds && utcSeconds <= oldRangeEndSeconds)
        return oldOffsetMilliseconds;

    oldOffsetMilliseconds = offsetMilliseconds;
    oldRangeStartSeconds = rangeStartSeconds;
    oldRangeEndSeconds = rangeEndSeconds;

    if (rangeStartSeconds <= utcSeconds) {
        int64_t newEndSeconds = Min(rangeEndSeconds + RangeExpansionAmount, MaxUnixTimeT);
        if (newEndSeconds >= utcSeconds) {
            int64_t endOffsetMilliseconds = computeDSTOffsetMilliseconds(newEndSeconds);
            if (endOffsetMilliseconds == offsetMilliseconds) {
                rangeEndSeconds = newEndSeconds;
                return offsetMilliseconds;
            }

            offsetMilliseconds = computeDSTOffsetMilliseconds(utcSeconds);
            if (offsetMilliseconds == endOffsetMilliseconds) {
                rangeStartSeconds = utcSeconds;
                rangeEndSeconds = newEndSeconds;
            } else {
                rangeEndSeconds = utcSeconds;
            }
            return offsetMilliseconds;
        }

        offsetMilliseconds = computeDSTOffsetMilliseconds(utcSeconds);
        rangeStartSeconds = rangeEndSeconds = utcSeconds;
        return offsetMilliseconds;
    }

    int64_t newStartSeconds = Max<int64_t>(rangeStartSeconds - RangeExpansionAmount, 0);
    if (newStartSeconds <= utcSeconds) {
        int64_t startOffsetMilliseconds = computeDSTOffsetMilliseconds(newStartSeconds);
        if (startOffsetMilliseconds == offsetMilliseconds) {
            rangeStartSeconds = newStartSeconds;
            return offsetMilliseconds;
        }

        offsetMilliseconds = computeDSTOffsetMilliseconds(utcSeconds);
        if (offsetMilliseconds == startOffsetMilliseconds) {
            rangeStartSeconds = newStartSeconds;
            rangeEndSeconds = utcSeconds;
        } else {
            rangeStartSeconds = utcSeconds;
        }
        return offsetMilliseconds;
    }

    rangeStartSeconds = rangeEndSeconds = utcSeconds;
    offsetMilliseconds = computeDSTOffsetMilliseconds(utcSeconds);
    return offsetMilliseconds;
}

void
js::DateTimeInfo::sanityCheck()
{
    MOZ_ASSERT(rangeStartSeconds <= rangeEndSeconds);
    MOZ_ASSERT_IF(rangeStartSeconds == INT64_MIN, rangeEndSeconds == INT64_MIN);
    MOZ_ASSERT_IF(rangeEndSeconds == INT64_MIN, rangeStartSeconds == INT64_MIN);
    MOZ_ASSERT_IF(rangeStartSeconds != INT64_MIN,
                  rangeStartSeconds >= 0 && rangeEndSeconds >= 0);
    MOZ_ASSERT_IF(rangeStartSeconds != INT64_MIN,
                  rangeStartSeconds <= MaxUnixTimeT && rangeEndSeconds <= MaxUnixTimeT);
}

static struct IcuTimeZoneInfo
{
    Atomic<bool, ReleaseAcquire> locked;
    enum { Valid = 0, NeedsUpdate } status;

    void acquire() {
        while (!locked.compareExchange(false, true))
            continue;
    }

    void release() {
        MOZ_ASSERT(locked, "should have been acquired");
        locked = false;
    }
} TZInfo;


JS_PUBLIC_API(void)
JS::ResetTimeZone()
{
    js::DateTimeInfo::updateTimeZoneAdjustment();

#if ENABLE_INTL_API && defined(ICU_TZ_HAS_RECREATE_DEFAULT)
    TZInfo.acquire();
    TZInfo.status = IcuTimeZoneInfo::NeedsUpdate;
    TZInfo.release();
#endif
}

void
js::ResyncICUDefaultTimeZone()
{
#if ENABLE_INTL_API && defined(ICU_TZ_HAS_RECREATE_DEFAULT)
    TZInfo.acquire();
    if (TZInfo.status == IcuTimeZoneInfo::NeedsUpdate) {
        icu::TimeZone::recreateDefault();
        TZInfo.status = IcuTimeZoneInfo::Valid;
    }
    TZInfo.release();
#endif
}
