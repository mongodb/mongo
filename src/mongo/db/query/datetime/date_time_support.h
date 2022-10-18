/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/db/service_context.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

struct _timelib_error_container;
struct _timelib_time;
struct _timelib_rel_time;
struct _timelib_tzdb;
struct _timelib_tzinfo;

namespace mongo {

using namespace std::string_literals;
static constexpr StringData kISOFormatString = "%Y-%m-%dT%H:%M:%S.%LZ"_sd;

/**
 * A set of standard measures of time used to express a length of time interval.
 */
enum class TimeUnit {
    year,
    quarter,  // A quarter of a year.
    month,
    week,
    day,
    hour,
    minute,
    second,
    millisecond
};

/**
 * Day of a week.
 */
enum class DayOfWeek : uint8_t {
    monday = 1,
    tuesday,
    wednesday,
    thursday,
    friday,
    saturday,
    sunday
};

static constexpr DayOfWeek kStartOfWeekDefault{DayOfWeek::sunday};

/**
 * A TimeZone object represents one way of formatting/reading dates to compute things like the day
 * of the week or the hour of a given date. Different TimeZone objects may report different answers
 * for the hour, minute, or second of a date, even when given the same date.
 */
class TimeZone {

public:
    /**
     * A struct with member variables describing the different parts of the date.
     */
    struct DateParts {
        DateParts(const _timelib_time&, Date_t);

        int year;
        int month;
        int dayOfMonth;
        int hour;
        int minute;
        int second;
        int millisecond;
    };

    /**
     * A struct with member variables describing the different parts of the ISO8601 date.
     */
    struct Iso8601DateParts {
        Iso8601DateParts(const _timelib_time&, Date_t);

        int year;
        int weekOfYear;
        int dayOfWeek;
        int hour;
        int minute;
        int second;
        int millisecond;
    };

    /**
     * A custom-deleter which destructs a timelib_time* when it goes out of scope.
     */
    struct TimelibTimeDeleter {
        TimelibTimeDeleter() = default;
        void operator()(_timelib_time* time);
    };

    explicit TimeZone(_timelib_tzinfo* tzInfo);
    explicit TimeZone(Seconds utcOffsetSeconds);
    TimeZone() = default;

    /**
     * Returns a Date_t populated with the argument values for the current timezone.
     */
    Date_t createFromDateParts(long long year,
                               long long month,
                               long long day,
                               long long hour,
                               long long minute,
                               long long second,
                               long long millisecond) const;

    /**
     * Returns a Date_t populated with the argument values for the current timezone.
     */
    Date_t createFromIso8601DateParts(long long isoYear,
                                      long long isoWeekYear,
                                      long long isoDayOfWeek,
                                      long long hour,
                                      long long minute,
                                      long long second,
                                      long long millisecond) const;
    /**
     * Returns a struct with members for each piece of the date.
     */
    DateParts dateParts(Date_t) const;

    /**
     * Returns a struct with members for each piece of the ISO8601 date.
     */
    Iso8601DateParts dateIso8601Parts(Date_t) const;

    /**
     * Returns the year according to the ISO 8601 standard. For example, Dec 31, 2014 is considered
     * part of 2014 by the ISO standard.
     */
    long long isoYear(Date_t) const;

    /**
     * Returns whether this is the zone representing UTC.
     */
    bool isUtcZone() const {
        return (_tzInfo == nullptr && !durationCount<Seconds>(_utcOffset));
    }

    /**
     * Returns whether this is a zone representing a UTC offset, like "+04:00".
     */
    bool isUtcOffsetZone() const {
        return durationCount<Seconds>(_utcOffset) != 0;
    }

    /**
     * Returns whether this is a zone representing an Olson time zone, like "Europe/London".
     */
    bool isTimeZoneIDZone() const {
        return _tzInfo != nullptr;
    }

    /**
     * Returns the weekday number, ranging from 1 (for Sunday) to 7 (for Saturday).
     */
    int dayOfWeek(Date_t) const;

    /**
     * Returns the weekday number in ISO 8601 format, ranging from 1 (for Monday) to 7 (for Sunday).
     */
    int isoDayOfWeek(Date_t) const;

    /**
     * Returns the day of the year, ranging from 1 to 366.
     */
    int dayOfYear(Date_t) const;

    /**
     * Returns the day of the month, ranging from 1 to 31.
     */
    int dayOfMonth(Date_t) const;

    /**
     * Returns the week number for a date as a number between 0 (the partial week that precedes the
     * first Sunday of the year) and 53.
     */
    int week(Date_t) const;

    /**
     * Returns the week number in ISO 8601 format, ranging from 1 to 53. Week numbers start at 1
     * with the week (Monday through Sunday) that contains the year’s first Thursday.
     */
    int isoWeek(Date_t) const;

    /**
     * Returns the number of seconds offset from UTC.
     */
    Seconds utcOffset(Date_t) const;

    /**
     * Adjusts 'timelibTime' according to this time zone definition.
     */
    void adjustTimeZone(_timelib_time* timelibTime) const;

    /**
     * Converts a date object to a string according to 'format'. 'format' can be any string literal,
     * containing 0 or more format specifiers like %Y (year) or %d (day of month). Callers must pass
     * a valid format string for 'format', i.e. one that has already been passed to
     * validateFormat(). May return a Status indicating that the date value is an unprintable range.
     */
    StatusWith<std::string> formatDate(StringData format, Date_t) const;

    /**
     * Like formatDate, except outputs to an output stream like a std::ostream or a StringBuilder.
     */
    template <typename OutputStream>
    auto outputDateWithFormat(OutputStream& os, StringData format, Date_t date) const {
        auto parts = dateParts(date);
        for (auto&& it = format.begin(); it != format.end(); ++it) {
            if (*it != '%') {
                os << *it;
                continue;
            }

            ++it;                           // next character is format modifier
            invariant(it != format.end());  // checked in validateFormat

            switch (*it) {
                case '%':  // Escaped literal %
                    os << '%';
                    break;
                case 'Y':  // Year
                {
                    if (auto status = insertPadded(os, parts.year, 4); status != Status::OK())
                        return status;
                    break;
                }
                case 'm':  // Month
                    if (auto status = insertPadded(os, parts.month, 2); status != Status::OK())
                        return status;
                    break;
                case 'd':  // Day of month
                    if (auto status = insertPadded(os, parts.dayOfMonth, 2); status != Status::OK())
                        return status;
                    break;
                case 'H':  // Hour
                    if (auto status = insertPadded(os, parts.hour, 2); status != Status::OK())
                        return status;
                    break;
                case 'M':  // Minute
                    if (auto status = insertPadded(os, parts.minute, 2); status != Status::OK())
                        return status;
                    break;
                case 'S':  // Second
                    if (auto status = insertPadded(os, parts.second, 2); status != Status::OK())
                        return status;
                    break;
                case 'L':  // Millisecond
                    if (auto status = insertPadded(os, parts.millisecond, 3);
                        status != Status::OK())
                        return status;
                    break;
                case 'j':  // Day of year
                    if (auto status = insertPadded(os, dayOfYear(date), 3); status != Status::OK())
                        return status;
                    break;
                case 'w':  // Day of week
                    if (auto status = insertPadded(os, dayOfWeek(date), 1); status != Status::OK())
                        return status;
                    break;
                case 'U':  // Week
                    if (auto status = insertPadded(os, week(date), 2); status != Status::OK())
                        return status;
                    break;
                case 'G':  // Iso year of week
                    if (auto status = insertPadded(os, isoYear(date), 4); status != Status::OK())
                        return status;
                    break;
                case 'V':  // Iso week
                    if (auto status = insertPadded(os, isoWeek(date), 2); status != Status::OK())
                        return status;
                    break;
                case 'u':  // Iso day of week
                    if (auto status = insertPadded(os, isoDayOfWeek(date), 1);
                        status != Status::OK())
                        return status;
                    break;
                case 'z':  // UTC offset as ±hhmm.
                {
                    auto offset = utcOffset(date);
                    os << ((offset.count() < 0) ? "-" : "+");  // sign
                    if (auto status = insertPadded(os, std::abs(durationCount<Hours>(offset)), 2);
                        status != Status::OK())  // hh
                        return status;
                    if (auto status =
                            insertPadded(os, std::abs(durationCount<Minutes>(offset)) % 60, 2);
                        status != Status::OK())  // mm
                        return status;
                    break;
                }
                case 'Z':  // UTC offset in minutes.
                    os << durationCount<Minutes>(utcOffset(date));
                    break;
                default:
                    // Should never happen as format is pre-validated
                    MONGO_UNREACHABLE;
            }
        }
        return Status::OK();
    }

    /**
     * Verifies that any '%' is followed by a valid format character, and that 'format' string
     * ends with an even number of '%' symbols.
     */
    static void validateToStringFormat(StringData format);
    static void validateFromStringFormat(StringData format);
    std::unique_ptr<_timelib_time, TimelibTimeDeleter> getTimelibTime(Date_t) const;

    _timelib_tzinfo* getTzInfo() const {
        return _tzInfo;
    }

    Seconds getUtcOffset() const {
        return _utcOffset;
    }

private:
    /**
     * Only works with 1 <= spaces <= 4 and 0 <= number <= 9999. If spaces is less than the digit
     * count of number we simply insert the number without padding.
     */
    template <typename OutputStream>
    static auto insertPadded(OutputStream& os, int number, int width) {
        invariant(width >= 1);
        invariant(width <= 4);

        if ((number < 0) || (number > 9999))
            return Status{ErrorCodes::Error{18537},
                          "Could not convert date to string: date component was outside "
                          "the supported range of 0-9999: "s +
                              std::to_string(number)};

        int digits = 1;

        if (number >= 1000) {
            digits = 4;
        } else if (number >= 100) {
            digits = 3;
        } else if (number >= 10) {
            digits = 2;
        }

        if (width > digits) {
            os.write("0000", width - digits);
        }
        os << number;
        return Status::OK();
    }

    // null if this TimeZone represents the default UTC time zone, or a UTC-offset time zone
    _timelib_tzinfo* _tzInfo{nullptr};

    // represents the UTC offset in seconds if _tzInfo is null and it is not 0
    Seconds _utcOffset{0};
};

/**
 * A C++ interface wrapping the third-party timelib library. A single instance of this class can be
 * accessed via the global service context.
 */
class TimeZoneDatabase {
    TimeZoneDatabase(const TimeZoneDatabase&) = delete;
    TimeZoneDatabase& operator=(const TimeZoneDatabase&) = delete;

public:
    /**
     * A custom-deleter which deletes 'timeZoneDatabase' if it is not the builtin time zone
     * database, which has static lifetime and should not be freed.
     */
    struct TimeZoneDBDeleter {
        TimeZoneDBDeleter() = default;
        void operator()(_timelib_tzdb* timeZoneDatabase);
    };

    /**
     * A custom-deleter which destructs a timelib_error_container* when it goes out of scope.
     */
    struct TimelibErrorContainerDeleter {
        TimelibErrorContainerDeleter() = default;
        void operator()(_timelib_error_container* errorContainer);
    };

    /**
     * Returns the TimeZoneDatabase object associated with the specified service context or nullptr
     * if none exists.
     */
    static const TimeZoneDatabase* get(ServiceContext* serviceContext);

    /**
     * Sets the TimeZoneDatabase object associated with the specified service context.
     */
    static void set(ServiceContext* serviceContext,
                    std::unique_ptr<TimeZoneDatabase> timeZoneDatabase);

    /**
     * Constructs a Date_t from a string description of a date, with an optional format specifier
     * string.
     *
     * 'dateString' may contain time zone information if the information is simply an offset from
     * UTC, in which case the returned Date_t will be adjusted accordingly.
     *
     * The supported format specifiers for the 'format' string are listed in
     * kDateFromStringFormatMap.
     *
     * Throws a AssertionException if any of the following occur:
     *  * The string cannot be parsed into a date.
     *  * The string specifies a time zone that is not simply an offset from UTC, like
     *    in the string "July 4, 2017 America/New_York".
     *  * 'tz' is provided, but 'dateString' specifies a timezone, like 'Z' in the
     *    string '2017-07-04T00:00:00Z'.
     *  * 'tz' is provided, but 'dateString' specifies an offset from UTC, like '-0400'
     *    in the string '2017-07-04 -0400'.
     *  * The string does not match the 'format' specifier.
     */
    Date_t fromString(StringData dateString,
                      const TimeZone& tz,
                      boost::optional<StringData> format = boost::none) const;

    /**
     * Returns a TimeZone object representing the UTC time zone.
     */
    static TimeZone utcZone();

    /**
     * Returns a boolean based on if 'timeZoneId' represents a valid timezone.
     */
    bool isTimeZoneIdentifier(StringData timeZoneId) const;

    /**
     * Returns a TimeZone object representing the zone given by 'timeZoneId', or throws an exception
     * if it is not a recognized time zone.
     */
    TimeZone getTimeZone(StringData timeZoneId) const;

    /**
     * Creates a TimeZoneDatabase object with time zone data loaded from timelib's built-in timezone
     * rules.
     */
    TimeZoneDatabase();

    /**
     * Creates a TimeZoneDatabase object using time zone rules given by 'timeZoneDatabase'.
     */
    TimeZoneDatabase(std::unique_ptr<_timelib_tzdb, TimeZoneDBDeleter> timeZoneDatabase);

    std::vector<std::string> getTimeZoneStrings() const;

    std::string toString() const;

private:
    struct TimelibTZInfoDeleter {
        void operator()(_timelib_tzinfo* tzInfo);
    };

    /**
     * Populates '_timeZones' with parsed time zone rules for each timezone specified by
     * 'timeZoneDatabase'.
     */
    void loadTimeZoneInfo(std::unique_ptr<_timelib_tzdb, TimeZoneDBDeleter> timeZoneDatabase);

    /**
     * Tries to find a UTC offset in 'offsetSpec' in an ISO8601 format (±HH, ±HHMM, or ±HH:MM) and
     * returns it as an offset to UTC in seconds.
     */
    boost::optional<Seconds> parseUtcOffset(StringData offsetSpec) const;

    // A map from the time zone name to the struct describing the timezone. These are pre-populated
    // at startup to avoid reading the source files repeatedly.
    StringMap<TimeZone> _timeZones;

    // The timelib structure which provides timezone information.
    std::unique_ptr<_timelib_tzdb, TimeZoneDBDeleter> _timeZoneDatabase;

    // The list of pre-load _timelib_tzinfo objects.
    std::vector<std::unique_ptr<_timelib_tzinfo, TimelibTZInfoDeleter>> _timeZoneInfos;
};

/**
 * Parses a string representation of an enumerator of TimeUnit type 'unitName' into a value of type
 * TimeUnit. Throws an exception with error code ErrorCodes::FailedToParse when passed an invalid
 * name.
 */
TimeUnit parseTimeUnit(StringData unitName);

/**
 * Returns true if 'unitName' is a valid time unit, meaning that it can be parsed by the
 * 'parseTimeUnit()' function into one of the units represented by the 'TimeUnit' enum. Otherwise
 * returns 'false'.
 */
bool isValidTimeUnit(StringData unitName);

/**
 * Inverse of parseTimeUnit.
 */
StringData serializeTimeUnit(TimeUnit unit);

/**
 * Parses a string 'dayOfWeek' to a DayOfWeek value. Supported day of week representations are
 * case-insensitive full words or three letter abbreviations - for example, sunday, Sun. Throws an
 * exception with error code ErrorCodes::FailedToParse when passed an invalid value.
 */
DayOfWeek parseDayOfWeek(StringData dayOfWeek);

/**
 * Returns true if 'dayOfWeek' is a valid representation of a day of a week, meaning that it can be
 * parsed by the 'parseDayOfWeek()' function into one of the days represented by the 'DayOfWeek'
 * enum. Otherwise returns 'false'.
 */
bool isValidDayOfWeek(StringData dayOfWeek);

/**
 * A custom-deleter which destructs a timelib_rel_time* when it goes out of scope.
 */
struct TimelibRelTimeDeleter {
    TimelibRelTimeDeleter() = default;
    void operator()(_timelib_rel_time* relTime);
};

/**
 * Creates and sets a timelib_rel_time structure representing a time interval
 * of 'amount' number of time 'units'.
 */
std::unique_ptr<_timelib_rel_time, TimelibRelTimeDeleter> getTimelibRelTime(TimeUnit unit,
                                                                            long long amount);

/**
 * Determines the number of upper boundaries of time intervals crossed when moving from time instant
 * 'startDate' to time instant 'endDate' in time zone 'timezone'. The time intervals are of length
 * equal to one 'unit' and aligned so that the lower/upper bound is located in time axis at instant
 * n*'unit', where n is an integer.
 *
 * If 'endDate' < 'startDate', then the returned number of crossed boundaries is negative.
 *
 * For 'unit' values 'hour' and smaller, when there is a transition from Daylight Saving Time to
 * standard time the function behaves as if standard time intervals overlap Daylight Saving Time
 * intervals. When there is a transition from standard time to Daylight Saving Time the function
 * behaves as if the last interval in standard time is longer by one hour.
 *
 * An example: if startDate=2011-01-31T00:00:00 (in 'timezone'), endDate=2011-02-01T00:00:00 (in
 * 'timezone'), unit='month', then the function returns 1, since a month boundary at
 * 2011-02-01T00:00:00 was crossed.
 *
 * The function operates in the Gregorian calendar. The function does not account for leap seconds.
 * For time instants before year 1583 the proleptic Gregorian calendar is used.
 *
 * startDate - starting time instant in UTC time zone.
 * endDate - ending time instant in UTC time zone.
 * unit - length of time intervals.
 * timezone - determines the timezone used for counting the boundaries as well as Daylight Saving
 * Time rules.
 * startOfWeek - the first day of a week used, to determine week boundaries when 'unit' is
 * TimeUnit::week. Otherwise, this parameter is ignored.
 */
long long dateDiff(Date_t startDate,
                   Date_t endDate,
                   TimeUnit unit,
                   const TimeZone& timezone,
                   DayOfWeek startOfWeek = kStartOfWeekDefault);

/**
 * Add time interval to a date. The interval duration is given in 'amount' number of 'units'.
 * The amount can be a negative number in which case the interval is subtracted from the date.
 * The result date is always in UTC.
 *
 * startDate - starting time instant
 * unit - length of time intervals, defined in the TimeUnit enumeration
 * amount - the amount of time units to be added
 * timezone - the timezone in which the start date is interpreted
 */
Date_t dateAdd(Date_t date, TimeUnit unit, long long amount, const TimeZone& timezone);

/**
 * Convert (approximately) a TimeUnit to a number of milliseconds.
 *
 * The answer is approximate because TimeUnit represents an amount of calendar time:
 * for example, some calendar days are 23 or 25 hours long due to daylight savings time.
 * This function assumes everything is "typical": days are 24 hours, minutes are 60 seconds.
 *
 * Large time units, 'month' or longer, are so variable that we don't try to pick a value: we
 * return a non-OK Status.
 */
StatusWith<long long> timeUnitTypicalMilliseconds(TimeUnit unit);

/**
 * Returns the lower bound of a bin the 'date' falls into in the time axis, or, in other words,
 * truncates the 'date' value. Bins are (1) uniformly spaced (in time unit sense); (2) do not
 * overlap; (3) bin size is 'binSize' 'unit''s; (4) defined in a timezone specified by 'timezone';
 * (5) one bin has a lower bound at 2000-01-01T00:00:00.000 (also called a reference point) in the
 * specified timezone, except for "week" time unit when the given 'startOfWeek' does not coincide
 * with the first of January, 2000. For weeks [exception to (5)] the bin is aligned to the earliest
 * first day of the week after the first of January, 2000.
 *
 * binSize - must be larger than 0.
 * timezone - determines boundaries of the bins as well as Daylight Saving Time rules.
 * startOfWeek - the first day of a week used to determine week boundaries when 'unit' is
 * TimeUnit::week. Otherwise, this parameter is ignored.
 */
Date_t truncateDate(Date_t date,
                    TimeUnit unit,
                    unsigned long long binSize,
                    const TimeZone& timezone,
                    DayOfWeek startOfWeek = kStartOfWeekDefault);
}  // namespace mongo
