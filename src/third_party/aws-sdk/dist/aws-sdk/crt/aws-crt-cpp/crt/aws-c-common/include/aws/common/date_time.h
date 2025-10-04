#ifndef AWS_COMMON_DATE_TIME_H
#define AWS_COMMON_DATE_TIME_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/common.h>

#include <time.h>

AWS_PUSH_SANE_WARNING_LEVEL

enum {
    AWS_DATE_TIME_STR_MAX_LEN = 100,
    AWS_DATE_TIME_STR_MAX_BASIC_LEN = 20,
};

struct aws_byte_buf;
struct aws_byte_cursor;

enum aws_date_format {
    AWS_DATE_FORMAT_RFC822,
    AWS_DATE_FORMAT_ISO_8601,
    AWS_DATE_FORMAT_ISO_8601_BASIC,
    AWS_DATE_FORMAT_AUTO_DETECT,
};

enum aws_date_month {
    AWS_DATE_MONTH_JANUARY = 0,
    AWS_DATE_MONTH_FEBRUARY,
    AWS_DATE_MONTH_MARCH,
    AWS_DATE_MONTH_APRIL,
    AWS_DATE_MONTH_MAY,
    AWS_DATE_MONTH_JUNE,
    AWS_DATE_MONTH_JULY,
    AWS_DATE_MONTH_AUGUST,
    AWS_DATE_MONTH_SEPTEMBER,
    AWS_DATE_MONTH_OCTOBER,
    AWS_DATE_MONTH_NOVEMBER,
    AWS_DATE_MONTH_DECEMBER,
};

enum aws_date_day_of_week {
    AWS_DATE_DAY_OF_WEEK_SUNDAY = 0,
    AWS_DATE_DAY_OF_WEEK_MONDAY,
    AWS_DATE_DAY_OF_WEEK_TUESDAY,
    AWS_DATE_DAY_OF_WEEK_WEDNESDAY,
    AWS_DATE_DAY_OF_WEEK_THURSDAY,
    AWS_DATE_DAY_OF_WEEK_FRIDAY,
    AWS_DATE_DAY_OF_WEEK_SATURDAY,
};

struct aws_date_time {
    time_t timestamp;
    uint16_t milliseconds;
    char tz[6];
    struct tm gmt_time;
    struct tm local_time;
    bool utc_assumed;
};

AWS_EXTERN_C_BEGIN

/**
 * Initializes dt to be the current system time.
 */
AWS_COMMON_API void aws_date_time_init_now(struct aws_date_time *dt);

/**
 * Initializes dt to be the time represented in milliseconds since unix epoch.
 */
AWS_COMMON_API void aws_date_time_init_epoch_millis(struct aws_date_time *dt, uint64_t ms_since_epoch);

/**
 * Initializes dt to be the time represented in seconds.millis since unix epoch.
 */
AWS_COMMON_API void aws_date_time_init_epoch_secs(struct aws_date_time *dt, double sec_ms);

/**
 * Initializes dt to be the time represented by date_str in format 'fmt'. Returns AWS_OP_SUCCESS if the
 * string was successfully parsed, returns  AWS_OP_ERR if parsing failed.
 *
 * The parser is lenient regarding AWS_DATE_FORMAT_ISO_8601 vs AWS_DATE_FORMAT_ISO_8601_BASIC.
 * Regardless of which you pass in, both "2002-10-02T08:05:09Z" and "20021002T080509Z" would be accepted.
 *
 * Notes for AWS_DATE_FORMAT_RFC822:
 * If no time zone information is provided, it is assumed to be local time (please don't do this).
 *
 * Only time zones indicating Universal Time (e.g. Z, UT, UTC, or GMT),
 * or offsets from UTC (e.g. +0100, -0700), are accepted.
 *
 * Really, it's just better if you always use Universal Time.
 */
AWS_COMMON_API int aws_date_time_init_from_str(
    struct aws_date_time *dt,
    const struct aws_byte_buf *date_str,
    enum aws_date_format fmt);

/**
 * aws_date_time_init variant that takes a byte_cursor rather than a byte_buf
 */
AWS_COMMON_API int aws_date_time_init_from_str_cursor(
    struct aws_date_time *dt,
    const struct aws_byte_cursor *date_str_cursor,
    enum aws_date_format fmt);

/**
 * Copies the current time as a formatted date string in local time into output_buf. If buffer is too small, it will
 * return AWS_OP_ERR. A good size suggestion is AWS_DATE_TIME_STR_MAX_LEN bytes. AWS_DATE_FORMAT_AUTO_DETECT is not
 * allowed.
 */
AWS_COMMON_API int aws_date_time_to_local_time_str(
    const struct aws_date_time *dt,
    enum aws_date_format fmt,
    struct aws_byte_buf *output_buf);

/**
 * Copies the current time as a formatted date string in utc time into output_buf. If buffer is too small, it will
 * return AWS_OP_ERR. A good size suggestion is AWS_DATE_TIME_STR_MAX_LEN bytes. AWS_DATE_FORMAT_AUTO_DETECT is not
 * allowed.
 */
AWS_COMMON_API int aws_date_time_to_utc_time_str(
    const struct aws_date_time *dt,
    enum aws_date_format fmt,
    struct aws_byte_buf *output_buf);

/**
 * Copies the current time as a formatted short date string in local time into output_buf. If buffer is too small, it
 * will return AWS_OP_ERR. A good size suggestion is AWS_DATE_TIME_STR_MAX_LEN bytes. AWS_DATE_FORMAT_AUTO_DETECT is not
 * allowed.
 */
AWS_COMMON_API int aws_date_time_to_local_time_short_str(
    const struct aws_date_time *dt,
    enum aws_date_format fmt,
    struct aws_byte_buf *output_buf);

/**
 * Copies the current time as a formatted short date string in utc time into output_buf. If buffer is too small, it will
 * return AWS_OP_ERR. A good size suggestion is AWS_DATE_TIME_STR_MAX_LEN bytes. AWS_DATE_FORMAT_AUTO_DETECT is not
 * allowed.
 */
AWS_COMMON_API int aws_date_time_to_utc_time_short_str(
    const struct aws_date_time *dt,
    enum aws_date_format fmt,
    struct aws_byte_buf *output_buf);

AWS_COMMON_API double aws_date_time_as_epoch_secs(const struct aws_date_time *dt);
AWS_COMMON_API uint64_t aws_date_time_as_nanos(const struct aws_date_time *dt);
AWS_COMMON_API uint64_t aws_date_time_as_millis(const struct aws_date_time *dt);
AWS_COMMON_API uint16_t aws_date_time_year(const struct aws_date_time *dt, bool local_time);
AWS_COMMON_API enum aws_date_month aws_date_time_month(const struct aws_date_time *dt, bool local_time);
AWS_COMMON_API uint8_t aws_date_time_month_day(const struct aws_date_time *dt, bool local_time);
AWS_COMMON_API enum aws_date_day_of_week aws_date_time_day_of_week(const struct aws_date_time *dt, bool local_time);
AWS_COMMON_API uint8_t aws_date_time_hour(const struct aws_date_time *dt, bool local_time);
AWS_COMMON_API uint8_t aws_date_time_minute(const struct aws_date_time *dt, bool local_time);
AWS_COMMON_API uint8_t aws_date_time_second(const struct aws_date_time *dt, bool local_time);
AWS_COMMON_API bool aws_date_time_dst(const struct aws_date_time *dt, bool local_time);

/**
 * returns the difference of a and b (a - b) in seconds.
 */
AWS_COMMON_API time_t aws_date_time_diff(const struct aws_date_time *a, const struct aws_date_time *b);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_DATE_TIME_H */
