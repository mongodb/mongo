/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Derick Rethans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef __TIMELIB_PRIVATE_H__
#define __TIMELIB_PRIVATE_H__

#ifdef HAVE_TIMELIB_CONFIG_H
# include "timelib_config.h"
#endif

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif

#ifdef _WIN32
# ifdef HAVE_WINSOCK2_H
#  include <winsock2.h>
# endif
#endif

#ifdef HAVE_STRING_H
# include <string.h>
#endif

#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#if defined(HAVE_STDINT_H)
# include <stdint.h>
#endif

#if HAVE_UNISTD_H
# include <unistd.h>
#endif

#if HAVE_IO_H
# include <io.h>
#endif

#if HAVE_DIRENT_H
# include <dirent.h>
#endif

#include <stdio.h>

#if HAVE_LIMITS_H
#include <limits.h>
#endif

#define TIMELIB_SPECIAL_WEEKDAY                   0x01
#define TIMELIB_SPECIAL_DAY_OF_WEEK_IN_MONTH      0x02
#define TIMELIB_SPECIAL_LAST_DAY_OF_WEEK_IN_MONTH 0x03

#define TIMELIB_SPECIAL_FIRST_DAY_OF_MONTH        0x01
#define TIMELIB_SPECIAL_LAST_DAY_OF_MONTH         0x02

#define TIMELIB_ZONETYPE_OFFSET 1
#define TIMELIB_ZONETYPE_ABBR   2
#define TIMELIB_ZONETYPE_ID     3

#define SECS_PER_ERA   TIMELIB_LL_CONST(12622780800)
#define SECS_PER_DAY   86400
#define DAYS_PER_YEAR    365
#define DAYS_PER_LYEAR   366
/* 400*365 days + 97 leap days */
#define DAYS_PER_LYEAR_PERIOD 146097
#define YEARS_PER_LYEAR_PERIOD 400

#define TIMELIB_TZINFO_PHP       0x01
#define TIMELIB_TZINFO_ZONEINFO  0x02

#define timelib_is_leap(y) ((y) % 4 == 0 && ((y) % 100 != 0 || (y) % 400 == 0))

#define TIMELIB_DEBUG(s)  if (0) { s }

typedef struct ttinfo
{
	int32_t      offset;
	int          isdst;
	unsigned int abbr_idx;

	unsigned int isstdcnt;
	unsigned int isgmtcnt;
} ttinfo;

typedef struct tlinfo
{
	int32_t  trans;
	int32_t  offset;
} tlinfo;


#ifndef LONG_MAX
#define LONG_MAX 2147483647L
#endif

#ifndef LONG_MIN
#define LONG_MIN (- LONG_MAX - 1)
#endif

#if defined(_MSC_VER) && !defined(strcasecmp)
#define strcasecmp stricmp
#endif

#if defined(_MSC_VER) && !defined(strncasecmp)
#define strncasecmp strnicmp
#endif

#endif
