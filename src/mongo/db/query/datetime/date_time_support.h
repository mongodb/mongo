/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/service_context.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

struct timelib_time;
struct _timelib_tzdb;
struct timelib_tzinfo;

namespace mongo {

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
        DateParts(const timelib_time&, Date_t);

        int year;
        int month;
        int dayOfMonth;
        int hour;
        int minute;
        int second;
        int millisecond;
    };

    explicit TimeZone(timelib_tzinfo* tzInfo);
    TimeZone() = default;

    /**
     * Returns a struct with members for each piece of the date.
     */
    DateParts dateParts(Date_t) const;

    /**
     * Returns the year according to the ISO 8601 standard. For example, Dec 31, 2014 is considered
     * part of 2014 by the ISO standard.
     */
    long long isoYear(Date_t) const;

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
     * Converts a date object to a string according to 'format'. 'format' can be any string literal,
     * containing 0 or more format specifiers like %Y (year) or %d (day of month). Callers must pass
     * a valid format string for 'format', i.e. one that has already been passed to
     * validateFormat().
     */
    std::string formatDate(StringData format, Date_t) const;

    /**
     * Like formatDate, except outputs to an output stream like a std::ostream or a StringBuilder.
     */
    template <typename OutputStream>
    void outputDateWithFormat(OutputStream& os, StringData format, Date_t date) const {
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
                    auto year = parts.year;
                    uassert(18537,
                            str::stream() << "$dateToString is only defined on year 0-9999,"
                                          << " tried to use year "
                                          << year,
                            (year >= 0) && (year <= 9999));
                    insertPadded(os, year, 4);
                    break;
                }
                case 'm':  // Month
                    insertPadded(os, parts.month, 2);
                    break;
                case 'd':  // Day of month
                    insertPadded(os, parts.dayOfMonth, 2);
                    break;
                case 'H':  // Hour
                    insertPadded(os, parts.hour, 2);
                    break;
                case 'M':  // Minute
                    insertPadded(os, parts.minute, 2);
                    break;
                case 'S':  // Second
                    insertPadded(os, parts.second, 2);
                    break;
                case 'L':  // Millisecond
                    insertPadded(os, parts.millisecond, 3);
                    break;
                case 'j':  // Day of year
                    insertPadded(os, dayOfYear(date), 3);
                    break;
                case 'w':  // Day of week
                    insertPadded(os, dayOfWeek(date), 1);
                    break;
                case 'U':  // Week
                    insertPadded(os, week(date), 2);
                    break;
                case 'G':  // Iso year of week
                    insertPadded(os, isoYear(date), 4);
                    break;
                case 'V':  // Iso week
                    insertPadded(os, isoWeek(date), 2);
                    break;
                case 'u':  // Iso day of week
                    insertPadded(os, isoDayOfWeek(date), 1);
                    break;
                default:
                    // Should never happen as format is pre-validated
                    invariant(false);
            }
        }
    }

    /**
     * Verifies that any '%' is followed by a valid format character, and that 'format' string
     * ends with an even number of '%' symbols
     */
    static void validateFormat(StringData format);

private:
    timelib_time getTimelibTime(Date_t) const;

    /**
     * Only works with 1 <= spaces <= 4 and 0 <= number <= 9999. If spaces is less than the digit
     * count of number we simply insert the number without padding.
     */
    template <typename OutputStream>
    void insertPadded(OutputStream& os, int number, int width) const {
        invariant(width >= 1);
        invariant(width <= 4);
        invariant(number >= 0);
        invariant(number <= 9999);

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
    }

    struct TimelibTZInfoDeleter {
        void operator()(timelib_tzinfo* tzInfo);
    };

    // null if this TimeZone represents the default UTC TimeZone.
    std::shared_ptr<timelib_tzinfo> _tzInfo;
};

/**
 * A C++ interface wrapping the third-party timelib library. A single instance of this class can be
 * accessed via the global service context.
 */
class TimeZoneDatabase {
    MONGO_DISALLOW_COPYING(TimeZoneDatabase);

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
     * Returns the TimeZoneDatabase object associated with the specified service context.
     */
    static const TimeZoneDatabase* get(ServiceContext* serviceContext);

    /**
     * Sets the TimeZoneDatabase object associated with the specified service context.
     */
    static void set(ServiceContext* serviceContext,
                    std::unique_ptr<TimeZoneDatabase> timeZoneDatabase);

    /**
     * Returns a TimeZone object representing the UTC time zone.
     */
    static TimeZone utcZone();

    /**
     * Creates a TimeZoneDatabase object with time zone data loaded from timelib's built-in timezone
     * rules.
     */
    TimeZoneDatabase();

    /**
     * Creates a TimeZoneDatabase object using time zone rules given by 'timeZoneDatabase'.
     */
    TimeZoneDatabase(std::unique_ptr<_timelib_tzdb, TimeZoneDBDeleter> timeZoneDatabase);

private:
    /**
     * Populates '_timeZones' with parsed time zone rules for each timezone specified by
     * 'timeZoneDatabase'.
     */
    void loadTimeZoneInfo(std::unique_ptr<_timelib_tzdb, TimeZoneDBDeleter> timeZoneDatabase);

    // A map from the time zone name to the struct describing the timezone. These are pre-populated
    // at startup to avoid reading the source files repeatedly.
    StringMap<TimeZone> _timeZones;

    // The timelib structure which provides timezone information.
    std::unique_ptr<_timelib_tzdb, TimeZoneDBDeleter> _timeZoneDatabase;
};

}  // namespace mongo
