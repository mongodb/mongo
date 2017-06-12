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

#ifndef __TIMELIB_H__
#define __TIMELIB_H__

#ifdef HAVE_TIMELIB_CONFIG_H
# include "timelib_config.h"
#endif

#include <stdlib.h>
#include <limits.h>
#include <inttypes.h>

# ifndef HAVE_INT32_T
#  if SIZEOF_INT == 4
typedef int int32_t;
#  elif SIZEOF_LONG == 4
typedef long int int32_t;
#  endif
# endif

# ifndef HAVE_UINT32_T
#  if SIZEOF_INT == 4
typedef unsigned int uint32_t;
#  elif SIZEOF_LONG == 4
typedef unsigned long int uint32_t;
#  endif
# endif

#ifdef _WIN32
# if _MSC_VER >= 1600
# include <stdint.h>
# endif
# ifndef SIZEOF_INT
#  define SIZEOF_INT 4
# endif
# ifndef SIZEOF_LONG
#  define SIZEOF_LONG 4
# endif
# ifndef int32_t
typedef __int32           int32_t;
# endif
# ifndef uint32_t
typedef unsigned __int32  uint32_t;
# endif
# ifndef int64_t
typedef __int64           int64_t;
# endif
# ifndef uint64_t
typedef unsigned __int64  uint64_t;
# endif
# ifndef PRId32
#  define PRId32       "I32d"
# endif
# ifndef PRIu32
#  define PRIu32       "I32u"
# endif
# ifndef PRId64
#  define PRId64       "I64d"
# endif
# ifndef PRIu64
#  define PRIu64       "I64u"
# endif
# ifndef INT32_MAX
#define INT32_MAX    _I32_MAX
# endif
# ifndef INT32_MIN
#define INT32_MIN    ((int32_t)_I32_MIN)
# endif
# ifndef UINT32_MAX
#define UINT32_MAX   _UI32_MAX
# endif
# ifndef INT64_MIN
#define INT64_MIN    ((int64_t)_I64_MIN)
# endif
# ifndef INT64_MAX
#define INT64_MAX    _I64_MAX
# endif
# ifndef UINT64_MAX
#define UINT64_MAX   _UI64_MAX
# endif
#endif

#if (defined(__x86_64__) || defined(__LP64__) || defined(_LP64) || defined(_WIN64)) && !defined(TIMELIB_FORCE_LONG32)
typedef int64_t timelib_long;
typedef uint64_t timelib_ulong;
# define TIMELIB_LONG_MAX INT64_MAX
# define TIMELIB_LONG_MIN INT64_MIN
# define TIMELIB_ULONG_MAX UINT64_MAX
# define TIMELIB_LONG_FMT "%" PRId64
# define TIMELIB_ULONG_FMT "%" PRIu64
#else
typedef int32_t timelib_long;
typedef uint32_t timelib_ulong;
# define TIMELIB_LONG_MAX INT32_MAX
# define TIMELIB_LONG_MIN INT32_MIN
# define TIMELIB_ULONG_MAX UINT32_MAX
# define TIMELIB_LONG_FMT "%" PRId32
# define TIMELIB_ULONG_FMT "%" PRIu32
#endif

#if defined(_MSC_VER)
typedef uint64_t timelib_ull;
typedef int64_t timelib_sll;
# define TIMELIB_LL_CONST(n) n ## i64
#else
typedef unsigned long long timelib_ull;
typedef signed long long timelib_sll;
# define TIMELIB_LL_CONST(n) n ## ll
#endif

typedef struct ttinfo ttinfo;
typedef struct tlinfo tlinfo;

typedef struct tlocinfo
{
	char country_code[3];
	double latitude;
	double longitude;
	char *comments;
} tlocinfo;

typedef struct timelib_tzinfo
{
	char    *name;
	struct {
		uint32_t ttisgmtcnt;
		uint32_t ttisstdcnt;
		uint32_t leapcnt;
		uint32_t timecnt;
		uint32_t typecnt;
		uint32_t charcnt;
	} bit32;
	struct {
		uint64_t ttisgmtcnt;
		uint64_t ttisstdcnt;
		uint64_t leapcnt;
		uint64_t timecnt;
		uint64_t typecnt;
		uint64_t charcnt;
	} bit64;

	int32_t *trans;
	unsigned char *trans_idx;

	ttinfo  *type;
	char    *timezone_abbr;

	tlinfo  *leap_times;
	unsigned char bc;
	tlocinfo location;
} timelib_tzinfo;

typedef struct timelib_rel_time {
	timelib_sll y, m, d; /* Years, Months and Days */
	timelib_sll h, i, s; /* Hours, mInutes and Seconds */
	double      f;       /* Fraction */

	int weekday; /* Stores the day in 'next monday' */
	int weekday_behavior; /* 0: the current day should *not* be counted when advancing forwards; 1: the current day *should* be counted */

	int first_last_day_of;
	int invert; /* Whether the difference should be inverted */
	timelib_sll days; /* Contains the number of *days*, instead of Y-M-D differences */

	struct {
		unsigned int type;
		timelib_sll amount;
	} special;

	unsigned int   have_weekday_relative, have_special_relative;
} timelib_rel_time;

typedef struct timelib_time_offset {
	int32_t      offset;
	unsigned int leap_secs;
	unsigned int is_dst;
	char        *abbr;
	timelib_sll  transistion_time;
} timelib_time_offset;

typedef struct timelib_time {
	timelib_sll      y, m, d;     /* Year, Month, Day */
	timelib_sll      h, i, s;     /* Hour, mInute, Second */
	double           f;           /* Fraction */
	int              z;           /* GMT offset in minutes */
	char            *tz_abbr;     /* Timezone abbreviation (display only) */
	timelib_tzinfo  *tz_info;     /* Timezone structure */
	signed int       dst;         /* Flag if we were parsing a DST zone */
	timelib_rel_time relative;

	timelib_sll      sse;         /* Seconds since epoch */

	unsigned int   have_time, have_date, have_zone, have_relative, have_weeknr_day;

	unsigned int   sse_uptodate; /* !0 if the sse member is up to date with the date/time members */
	unsigned int   tim_uptodate; /* !0 if the date/time members are up to date with the sse member */
	unsigned int   is_localtime; /*  1 if the current struct represents localtime, 0 if it is in GMT */
	unsigned int   zone_type;    /*  1 time offset,
	                              *  3 TimeZone identifier,
	                              *  2 TimeZone abbreviation */
} timelib_time;

typedef struct timelib_abbr_info {
	timelib_sll  utc_offset;
	char        *abbr;
	int          dst;
} timelib_abbr_info;

typedef struct timelib_error_message {
	int         position;
	char        character;
	char       *message;
} timelib_error_message;

typedef struct timelib_error_container {
	struct timelib_error_message *error_messages;
	struct timelib_error_message *warning_messages;
	int                           error_count;
	int                           warning_count;
} timelib_error_container;

typedef struct _timelib_tz_lookup_table {
	char       *name;
	int         type;
	float       gmtoffset;
	char       *full_tz_name;
} timelib_tz_lookup_table;

typedef struct _timelib_tzdb_index_entry {
	char *id;
	unsigned int pos;
} timelib_tzdb_index_entry;

typedef struct _timelib_tzdb {
	char                           *version;
	int                             index_size;
	const timelib_tzdb_index_entry *index;
	const unsigned char            *data;
} timelib_tzdb;

#ifndef timelib_malloc
# define timelib_malloc  malloc
# define timelib_realloc realloc
# define timelib_calloc  calloc
# define timelib_strdup  strdup
# define timelib_free    free
#endif

#define TIMELIB_VERSION 201705
#define TIMELIB_ASCII_VERSION "2017.05beta2"

#define TIMELIB_NONE             0x00
#define TIMELIB_OVERRIDE_TIME    0x01
#define TIMELIB_NO_CLONE         0x02

#define TIMELIB_UNSET   -99999

/* An entry for each of these error codes is also in the
 * timelib_error_messages array in timelib.c */
#define TIMELIB_ERROR_NO_ERROR                            0x00
#define TIMELIB_ERROR_CANNOT_ALLOCATE                     0x01
#define TIMELIB_ERROR_CORRUPT_TRANSITIONS_DONT_INCREASE   0x02
#define TIMELIB_ERROR_CORRUPT_NO_64BIT_PREAMBLE           0x03
#define TIMELIB_ERROR_CORRUPT_NO_ABBREVIATION             0x04
#define TIMELIB_ERROR_UNSUPPORTED_VERSION                 0x05
#define TIMELIB_ERROR_NO_SUCH_TIMEZONE                    0x06

#ifdef __cplusplus
extern "C" {
#endif

/* Function pointers */
typedef timelib_tzinfo* (*timelib_tz_get_wrapper)(char *tzname, const timelib_tzdb *tzdb, int *error_code);

/* From dow.c */
timelib_sll timelib_day_of_week(timelib_sll y, timelib_sll m, timelib_sll d);
timelib_sll timelib_iso_day_of_week(timelib_sll y, timelib_sll m, timelib_sll d);
timelib_sll timelib_day_of_year(timelib_sll y, timelib_sll m, timelib_sll d);
timelib_sll timelib_daynr_from_weeknr(timelib_sll iy, timelib_sll iw, timelib_sll id);
timelib_sll timelib_days_in_month(timelib_sll y, timelib_sll m);

void timelib_isoweek_from_date(timelib_sll y, timelib_sll m, timelib_sll d, timelib_sll *iw, timelib_sll *iy);
void timelib_isodate_from_date(timelib_sll y, timelib_sll m, timelib_sll d, timelib_sll *iy, timelib_sll *iw, timelib_sll *id);

void timelib_date_from_isodate(timelib_sll iy, timelib_sll iw, timelib_sll id, timelib_sll *y, timelib_sll *m, timelib_sll *d);

int timelib_valid_time(timelib_sll h, timelib_sll i, timelib_sll s);
int timelib_valid_date(timelib_sll y, timelib_sll m, timelib_sll d);

/* From parse_date.re */
timelib_time *timelib_strtotime(char *s, size_t len, timelib_error_container **errors, const timelib_tzdb *tzdb, timelib_tz_get_wrapper tz_get_wrapper);
timelib_time *timelib_parse_from_format(char *format, char *s, size_t len, timelib_error_container **errors, const timelib_tzdb *tzdb, timelib_tz_get_wrapper tz_get_wrapper);
void timelib_fill_holes(timelib_time *parsed, timelib_time *now, int options);
char *timelib_timezone_id_from_abbr(const char *abbr, timelib_long gmtoffset, int isdst);
const timelib_tz_lookup_table *timelib_timezone_abbreviations_list(void);
timelib_long timelib_parse_tz_cor(char**);
timelib_long timelib_parse_zone(char **ptr, int *dst, timelib_time *t, int *tz_not_found, const timelib_tzdb *tzdb, timelib_tz_get_wrapper tz_wrapper);

/* From parse_iso_intervals.re */
void timelib_strtointerval(char *s, size_t len,
                           timelib_time **begin, timelib_time **end,
						   timelib_rel_time **period, int *recurrences,
						   struct timelib_error_container **errors);


/* From tm2unixtime.c */
void timelib_update_ts(timelib_time* time, timelib_tzinfo* tzi);
void timelib_do_normalize(timelib_time *base);
void timelib_do_rel_normalize(timelib_time *base, timelib_rel_time *rt);

/* From unixtime2tm.c */
int timelib_apply_localtime(timelib_time *t, unsigned int localtime);
void timelib_unixtime2gmt(timelib_time* tm, timelib_sll ts);
void timelib_unixtime2local(timelib_time *tm, timelib_sll ts);
void timelib_update_from_sse(timelib_time *tm);
void timelib_set_timezone_from_offset(timelib_time *t, timelib_sll utc_offset);
void timelib_set_timezone_from_abbr(timelib_time *t, timelib_abbr_info abbr_info);
void timelib_set_timezone(timelib_time *t, timelib_tzinfo *tz);

/* From parse_tz.c */
int timelib_timezone_id_is_valid(char *timezone, const timelib_tzdb *tzdb);
timelib_tzinfo *timelib_parse_tzfile(char *timezone, const timelib_tzdb *tzdb, int *error_code);
int timelib_timestamp_is_in_dst(timelib_sll ts, timelib_tzinfo *tz);
timelib_time_offset *timelib_get_time_zone_info(timelib_sll ts, timelib_tzinfo *tz);
timelib_sll timelib_get_current_offset(timelib_time *t);
void timelib_dump_tzinfo(timelib_tzinfo *tz);
const timelib_tzdb *timelib_builtin_db(void);
const timelib_tzdb_index_entry *timelib_timezone_identifiers_list(timelib_tzdb *tzdb, int *count);

/* From parse_zoneinfo.c */
timelib_tzdb *timelib_zoneinfo(char *directory);
void timelib_zoneinfo_dtor(timelib_tzdb *tzdb);

/* From timelib.c */

/* Returns a static string containing an error message belonging to a specific
 * error code. */
const char *timelib_get_error_message(int error_code);

timelib_tzinfo* timelib_tzinfo_ctor(char *name);
void timelib_time_tz_abbr_update(timelib_time* tm, char* tz_abbr);
void timelib_time_tz_name_update(timelib_time* tm, char* tz_name);
void timelib_tzinfo_dtor(timelib_tzinfo *tz);
timelib_tzinfo* timelib_tzinfo_clone(timelib_tzinfo *tz);

timelib_rel_time* timelib_rel_time_ctor(void);
void timelib_rel_time_dtor(timelib_rel_time* t);
timelib_rel_time* timelib_rel_time_clone(timelib_rel_time *tz);

timelib_time* timelib_time_ctor(void);
void timelib_time_set_option(timelib_time* tm, int option, void* option_value);
void timelib_time_dtor(timelib_time* t);
timelib_time* timelib_time_clone(timelib_time* orig);
int timelib_time_compare(timelib_time *t1, timelib_time *t2);

timelib_time_offset* timelib_time_offset_ctor(void);
void timelib_time_offset_dtor(timelib_time_offset* t);

void timelib_error_container_dtor(timelib_error_container *errors);

timelib_long timelib_date_to_int(timelib_time *d, int *error);
void timelib_dump_date(timelib_time *d, int options);
void timelib_dump_rel_time(timelib_rel_time *d);

void timelib_decimal_hour_to_hms(double h, int *hour, int *min, int *sec);
timelib_long timelib_parse_tz_cor(char **ptr);

/* from astro.c */
double timelib_ts_to_juliandate(timelib_sll ts);
int timelib_astro_rise_set_altitude(timelib_time *time, double lon, double lat, double altit, int upper_limb, double *h_rise, double *h_set, timelib_sll *ts_rise, timelib_sll *ts_set, timelib_sll *ts_transit);

/* from interval.c */
timelib_rel_time *timelib_diff(timelib_time *one, timelib_time *two);
timelib_time *timelib_add(timelib_time *t, timelib_rel_time *interval);
timelib_time *timelib_sub(timelib_time *t, timelib_rel_time *interval);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
