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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

namespace {

const auto getTimeZoneDatabaseDecorable =
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
    {'b', TIMELIB_FORMAT_TEXTUAL_MONTH_3_LETTER},
    {'B', TIMELIB_FORMAT_TEXTUAL_MONTH_FULL},
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
    return getTimeZoneDatabaseDecorable(serviceContext).get();
}

void TimeZoneDatabase::set(ServiceContext* serviceContext,
                           std::unique_ptr<TimeZoneDatabase> dateTimeSupport) {
    getTimeZoneDatabaseDecorable(serviceContext) = std::move(dateTimeSupport);
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

void TimeZoneDatabase::TimelibTZInfoDeleter::operator()(timelib_tzinfo* tzInfo) {
    if (tzInfo) {
        timelib_tzinfo_dtor(tzInfo);
    }
}

void TimeZoneDatabase::loadTimeZoneInfo(
    std::unique_ptr<timelib_tzdb, TimeZoneDBDeleter> timeZoneDatabase) {
    invariant(timeZoneDatabase);
    _timeZoneDatabase = std::move(timeZoneDatabase);
    int nTimeZones;
    auto timezone_identifier_list =
        timelib_timezone_identifiers_list(_timeZoneDatabase.get(), &nTimeZones);
    for (int i = 0; i < nTimeZones; ++i) {
        const auto& entry = timezone_identifier_list[i];
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

        invariant(errorCode == TIMELIB_ERROR_NO_ERROR ||
                  errorCode == TIMELIB_ERROR_EMPTY_POSIX_STRING);

        if (strcmp(entry.id, "UTC") == 0) {
            _timeZones[entry.id] = TimeZone{nullptr};
            timelib_tzinfo_dtor(tzInfo);
        } else {
            _timeZoneInfos.emplace_back(
                std::unique_ptr<_timelib_tzinfo, TimelibTZInfoDeleter>(tzInfo));
            _timeZones[entry.id] = TimeZone{tzInfo};
        }
    }
}

TimeZone TimeZoneDatabase::utcZone() {
    return TimeZone{nullptr};
}

static timelib_tzinfo* timezonedatabase_gettzinfowrapper(const char* tz_id,
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
        timelib_set_timezone(timelibTime, _tzInfo);
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

    auto returnValue =
        Date_t::fromMillisSinceEpoch(durationCount<Milliseconds>(Seconds(newTime->sse)) +
                                     durationCount<Milliseconds>(Microseconds(newTime->us)));

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

    auto returnValue =
        Date_t::fromMillisSinceEpoch(durationCount<Milliseconds>(Seconds(newTime->sse)) +
                                     durationCount<Milliseconds>(Microseconds(newTime->us)));

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


TimeZone::TimeZone(timelib_tzinfo* tzInfo) : _tzInfo(tzInfo), _utcOffset(0) {}

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
    if (isTimeZoneIDZone()) {
        int32_t timezoneOffsetFromUTC = 0;
        int result =
            timelib_get_time_zone_offset_info(durationCount<Seconds>(date.toDurationSinceEpoch()),
                                              _tzInfo,
                                              &timezoneOffsetFromUTC,
                                              nullptr,
                                              nullptr);
        uassert(6828900, "Failed to obtain timezone offset", result);
        return Seconds(timezoneOffsetFromUTC);
    } else {
        return _utcOffset;
    }
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
constexpr auto kMonthsInOneYear = 12LL;
constexpr auto kDaysInNonLeapYear = 365LL;
constexpr auto kHoursPerDay = 24LL;
constexpr auto kMinutesPerHour = 60LL;
constexpr auto kSecondsPerMinute = 60LL;
constexpr auto kMillisecondsPerSecond = 1000LL;
constexpr int kDaysPerWeek = 7;
constexpr auto kQuartersPerYear = 4LL;
constexpr auto kQuarterLengthInMonths = 3LL;
constexpr long kMillisecondsPerDay{kHoursPerDay * kMinutesPerHour * kSecondsPerMinute *
                                   kMillisecondsPerSecond};
constexpr long kLeapYearReferencePoint = -1000000000L;

/**
 * A Date with only year, month and day of month components.
 */
struct Date {
    Date(const timelib_time& timelibTime)
        : year{timelibTime.y},
          month{static_cast<int>(timelibTime.m)},
          dayOfMonth{static_cast<int>(timelibTime.d)} {}
    Date(long long year, int month, int dayOfMonth)
        : year{year}, month{month}, dayOfMonth{dayOfMonth} {}
    long long year;
    int month;  // January = 1.
    int dayOfMonth;
};

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
 * 'startYear'+1, .., 'endYear'-1. 'startYear' and 'endYear' are expected to be from the range
 * (-1000'000'000; +1000'000'000).
 */
inline long long daysBetweenYears(long startYear, long endYear) {
    return leapYearsSinceReferencePoint(endYear - 1) - leapYearsSinceReferencePoint(startYear - 1) +
        (endYear - startYear) * kDaysInNonLeapYear;
}

/**
 * Determines a correction needed in number of hours when calculating passed hours between two time
 * instants 'startInstant' and 'endInstant' due to different UTC offsets.
 */
inline long long utcOffsetCorrectionForHours(timelib_time* startInstant, timelib_time* endInstant) {
    return (startInstant->z - endInstant->z) / (kMinutesPerHour * kSecondsPerMinute);
}

/**
 * Determines a correction needed in number of minutes when calculating passed minutes between two
 * time instants 'startInstant' and 'endInstant' due to different UTC offsets.
 */
inline long long utcOffsetCorrectionForMinutes(timelib_time* startInstant,
                                               timelib_time* endInstant) {
    return (startInstant->z - endInstant->z) / kSecondsPerMinute;
}

/**
 * Determines a correction needed in number of seconds when calculating passed seconds between two
 * time instants 'startInstant' and 'endInstant' due to different UTC offsets.
 */
inline long long utcOffsetCorrectionForSeconds(timelib_time* startInstant,
                                               timelib_time* endInstant) {
    return startInstant->z - endInstant->z;
}
inline long long dateDiffYear(Date startInstant, Date endInstant) {
    return endInstant.year - startInstant.year;
}

/**
 * Determines which quarter month 'month' belongs to. 'month' value range is 1..12. Returns a number
 * of a quarter, where 0 corresponds to the first quarter.
 */
inline int quarter(int month) {
    return (month - 1) / kQuarterLengthInMonths;
}
inline long long dateDiffQuarter(Date startInstant, Date endInstant) {
    return quarter(endInstant.month) - quarter(startInstant.month) +
        dateDiffYear(startInstant, endInstant) * kQuartersPerYear;
}
inline long long dateDiffMonth(Date startInstant, Date endInstant) {
    return endInstant.month - startInstant.month +
        dateDiffYear(startInstant, endInstant) * kMonthsInOneYear;
}
inline long long dateDiffDay(Date startInstant, Date endInstant) {
    return timelib_day_of_year(endInstant.year, endInstant.month, endInstant.dayOfMonth) -
        timelib_day_of_year(startInstant.year, startInstant.month, startInstant.dayOfMonth) +
        daysBetweenYears(startInstant.year, endInstant.year);
}

/**
 * Determines which day of the week time instant 'timeInstant' is in given that the week starts on
 * day 'startOfWeek'. Returns 0 for the first day, and 6 - for the last.
 */
inline unsigned int dayOfWeek(Date timeInstant, DayOfWeek startOfWeek) {
    // We use 'timelib_iso_day_of_week()' since it returns value 1 for Monday.
    return (timelib_iso_day_of_week(timeInstant.year, timeInstant.month, timeInstant.dayOfMonth) -
            static_cast<uint8_t>(startOfWeek) + kDaysPerWeek) %
        kDaysPerWeek;
}

/**
 * Determines a number of weeks between time instant 'startInstant' and 'endInstant' when the first
 * day of the week is 'startOfWeek'.
 */
inline long long dateDiffWeek(Date startInstant, Date endInstant, DayOfWeek startOfWeek) {
    return (dateDiffDay(startInstant, endInstant) + dayOfWeek(startInstant, startOfWeek) -
            dayOfWeek(endInstant, startOfWeek)) /
        kDaysPerWeek;
}
inline long long dateDiffHourWithoutUTCOffsetCorrection(timelib_time* startInstant,
                                                        timelib_time* endInstant) {
    return endInstant->h - startInstant->h + dateDiffDay(*startInstant, *endInstant) * kHoursPerDay;
}
inline long long dateDiffHour(timelib_time* startInstant, timelib_time* endInstant) {
    return dateDiffHourWithoutUTCOffsetCorrection(startInstant, endInstant) +
        utcOffsetCorrectionForHours(startInstant, endInstant);
}
inline long long dateDiffMinuteWithoutUTCOffsetCorrection(timelib_time* startInstant,
                                                          timelib_time* endInstant) {
    return endInstant->i - startInstant->i +
        dateDiffHourWithoutUTCOffsetCorrection(startInstant, endInstant) * kMinutesPerHour;
}
inline long long dateDiffMinute(timelib_time* startInstant, timelib_time* endInstant) {
    return dateDiffMinuteWithoutUTCOffsetCorrection(startInstant, endInstant) +
        utcOffsetCorrectionForMinutes(startInstant, endInstant);
}
inline long long dateDiffSecond(timelib_time* startInstant, timelib_time* endInstant) {
    return endInstant->s - startInstant->s +
        dateDiffMinuteWithoutUTCOffsetCorrection(startInstant, endInstant) * kSecondsPerMinute +
        utcOffsetCorrectionForSeconds(startInstant, endInstant);
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

// A mapping from string representations of a day of a week to DayOfWeek.
static const StringMap<DayOfWeek> dayOfWeekNameToDayOfWeekMap{
    {"monday", DayOfWeek::monday},
    {"mon", DayOfWeek::monday},
    {"tuesday", DayOfWeek::tuesday},
    {"tue", DayOfWeek::tuesday},
    {"wednesday", DayOfWeek::wednesday},
    {"wed", DayOfWeek::wednesday},
    {"thursday", DayOfWeek::thursday},
    {"thu", DayOfWeek::thursday},
    {"friday", DayOfWeek::friday},
    {"fri", DayOfWeek::friday},
    {"saturday", DayOfWeek::saturday},
    {"sat", DayOfWeek::saturday},
    {"sunday", DayOfWeek::sunday},
    {"sun", DayOfWeek::sunday},
};

}  // namespace

long long dateDiff(Date_t startDate,
                   Date_t endDate,
                   TimeUnit unit,
                   const TimeZone& timezone,
                   DayOfWeek startOfWeek) {
    if (TimeUnit::millisecond == unit) {
        return dateDiffMillisecond(startDate, endDate);
    }

    // Translate the time instants to the given timezone.
    auto startDateInTimeZone = timezone.getTimelibTime(startDate);
    auto endDateInTimeZone = timezone.getTimelibTime(endDate);
    switch (unit) {
        case TimeUnit::year:
            return dateDiffYear(*startDateInTimeZone, *endDateInTimeZone);
        case TimeUnit::quarter:
            return dateDiffQuarter(*startDateInTimeZone, *endDateInTimeZone);
        case TimeUnit::month:
            return dateDiffMonth(*startDateInTimeZone, *endDateInTimeZone);
        case TimeUnit::week:
            return dateDiffWeek(*startDateInTimeZone, *endDateInTimeZone, startOfWeek);
        case TimeUnit::day:
            return dateDiffDay(*startDateInTimeZone, *endDateInTimeZone);
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
    MONGO_UNREACHABLE_TASSERT(5339903);
}

DayOfWeek parseDayOfWeek(StringData dayOfWeek) {
    // Perform case-insensitive lookup.
    auto iterator = dayOfWeekNameToDayOfWeekMap.find(str::toLower(dayOfWeek));
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "unknown day of week value: " << dayOfWeek,
            iterator != dayOfWeekNameToDayOfWeekMap.end());
    return iterator->second;
}

bool isValidDayOfWeek(StringData dayOfWeek) {
    // Perform case-insensitive lookup.
    return dayOfWeekNameToDayOfWeekMap.find(str::toLower(dayOfWeek)) !=
        dayOfWeekNameToDayOfWeekMap.end();
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
 * A helper function that computes the number of days to add to get an equivalent result as from
 * adding an 'amount' number of 'unit's in two use cases:
 * In case the date is in UTC, a last day adjustment is needed if the day is greater than 28th.
 * In case the date is in a timezone different from UTC, the time interval is always converted into
 * a number of days to produce correct result in this timezone. This may also include a last day
 * adjustment.
 *
 * The last day adjustment computation makes sure that the day in the result date is not greater
 * than the last valid day in the respective month. Example: 2020-10-31 + 1 month -> day adjustment
 * is needed since there is no 31st of November. The function computes adjusted time interval of 30
 * days. For dates in UTC and day smaller than 29th, the function returns boost::none.
 *
 * tm: start date of the operation
 * unit: the time unit
 * amount: the amount of time units to be added
 * returns optional intervalInDays : adjusted time interval in number of days if adjustment is
 * needed
 */
boost::optional<long long> daysToAdd(timelib_time* tm, TimeUnit unit, long long amount) {
    if (unit != TimeUnit::year && unit != TimeUnit::quarter && unit != TimeUnit::month) {
        return boost::none;
    }
    if (tm->d <= 28 && tm->z == 0) {
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
    auto targetDay = std::min(tm->d, maxResDay);
    long long intervalInDays = timelib_day_of_year(resYear, resMonth, targetDay) -
        timelib_day_of_year(tm->y, tm->m, tm->d) + daysBetweenYears(tm->y, resYear);
    return boost::make_optional(intervalInDays);
}

/**
 * Determines a distance of 'value' to the lower bound of a bin 'value' falls into. It assumes that
 * there is a set of bins with following bounds .., [-'binSize', 0), [0, 'binSize'), ['binSize',
 * 2*'binSize'), ..
 *
 * binSize - bin size. Must be greater than 0.
 */
inline long long distanceToBinLowerBound(long long value, long long binSize) {
    tassert(5439019, "expected binSize > 0", binSize > 0);
    long long remainder = value % binSize;
    if (remainder < 0) {
        remainder += binSize;
    }
    return remainder;
}

/**
 * An optimized version of date truncation algorithm that works with bins in milliseconds, seconds,
 * minutes and hours.
 */
inline Date_t truncateDateMillis(Date_t date,
                                 Date_t referencePoint,
                                 unsigned long long binSizeMillis) {
    tassert(5439020,
            "expected binSizeMillis to be convertable to a 64-bit signed integer",
            binSizeMillis <=
                static_cast<unsigned long long>(std::numeric_limits<long long>::max()));
    long long shiftedDate;
    uassert(5439000,
            "dateTrunc overflowed",
            !overflow::sub(
                date.toMillisSinceEpoch(), referencePoint.toMillisSinceEpoch(), &shiftedDate));
    long long result;
    uassert(5439001,
            "dateTrunc overflowed",
            !overflow::sub(date.toMillisSinceEpoch(),
                           distanceToBinLowerBound(shiftedDate, binSizeMillis),
                           &result));
    return Date_t::fromMillisSinceEpoch(result);
}

inline long long binSizeInMillis(unsigned long long binSize, unsigned long millisPerUnit) {
    long long binSizeInMillis;
    uassert(
        5439002, "dateTrunc overflowed", !overflow::mul(binSize, millisPerUnit, &binSizeInMillis));
    return binSizeInMillis;
}

/**
 * The same as 'truncateDate(Date_t, TimeUnit, unsigned long long binSize, const TimeZone&,
 * DayOfWeek)', but additionally accepts a reference point 'referencePoint', that is expected to be
 * aligned to the given time unit.
 *
 * referencePoint - a reference point for bins. It is a pair of two different representations -
 * milliseconds since Unix epoch and date component based to avoid the cost of converting from one
 * representation to another.
 */
Date_t truncateDate(Date_t date,
                    TimeUnit unit,
                    unsigned long long binSize,
                    std::pair<Date_t, Date> referencePoint,
                    const TimeZone& timezone,
                    DayOfWeek startOfWeek) {
    switch (unit) {
        case TimeUnit::millisecond:
            return truncateDateMillis(date, referencePoint.first, binSize);
        case TimeUnit::second:
            return truncateDateMillis(
                date, referencePoint.first, binSizeInMillis(binSize, kMillisecondsPerSecond));
        case TimeUnit::minute:
            return truncateDateMillis(
                date,
                referencePoint.first,
                binSizeInMillis(binSize, kSecondsPerMinute * kMillisecondsPerSecond));
        case TimeUnit::hour:
            return truncateDateMillis(
                date,
                referencePoint.first,
                binSizeInMillis(binSize,
                                kMinutesPerHour * kSecondsPerMinute * kMillisecondsPerSecond));
        default: {
            uassert(
                5439006,
                "dateTrunc unsupported binSize value",
                binSize <=
                    100'000'000'000);  // This is a limit up to which dateAdd() can properly handle.
            const auto dateInTimeZone = timezone.getTimelibTime(date);
            long long distanceFromReferencePoint;
            switch (unit) {
                case TimeUnit::day:
                    distanceFromReferencePoint =
                        dateDiffDay(referencePoint.second, *dateInTimeZone);
                    break;
                case TimeUnit::week:
                    distanceFromReferencePoint =
                        dateDiffWeek(referencePoint.second, *dateInTimeZone, startOfWeek);
                    break;
                case TimeUnit::month:
                    distanceFromReferencePoint =
                        dateDiffMonth(referencePoint.second, *dateInTimeZone);
                    break;
                case TimeUnit::quarter:
                    distanceFromReferencePoint =
                        dateDiffQuarter(referencePoint.second, *dateInTimeZone);
                    break;
                case TimeUnit::year:
                    distanceFromReferencePoint =
                        dateDiffYear(referencePoint.second, *dateInTimeZone);
                    break;
                default:
                    MONGO_UNREACHABLE_TASSERT(5439021);
            }

            // Determine a distance of the lower bound of a bin 'date' falls into from the reference
            // point.
            long long binLowerBoundFromRefPoint;
            uassert(5439004,
                    "dateTrunc overflowed",
                    !overflow::sub(distanceFromReferencePoint,
                                   distanceToBinLowerBound(distanceFromReferencePoint, binSize),
                                   &binLowerBoundFromRefPoint));

            // Determine the lower bound of a bin the 'date' falls into.
            return dateAdd(referencePoint.first, unit, binLowerBoundFromRefPoint, timezone);
        }
    }
}

/**
 * Returns the default reference point used in $dateTrunc computation that is tied to 'timezone'. It
 * must be aligned to time unit 'unit'. This function returns a pair of representations of the
 * reference point.
 */
std::pair<Date_t, Date> defaultReferencePointForDateTrunc(const TimeZone& timezone,
                                                          TimeUnit unit,
                                                          DayOfWeek startOfWeek) {
    // We use a more resource efficient way than 'TimeZone::createFromDateParts()' to get reference
    // point value in 'timezone'.
    constexpr long long kReferencePointInUTCMillis = 946684800000LL;  // 2000-01-01T00:00:00.000Z
    Date referencePoint{2000, 1, 1};
    long long referencePointMillis = kReferencePointInUTCMillis -
        durationCount<Milliseconds>(timezone.utcOffset(
            Date_t::fromMillisSinceEpoch(kReferencePointInUTCMillis)));
    dassert(timezone.createFromDateParts(2000, 1, 1, 0, 0, 0, 0).toMillisSinceEpoch() ==
            referencePointMillis);

    if (TimeUnit::week == unit) {
        // Find the nearest to 'referencePoint' first day of the week that is in the future.
        constexpr DayOfWeek kReferencePointDayOfWeek{
            DayOfWeek::saturday};  // 2000-01-01 is Saturday.
        int referencePointDayOfWeek = (static_cast<uint8_t>(kReferencePointDayOfWeek) -
                                       static_cast<uint8_t>(startOfWeek) + kDaysPerWeek) %
            kDaysPerWeek;
        int daysToAdjustBy = (kDaysPerWeek - referencePointDayOfWeek) % kDaysPerWeek;

        // If the reference point was an arbitrary value, we would need to use 'dateAdd()' function
        // to correctly add a number of days to account for Daylight Saving Time (DST) transitions
        // that may happen between the initial reference point and the resulting date (DST has a
        // different offset from UTC than Standard Time). However, since the reference point is the
        // first of January, 2000 and Daylight Saving Time transitions did not happen in the first
        // half of January in year 2000, it is correct to just add a number of milliseconds in
        // 'daysToAdjustBy' days.
        referencePointMillis += daysToAdjustBy * kMillisecondsPerDay;
        referencePoint.dayOfMonth += daysToAdjustBy;
    }
    return {Date_t::fromMillisSinceEpoch(referencePointMillis), referencePoint};
}

/**
 * Determines if function 'dateAdd()' parameter 'amount' and 'unit' values are valid - the
 * amount roughly fits the range of Date_t type.
 */
bool isDateAddAmountValid(long long amount, TimeUnit unit) {
    constexpr long long maxDays{
        std::numeric_limits<unsigned long long>::max() / kMillisecondsPerDay + 1};
    constexpr auto maxYears = maxDays / 365 /* minimum number of days per year*/ + 1;
    constexpr auto maxQuarters = maxYears * kQuartersPerYear;
    constexpr auto maxMonths = maxYears * kMonthsInOneYear;
    constexpr auto maxWeeks = maxDays / kDaysPerWeek;
    constexpr auto maxHours = maxDays * kHoursPerDay;
    constexpr auto maxMinutes = maxHours * kMinutesPerHour;
    constexpr auto maxSeconds = maxMinutes * kSecondsPerMinute;
    const auto maxAbsoluteAmountValue = [&](TimeUnit unit) {
        switch (unit) {
            case TimeUnit::year:
                return maxYears;
            case TimeUnit::quarter:
                return maxQuarters;
            case TimeUnit::month:
                return maxMonths;
            case TimeUnit::week:
                return maxWeeks;
            case TimeUnit::day:
                return maxDays;
            case TimeUnit::hour:
                return maxHours;
            case TimeUnit::minute:
                return maxMinutes;
            case TimeUnit::second:
                return maxSeconds;
            default:
                MONGO_UNREACHABLE_TASSERT(5976501);
        }
    }(unit);
    return -maxAbsoluteAmountValue < amount && amount < maxAbsoluteAmountValue;
}
}  // namespace

Date_t dateAdd(Date_t date, TimeUnit unit, long long amount, const TimeZone& timezone) {
    if (unit == TimeUnit::millisecond) {
        return date + Milliseconds(amount);
    }

    // Check that 'amount' value is within an acceptable range. If the value is within acceptable
    // range, then the addition algorithm is expected to not overflow. The final determination if
    // the result can be represented as Date_t is done after the addition result is computed.
    uassert(5976500,
            str::stream() << "invalid dateAdd 'amount' parameter value: " << amount << " "
                          << serializeTimeUnit(unit),
            isDateAddAmountValid(amount, unit));

    auto localTime = timezone.getTimelibTime(date);
    auto microSec = durationCount<Microseconds>(Milliseconds(date.toMillisSinceEpoch() % 1000));
    localTime->us = microSec;

    // Check if an adjustment for the last day of month is necessary.
    auto intervalInDays = daysToAdd(localTime.get(), unit, amount);
    if (intervalInDays) {
        unit = TimeUnit::day;
        amount = intervalInDays.value();
    }

    auto interval = getTimelibRelTime(unit, amount);

    timelib_time* timeAfterAddition;
    // For time units of day or larger perform the computation in the local timezone. This
    // keeps the values of hour, minute, second, and millisecond components from the input date
    // the same in the result date regardless of transitions from DST to Standard Time and vice
    // versa that may happen between the input date and the result.
    if (timezone.isUtcZone() || timezone.isUtcOffsetZone() || interval->d || interval->m ||
        interval->y) {
        timeAfterAddition = timelib_add(localTime.get(), interval.get());
    } else {
        // For time units of hour or smaller and a timezone different from UTC perform the
        // computation in UTC. In this case we don't want to apply the DST correction to the return
        // date, which would happen by default if we used the timelib_add() function with local
        // time. For example:
        //    {$dateAdd: { startDate: ISODate("2020-11-01T05:50:02Z"),
        //     unit: "hour", amount: 1, timezone: "America/New_York"}}
        // returns ISODate("2020-11-01T07:50:02Z") when we call timelib_add(localTime ...)
        // and ISODate("2020-11-01T06:50:02Z") when we call timelib_add(utcTime ...).
        auto utcTime = createTimelibTime();
        timelib_unixtime2gmt(utcTime.get(), seconds(date));
        utcTime->us = microSec;
        timeAfterAddition = timelib_add(utcTime.get(), interval.get());
    }

    long long res;
    if (overflow::mul(timeAfterAddition->sse, 1000L, &res)) {
        timelib_time_dtor(timeAfterAddition);
        uasserted(5166406, "dateAdd overflowed");
    }

    auto returnDate = Date_t::fromMillisSinceEpoch(
        durationCount<Milliseconds>(Seconds(timeAfterAddition->sse)) +
        durationCount<Milliseconds>(Microseconds(timeAfterAddition->us)));
    timelib_time_dtor(timeAfterAddition);
    return returnDate;
}

StatusWith<long long> timeUnitTypicalMilliseconds(TimeUnit unit) {
    auto constexpr millisecond = 1;
    auto constexpr second = millisecond * kMillisecondsPerSecond;
    auto constexpr minute = second * kSecondsPerMinute;
    auto constexpr hour = minute * kMinutesPerHour;
    auto constexpr day = hour * kHoursPerDay;
    auto constexpr week = day * kDaysPerWeek;

    switch (unit) {
        case TimeUnit::millisecond:
            return millisecond;
        case TimeUnit::second:
            return second;
        case TimeUnit::minute:
            return minute;
        case TimeUnit::hour:
            return hour;
        case TimeUnit::day:
            return day;
        case TimeUnit::week:
            return week;
        case TimeUnit::month:
        case TimeUnit::quarter:
        case TimeUnit::year:
            return Status(ErrorCodes::BadValue,
                          str::stream() << "TimeUnit is too big: " << serializeTimeUnit(unit));
    }
    MONGO_UNREACHABLE_TASSERT(5423303);
}

Date_t truncateDate(Date_t date,
                    TimeUnit unit,
                    unsigned long long binSize,
                    const TimeZone& timezone,
                    DayOfWeek startOfWeek) {
    uassert(5439005, "expected binSize > 0", binSize > 0);

    // Determine a reference point aligned to the natural boundaries of time unit 'unit'.
    const auto referencePoint{defaultReferencePointForDateTrunc(timezone, unit, startOfWeek)};
    return truncateDate(date, unit, binSize, referencePoint, timezone, startOfWeek);
}
}  // namespace mongo
