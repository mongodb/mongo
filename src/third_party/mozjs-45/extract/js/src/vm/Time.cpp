/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* PR time code. */

#include "vm/Time.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#ifdef SOLARIS
#define _REENTRANT 1
#endif
#include <string.h>
#include <time.h>

#include "jstypes.h"
#include "jsutil.h"

#ifdef XP_WIN
#include <windef.h>
#include <winbase.h>
#include <crtdbg.h>   /* for _CrtSetReportMode */
#include <mmsystem.h> /* for timeBegin/EndPeriod */
#include <stdlib.h>   /* for _set_invalid_parameter_handler */

// MONGODB MODIFICATION: Remove unneeded dependency on NSPR
//#include "prinit.h"

#endif

#ifdef XP_UNIX

#ifdef _SVID_GETTOD   /* Defined only on Solaris, see Solaris <sys/types.h> */
extern int gettimeofday(struct timeval* tv);
#endif

#include <sys/time.h>

#endif /* XP_UNIX */

using mozilla::DebugOnly;

#if defined(XP_UNIX)
int64_t
PRMJ_Now()
{
    struct timeval tv;

#ifdef _SVID_GETTOD   /* Defined only on Solaris, see Solaris <sys/types.h> */
    gettimeofday(&tv);
#else
    gettimeofday(&tv, 0);
#endif /* _SVID_GETTOD */

    return int64_t(tv.tv_sec) * PRMJ_USEC_PER_SEC + int64_t(tv.tv_usec);
}

#else

// Returns the number of microseconds since the Unix epoch.
static double
FileTimeToUnixMicroseconds(const FILETIME& ft)
{
    // Get the time in 100ns intervals.
    int64_t t = (int64_t(ft.dwHighDateTime) << 32) | int64_t(ft.dwLowDateTime);

    // The Windows epoch is around 1600. The Unix epoch is around 1970.
    // Subtract the difference.
    static const int64_t TimeToEpochIn100ns = 0x19DB1DED53E8000;
    t -= TimeToEpochIn100ns;

    // Divide by 10 to convert to microseconds.
    return double(t) * 0.1;
}

struct CalibrationData {
    double freq;         /* The performance counter frequency */
    double offset;       /* The low res 'epoch' */
    double timer_offset; /* The high res 'epoch' */

    bool calibrated;

    CRITICAL_SECTION data_lock;
};

static CalibrationData calibration = { 0 };

static void
NowCalibrate()
{
    MOZ_ASSERT(calibration.freq > 0);

    // By wrapping a timeBegin/EndPeriod pair of calls around this loop,
    // the loop seems to take much less time (1 ms vs 15ms) on Vista.
    timeBeginPeriod(1);
    FILETIME ft, ftStart;
    GetSystemTimeAsFileTime(&ftStart);
    do {
        GetSystemTimeAsFileTime(&ft);
    } while (memcmp(&ftStart, &ft, sizeof(ft)) == 0);
    timeEndPeriod(1);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    calibration.offset = FileTimeToUnixMicroseconds(ft);
    calibration.timer_offset = double(now.QuadPart);
    calibration.calibrated = true;
}

static const unsigned DataLockSpinCount = 4096;

static void (WINAPI* pGetSystemTimePreciseAsFileTime)(LPFILETIME) = nullptr;

void
PRMJ_NowInit()
{
    memset(&calibration, 0, sizeof(calibration));

    // According to the documentation, QueryPerformanceFrequency will never
    // return false or return a non-zero frequency on systems that run
    // Windows XP or later. Also, the frequency is fixed so we only have to
    // query it once.
    LARGE_INTEGER liFreq;
    DebugOnly<BOOL> res = QueryPerformanceFrequency(&liFreq);
    MOZ_ASSERT(res);
    calibration.freq = double(liFreq.QuadPart);
    MOZ_ASSERT(calibration.freq > 0.0);

    InitializeCriticalSectionAndSpinCount(&calibration.data_lock, DataLockSpinCount);

    // Windows 8 has a new API function we can use.
    // MONGODB MODIFICATION: Use ANSI version of WINAPI
    if (HMODULE h = GetModuleHandleA("kernel32.dll")) {
        pGetSystemTimePreciseAsFileTime =
            (void (WINAPI*)(LPFILETIME))GetProcAddress(h, "GetSystemTimePreciseAsFileTime");
    }
}

void
PRMJ_NowShutdown()
{
    DeleteCriticalSection(&calibration.data_lock);
}

#define MUTEX_LOCK(m) EnterCriticalSection(m)
#define MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#define MUTEX_SETSPINCOUNT(m, c) SetCriticalSectionSpinCount((m),(c))

// Please see bug 363258 for why the win32 timing code is so complex.
int64_t
PRMJ_Now()
{
    if (pGetSystemTimePreciseAsFileTime) {
        // Windows 8 has a new API function that does all the work.
        FILETIME ft;
        pGetSystemTimePreciseAsFileTime(&ft);
        return int64_t(FileTimeToUnixMicroseconds(ft));
    }

    bool calibrated = false;
    bool needsCalibration = !calibration.calibrated;
    double cachedOffset = 0.0;
    while (true) {
        if (needsCalibration) {
            MUTEX_LOCK(&calibration.data_lock);

            // Recalibrate only if no one else did before us.
            if (calibration.offset == cachedOffset) {
                // Since calibration can take a while, make any other
                // threads immediately wait.
                MUTEX_SETSPINCOUNT(&calibration.data_lock, 0);

                NowCalibrate();

                calibrated = true;

                // Restore spin count.
                MUTEX_SETSPINCOUNT(&calibration.data_lock, DataLockSpinCount);
            }

            MUTEX_UNLOCK(&calibration.data_lock);
        }

        // Calculate a low resolution time.
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        double lowresTime = FileTimeToUnixMicroseconds(ft);

        // Grab high resolution time.
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double highresTimerValue = double(now.QuadPart);

        MUTEX_LOCK(&calibration.data_lock);
        double highresTime = calibration.offset +
            PRMJ_USEC_PER_SEC * (highresTimerValue - calibration.timer_offset) / calibration.freq;
        cachedOffset = calibration.offset;
        MUTEX_UNLOCK(&calibration.data_lock);

        // Assume the NT kernel ticks every 15.6 ms. Unfortunately there's no
        // good way to determine this (NtQueryTimerResolution is an undocumented
        // API), but 15.6 ms seems to be the max possible value. Hardcoding 15.6
        // means we'll recalibrate if the highres and lowres timers diverge by
        // more than 30 ms.
        static const double KernelTickInMicroseconds = 15625.25;

        // Check for clock skew.
        double diff = lowresTime - highresTime;

        // For some reason that I have not determined, the skew can be
        // up to twice a kernel tick. This does not seem to happen by
        // itself, but I have only seen it triggered by another program
        // doing some kind of file I/O. The symptoms are a negative diff
        // followed by an equally large positive diff.
        if (mozilla::Abs(diff) <= 2 * KernelTickInMicroseconds) {
            // No detectable clock skew.
            return int64_t(highresTime);
        }

        if (calibrated) {
            // If we already calibrated once this instance, and the
            // clock is still skewed, then either the processor(s) are
            // wildly changing clockspeed or the system is so busy that
            // we get switched out for long periods of time. In either
            // case, it would be infeasible to make use of high
            // resolution results for anything, so let's resort to old
            // behavior for this call. It's possible that in the
            // future, the user will want the high resolution timer, so
            // we don't disable it entirely.
            return int64_t(lowresTime);
        }

        // It is possible that when we recalibrate, we will return a
        // value less than what we have returned before; this is
        // unavoidable. We cannot tell the different between a
        // faulty QueryPerformanceCounter implementation and user
        // changes to the operating system time. Since we must
        // respect user changes to the operating system time, we
        // cannot maintain the invariant that Date.now() never
        // decreases; the old implementation has this behavior as
        // well.
        needsCalibration = true;
    }
}
#endif

#ifdef XP_WIN
static void
PRMJ_InvalidParameterHandler(const wchar_t* expression,
                             const wchar_t* function,
                             const wchar_t* file,
                             unsigned int   line,
                             uintptr_t      pReserved)
{
    /* empty */
}
#endif

/* Format a time value into a buffer. Same semantics as strftime() */
size_t
PRMJ_FormatTime(char* buf, int buflen, const char* fmt, PRMJTime* prtm)
{
    size_t result = 0;
#if defined(XP_UNIX) || defined(XP_WIN)
    struct tm a;
    int fake_tm_year = 0;
#ifdef XP_WIN
    _invalid_parameter_handler oldHandler;
    int oldReportMode;
#endif

    memset(&a, 0, sizeof(struct tm));

    a.tm_sec = prtm->tm_sec;
    a.tm_min = prtm->tm_min;
    a.tm_hour = prtm->tm_hour;
    a.tm_mday = prtm->tm_mday;
    a.tm_mon = prtm->tm_mon;
    a.tm_wday = prtm->tm_wday;

    /*
     * On systems where |struct tm| has members tm_gmtoff and tm_zone, we
     * must fill in those values, or else strftime will return wrong results
     * (e.g., bug 511726, bug 554338).
     */
#if defined(HAVE_LOCALTIME_R) && defined(HAVE_TM_ZONE_TM_GMTOFF)
    {
        /*
         * Fill out |td| to the time represented by |prtm|, leaving the
         * timezone fields zeroed out. localtime_r will then fill in the
         * timezone fields for that local time according to the system's
         * timezone parameters.
         */
        struct tm td;
        memset(&td, 0, sizeof(td));
        td.tm_sec = prtm->tm_sec;
        td.tm_min = prtm->tm_min;
        td.tm_hour = prtm->tm_hour;
        td.tm_mday = prtm->tm_mday;
        td.tm_mon = prtm->tm_mon;
        td.tm_wday = prtm->tm_wday;
        td.tm_year = prtm->tm_year - 1900;
        td.tm_yday = prtm->tm_yday;
        td.tm_isdst = prtm->tm_isdst;
        time_t t = mktime(&td);
        localtime_r(&t, &td);

        a.tm_gmtoff = td.tm_gmtoff;
        a.tm_zone = td.tm_zone;
    }
#endif

    /*
     * Years before 1900 and after 9999 cause strftime() to abort on Windows.
     * To avoid that we replace it with FAKE_YEAR_BASE + year % 100 and then
     * replace matching substrings in the strftime() result with the real year.
     * Note that FAKE_YEAR_BASE should be a multiple of 100 to make 2-digit
     * year formats (%y) work correctly (since we won't find the fake year
     * in that case).
     * e.g. new Date(1873, 0).toLocaleFormat('%Y %y') => "1873 73"
     * See bug 327869.
     */
#define FAKE_YEAR_BASE 9900
    if (prtm->tm_year < 1900 || prtm->tm_year > 9999) {
        fake_tm_year = FAKE_YEAR_BASE + prtm->tm_year % 100;
        a.tm_year = fake_tm_year - 1900;
    }
    else {
        a.tm_year = prtm->tm_year - 1900;
    }
    a.tm_yday = prtm->tm_yday;
    a.tm_isdst = prtm->tm_isdst;

    /*
     * Even with the above, SunOS 4 seems to detonate if tm_zone and tm_gmtoff
     * are null.  This doesn't quite work, though - the timezone is off by
     * tzoff + dst.  (And mktime seems to return -1 for the exact dst
     * changeover time.)
     */

#ifdef XP_WIN
    oldHandler = _set_invalid_parameter_handler(PRMJ_InvalidParameterHandler);
    oldReportMode = _CrtSetReportMode(_CRT_ASSERT, 0);
#endif

    result = strftime(buf, buflen, fmt, &a);

#ifdef XP_WIN
    _set_invalid_parameter_handler(oldHandler);
    _CrtSetReportMode(_CRT_ASSERT, oldReportMode);
#endif

    if (fake_tm_year && result) {
        char real_year[16];
        char fake_year[16];
        size_t real_year_len;
        size_t fake_year_len;
        char* p;

        sprintf(real_year, "%d", prtm->tm_year);
        real_year_len = strlen(real_year);
        sprintf(fake_year, "%d", fake_tm_year);
        fake_year_len = strlen(fake_year);

        /* Replace the fake year in the result with the real year. */
        for (p = buf; (p = strstr(p, fake_year)); p += real_year_len) {
            size_t new_result = result + real_year_len - fake_year_len;
            if ((int)new_result >= buflen) {
                return 0;
            }
            memmove(p + real_year_len, p + fake_year_len, strlen(p + fake_year_len));
            memcpy(p, real_year, real_year_len);
            result = new_result;
            *(buf + result) = '\0';
        }
    }
#endif
    return result;
}
