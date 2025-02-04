/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/date_time.h>

#include <aws/common/array_list.h>
#include <aws/common/byte_buf.h>
#include <aws/common/byte_order.h>
#include <aws/common/clock.h>
#include <aws/common/string.h>
#include <aws/common/time.h>

#include <ctype.h>
#include <math.h>

static const char *RFC822_DATE_FORMAT_STR_MINUS_Z = "%a, %d %b %Y %H:%M:%S GMT";
static const char *RFC822_DATE_FORMAT_STR_WITH_Z = "%a, %d %b %Y %H:%M:%S %Z";
static const char *RFC822_SHORT_DATE_FORMAT_STR = "%a, %d %b %Y";
static const char *ISO_8601_LONG_DATE_FORMAT_STR = "%Y-%m-%dT%H:%M:%SZ";
static const char *ISO_8601_SHORT_DATE_FORMAT_STR = "%Y-%m-%d";
static const char *ISO_8601_LONG_BASIC_DATE_FORMAT_STR = "%Y%m%dT%H%M%SZ";
static const char *ISO_8601_SHORT_BASIC_DATE_FORMAT_STR = "%Y%m%d";

#define STR_TRIPLET_TO_INDEX(str)                                                                                      \
    (((uint32_t)tolower((uint8_t)((str)[0])) << 0) | ((uint32_t)tolower((uint8_t)((str)[1])) << 8) |                   \
     ((uint32_t)tolower((uint8_t)((str)[2])) << 16))

static uint32_t s_jan = 0;
static uint32_t s_feb = 0;
static uint32_t s_mar = 0;
static uint32_t s_apr = 0;
static uint32_t s_may = 0;
static uint32_t s_jun = 0;
static uint32_t s_jul = 0;
static uint32_t s_aug = 0;
static uint32_t s_sep = 0;
static uint32_t s_oct = 0;
static uint32_t s_nov = 0;
static uint32_t s_dec = 0;

static uint32_t s_utc = 0;
static uint32_t s_gmt = 0;

static void s_check_init_str_to_int(void) {
    if (!s_jan) {
        s_jan = STR_TRIPLET_TO_INDEX("jan");
        s_feb = STR_TRIPLET_TO_INDEX("feb");
        s_mar = STR_TRIPLET_TO_INDEX("mar");
        s_apr = STR_TRIPLET_TO_INDEX("apr");
        s_may = STR_TRIPLET_TO_INDEX("may");
        s_jun = STR_TRIPLET_TO_INDEX("jun");
        s_jul = STR_TRIPLET_TO_INDEX("jul");
        s_aug = STR_TRIPLET_TO_INDEX("aug");
        s_sep = STR_TRIPLET_TO_INDEX("sep");
        s_oct = STR_TRIPLET_TO_INDEX("oct");
        s_nov = STR_TRIPLET_TO_INDEX("nov");
        s_dec = STR_TRIPLET_TO_INDEX("dec");
        s_utc = STR_TRIPLET_TO_INDEX("utc");
        s_gmt = STR_TRIPLET_TO_INDEX("gmt");
    }
}

/* Get the 0-11 monthly number from a string representing Month. Case insensitive and will stop on abbreviation*/
static int get_month_number_from_str(const char *time_string, size_t start_index, size_t stop_index) {
    s_check_init_str_to_int();

    if (stop_index - start_index < 3) {
        return -1;
    }

    /* This AND forces the string to lowercase (assuming ASCII) */
    uint32_t comp_val = STR_TRIPLET_TO_INDEX(time_string + start_index);

    /* this can't be a switch, because I can't make it a constant expression. */
    if (s_jan == comp_val) {
        return 0;
    }

    if (s_feb == comp_val) {
        return 1;
    }

    if (s_mar == comp_val) {
        return 2;
    }

    if (s_apr == comp_val) {
        return 3;
    }

    if (s_may == comp_val) {
        return 4;
    }

    if (s_jun == comp_val) {
        return 5;
    }

    if (s_jul == comp_val) {
        return 6;
    }

    if (s_aug == comp_val) {
        return 7;
    }

    if (s_sep == comp_val) {
        return 8;
    }

    if (s_oct == comp_val) {
        return 9;
    }

    if (s_nov == comp_val) {
        return 10;
    }

    if (s_dec == comp_val) {
        return 11;
    }

    return -1;
}

/* Detects whether or not the passed in timezone string is a UTC zone. */
static bool is_utc_time_zone(const char *str) {
    s_check_init_str_to_int();

    size_t len = strlen(str);

    if (len > 0) {
        if (tolower((uint8_t)str[0]) == 'z') {
            return true;
        }

        /* offsets count since their usable */
        if (len == 5 && (str[0] == '+' || str[0] == '-')) {
            return true;
        }

        if (len == 2) {
            return tolower((uint8_t)str[0]) == 'u' && tolower((uint8_t)str[1]) == 't';
        }

        if (len < 3) {
            return false;
        }

        uint32_t comp_val = STR_TRIPLET_TO_INDEX(str);

        if (comp_val == s_utc || comp_val == s_gmt) {
            return true;
        }
    }

    return false;
}

struct tm s_get_time_struct(struct aws_date_time *dt, bool local_time) {
    struct tm time;
    AWS_ZERO_STRUCT(time);
    if (local_time) {
        aws_localtime(dt->timestamp, &time);
    } else {
        aws_gmtime(dt->timestamp, &time);
    }

    return time;
}

void aws_date_time_init_now(struct aws_date_time *dt) {
    uint64_t current_time_ns = 0;
    aws_sys_clock_get_ticks(&current_time_ns);
    aws_date_time_init_epoch_millis(
        dt, aws_timestamp_convert(current_time_ns, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_MILLIS, NULL));
}

void aws_date_time_init_epoch_millis(struct aws_date_time *dt, uint64_t ms_since_epoch) {
    uint64_t milliseconds = 0;
    dt->timestamp =
        (time_t)aws_timestamp_convert(ms_since_epoch, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_SECS, &milliseconds);
    dt->milliseconds = (uint16_t)milliseconds;
    dt->gmt_time = s_get_time_struct(dt, false);
    dt->local_time = s_get_time_struct(dt, true);
}

void aws_date_time_init_epoch_secs(struct aws_date_time *dt, double sec_ms) {
    double integral = 0;
    dt->milliseconds = (uint16_t)(round(modf(sec_ms, &integral) * AWS_TIMESTAMP_MILLIS));
    dt->timestamp = (time_t)integral;
    dt->gmt_time = s_get_time_struct(dt, false);
    dt->local_time = s_get_time_struct(dt, true);
}

enum parser_state {
    ON_WEEKDAY,
    ON_SPACE_DELIM,
    ON_YEAR,
    ON_MONTH,
    ON_MONTH_DAY,
    ON_HOUR,
    ON_MINUTE,
    ON_SECOND,
    ON_TZ,
    FINISHED,
};

static bool s_parse_rfc_822(
    const struct aws_byte_cursor *date_str_cursor,
    struct tm *parsed_time,
    struct aws_date_time *dt) {
    size_t len = date_str_cursor->len;

    size_t index = 0;
    size_t state_start_index = 0;
    int state = ON_WEEKDAY;
    bool error = false;

    AWS_ZERO_STRUCT(*parsed_time);

    while (!error && index < len) {
        char c = (char)date_str_cursor->ptr[index];

        switch (state) {
            /* week day abbr is optional. */
            case ON_WEEKDAY:
                if (c == ',') {
                    state = ON_SPACE_DELIM;
                    state_start_index = index + 1;
                } else if (aws_isdigit(c)) {
                    state = ON_MONTH_DAY;
                } else if (!aws_isalpha(c)) {
                    error = true;
                }
                break;
            case ON_SPACE_DELIM:
                if (aws_isspace(c)) {
                    state = ON_MONTH_DAY;
                    state_start_index = index + 1;
                } else {
                    error = true;
                }
                break;
            case ON_MONTH_DAY:
                if (aws_isdigit(c)) {
                    parsed_time->tm_mday = parsed_time->tm_mday * 10 + (c - '0');
                } else if (aws_isspace(c)) {
                    state = ON_MONTH;
                    state_start_index = index + 1;
                } else {
                    error = true;
                }
                break;
            case ON_MONTH:
                if (aws_isspace(c)) {
                    int monthNumber =
                        get_month_number_from_str((const char *)date_str_cursor->ptr, state_start_index, index + 1);

                    if (monthNumber > -1) {
                        state = ON_YEAR;
                        state_start_index = index + 1;
                        parsed_time->tm_mon = monthNumber;
                    } else {
                        error = true;
                    }
                } else if (!aws_isalpha(c)) {
                    error = true;
                }
                break;
            /* year can be 4 or 2 digits. */
            case ON_YEAR:
                if (aws_isspace(c) && index - state_start_index == 4) {
                    state = ON_HOUR;
                    state_start_index = index + 1;
                    parsed_time->tm_year -= 1900;
                } else if (aws_isspace(c) && index - state_start_index == 2) {
                    state = 5;
                    state_start_index = index + 1;
                    parsed_time->tm_year += 2000 - 1900;
                } else if (aws_isdigit(c)) {
                    parsed_time->tm_year = parsed_time->tm_year * 10 + (c - '0');
                } else {
                    error = true;
                }
                break;
            case ON_HOUR:
                if (c == ':' && index - state_start_index == 2) {
                    state = ON_MINUTE;
                    state_start_index = index + 1;
                } else if (aws_isdigit(c)) {
                    parsed_time->tm_hour = parsed_time->tm_hour * 10 + (c - '0');
                } else {
                    error = true;
                }
                break;
            case ON_MINUTE:
                if (c == ':' && index - state_start_index == 2) {
                    state = ON_SECOND;
                    state_start_index = index + 1;
                } else if (aws_isdigit(c)) {
                    parsed_time->tm_min = parsed_time->tm_min * 10 + (c - '0');
                } else {
                    error = true;
                }
                break;
            case ON_SECOND:
                if (aws_isspace(c) && index - state_start_index == 2) {
                    state = ON_TZ;
                    state_start_index = index + 1;
                } else if (aws_isdigit(c)) {
                    parsed_time->tm_sec = parsed_time->tm_sec * 10 + (c - '0');
                } else {
                    error = true;
                }
                break;
            case ON_TZ:
                if ((aws_isalnum(c) || c == '-' || c == '+') && (index - state_start_index) < 5) {
                    dt->tz[index - state_start_index] = c;
                } else {
                    error = true;
                }

                break;
            default:
                error = true;
                break;
        }

        index++;
    }

    if (dt->tz[0] != 0) {
        if (is_utc_time_zone(dt->tz)) {
            dt->utc_assumed = true;
        } else {
            error = true;
        }
    }

    return error || state != ON_TZ ? false : true;
}

/* Returns true if the next N characters are digits, advancing the string and getting their numeric value */
static bool s_read_n_digits(struct aws_byte_cursor *str, size_t n, int *out_val) {
    int val = 0;
    if (str->len < n) {
        return false;
    }

    for (size_t i = 0; i < n; ++i) {
        uint8_t c = str->ptr[i];
        if (aws_isdigit(c)) {
            val = val * 10 + (c - '0');
        } else {
            return false;
        }
    }

    aws_byte_cursor_advance(str, n);
    *out_val = val;
    return true;
}

/* Returns true if there's 1 more character, advancing the string and getting the character's value. */
static bool s_read_1_char(struct aws_byte_cursor *str, uint8_t *out_c) {
    if (str->len == 0) {
        return false;
    }

    *out_c = str->ptr[0];
    aws_byte_cursor_advance(str, 1);
    return true;
}

/* Returns true (and advances str) if next character is c */
static bool s_advance_if_next_char_is(struct aws_byte_cursor *str, uint8_t c) {
    if (str->len == 0 || str->ptr[0] != c) {
        return false;
    }

    aws_byte_cursor_advance(str, 1);
    return true;
}

/* If the (optional) fractional seconds (".123" or ",123") are next, str is advanced.
 * Returns false if there was an error */
static bool s_skip_optional_fractional_seconds(struct aws_byte_cursor *str) {
    if (str->len == 0) {
        return true;
    }

    uint8_t c = str->ptr[0];
    if (c != '.' && c != ',') {
        return true;
    }

    size_t num_digits = 0;
    for (size_t i = 1; i < str->len; ++i) {
        if (aws_isdigit(str->ptr[i])) {
            ++num_digits;
        } else {
            break;
        }
    }

    if (num_digits == 0) {
        return false;
    }

    aws_byte_cursor_advance(str, 1 + num_digits);
    return true;
}

/* Parses ISO 8601, both extended and basic format are accepted.
 * Returns true if successful. */
static bool s_parse_iso_8601(struct aws_byte_cursor str, struct tm *parsed_time, time_t *seconds_offset) {
    AWS_ZERO_STRUCT(*parsed_time);
    *seconds_offset = 0;
    uint8_t c = 0;

    /* read year */
    if (!s_read_n_digits(&str, 4, &parsed_time->tm_year)) {
        return false;
    }
    parsed_time->tm_year -= 1900;

    /* be lenient, allow date with separator or not */
    bool has_date_separator = s_advance_if_next_char_is(&str, '-');

    /* read month */
    if (!s_read_n_digits(&str, 2, &parsed_time->tm_mon)) {
        return false;
    }
    parsed_time->tm_mon -= 1;

    if (has_date_separator) {
        if (!s_read_1_char(&str, &c) || c != '-') {
            return false;
        }
    }

    /* read month-day */
    if (!s_read_n_digits(&str, 2, &parsed_time->tm_mday)) {
        return false;
    }

    /* ISO8601 supports date only with no time portion */
    if (str.len == 0) {
        return true;
    }

    /* followed by T or space (allowed by rfc3339#section-5.6) */
    if (!s_read_1_char(&str, &c) || (tolower(c) != 't' && c != ' ')) {
        return false;
    }

    /* read hours */
    if (!s_read_n_digits(&str, 2, &parsed_time->tm_hour)) {
        return false;
    }

    /* be lenient, allow time with separator or not */
    bool has_time_separator = s_advance_if_next_char_is(&str, ':');

    /* read minutes */
    if (!s_read_n_digits(&str, 2, &parsed_time->tm_min)) {
        return false;
    }

    if (has_time_separator) {
        if (!s_read_1_char(&str, &c) || c != ':') {
            return false;
        }
    }

    /* read seconds */
    if (!s_read_n_digits(&str, 2, &parsed_time->tm_sec)) {
        return false;
    }

    /* fractional seconds are optional (discard value since tm struct has no corresponding field) */
    if (!s_skip_optional_fractional_seconds(&str)) {
        return false;
    }

    /* read final Z, or (+/-) indicating there will be an offset */
    if (!s_read_1_char(&str, &c)) {
        return false;
    }

    if (tolower(c) == 'z') {
        /* Success! */
        return true;
    }

    if (c != '+' && c != '-') {
        return false;
    }

    bool negative_offset = c == '-';

    /* read hours offset */
    int hours_offset = 0;
    if (!s_read_n_digits(&str, 2, &hours_offset)) {
        return false;
    }

    /* be lenient, allow offset with separator or not */
    s_advance_if_next_char_is(&str, ':');

    /* read minutes offset */
    int minutes_offset = 0;
    if (!s_read_n_digits(&str, 2, &minutes_offset)) {
        return false;
    }

    /* Success! */
    *seconds_offset = (time_t)(hours_offset * 3600 + minutes_offset * 60) * (negative_offset ? -1 : 1);
    return true;
}

int aws_date_time_init_from_str_cursor(
    struct aws_date_time *dt,
    const struct aws_byte_cursor *date_str_cursor,
    enum aws_date_format fmt) {
    AWS_ERROR_PRECONDITION(date_str_cursor->len <= AWS_DATE_TIME_STR_MAX_LEN, AWS_ERROR_OVERFLOW_DETECTED);

    AWS_ZERO_STRUCT(*dt);

    struct tm parsed_time;
    bool successfully_parsed = false;

    time_t seconds_offset = 0;
    if (fmt == AWS_DATE_FORMAT_ISO_8601 || fmt == AWS_DATE_FORMAT_ISO_8601_BASIC ||
        fmt == AWS_DATE_FORMAT_AUTO_DETECT) {
        if (s_parse_iso_8601(*date_str_cursor, &parsed_time, &seconds_offset)) {
            dt->utc_assumed = true;
            successfully_parsed = true;
        }
    }

    if (fmt == AWS_DATE_FORMAT_RFC822 || (fmt == AWS_DATE_FORMAT_AUTO_DETECT && !successfully_parsed)) {
        if (s_parse_rfc_822(date_str_cursor, &parsed_time, dt)) {
            successfully_parsed = true;

            if (dt->utc_assumed) {
                if (dt->tz[0] == '+' || dt->tz[0] == '-') {
                    /* in this format, the offset is in format +/-HHMM so convert that to seconds and we'll use
                     * the offset later. */
                    char min_str[3] = {0};
                    char hour_str[3] = {0};
                    hour_str[0] = dt->tz[1];
                    hour_str[1] = dt->tz[2];
                    min_str[0] = dt->tz[3];
                    min_str[1] = dt->tz[4];

                    long hour = strtol(hour_str, NULL, 10);
                    long min = strtol(min_str, NULL, 10);
                    seconds_offset = (time_t)(hour * 3600 + min * 60);

                    if (dt->tz[0] == '-') {
                        seconds_offset = -seconds_offset;
                    }
                }
            }
        }
    }

    if (!successfully_parsed) {
        return aws_raise_error(AWS_ERROR_INVALID_DATE_STR);
    }

    if (dt->utc_assumed || seconds_offset) {
        dt->timestamp = aws_timegm(&parsed_time);
    } else {
        dt->timestamp = mktime(&parsed_time);
    }

    /* negative means we need to move west (increase the timestamp), positive means head east, so decrease the
     * timestamp. */
    dt->timestamp -= seconds_offset;

    dt->milliseconds = 0U;
    dt->gmt_time = s_get_time_struct(dt, false);
    dt->local_time = s_get_time_struct(dt, true);

    return AWS_OP_SUCCESS;
}

int aws_date_time_init_from_str(
    struct aws_date_time *dt,
    const struct aws_byte_buf *date_str,
    enum aws_date_format fmt) {
    AWS_ERROR_PRECONDITION(date_str->len <= AWS_DATE_TIME_STR_MAX_LEN, AWS_ERROR_OVERFLOW_DETECTED);

    struct aws_byte_cursor date_cursor = aws_byte_cursor_from_buf(date_str);
    return aws_date_time_init_from_str_cursor(dt, &date_cursor, fmt);
}

static inline int s_date_to_str(const struct tm *tm, const char *format_str, struct aws_byte_buf *output_buf) {
    size_t remaining_space = output_buf->capacity - output_buf->len;
    size_t bytes_written = strftime((char *)output_buf->buffer + output_buf->len, remaining_space, format_str, tm);

    if (bytes_written == 0) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    output_buf->len += bytes_written;

    return AWS_OP_SUCCESS;
}

int aws_date_time_to_local_time_str(
    const struct aws_date_time *dt,
    enum aws_date_format fmt,
    struct aws_byte_buf *output_buf) {
    AWS_ASSERT(fmt != AWS_DATE_FORMAT_AUTO_DETECT);

    switch (fmt) {
        case AWS_DATE_FORMAT_RFC822:
            return s_date_to_str(&dt->local_time, RFC822_DATE_FORMAT_STR_WITH_Z, output_buf);

        case AWS_DATE_FORMAT_ISO_8601:
            return s_date_to_str(&dt->local_time, ISO_8601_LONG_DATE_FORMAT_STR, output_buf);

        case AWS_DATE_FORMAT_ISO_8601_BASIC:
            return s_date_to_str(&dt->local_time, ISO_8601_LONG_BASIC_DATE_FORMAT_STR, output_buf);

        default:
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
}

int aws_date_time_to_utc_time_str(
    const struct aws_date_time *dt,
    enum aws_date_format fmt,
    struct aws_byte_buf *output_buf) {
    AWS_ASSERT(fmt != AWS_DATE_FORMAT_AUTO_DETECT);

    switch (fmt) {
        case AWS_DATE_FORMAT_RFC822:
            return s_date_to_str(&dt->gmt_time, RFC822_DATE_FORMAT_STR_MINUS_Z, output_buf);

        case AWS_DATE_FORMAT_ISO_8601:
            return s_date_to_str(&dt->gmt_time, ISO_8601_LONG_DATE_FORMAT_STR, output_buf);

        case AWS_DATE_FORMAT_ISO_8601_BASIC:
            return s_date_to_str(&dt->gmt_time, ISO_8601_LONG_BASIC_DATE_FORMAT_STR, output_buf);

        default:
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
}

int aws_date_time_to_local_time_short_str(
    const struct aws_date_time *dt,
    enum aws_date_format fmt,
    struct aws_byte_buf *output_buf) {
    AWS_ASSERT(fmt != AWS_DATE_FORMAT_AUTO_DETECT);

    switch (fmt) {
        case AWS_DATE_FORMAT_RFC822:
            return s_date_to_str(&dt->local_time, RFC822_SHORT_DATE_FORMAT_STR, output_buf);

        case AWS_DATE_FORMAT_ISO_8601:
            return s_date_to_str(&dt->local_time, ISO_8601_SHORT_DATE_FORMAT_STR, output_buf);

        case AWS_DATE_FORMAT_ISO_8601_BASIC:
            return s_date_to_str(&dt->local_time, ISO_8601_SHORT_BASIC_DATE_FORMAT_STR, output_buf);

        default:
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
}

int aws_date_time_to_utc_time_short_str(
    const struct aws_date_time *dt,
    enum aws_date_format fmt,
    struct aws_byte_buf *output_buf) {
    AWS_ASSERT(fmt != AWS_DATE_FORMAT_AUTO_DETECT);

    switch (fmt) {
        case AWS_DATE_FORMAT_RFC822:
            return s_date_to_str(&dt->gmt_time, RFC822_SHORT_DATE_FORMAT_STR, output_buf);

        case AWS_DATE_FORMAT_ISO_8601:
            return s_date_to_str(&dt->gmt_time, ISO_8601_SHORT_DATE_FORMAT_STR, output_buf);

        case AWS_DATE_FORMAT_ISO_8601_BASIC:
            return s_date_to_str(&dt->gmt_time, ISO_8601_SHORT_BASIC_DATE_FORMAT_STR, output_buf);

        default:
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
}

double aws_date_time_as_epoch_secs(const struct aws_date_time *dt) {
    return (double)dt->timestamp + (double)(dt->milliseconds / 1000.0);
}

uint64_t aws_date_time_as_nanos(const struct aws_date_time *dt) {
    return aws_timestamp_convert((uint64_t)dt->timestamp, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL) +
           aws_timestamp_convert((uint64_t)dt->milliseconds, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);
}

uint64_t aws_date_time_as_millis(const struct aws_date_time *dt) {
    return aws_timestamp_convert((uint64_t)dt->timestamp, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_MILLIS, NULL) +
           (uint64_t)dt->milliseconds;
}

uint16_t aws_date_time_year(const struct aws_date_time *dt, bool local_time) {
    const struct tm *time = local_time ? &dt->local_time : &dt->gmt_time;

    return (uint16_t)(time->tm_year + 1900);
}

enum aws_date_month aws_date_time_month(const struct aws_date_time *dt, bool local_time) {
    const struct tm *time = local_time ? &dt->local_time : &dt->gmt_time;

    return time->tm_mon;
}

uint8_t aws_date_time_month_day(const struct aws_date_time *dt, bool local_time) {
    const struct tm *time = local_time ? &dt->local_time : &dt->gmt_time;

    return (uint8_t)time->tm_mday;
}

enum aws_date_day_of_week aws_date_time_day_of_week(const struct aws_date_time *dt, bool local_time) {
    const struct tm *time = local_time ? &dt->local_time : &dt->gmt_time;

    return time->tm_wday;
}

uint8_t aws_date_time_hour(const struct aws_date_time *dt, bool local_time) {
    const struct tm *time = local_time ? &dt->local_time : &dt->gmt_time;

    return (uint8_t)time->tm_hour;
}

uint8_t aws_date_time_minute(const struct aws_date_time *dt, bool local_time) {
    const struct tm *time = local_time ? &dt->local_time : &dt->gmt_time;

    return (uint8_t)time->tm_min;
}

uint8_t aws_date_time_second(const struct aws_date_time *dt, bool local_time) {
    const struct tm *time = local_time ? &dt->local_time : &dt->gmt_time;

    return (uint8_t)time->tm_sec;
}

bool aws_date_time_dst(const struct aws_date_time *dt, bool local_time) {
    const struct tm *time = local_time ? &dt->local_time : &dt->gmt_time;

    return (bool)time->tm_isdst;
}

time_t aws_date_time_diff(const struct aws_date_time *a, const struct aws_date_time *b) {
    return a->timestamp - b->timestamp;
}
