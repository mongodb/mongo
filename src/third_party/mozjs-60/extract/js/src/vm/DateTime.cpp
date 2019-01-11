/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/DateTime.h"

#if defined(XP_WIN)
#include "mozilla/UniquePtr.h"

#include <cstdlib>
#include <cstring>
#endif /* defined(XP_WIN) */
#include <time.h>

#include "jsutil.h"

#include "js/Date.h"
#include "threading/ExclusiveData.h"

#if ENABLE_INTL_API
#include "unicode/timezone.h"
#if defined(XP_WIN)
#include "unicode/unistr.h"
#endif
#endif /* ENABLE_INTL_API */

#include "vm/MutexIDs.h"

using mozilla::UnspecifiedNaN;

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
        // ignoring DST.  Turn off DST in the broken-down time.  Create a fresh
        // copy of |local|, because mktime() will reset tm_isdst = 1 and will
        // adjust tm_hour and tm_hour accordingly.
        struct tm localNoDST = local;
        localNoDST.tm_isdst = 0;

        // Compute a |time_t t| corresponding to the broken-down time with DST
        // off.  This has boundary-condition issues (for about the duration of
        // a DST offset) near the time a location moves to a different time
        // zone.  But 1) errors will be transient; 2) locations rarely change
        // time zone; and 3) in the absence of an API that provides the time
        // zone offset directly, this may be the best we can do.
        currentNoDST = mktime(&localNoDST);
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

js::DateTimeInfo::DateTimeInfo()
{
    // Set to an impossible TZA so that the comparison in
    // |internalUpdateTimeZoneAdjustment()| initially fails, causing the
    // remaining fields to be properly initialized at first adjustment.
    localTZA_ = UnspecifiedNaN<double>();
    internalUpdateTimeZoneAdjustment();
}

int64_t
js::DateTimeInfo::computeDSTOffsetMilliseconds(int64_t utcSeconds)
{
    MOZ_ASSERT(utcSeconds >= 0);
    MOZ_ASSERT(utcSeconds <= MaxUnixTimeT);

    struct tm tm;
    if (!ComputeLocalTime(static_cast<time_t>(utcSeconds), &tm))
        return 0;

    // NB: The offset isn't computed correctly when the standard local offset
    //     at |utcSeconds| is different from |utcToLocalStandardOffsetSeconds|.
    int32_t dayoff = int32_t((utcSeconds + utcToLocalStandardOffsetSeconds) % SecondsPerDay);
    int32_t tmoff = tm.tm_sec + (tm.tm_min * SecondsPerMinute) + (tm.tm_hour * SecondsPerHour);

    int32_t diff = tmoff - dayoff;

    if (diff < 0)
        diff += SecondsPerDay;
    else if (uint32_t(diff) >= SecondsPerDay)
        diff -= SecondsPerDay;

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

/* static */ js::ExclusiveData<js::DateTimeInfo>*
js::DateTimeInfo::instance;

/* static */ js::ExclusiveData<js::IcuTimeZoneStatus>*
js::IcuTimeZoneState;

bool
js::InitDateTimeState()
{

    MOZ_ASSERT(!DateTimeInfo::instance,
               "we should be initializing only once");

    DateTimeInfo::instance = js_new<ExclusiveData<DateTimeInfo>>(mutexid::DateTimeInfoMutex);
    if (!DateTimeInfo::instance)
        return false;

    MOZ_ASSERT(!IcuTimeZoneState,
               "we should be initializing only once");

    IcuTimeZoneState = js_new<ExclusiveData<IcuTimeZoneStatus>>(mutexid::IcuTimeZoneStateMutex);
    if (!IcuTimeZoneState) {
        js_delete(DateTimeInfo::instance);
        DateTimeInfo::instance = nullptr;
        return false;
    }

    return true;
}

/* static */ void
js::FinishDateTimeState()
{
    js_delete(IcuTimeZoneState);
    IcuTimeZoneState = nullptr;

    js_delete(DateTimeInfo::instance);
    DateTimeInfo::instance = nullptr;
}

JS_PUBLIC_API(void)
JS::ResetTimeZone()
{
    js::DateTimeInfo::updateTimeZoneAdjustment();

#if ENABLE_INTL_API && defined(ICU_TZ_HAS_RECREATE_DEFAULT)
    js::IcuTimeZoneState->lock().get() = js::IcuTimeZoneStatus::NeedsUpdate;
#endif
}

#if defined(XP_WIN)
static bool
IsOlsonCompatibleWindowsTimeZoneId(const char* tz)
{
    // ICU ignores the TZ environment variable on Windows and instead directly
    // invokes Win API functions to retrieve the current time zone. But since
    // we're still using the POSIX-derived localtime_s() function on Windows
    // and localtime_s() does return a time zone adjusted value based on the
    // TZ environment variable, we need to manually adjust the default ICU
    // time zone if TZ is set.
    //
    // Windows supports the following format for TZ: tzn[+|-]hh[:mm[:ss]][dzn]
    // where "tzn" is the time zone name for standard time, the time zone
    // offset is positive for time zones west of GMT, and "dzn" is the
    // optional time zone name when daylight savings are observed. Daylight
    // savings are always based on the U.S. daylight saving rules, that means
    // for example it's not possible to use "TZ=CET-1CEST" to select the IANA
    // time zone "CET".
    //
    // When comparing this restricted format for TZ to all IANA time zone
    // names, the following time zones are in the intersection of what's
    // supported by Windows and is also a valid IANA time zone identifier.
    //
    // Even though the time zone offset is marked as mandatory on MSDN, it
    // appears it defaults to zero when omitted. This in turn means we can
    // also allow the time zone identifiers "UCT", "UTC", and "GMT".

    static const char* const allowedIds[] = {
        // From tzdata's "northamerica" file:
        "EST5EDT",
        "CST6CDT",
        "MST7MDT",
        "PST8PDT",

        // From tzdata's "backward" file:
        "GMT+0",
        "GMT-0",
        "GMT0",
        "UCT",
        "UTC",

        // From tzdata's "etcetera" file:
        "GMT",
    };
    for (const auto& allowedId : allowedIds) {
        if (std::strcmp(allowedId, tz) == 0)
            return true;
    }
    return false;
}
#endif

void
js::ResyncICUDefaultTimeZone()
{
#if ENABLE_INTL_API && defined(ICU_TZ_HAS_RECREATE_DEFAULT)
    auto guard = IcuTimeZoneState->lock();
    if (guard.get() == IcuTimeZoneStatus::NeedsUpdate) {
        bool recreate = true;
#if defined(XP_WIN)
        // If TZ is set and its value is valid under Windows' and IANA's time
        // zone identifier rules, update the ICU default time zone to use this
        // value.
        const char* tz = std::getenv("TZ");
        if (tz && IsOlsonCompatibleWindowsTimeZoneId(tz)) {
            icu::UnicodeString tzid(tz, -1, US_INV);
            mozilla::UniquePtr<icu::TimeZone> newTimeZone(icu::TimeZone::createTimeZone(tzid));
            MOZ_ASSERT(newTimeZone);
            if (*newTimeZone != icu::TimeZone::getUnknown()) {
                // adoptDefault() takes ownership of the time zone.
                icu::TimeZone::adoptDefault(newTimeZone.release());
                recreate = false;
            }
        } else {
            // If |tz| isn't a supported time zone identifier, use the default
            // Windows time zone for ICU.
            // TODO: Handle invalid time zone identifiers (bug 342068).
        }
#endif
        if (recreate)
            icu::TimeZone::recreateDefault();
        guard.get() = IcuTimeZoneStatus::Valid;
    }
#endif
}
