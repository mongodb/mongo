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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <limits>
#include <memory>
#include <timelib.h>

#include "mongo/db/query/datetime/date_time_support.h"

#include "mongo/base/init.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

const auto getTimeZoneDatabase =
    ServiceContext::declareDecoration<std::unique_ptr<TimeZoneDatabase>>();

std::unique_ptr<_timelib_time, TimeZone::TimelibTimeDeleter> createTimelibTime() {
    return std::unique_ptr<_timelib_time, TimeZone::TimelibTimeDeleter>(timelib_time_ctor());
}

// Converts a date to a number of seconds, being careful to round appropriately for negative numbers
// of seconds.
long long seconds(Date_t date) {
    // We want the division below to truncate toward -inf rather than 0
    // eg Dec 31, 1969 23:59:58.001 should be -2 seconds rather than -1
    // This is needed to get the correct values from coerceToTM
    constexpr auto needsRounding = -1999 / 1000 != -2;  // This is implementaiton defined.
    if (auto millis = date.toMillisSinceEpoch(); millis < 0 && millis % 1000 != 0 && needsRounding)
        return durationCount<Seconds>(Milliseconds(millis)) - 1ll;
    else
        return durationCount<Seconds>(Milliseconds(millis));
}

//
// Format specifier map when parsing a date from a string with a required format.
//
const std::vector<timelib_format_specifier> kDateFromStringFormatMap = {
    {'d', TIMELIB_FORMAT_DAY_TWO_DIGIT},
    {'G', TIMELIB_FORMAT_YEAR_ISO},
    {'H', TIMELIB_FORMAT_HOUR_TWO_DIGIT_24_MAX},
    {'L', TIMELIB_FORMAT_MILLISECOND_THREE_DIGIT},
    {'m', TIMELIB_FORMAT_MONTH_TWO_DIGIT},
    {'M', TIMELIB_FORMAT_MINUTE_TWO_DIGIT},
    {'S', TIMELIB_FORMAT_SECOND_TWO_DIGIT},
    {'u', TIMELIB_FORMAT_DAY_OF_WEEK_ISO},
    {'V', TIMELIB_FORMAT_WEEK_OF_YEAR_ISO},
    {'Y', TIMELIB_FORMAT_YEAR_FOUR_DIGIT},
    {'z', TIMELIB_FORMAT_TIMEZONE_OFFSET},
    {'Z', TIMELIB_FORMAT_TIMEZONE_OFFSET_MINUTES},
    {'\0', TIMELIB_FORMAT_END}};

//
// Format specifier map when converting a date to a string.
//
const std::vector<timelib_format_specifier> kDateToStringFormatMap = {
    {'d', TIMELIB_FORMAT_DAY_TWO_DIGIT},
    {'G', TIMELIB_FORMAT_YEAR_ISO},
    {'H', TIMELIB_FORMAT_HOUR_TWO_DIGIT_24_MAX},
    {'j', TIMELIB_FORMAT_DAY_OF_YEAR},
    {'L', TIMELIB_FORMAT_MILLISECOND_THREE_DIGIT},
    {'m', TIMELIB_FORMAT_MONTH_TWO_DIGIT},
    {'M', TIMELIB_FORMAT_MINUTE_TWO_DIGIT},
    {'S', TIMELIB_FORMAT_SECOND_TWO_DIGIT},
    {'w', TIMELIB_FORMAT_DAY_OF_WEEK},
    {'u', TIMELIB_FORMAT_DAY_OF_WEEK_ISO},
    {'U', TIMELIB_FORMAT_WEEK_OF_YEAR},
    {'V', TIMELIB_FORMAT_WEEK_OF_YEAR_ISO},
    {'Y', TIMELIB_FORMAT_YEAR_FOUR_DIGIT},
    {'z', TIMELIB_FORMAT_TIMEZONE_OFFSET},
    {'Z', TIMELIB_FORMAT_TIMEZONE_OFFSET_MINUTES}};


// Verifies that any '%' is followed by a valid format character as indicated by 'allowedFormats',
// and that the 'format' string ends with an even number of '%' symbols.
void validateFormat(StringData format,
                    const std::vector<timelib_format_specifier>& allowedFormats) {
    for (auto it = format.begin(); it != format.end(); ++it) {
        if (*it != '%') {
            continue;
        }

        ++it;  // next character must be format modifier
        uassert(18535, "Unmatched '%' at end of format string", it != format.end());

        const bool validSpecifier = (*it == '%') ||
            std::find_if(allowedFormats.begin(), allowedFormats.end(), [=](const auto& format) {
                return format.specifier == *it;
            }) != allowedFormats.end();
        uassert(18536,
                str::stream() << "Invalid format character '%" << *it << "' in format string",
                validSpecifier);
    }
}

}  // namespace

const TimeZoneDatabase* TimeZoneDatabase::get(ServiceContext* serviceContext) {
    return getTimeZoneDatabase(serviceContext).get();
}

void TimeZoneDatabase::set(ServiceContext* serviceContext,
                           std::unique_ptr<TimeZoneDatabase> dateTimeSupport) {
    getTimeZoneDatabase(serviceContext) = std::move(dateTimeSupport);
}

TimeZoneDatabase::TimeZoneDatabase() {
    loadTimeZoneInfo({const_cast<timelib_tzdb*>(timelib_builtin_db()), TimeZoneDBDeleter()});
}

TimeZoneDatabase::TimeZoneDatabase(
    std::unique_ptr<timelib_tzdb, TimeZoneDBDeleter> timeZoneDatabase) {
    loadTimeZoneInfo(std::move(timeZoneDatabase));
}

void TimeZoneDatabase::TimeZoneDBDeleter::operator()(timelib_tzdb* timeZoneDatabase) {
    if (timeZoneDatabase != timelib_builtin_db()) {
        timelib_zoneinfo_dtor(timeZoneDatabase);
    }
}

void TimeZoneDatabase::TimelibErrorContainerDeleter::operator()(
    timelib_error_container* errorContainer) {
    timelib_error_container_dtor(errorContainer);
}

void TimeZoneDatabase::loadTimeZoneInfo(
    std::unique_ptr<timelib_tzdb, TimeZoneDBDeleter> timeZoneDatabase) {
    invariant(timeZoneDatabase);
    _timeZoneDatabase = std::move(timeZoneDatabase);
    int nTimeZones;
    auto timezone_identifier_list =
        timelib_timezone_identifiers_list(_timeZoneDatabase.get(), &nTimeZones);
    for (int i = 0; i < nTimeZones; ++i) {
        auto entry = timezone_identifier_list[i];
        int errorCode = TIMELIB_ERROR_NO_ERROR;
        auto tzInfo = timelib_parse_tzfile(entry.id, _timeZoneDatabase.get(), &errorCode);
        if (!tzInfo) {
            invariant(errorCode != TIMELIB_ERROR_NO_ERROR);
            fassertFailedWithStatusNoTrace(
                40475,
                {ErrorCodes::FailedToParse,
                 str::stream() << "failed to parse time zone file for time zone identifier \""
                               << entry.id << "\": " << timelib_get_error_message(errorCode)});
        }

        invariant(errorCode == TIMELIB_ERROR_NO_ERROR);
        _timeZones[entry.id] = TimeZone{tzInfo};
    }
}

TimeZone TimeZoneDatabase::utcZone() {
    return TimeZone{nullptr};
}

static timelib_tzinfo* timezonedatabase_gettzinfowrapper(char* tz_id,
                                                         const _timelib_tzdb* db,
                                                         int* error) {
    return nullptr;
}

Date_t TimeZoneDatabase::fromString(StringData dateString,
                                    const TimeZone& tz,
                                    boost::optional<StringData> format) const {
    std::unique_ptr<timelib_error_container, TimeZoneDatabase::TimelibErrorContainerDeleter>
        errors{};
    timelib_error_container* rawErrors;

    timelib_time* rawTime;
    if (!format) {
        // Without a format, timelib will attempt to parse a string as best as it can, accepting a
        // variety of formats.
        rawTime = timelib_strtotime(const_cast<char*>(dateString.rawData()),
                                    dateString.size(),
                                    &rawErrors,
                                    _timeZoneDatabase.get(),
                                    timezonedatabase_gettzinfowrapper);
    } else {
        const timelib_format_config dateFormatConfig = {
            &kDateFromStringFormatMap[0],
            // Format specifiers must be prefixed by '%'.
            '%'};
        rawTime = timelib_parse_from_format_with_map(const_cast<char*>(format->rawData()),
                                                     const_cast<char*>(dateString.rawData()),
                                                     dateString.size(),
                                                     &rawErrors,
                                                     _timeZoneDatabase.get(),
                                                     timezonedatabase_gettzinfowrapper,
                                                     &dateFormatConfig);
    }
    std::unique_ptr<timelib_time, TimeZone::TimelibTimeDeleter> parsedTime(rawTime);

    errors.reset(rawErrors);

    // If the parsed string has a warning or error, throw an error.
    if (errors->warning_count || errors->error_count) {
        StringBuilder sb;

        sb << "Error parsing date string '" << dateString << "'";

        for (int i = 0; i < errors->error_count; ++i) {
            auto error = errors->error_messages[i];

            sb << "; " << error.position << ": ";
            // We need to override the error message for unknown time zone identifiers, as we never
            // make them available. We also change the error code to signal this is a different
            // error than a normal parse error.
            if (error.error_code == TIMELIB_ERR_TZID_NOT_FOUND) {
                sb << "passing a time zone identifier as part of the string is not allowed";
            } else {
                sb << error.message;
            }
            sb << " '" << error.character << "'";
        }

        for (int i = 0; i < errors->warning_count; ++i) {
            sb << "; " << errors->warning_messages[i].position << ": "
               << errors->warning_messages[i].message << " '"
               << errors->warning_messages[i].character << "'";
        }

        uasserted(ErrorCodes::ConversionFailure, sb.str());
    }

    // If the time portion is fully missing, initialize to 0. This allows for the '%Y-%m-%d' format
    // to be passed too.
    if (parsedTime->h == TIMELIB_UNSET && parsedTime->i == TIMELIB_UNSET &&
        parsedTime->s == TIMELIB_UNSET) {
        parsedTime->h = parsedTime->i = parsedTime->s = parsedTime->us = 0;
    }

    if (parsedTime->y == TIMELIB_UNSET || parsedTime->m == TIMELIB_UNSET ||
        parsedTime->d == TIMELIB_UNSET || parsedTime->h == TIMELIB_UNSET ||
        parsedTime->i == TIMELIB_UNSET || parsedTime->s == TIMELIB_UNSET) {
        uasserted(ErrorCodes::ConversionFailure,
                  str::stream()
                      << "an incomplete date/time string has been found, with elements missing: \""
                      << dateString << "\"");
    }

    if (!tz.isUtcZone()) {
        switch (parsedTime->zone_type) {
            case 0:
                // Do nothing, as this indicates there is no associated time zone information.
                break;
            case 1:
                uasserted(ErrorCodes::ConversionFailure,
                          "you cannot pass in a date/time string with GMT "
                          "offset together with a timezone argument");
                break;
            case 2:
                uasserted(
                    ErrorCodes::ConversionFailure,
                    str::stream()
                        << "you cannot pass in a date/time string with time zone information ('"
                        << parsedTime.get()->tz_abbr << "') together with a timezone argument");
                break;
            default:  // should technically not be possible to reach
                uasserted(ErrorCodes::ConversionFailure,
                          "you cannot pass in a date/time string with "
                          "time zone information and a timezone argument "
                          "at the same time");
                break;
        }
    }

    tz.adjustTimeZone(parsedTime.get());

    return Date_t::fromMillisSinceEpoch(
        durationCount<Milliseconds>(Seconds(parsedTime->sse) + Microseconds(parsedTime->us)));
}

boost::optional<Seconds> TimeZoneDatabase::parseUtcOffset(StringData offsetSpec) const {
    // Needs to start with either '+' or '-'.
    if (!offsetSpec.empty() && (offsetSpec[0] == '+' || offsetSpec[0] == '-')) {
        auto bias = offsetSpec[0] == '+' ? 1 : -1;

        // ±HH
        if (offsetSpec.size() == 3 && ctype::isDigit(offsetSpec[1]) &&
            ctype::isDigit(offsetSpec[2])) {
            int offset;
            if (NumberParser().base(10)(offsetSpec.substr(1, 2), &offset).isOK()) {
                return duration_cast<Seconds>(Hours(bias * offset));
            }
            return boost::none;
        }

        // ±HHMM
        if (offsetSpec.size() == 5 && ctype::isDigit(offsetSpec[1]) &&
            ctype::isDigit(offsetSpec[2]) && ctype::isDigit(offsetSpec[3]) &&
            ctype::isDigit(offsetSpec[4])) {
            int offset;
            if (NumberParser().base(10)(offsetSpec.substr(1, 4), &offset).isOK()) {
                return duration_cast<Seconds>(Hours(bias * (offset / 100L)) +
                                              Minutes(bias * (offset % 100)));
            }
            return boost::none;
        }

        // ±HH:MM
        if (offsetSpec.size() == 6 && ctype::isDigit(offsetSpec[1]) &&
            ctype::isDigit(offsetSpec[2]) && offsetSpec[3] == ':' &&
            ctype::isDigit(offsetSpec[4]) && ctype::isDigit(offsetSpec[5])) {
            int hourOffset, minuteOffset;
            if (!NumberParser().base(10)(offsetSpec.substr(1, 2), &hourOffset).isOK()) {
                return boost::none;
            }
            if (!NumberParser().base(10)(offsetSpec.substr(4, 2), &minuteOffset).isOK()) {
                return boost::none;
            }
            return duration_cast<Seconds>(Hours(bias * hourOffset) + Minutes(bias * minuteOffset));
        }
    }
    return boost::none;
}

bool TimeZoneDatabase::isTimeZoneIdentifier(StringData timeZoneId) const {
    return (_timeZones.find(timeZoneId) != _timeZones.end()) ||
        static_cast<bool>(parseUtcOffset(timeZoneId));
}

TimeZone TimeZoneDatabase::getTimeZone(StringData timeZoneId) const {
    auto tz = _timeZones.find(timeZoneId);
    if (tz != _timeZones.end()) {
        return tz->second;
    }

    // Check for a possible UTC offset
    if (auto UtcOffset = parseUtcOffset(timeZoneId)) {
        return TimeZone(*UtcOffset);
    }

    uasserted(40485,
              str::stream() << "unrecognized time zone identifier: \"" << timeZoneId << "\"");
}

std::vector<std::string> TimeZoneDatabase::getTimeZoneStrings() const {
    std::vector<std::string> timeZoneStrings = {};

    for (auto const& timezone : _timeZones) {
        timeZoneStrings.push_back(timezone.first);
    }

    return timeZoneStrings;
}

void TimeZone::adjustTimeZone(timelib_time* timelibTime) const {
    if (isTimeZoneIDZone()) {
        timelib_set_timezone(timelibTime, _tzInfo.get());
    } else if (isUtcOffsetZone()) {
        timelib_set_timezone_from_offset(timelibTime, durationCount<Seconds>(_utcOffset));
    }
    timelib_update_ts(timelibTime, nullptr);
    timelib_update_from_sse(timelibTime);
}

Date_t TimeZone::createFromDateParts(long long year,
                                     long long month,
                                     long long day,
                                     long long hour,
                                     long long minute,
                                     long long second,
                                     long long millisecond) const {
    auto newTime = createTimelibTime();

    newTime->y = year;
    newTime->m = month;
    newTime->d = day;
    newTime->h = hour;
    newTime->i = minute;
    newTime->s = second;
    newTime->us = durationCount<Microseconds>(Milliseconds(millisecond));

    adjustTimeZone(newTime.get());

    auto returnValue = Date_t::fromMillisSinceEpoch(
        durationCount<Milliseconds>(Seconds(newTime->sse) + Microseconds(newTime->us)));

    return returnValue;
}

Date_t TimeZone::createFromIso8601DateParts(long long isoYear,
                                            long long isoWeekYear,
                                            long long isoDayOfWeek,
                                            long long hour,
                                            long long minute,
                                            long long second,
                                            long long millisecond) const {
    auto newTime = createTimelibTime();

    timelib_date_from_isodate(
        isoYear, isoWeekYear, isoDayOfWeek, &newTime->y, &newTime->m, &newTime->d);
    newTime->h = hour;
    newTime->i = minute;
    newTime->s = second;
    newTime->us = durationCount<Microseconds>(Milliseconds(millisecond));

    adjustTimeZone(newTime.get());

    auto returnValue = Date_t::fromMillisSinceEpoch(
        durationCount<Milliseconds>(Seconds(newTime->sse) + Microseconds(newTime->us)));

    return returnValue;
}

TimeZone::DateParts::DateParts(const timelib_time& timelib_time, Date_t date)
    : year(timelib_time.y),
      month(timelib_time.m),
      dayOfMonth(timelib_time.d),
      hour(timelib_time.h),
      minute(timelib_time.i),
      second(timelib_time.s) {
    const int ms = date.toMillisSinceEpoch() % 1000LL;
    // Add 1000 since dates before 1970 would have negative milliseconds.
    millisecond = ms >= 0 ? ms : 1000 + ms;
}

TimeZone::Iso8601DateParts::Iso8601DateParts(const timelib_time& timelib_time, Date_t date)
    : hour(timelib_time.h), minute(timelib_time.i), second(timelib_time.s) {

    timelib_sll tmpIsoYear, tmpIsoWeekOfYear, tmpIsoDayOfWeek;

    timelib_isodate_from_date(timelib_time.y,
                              timelib_time.m,
                              timelib_time.d,
                              &tmpIsoYear,
                              &tmpIsoWeekOfYear,
                              &tmpIsoDayOfWeek);

    year = static_cast<int>(tmpIsoYear);
    weekOfYear = static_cast<int>(tmpIsoWeekOfYear);
    dayOfWeek = static_cast<int>(tmpIsoDayOfWeek);

    const int ms = date.toMillisSinceEpoch() % 1000LL;
    // Add 1000 since dates before 1970 would have negative milliseconds.
    millisecond = ms >= 0 ? ms : 1000 + ms;
}


void TimeZone::TimelibTZInfoDeleter::operator()(timelib_tzinfo* tzInfo) {
    if (tzInfo) {
        timelib_tzinfo_dtor(tzInfo);
    }
}

TimeZone::TimeZone(timelib_tzinfo* tzInfo)
    : _tzInfo(tzInfo, TimelibTZInfoDeleter()), _utcOffset(0) {}

TimeZone::TimeZone(Seconds utcOffsetSeconds) : _tzInfo(nullptr), _utcOffset(utcOffsetSeconds) {}

void TimeZone::TimelibTimeDeleter::operator()(timelib_time* time) {
    timelib_time_dtor(time);
}

std::unique_ptr<timelib_time, TimeZone::TimelibTimeDeleter> TimeZone::getTimelibTime(
    Date_t date) const {
    auto time = createTimelibTime();

    timelib_unixtime2gmt(time.get(), seconds(date));
    adjustTimeZone(time.get());
    timelib_unixtime2local(time.get(), seconds(date));

    return time;
}

TimeZone::Iso8601DateParts TimeZone::dateIso8601Parts(Date_t date) const {
    auto time = getTimelibTime(date);
    return Iso8601DateParts(*time, date);
}

TimeZone::DateParts TimeZone::dateParts(Date_t date) const {
    auto time = getTimelibTime(date);
    return DateParts(*time, date);
}

int TimeZone::dayOfWeek(Date_t date) const {
    auto time = getTimelibTime(date);
    // timelib_day_of_week() returns a number in the range [0,6], we want [1,7], so add one.
    return timelib_day_of_week(time->y, time->m, time->d) + 1;
}

int TimeZone::week(Date_t date) const {
    int weekDay = dayOfWeek(date);
    int yearDay = dayOfYear(date);
    int prevSundayDayOfYear = yearDay - weekDay;        // may be negative
    int nextSundayDayOfYear = prevSundayDayOfYear + 7;  // must be positive

    // Return the zero based index of the week of the next sunday, equal to the one based index
    // of the week of the previous sunday, which is to be returned.
    int nextSundayWeek = nextSundayDayOfYear / 7;

    return nextSundayWeek;
}

int TimeZone::dayOfYear(Date_t date) const {
    auto time = getTimelibTime(date);
    // timelib_day_of_year() returns a number in the range [0,365], we want [1,366], so add one.
    return timelib_day_of_year(time->y, time->m, time->d) + 1;
}

int TimeZone::dayOfMonth(Date_t date) const {
    auto time = getTimelibTime(date);
    return time->d;
}

int TimeZone::isoDayOfWeek(Date_t date) const {
    auto time = getTimelibTime(date);
    return timelib_iso_day_of_week(time->y, time->m, time->d);
}

int TimeZone::isoWeek(Date_t date) const {
    auto time = getTimelibTime(date);
    long long isoWeek;
    long long isoYear;
    timelib_isoweek_from_date(time->y, time->m, time->d, &isoWeek, &isoYear);
    return isoWeek;
}

long long TimeZone::isoYear(Date_t date) const {
    auto time = getTimelibTime(date);
    long long isoWeek;
    long long isoYear;
    timelib_isoweek_from_date(time->y, time->m, time->d, &isoWeek, &isoYear);
    return isoYear;
}

Seconds TimeZone::utcOffset(Date_t date) const {
    auto time = getTimelibTime(date);

    return Seconds(time->z);
}

void TimeZone::validateToStringFormat(StringData format) {
    return validateFormat(format, kDateToStringFormatMap);
}

void TimeZone::validateFromStringFormat(StringData format) {
    return validateFormat(format, kDateFromStringFormatMap);
}

StatusWith<std::string> TimeZone::formatDate(StringData format, Date_t date) const {
    StringBuilder formatted;
    if (auto status = outputDateWithFormat(formatted, format, date); status != Status::OK())
        return status;
    else
        return formatted.str();
}

namespace {
auto const kMonthsInOneYear = 12LL;
auto const kDaysInNonLeapYear = 365LL;
auto const kHoursPerDay = 24LL;
auto const kMinutesPerHour = 60LL;
auto const kSecondsPerMinute = 60LL;
auto const kDaysPerWeek = 7LL;
auto const kQuartersPerYear = 4LL;
auto const kQuarterLengthInMonths = 3LL;
auto const kLeapYearReferencePoint = -1000000000L;

/**
 * Determines a number of leap years in a year range (leap year reference point; 'year'].
 */
inline long leapYearsSinceReferencePoint(long year) {
    // Count a number of leap years that happened since the reference point, where a leap year is
    // when year%4==0, excluding years when year%100==0, except when year%400==0.
    auto yearsSinceReferencePoint = year - kLeapYearReferencePoint;
    return yearsSinceReferencePoint / 4 - yearsSinceReferencePoint / 100 +
        yearsSinceReferencePoint / 400;
}

/**
 * Sums the number of days in the Gregorian calendar in years: 'startYear',
 * 'startYear'+1, .., 'endYear'-1.
 */
inline long long daysBetweenYears(long startYear, long endYear) {
    return leapYearsSinceReferencePoint(endYear - 1) - leapYearsSinceReferencePoint(startYear - 1) +
        (endYear - startYear) * kDaysInNonLeapYear;
}

/**
 * Determines a correction needed in number of hours when calculating passed hours between two time
 * instants 'startInstant' and 'endInstant' due to the Daylight Savings Time. Returns 0, if both
 * time instants 'startInstant' and 'endInstant' are either in Standard Time (ST) or in Daylight
 * Saving Time (DST); returns 1, if 'endInstant' is in ST and 'startInstant' is in DST and
 * 'endInstant' > 'startInstant' or 'endInstant' is in DST and 'startInstant' is in ST and
 * 'endInstant' < 'startInstant'; otherwise returns -1.
 */
inline long long dstCorrection(timelib_time* startInstant, timelib_time* endInstant) {
    return (startInstant->z - endInstant->z) / (kMinutesPerHour * kSecondsPerMinute);
}

inline long long dateDiffYear(timelib_time* startInstant, timelib_time* endInstant) {
    return endInstant->y - startInstant->y;
}

/**
 * Determines which quarter month 'month' belongs to. 'month' value range is 1..12. Returns a number
 * of a quarter, where 0 corresponds to the first quarter.
 */
inline int quarter(int month) {
    return (month - 1) / kQuarterLengthInMonths;
}
inline long long dateDiffQuarter(timelib_time* startInstant, timelib_time* endInstant) {
    return quarter(endInstant->m) - quarter(startInstant->m) +
        dateDiffYear(startInstant, endInstant) * kQuartersPerYear;
}
inline long long dateDiffMonth(timelib_time* startInstant, timelib_time* endInstant) {
    return endInstant->m - startInstant->m +
        dateDiffYear(startInstant, endInstant) * kMonthsInOneYear;
}
inline long long dateDiffDay(timelib_time* startInstant, timelib_time* endInstant) {
    return timelib_day_of_year(endInstant->y, endInstant->m, endInstant->d) -
        timelib_day_of_year(startInstant->y, startInstant->m, startInstant->d) +
        daysBetweenYears(startInstant->y, endInstant->y);
}
inline long long dateDiffWeek(timelib_time* startInstant, timelib_time* endInstant) {
    // We use 'timelib_iso_day_of_week()' since it considers Monday as the first day of the week.
    return (dateDiffDay(startInstant, endInstant) +
            timelib_iso_day_of_week(startInstant->y, startInstant->m, startInstant->d) -
            timelib_iso_day_of_week(endInstant->y, endInstant->m, endInstant->d)) /
        kDaysPerWeek;
}
inline long long dateDiffHour(timelib_time* startInstant, timelib_time* endInstant) {
    return endInstant->h - startInstant->h + dateDiffDay(startInstant, endInstant) * kHoursPerDay +
        dstCorrection(startInstant, endInstant);
}
inline long long dateDiffMinute(timelib_time* startInstant, timelib_time* endInstant) {
    return endInstant->i - startInstant->i +
        dateDiffHour(startInstant, endInstant) * kMinutesPerHour;
}
inline long long dateDiffSecond(timelib_time* startInstant, timelib_time* endInstant) {
    return endInstant->s - startInstant->s +
        dateDiffMinute(startInstant, endInstant) * kSecondsPerMinute;
}
inline long long dateDiffMillisecond(Date_t startDate, Date_t endDate) {
    long long result;
    uassert(5166308,
            "dateDiff overflowed",
            !overflow::sub(endDate.toMillisSinceEpoch(), startDate.toMillisSinceEpoch(), &result));
    return result;
}

// A mapping from a string expression of TimeUnit to TimeUnit.
static const StringMap<TimeUnit> timeUnitNameToTimeUnitMap{
    {"year", TimeUnit::year},
    {"quarter", TimeUnit::quarter},
    {"month", TimeUnit::month},
    {"week", TimeUnit::week},
    {"day", TimeUnit::day},
    {"hour", TimeUnit::hour},
    {"minute", TimeUnit::minute},
    {"second", TimeUnit::second},
    {"millisecond", TimeUnit::millisecond},
};
}  // namespace

long long dateDiff(Date_t startDate, Date_t endDate, TimeUnit unit, const TimeZone& timezone) {
    if (TimeUnit::millisecond == unit) {
        return dateDiffMillisecond(startDate, endDate);
    }

    // Translate the time instants to the given timezone.
    auto startDateInTimeZone = timezone.getTimelibTime(startDate);
    auto endDateInTimeZone = timezone.getTimelibTime(endDate);
    switch (unit) {
        case TimeUnit::year:
            return dateDiffYear(startDateInTimeZone.get(), endDateInTimeZone.get());
        case TimeUnit::quarter:
            return dateDiffQuarter(startDateInTimeZone.get(), endDateInTimeZone.get());
        case TimeUnit::month:
            return dateDiffMonth(startDateInTimeZone.get(), endDateInTimeZone.get());
        case TimeUnit::week:
            return dateDiffWeek(startDateInTimeZone.get(), endDateInTimeZone.get());
        case TimeUnit::day:
            return dateDiffDay(startDateInTimeZone.get(), endDateInTimeZone.get());
        case TimeUnit::hour:
            return dateDiffHour(startDateInTimeZone.get(), endDateInTimeZone.get());
        case TimeUnit::minute:
            return dateDiffMinute(startDateInTimeZone.get(), endDateInTimeZone.get());
        case TimeUnit::second:
            return dateDiffSecond(startDateInTimeZone.get(), endDateInTimeZone.get());
        default:
            MONGO_UNREACHABLE;
    }
}

TimeUnit parseTimeUnit(StringData unitName) {
    auto iterator = timeUnitNameToTimeUnitMap.find(unitName);
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "unknown time unit value: " << unitName,
            iterator != timeUnitNameToTimeUnitMap.end());
    return iterator->second;
}

bool isValidTimeUnit(StringData unitName) {
    return timeUnitNameToTimeUnitMap.find(unitName) != timeUnitNameToTimeUnitMap.end();
}

StringData serializeTimeUnit(TimeUnit unit) {
    switch (unit) {
        case TimeUnit::year:
            return "year"_sd;
        case TimeUnit::quarter:
            return "quarter"_sd;
        case TimeUnit::month:
            return "month"_sd;
        case TimeUnit::week:
            return "week"_sd;
        case TimeUnit::day:
            return "day"_sd;
        case TimeUnit::hour:
            return "hour"_sd;
        case TimeUnit::minute:
            return "minute"_sd;
        case TimeUnit::second:
            return "second"_sd;
        case TimeUnit::millisecond:
            return "millisecond"_sd;
    }
    MONGO_UNREACHABLE_TASSERT(5339900);
}

void TimelibRelTimeDeleter::operator()(timelib_rel_time* relTime) {
    timelib_rel_time_dtor(relTime);
}

std::unique_ptr<_timelib_rel_time, TimelibRelTimeDeleter> createTimelibRelTime() {
    return std::unique_ptr<_timelib_rel_time, TimelibRelTimeDeleter>(timelib_rel_time_ctor());
}

std::unique_ptr<timelib_rel_time, TimelibRelTimeDeleter> getTimelibRelTime(TimeUnit unit,
                                                                           long long amount) {
    auto relTime = createTimelibRelTime();
    switch (unit) {
        case TimeUnit::year:
            relTime->y = amount;
            break;
        case TimeUnit::quarter:
            relTime->m = amount * kQuarterLengthInMonths;
            break;
        case TimeUnit::month:
            relTime->m = amount;
            break;
        case TimeUnit::week:
            relTime->d = amount * kDaysPerWeek;
            break;
        case TimeUnit::day:
            relTime->d = amount;
            break;
        case TimeUnit::hour:
            relTime->h = amount;
            break;
        case TimeUnit::minute:
            relTime->i = amount;
            break;
        case TimeUnit::second:
            relTime->s = amount;
            break;
        case TimeUnit::millisecond:
            relTime->us = durationCount<Microseconds>(Milliseconds(amount));
            break;
        default:
            MONGO_UNREACHABLE;
    }
    return relTime;
}

namespace {
/**
 * A helper function that adds an amount of months to a month given by 'year' and 'month'.
 * The amount can be a negative number. Returns the new month as a [year, month] pair.
 */
std::pair<long long, long long> addMonths(long long year, long long month, long long amount) {
    auto m = month + amount;
    auto y = year;
    if (m > 12) {
        y += m / 12;
        m -= 12 * (m / 12);
    }
    if (m <= 0) {
        auto yearsInBetween = (-m) / 12 + 1;
        m += 12 * yearsInBetween;
        y -= yearsInBetween;
    }
    return {y, m};
}

/**
 * A helper function that checks if last day adjustment is needed for a dateAdd operation.
 * If yes, the function computes and returns the time interval in a number of days, so that the day
 * in the result date is the last valid day in the respective month. Example: 2020-10-31 + 1 month
 * -> day adjustment is needed since there is no 31st of November. The function computes adjusted
 * time interval of 30 days.
 *
 * tm: start date of the operation
 * unit: the time unit
 * amount: the amount of time units to be added
 * returns optional intervalInDays : adjusted time interval in number of days if adjustment is
 * needed
 */
boost::optional<long long> needsDayAdjustment(timelib_time* tm, TimeUnit unit, long long amount) {
    if (tm->d <= 28) {
        return boost::none;
    }
    if (unit == TimeUnit::year) {
        unit = TimeUnit::month;
        amount *= kMonthsInOneYear;
    }
    if (unit == TimeUnit::quarter) {
        unit = TimeUnit::month;
        amount *= kQuarterLengthInMonths;
    }

    auto [resYear, resMonth] = addMonths(tm->y, tm->m, amount);
    auto maxResDay = timelib_days_in_month(resYear, resMonth);

    if (tm->d > maxResDay) {
        long long intervalInDays = timelib_day_of_year(resYear, resMonth, maxResDay) -
            timelib_day_of_year(tm->y, tm->m, tm->d) + daysBetweenYears(tm->y, resYear);
        return boost::make_optional(intervalInDays);
    }
    return boost::none;
}

/**
 * A helper function that computes DST correction in seconds for start and end seconds-since-epoch
 * for the given timezone argument.
 */
long long dateAddDSTCorrection(long long startSse, long long endSse, const TimeZone& timezone) {
    auto tz = timezone.getTzInfo();
    if (!tz) {
        return 0;
    }
    auto startOffset = timelib_get_time_zone_info(startSse, tz.get());
    auto endOffset = timelib_get_time_zone_info(endSse, tz.get());
    long long corr = startOffset->offset - endOffset->offset;
    timelib_time_offset_dtor(startOffset);
    timelib_time_offset_dtor(endOffset);
    return corr;
}

}  // namespace

Date_t dateAdd(Date_t date, TimeUnit unit, long long amount, const TimeZone& timezone) {
    auto utcTime = createTimelibTime();
    timelib_unixtime2gmt(utcTime.get(), seconds(date));

    // Check if an adjustment for the last day of month is necessary.
    if (unit == TimeUnit::year || unit == TimeUnit::quarter || unit == TimeUnit::month) {
        auto intervalInDays = [&]() {
            if (timezone.isUtcZone()) {
                return needsDayAdjustment(utcTime.get(), unit, amount);
            }
            // If a timezone is provided, the last day adjustment is computed in this timezone.
            auto localTime = timezone.getTimelibTime(date);
            return needsDayAdjustment(localTime.get(), unit, amount);
        }();
        if (intervalInDays) {
            unit = TimeUnit::day;
            amount = intervalInDays.get();
        }
    }

    auto interval = getTimelibRelTime(unit, amount);

    // Compute the result date in UTC and if needed later perform a DST correction for a timezone.
    // The alternative computation in the associated timezone gives incorrect results when the
    // computed date falls into the missing hour during the standard time-to-DST transition or
    // falls into the repeated hour during the DST-to-standard time transition.
    auto newTime = timelib_add(utcTime.get(), interval.get());

    // Check the DST offsets in the given timezone and compute a correction if the time unit is
    // a day or a larger unit. UTC and offset-based timezones do not have DST and do not need
    // this correction.
    if ((interval->d || interval->m || interval->y) && timezone.isTimeZoneIDZone()) {
        newTime->sse += dateAddDSTCorrection(utcTime->sse, newTime->sse, timezone);
    }

    long long res;
    if (overflow::mul(newTime->sse, 1000L, &res)) {
        timelib_time_dtor(newTime);
        uasserted(5166406, "dateAdd overflowed");
    }
    auto returnDate = Date_t::fromMillisSinceEpoch(
        durationCount<Milliseconds>(Seconds(newTime->sse) + Microseconds(newTime->us)));
    timelib_time_dtor(newTime);
    return returnDate;
}
}  // namespace mongo
