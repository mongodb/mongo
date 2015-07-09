/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef prmjtime_h
#define prmjtime_h

#include <stddef.h>
#include <stdint.h>

/*
 * Broken down form of 64 bit time value.
 */
struct PRMJTime {
    int32_t tm_usec;            /* microseconds of second (0-999999) */
    int8_t tm_sec;              /* seconds of minute (0-59) */
    int8_t tm_min;              /* minutes of hour (0-59) */
    int8_t tm_hour;             /* hour of day (0-23) */
    int8_t tm_mday;             /* day of month (1-31) */
    int8_t tm_mon;              /* month of year (0-11) */
    int8_t tm_wday;             /* 0=sunday, 1=monday, ... */
    int32_t tm_year;            /* absolute year, AD */
    int16_t tm_yday;            /* day of year (0 to 365) */
    int8_t tm_isdst;            /* non-zero if DST in effect */
};

/* Some handy constants */
#define PRMJ_USEC_PER_SEC       1000000L
#define PRMJ_USEC_PER_MSEC      1000L

/* Return the current local time in micro-seconds */
extern int64_t
PRMJ_Now();

/* Initialize the resources associated with PRMJ_Now. */
#if defined(XP_WIN)
extern void
PRMJ_NowInit();
#else
inline void
PRMJ_NowInit() {}
#endif

/* Release the resources associated with PRMJ_Now; don't call PRMJ_Now again */
#ifdef XP_WIN
extern void
PRMJ_NowShutdown();
#else
inline void
PRMJ_NowShutdown() {}
#endif

/* Format a time value into a buffer. Same semantics as strftime() */
extern size_t
PRMJ_FormatTime(char* buf, int buflen, const char* fmt, PRMJTime* tm);

#endif /* prmjtime_h */
