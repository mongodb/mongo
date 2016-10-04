/*    Copyright 2010 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/time_support.h"

#include <boost/thread/tss.hpp>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

#include "mongo/base/init.h"
#include "mongo/base/parse_number.h"
#include "mongo/bson/util/builder.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

#ifdef _WIN32
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/timer.h"
#include <boost/date_time/filetime_functions.hpp>
#endif

#ifdef __sun
// Some versions of Solaris do not have timegm defined, so fall back to our implementation when
// building on Solaris.  See SERVER-13446.
extern "C" time_t timegm(struct tm* const tmp);
#endif

namespace mongo {

Date_t Date_t::max() {
    return fromMillisSinceEpoch(std::numeric_limits<long long>::max());
}

Date_t Date_t::now() {
    return fromMillisSinceEpoch(curTimeMillis64());
}

Date_t::Date_t(stdx::chrono::system_clock::time_point tp)
    : millis(durationCount<Milliseconds>(tp - stdx::chrono::system_clock::from_time_t(0))) {}

stdx::chrono::system_clock::time_point Date_t::toSystemTimePoint() const {
    return stdx::chrono::system_clock::from_time_t(0) + toDurationSinceEpoch().toSystemDuration();
}

bool Date_t::isFormattable() const {
    if (millis < 0) {
        return false;
    }
    if (sizeof(time_t) == sizeof(int32_t)) {
        return millis < 2147483647000LL;  // "2038-01-19T03:14:07Z"
    } else {
        return millis < 32535215999000LL;  // "3000-12-31T23:59:59Z"
    }
}


// jsTime_virtual_skew is just for testing. a test command manipulates it.
long long jsTime_virtual_skew = 0;
boost::thread_specific_ptr<long long> jsTime_virtual_thread_skew;

using std::string;

void time_t_to_Struct(time_t t, struct tm* buf, bool local) {
#if defined(_WIN32)
    if (local)
        localtime_s(buf, &t);
    else
        gmtime_s(buf, &t);
#else
    if (local)
        localtime_r(&t, buf);
    else
        gmtime_r(&t, buf);
#endif
}

std::string time_t_to_String_short(time_t t) {
    char buf[64];
#if defined(_WIN32)
    ctime_s(buf, sizeof(buf), &t);
#else
    ctime_r(&t, buf);
#endif
    buf[19] = 0;
    if (buf[0] && buf[1] && buf[2] && buf[3])
        return buf + 4;  // skip day of week
    return buf;
}


// uses ISO 8601 dates without trailing Z
// colonsOk should be false when creating filenames
string terseCurrentTime(bool colonsOk) {
    struct tm t;
    time_t_to_Struct(time(0), &t);

    const char* fmt = (colonsOk ? "%Y-%m-%dT%H:%M:%S" : "%Y-%m-%dT%H-%M-%S");
    char buf[32];
    fassert(16226, strftime(buf, sizeof(buf), fmt, &t) == 19);
    return buf;
}

string terseUTCCurrentTime() {
    return terseCurrentTime(false) + "Z";
}

#define MONGO_ISO_DATE_FMT_NO_TZ "%Y-%m-%dT%H:%M:%S"

namespace {
struct DateStringBuffer {
    static const int dataCapacity = 64;
    char data[dataCapacity];
    int size;
};

void _dateToISOString(Date_t date, bool local, DateStringBuffer* result) {
    invariant(date.isFormattable());
    static const int bufSize = DateStringBuffer::dataCapacity;
    char* const buf = result->data;
    struct tm t;
    time_t_to_Struct(date.toTimeT(), &t, local);
    int pos = strftime(buf, bufSize, MONGO_ISO_DATE_FMT_NO_TZ, &t);
    dassert(0 < pos);
    char* cur = buf + pos;
    int bufRemaining = bufSize - pos;
    pos = snprintf(cur, bufRemaining, ".%03d", static_cast<int32_t>(date.asInt64() % 1000));
    dassert(bufRemaining > pos && pos > 0);
    cur += pos;
    bufRemaining -= pos;
    if (local) {
        static const int localTzSubstrLen = 5;
        dassert(bufRemaining >= localTzSubstrLen + 1);
#ifdef _WIN32
        // NOTE(schwerin): The value stored by _get_timezone is the value one adds to local time
        // to get UTC.  This is opposite of the ISO-8601 meaning of the timezone offset.
        // NOTE(schwerin): Microsoft's timezone code always assumes US rules for daylight
        // savings time.  We can do no better without completely reimplementing localtime_s and
        // related time library functions.
        long msTimeZone;
        _get_timezone(&msTimeZone);
        if (t.tm_isdst)
            msTimeZone -= 3600;
        const bool tzIsWestOfUTC = msTimeZone > 0;
        const long tzOffsetSeconds = msTimeZone * (tzIsWestOfUTC ? 1 : -1);
        const long tzOffsetHoursPart = tzOffsetSeconds / 3600;
        const long tzOffsetMinutesPart = (tzOffsetSeconds / 60) % 60;
        snprintf(cur,
                 localTzSubstrLen + 1,
                 "%c%02ld%02ld",
                 tzIsWestOfUTC ? '-' : '+',
                 tzOffsetHoursPart,
                 tzOffsetMinutesPart);
#else
        strftime(cur, bufRemaining, "%z", &t);
#endif
        cur += localTzSubstrLen;
    } else {
        dassert(bufRemaining >= 2);
        *cur = 'Z';
        ++cur;
    }
    result->size = cur - buf;
    dassert(result->size < DateStringBuffer::dataCapacity);
}

void _dateToCtimeString(Date_t date, DateStringBuffer* result) {
    static const size_t ctimeSubstrLen = 19;
    static const size_t millisSubstrLen = 4;
    time_t t = date.toTimeT();
#if defined(_WIN32)
    ctime_s(result->data, sizeof(result->data), &t);
#else
    ctime_r(&t, result->data);
#endif
    char* milliSecStr = result->data + ctimeSubstrLen;
    snprintf(
        milliSecStr, millisSubstrLen + 1, ".%03d", static_cast<int32_t>(date.asInt64() % 1000));
    result->size = ctimeSubstrLen + millisSubstrLen;
}
}  // namespace

std::string dateToISOStringUTC(Date_t date) {
    DateStringBuffer buf;
    _dateToISOString(date, false, &buf);
    return std::string(buf.data, buf.size);
}

std::string dateToISOStringLocal(Date_t date) {
    DateStringBuffer buf;
    _dateToISOString(date, true, &buf);
    return std::string(buf.data, buf.size);
}

std::string dateToCtimeString(Date_t date) {
    DateStringBuffer buf;
    _dateToCtimeString(date, &buf);
    return std::string(buf.data, buf.size);
}

void outputDateAsISOStringUTC(std::ostream& os, Date_t date) {
    DateStringBuffer buf;
    _dateToISOString(date, false, &buf);
    os << StringData(buf.data, buf.size);
}

void outputDateAsISOStringLocal(std::ostream& os, Date_t date) {
    DateStringBuffer buf;
    _dateToISOString(date, true, &buf);
    os << StringData(buf.data, buf.size);
}

void outputDateAsCtime(std::ostream& os, Date_t date) {
    DateStringBuffer buf;
    _dateToCtimeString(date, &buf);
    os << StringData(buf.data, buf.size);
}

namespace {
StringData getNextToken(StringData currentString,
                        StringData terminalChars,
                        size_t startIndex,
                        size_t* endIndex) {
    size_t index = startIndex;

    if (index == std::string::npos) {
        *endIndex = std::string::npos;
        return StringData();
    }

    for (; index < currentString.size(); index++) {
        if (terminalChars.find(currentString[index]) != std::string::npos) {
            break;
        }
    }

    // substr just returns the rest of the string if the length passed in is greater than the
    // number of characters remaining, and since std::string::npos is the length of the largest
    // possible string we know (std::string::npos - startIndex) is at least as long as the rest
    // of the string.  That means this handles both the case where we hit a terminating
    // character and we want a substring, and the case where didn't and just want the rest of
    // the string.
    *endIndex = (index < currentString.size() ? index : std::string::npos);
    return currentString.substr(startIndex, index - startIndex);
}

// Check to make sure that the string only consists of digits
bool isOnlyDigits(StringData toCheck) {
    StringData digits("0123456789");
    for (StringData::const_iterator iterator = toCheck.begin(); iterator != toCheck.end();
         iterator++) {
        if (digits.find(*iterator) == std::string::npos) {
            return false;
        }
    }
    return true;
}

Status parseTimeZoneFromToken(StringData tzStr, int* tzAdjSecs) {
    *tzAdjSecs = 0;

    if (!tzStr.empty()) {
        if (tzStr[0] == 'Z') {
            if (tzStr.size() != 1) {
                StringBuilder sb;
                sb << "Found trailing characters in time zone specifier:  " << tzStr;
                return Status(ErrorCodes::BadValue, sb.str());
            }
        } else if (tzStr[0] == '+' || tzStr[0] == '-') {
            if (tzStr.size() != 5 || !isOnlyDigits(tzStr.substr(1, 4))) {
                StringBuilder sb;
                sb << "Time zone adjustment string should be four digits:  " << tzStr;
                return Status(ErrorCodes::BadValue, sb.str());
            }

            // Parse the hours component of the time zone offset.  Note that
            // parseNumberFromStringWithBase correctly handles the sign bit, so leave that in.
            StringData tzHoursStr = tzStr.substr(0, 3);
            int tzAdjHours = 0;
            Status status = parseNumberFromStringWithBase(tzHoursStr, 10, &tzAdjHours);
            if (!status.isOK()) {
                return status;
            }

            if (tzAdjHours < -23 || tzAdjHours > 23) {
                StringBuilder sb;
                sb << "Time zone hours adjustment out of range:  " << tzAdjHours;
                return Status(ErrorCodes::BadValue, sb.str());
            }

            StringData tzMinutesStr = tzStr.substr(3, 2);
            int tzAdjMinutes = 0;
            status = parseNumberFromStringWithBase(tzMinutesStr, 10, &tzAdjMinutes);
            if (!status.isOK()) {
                return status;
            }

            if (tzAdjMinutes < 0 || tzAdjMinutes > 59) {
                StringBuilder sb;
                sb << "Time zone minutes adjustment out of range:  " << tzAdjMinutes;
                return Status(ErrorCodes::BadValue, sb.str());
            }

            // Use the sign that parseNumberFromStringWithBase found to determine if we need to
            // flip the sign of our minutes component.  Also, we need to flip the sign of our
            // final result, because the offset passed in by the user represents how far off the
            // time they are giving us is from UTC, which means that we have to go the opposite
            // way to compensate and get the UTC time
            *tzAdjSecs =
                (-1) * ((tzAdjHours < 0 ? -1 : 1) * (tzAdjMinutes * 60) + (tzAdjHours * 60 * 60));

            // Disallow adjustiment of 24 hours or more in either direction (should be checked
            // above as the separate components of minutes and hours)
            fassert(17318, *tzAdjSecs > -86400 && *tzAdjSecs < 86400);
        } else {
            StringBuilder sb;
            sb << "Invalid time zone string:  \"" << tzStr
               << "\".  Found invalid character at the beginning of time "
               << "zone specifier: " << tzStr[0];
            return Status(ErrorCodes::BadValue, sb.str());
        }
    } else {
        return Status(ErrorCodes::BadValue, "Missing required time zone specifier for date");
    }

    return Status::OK();
}

Status parseMillisFromToken(StringData millisStr, int* resultMillis) {
    *resultMillis = 0;

    if (!millisStr.empty()) {
        if (millisStr.size() > 3 || !isOnlyDigits(millisStr)) {
            StringBuilder sb;
            sb << "Millisecond string should be at most three digits:  " << millisStr;
            return Status(ErrorCodes::BadValue, sb.str());
        }

        Status status = parseNumberFromStringWithBase(millisStr, 10, resultMillis);
        if (!status.isOK()) {
            return status;
        }

        // Treat the digits differently depending on how many there are.  1 digit = hundreds of
        // milliseconds, 2 digits = tens of milliseconds, 3 digits = milliseconds.
        int millisMagnitude = 1;
        if (millisStr.size() == 2) {
            millisMagnitude = 10;
        } else if (millisStr.size() == 1) {
            millisMagnitude = 100;
        }

        *resultMillis = *resultMillis * millisMagnitude;

        if (*resultMillis < 0 || *resultMillis > 1000) {
            StringBuilder sb;
            sb << "Millisecond out of range:  " << *resultMillis;
            return Status(ErrorCodes::BadValue, sb.str());
        }
    }

    return Status::OK();
}

Status parseTmFromTokens(StringData yearStr,
                         StringData monthStr,
                         StringData dayStr,
                         StringData hourStr,
                         StringData minStr,
                         StringData secStr,
                         std::tm* resultTm) {
    memset(resultTm, 0, sizeof(*resultTm));

    // Parse year
    if (yearStr.size() != 4 || !isOnlyDigits(yearStr)) {
        StringBuilder sb;
        sb << "Year string should be four digits:  " << yearStr;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    Status status = parseNumberFromStringWithBase(yearStr, 10, &resultTm->tm_year);
    if (!status.isOK()) {
        return status;
    }

    if (resultTm->tm_year < 1970 || resultTm->tm_year > 9999) {
        StringBuilder sb;
        sb << "Year out of range:  " << resultTm->tm_year;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    resultTm->tm_year -= 1900;

    // Parse month
    if (monthStr.size() != 2 || !isOnlyDigits(monthStr)) {
        StringBuilder sb;
        sb << "Month string should be two digits:  " << monthStr;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    status = parseNumberFromStringWithBase(monthStr, 10, &resultTm->tm_mon);
    if (!status.isOK()) {
        return status;
    }

    if (resultTm->tm_mon < 1 || resultTm->tm_mon > 12) {
        StringBuilder sb;
        sb << "Month out of range:  " << resultTm->tm_mon;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    resultTm->tm_mon -= 1;

    // Parse day
    if (dayStr.size() != 2 || !isOnlyDigits(dayStr)) {
        StringBuilder sb;
        sb << "Day string should be two digits:  " << dayStr;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    status = parseNumberFromStringWithBase(dayStr, 10, &resultTm->tm_mday);
    if (!status.isOK()) {
        return status;
    }

    if (resultTm->tm_mday < 1 || resultTm->tm_mday > 31) {
        StringBuilder sb;
        sb << "Day out of range:  " << resultTm->tm_mday;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    // Parse hour
    if (hourStr.size() != 2 || !isOnlyDigits(hourStr)) {
        StringBuilder sb;
        sb << "Hour string should be two digits:  " << hourStr;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    status = parseNumberFromStringWithBase(hourStr, 10, &resultTm->tm_hour);
    if (!status.isOK()) {
        return status;
    }

    if (resultTm->tm_hour < 0 || resultTm->tm_hour > 23) {
        StringBuilder sb;
        sb << "Hour out of range:  " << resultTm->tm_hour;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    // Parse minute
    if (minStr.size() != 2 || !isOnlyDigits(minStr)) {
        StringBuilder sb;
        sb << "Minute string should be two digits:  " << minStr;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    status = parseNumberFromStringWithBase(minStr, 10, &resultTm->tm_min);
    if (!status.isOK()) {
        return status;
    }

    if (resultTm->tm_min < 0 || resultTm->tm_min > 59) {
        StringBuilder sb;
        sb << "Minute out of range:  " << resultTm->tm_min;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    // Parse second if it exists
    if (secStr.empty()) {
        return Status::OK();
    }

    if (secStr.size() != 2 || !isOnlyDigits(secStr)) {
        StringBuilder sb;
        sb << "Second string should be two digits:  " << secStr;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    status = parseNumberFromStringWithBase(secStr, 10, &resultTm->tm_sec);
    if (!status.isOK()) {
        return status;
    }

    if (resultTm->tm_sec < 0 || resultTm->tm_sec > 59) {
        StringBuilder sb;
        sb << "Second out of range:  " << resultTm->tm_sec;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    return Status::OK();
}

Status parseTm(StringData dateString, std::tm* resultTm, int* resultMillis, int* tzAdjSecs) {
    size_t yearEnd = std::string::npos;
    size_t monthEnd = std::string::npos;
    size_t dayEnd = std::string::npos;
    size_t hourEnd = std::string::npos;
    size_t minEnd = std::string::npos;
    size_t secEnd = std::string::npos;
    size_t millisEnd = std::string::npos;
    size_t tzEnd = std::string::npos;
    StringData yearStr, monthStr, dayStr, hourStr, minStr, secStr, millisStr, tzStr;

    yearStr = getNextToken(dateString, "-", 0, &yearEnd);
    monthStr = getNextToken(dateString, "-", yearEnd + 1, &monthEnd);
    dayStr = getNextToken(dateString, "T", monthEnd + 1, &dayEnd);
    hourStr = getNextToken(dateString, ":", dayEnd + 1, &hourEnd);
    minStr = getNextToken(dateString, ":+-Z", hourEnd + 1, &minEnd);

    // Only look for seconds if the character we matched for the end of the minutes token is a
    // colon
    if (minEnd != std::string::npos && dateString[minEnd] == ':') {
        // Make sure the string doesn't end with ":"
        if (minEnd == dateString.size() - 1) {
            StringBuilder sb;
            sb << "Invalid date:  " << dateString << ".  Ends with \"" << dateString[minEnd]
               << "\" character";
            return Status(ErrorCodes::BadValue, sb.str());
        }

        secStr = getNextToken(dateString, ".+-Z", minEnd + 1, &secEnd);

        // Make sure we actually got something for seconds, since here we know they are expected
        if (secStr.empty()) {
            StringBuilder sb;
            sb << "Missing seconds in date: " << dateString;
            return Status(ErrorCodes::BadValue, sb.str());
        }
    }

    // Only look for milliseconds if the character we matched for the end of the seconds token
    // is a period
    if (secEnd != std::string::npos && dateString[secEnd] == '.') {
        // Make sure the string doesn't end with "."
        if (secEnd == dateString.size() - 1) {
            StringBuilder sb;
            sb << "Invalid date:  " << dateString << ".  Ends with \"" << dateString[secEnd]
               << "\" character";
            return Status(ErrorCodes::BadValue, sb.str());
        }

        millisStr = getNextToken(dateString, "+-Z", secEnd + 1, &millisEnd);

        // Make sure we actually got something for millis, since here we know they are expected
        if (millisStr.empty()) {
            StringBuilder sb;
            sb << "Missing seconds in date: " << dateString;
            return Status(ErrorCodes::BadValue, sb.str());
        }
    }

    // Now look for the time zone specifier depending on which prefix of the time we provided
    if (millisEnd != std::string::npos) {
        tzStr = getNextToken(dateString, "", millisEnd, &tzEnd);
    } else if (secEnd != std::string::npos && dateString[secEnd] != '.') {
        tzStr = getNextToken(dateString, "", secEnd, &tzEnd);
    } else if (minEnd != std::string::npos && dateString[minEnd] != ':') {
        tzStr = getNextToken(dateString, "", minEnd, &tzEnd);
    }

    Status status = parseTmFromTokens(yearStr, monthStr, dayStr, hourStr, minStr, secStr, resultTm);
    if (!status.isOK()) {
        return status;
    }

    status = parseTimeZoneFromToken(tzStr, tzAdjSecs);
    if (!status.isOK()) {
        return status;
    }

    status = parseMillisFromToken(millisStr, resultMillis);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

}  // namespace

StatusWith<Date_t> dateFromISOString(StringData dateString) {
    std::tm theTime;
    int millis = 0;
    int tzAdjSecs = 0;
    Status status = parseTm(dateString, &theTime, &millis, &tzAdjSecs);
    if (!status.isOK()) {
        return StatusWith<Date_t>(ErrorCodes::BadValue, status.reason());
    }

    unsigned long long resultMillis = 0;

#if defined(_WIN32)
    SYSTEMTIME dateStruct;
    dateStruct.wMilliseconds = millis;
    dateStruct.wSecond = theTime.tm_sec;
    dateStruct.wMinute = theTime.tm_min;
    dateStruct.wHour = theTime.tm_hour;
    dateStruct.wDay = theTime.tm_mday;
    dateStruct.wDayOfWeek = -1; /* ignored */
    dateStruct.wMonth = theTime.tm_mon + 1;
    dateStruct.wYear = theTime.tm_year + 1900;

    // Output parameter for SystemTimeToFileTime
    FILETIME fileTime;

    // the wDayOfWeek member of SYSTEMTIME is ignored by this function
    if (SystemTimeToFileTime(&dateStruct, &fileTime) == 0) {
        StringBuilder sb;
        sb << "Error converting Windows system time to file time for date:  " << dateString
           << ".  Error code:  " << GetLastError();
        return StatusWith<Date_t>(ErrorCodes::BadValue, sb.str());
    }

    // The Windows FILETIME structure contains two parts of a 64-bit value representing the
    // number of 100-nanosecond intervals since January 1, 1601
    unsigned long long windowsTimeOffset =
        (static_cast<unsigned long long>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;

    // There are 11644473600 seconds between the unix epoch and the windows epoch
    // 100-nanoseconds = milliseconds * 10000
    unsigned long long epochDifference = 11644473600000 * 10000;

    // removes the diff between 1970 and 1601
    windowsTimeOffset -= epochDifference;

    // 1 milliseconds = 1000000 nanoseconds = 10000 100-nanosecond intervals
    resultMillis = windowsTimeOffset / 10000;
#else
    struct tm dateStruct = {0};
    dateStruct.tm_sec = theTime.tm_sec;
    dateStruct.tm_min = theTime.tm_min;
    dateStruct.tm_hour = theTime.tm_hour;
    dateStruct.tm_mday = theTime.tm_mday;
    dateStruct.tm_mon = theTime.tm_mon;
    dateStruct.tm_year = theTime.tm_year;
    dateStruct.tm_wday = 0;
    dateStruct.tm_yday = 0;

    resultMillis = (1000 * static_cast<unsigned long long>(timegm(&dateStruct))) + millis;
#endif

    resultMillis += (tzAdjSecs * 1000);

    if (resultMillis > static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
        return {ErrorCodes::BadValue, str::stream() << dateString << " is too far in the future"};
    }
    return Date_t::fromMillisSinceEpoch(static_cast<long long>(resultMillis));
}

#undef MONGO_ISO_DATE_FMT_NO_TZ

std::string Date_t::toString() const {
    if (isFormattable()) {
        return dateToISOStringLocal(*this);
    } else {
        return str::stream() << "Date(" << millis << ")";
    }
}

time_t Date_t::toTimeT() const {
    const auto secs = millis / 1000;
    verify(secs >= std::numeric_limits<time_t>::min());
    verify(secs <= std::numeric_limits<time_t>::max());
    return secs;
}

boost::gregorian::date currentDate() {
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    return now.date();
}

// parses time of day in "hh:mm" format assuming 'hh' is 00-23
bool toPointInTime(const string& str, boost::posix_time::ptime* timeOfDay) {
    int hh = 0;
    int mm = 0;
    if (2 != sscanf(str.c_str(), "%d:%d", &hh, &mm)) {
        return false;
    }

    // verify that time is well formed
    if ((hh / 24) || (mm / 60)) {
        return false;
    }

    boost::posix_time::ptime res(currentDate(),
                                 boost::posix_time::hours(hh) + boost::posix_time::minutes(mm));
    *timeOfDay = res;
    return true;
}

void sleepsecs(int s) {
    stdx::this_thread::sleep_for(Seconds(s).toSystemDuration());
}

void sleepmillis(long long s) {
    stdx::this_thread::sleep_for(Milliseconds(s).toSystemDuration());
}
void sleepmicros(long long s) {
    stdx::this_thread::sleep_for(Microseconds(s).toSystemDuration());
}

void Backoff::nextSleepMillis() {
    // Get the current time
    unsigned long long currTimeMillis = curTimeMillis64();

    int lastSleepMillis = _lastSleepMillis;

    if (_lastErrorTimeMillis == 0 || _lastErrorTimeMillis > currTimeMillis /* VM bugs exist */)
        _lastErrorTimeMillis = currTimeMillis;
    unsigned long long lastErrorTimeMillis = _lastErrorTimeMillis;
    _lastErrorTimeMillis = currTimeMillis;

    lastSleepMillis = getNextSleepMillis(lastSleepMillis, currTimeMillis, lastErrorTimeMillis);

    // Store the last slept time
    _lastSleepMillis = lastSleepMillis;
    sleepmillis(lastSleepMillis);
}

int Backoff::getNextSleepMillis(int lastSleepMillis,
                                unsigned long long currTimeMillis,
                                unsigned long long lastErrorTimeMillis) const {
    // Backoff logic

    // Get the time since the last error
    unsigned long long timeSinceLastErrorMillis = currTimeMillis - lastErrorTimeMillis;

    // Makes the cast below safe
    verify(_resetAfterMillis >= 0);

    // If we haven't seen another error recently (3x the max wait time), reset our
    // wait counter.
    if (timeSinceLastErrorMillis > (unsigned)(_resetAfterMillis))
        lastSleepMillis = 0;

    // Makes the test below sane
    verify(_maxSleepMillis > 0);

    // Wait a power of two millis
    if (lastSleepMillis == 0)
        lastSleepMillis = 1;
    else
        lastSleepMillis = std::min(lastSleepMillis * 2, _maxSleepMillis);

    return lastSleepMillis;
}

extern long long jsTime_virtual_skew;
extern boost::thread_specific_ptr<long long> jsTime_virtual_thread_skew;

// DO NOT TOUCH except for testing
void jsTimeVirtualSkew(long long skew) {
    jsTime_virtual_skew = skew;
}
long long getJSTimeVirtualSkew() {
    return jsTime_virtual_skew;
}

void jsTimeVirtualThreadSkew(long long skew) {
    jsTime_virtual_thread_skew.reset(new long long(skew));
}
long long getJSTimeVirtualThreadSkew() {
    if (jsTime_virtual_thread_skew.get()) {
        return *(jsTime_virtual_thread_skew.get());
    } else
        return 0;
}

/** Date_t is milliseconds since epoch */
Date_t jsTime() {
    return Date_t::now() + Milliseconds(getJSTimeVirtualThreadSkew()) +
        Milliseconds(getJSTimeVirtualSkew());
}

#ifdef _WIN32  // no gettimeofday on windows
unsigned long long curTimeMillis64() {
    using stdx::chrono::system_clock;
    return static_cast<unsigned long long>(
        durationCount<Milliseconds>(system_clock::now() - system_clock::from_time_t(0)));
}

static unsigned long long getFiletime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return *reinterpret_cast<unsigned long long*>(&ft);
}

static unsigned long long getPerfCounter() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

static unsigned long long baseFiletime = 0;
static unsigned long long basePerfCounter = 0;
static unsigned long long resyncInterval = 0;
static SimpleMutex _curTimeMicros64ReadMutex;
static SimpleMutex _curTimeMicros64ResyncMutex;

typedef WINBASEAPI VOID(WINAPI* pGetSystemTimePreciseAsFileTime)(
    _Out_ LPFILETIME lpSystemTimeAsFileTime);

static pGetSystemTimePreciseAsFileTime GetSystemTimePreciseAsFileTimeFunc;

MONGO_INITIALIZER(Init32TimeSupport)(InitializerContext*) {
    HINSTANCE kernelLib = LoadLibraryA("kernel32.dll");
    if (kernelLib) {
        GetSystemTimePreciseAsFileTimeFunc = reinterpret_cast<pGetSystemTimePreciseAsFileTime>(
            GetProcAddress(kernelLib, "GetSystemTimePreciseAsFileTime"));
    }

    return Status::OK();
}

static unsigned long long resyncTime() {
    stdx::lock_guard<SimpleMutex> lkResync(_curTimeMicros64ResyncMutex);
    unsigned long long ftOld;
    unsigned long long ftNew;
    ftOld = ftNew = getFiletime();
    do {
        ftNew = getFiletime();
    } while (ftOld == ftNew);  // wait for filetime to change

    unsigned long long newPerfCounter = getPerfCounter();

    // Make sure that we use consistent values for baseFiletime and basePerfCounter.
    //
    stdx::lock_guard<SimpleMutex> lkRead(_curTimeMicros64ReadMutex);
    baseFiletime = ftNew;
    basePerfCounter = newPerfCounter;
    resyncInterval = 60 * SystemTickSource::get()->getTicksPerSecond();
    return newPerfCounter;
}

unsigned long long curTimeMicros64() {
    // Windows 8/2012 & later support a <1us time function
    if (GetSystemTimePreciseAsFileTimeFunc != NULL) {
        FILETIME time;
        GetSystemTimePreciseAsFileTimeFunc(&time);
        return boost::date_time::winapi::file_time_to_microseconds(time);
    }

    // Get a current value for QueryPerformanceCounter; if it is not time to resync we will
    // use this value.
    //
    unsigned long long perfCounter = getPerfCounter();

    // Periodically resync the timer so that we don't let timer drift accumulate.  Testing
    // suggests that we drift by about one microsecond per minute, so resynching once per
    // minute should keep drift to no more than one microsecond.
    //
    if ((perfCounter - basePerfCounter) > resyncInterval) {
        perfCounter = resyncTime();
    }

    // Make sure that we use consistent values for baseFiletime and basePerfCounter.
    //
    stdx::lock_guard<SimpleMutex> lkRead(_curTimeMicros64ReadMutex);

    // Compute the current time in FILETIME format by adding our base FILETIME and an offset
    // from that time based on QueryPerformanceCounter.  The math is (logically) to compute the
    // fraction of a second elapsed since 'baseFiletime' by taking the difference in ticks
    // and dividing by the tick frequency, then scaling this fraction up to units of 100
    // nanoseconds to match the FILETIME format.  We do the multiplication first to avoid
    // truncation while using only integer instructions.
    //
    unsigned long long computedTime = baseFiletime +
        ((perfCounter - basePerfCounter) * 10 * 1000 * 1000) /
            SystemTickSource::get()->getTicksPerSecond();

    // Convert the computed FILETIME into microseconds since the Unix epoch (1/1/1970).
    //
    return boost::date_time::winapi::file_time_to_microseconds(computedTime);
}

#else
#include <sys/time.h>
unsigned long long curTimeMillis64() {
    timeval tv;
    gettimeofday(&tv, NULL);
    return ((unsigned long long)tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

unsigned long long curTimeMicros64() {
    timeval tv;
    gettimeofday(&tv, NULL);
    return (((unsigned long long)tv.tv_sec) * 1000 * 1000) + tv.tv_usec;
}
#endif

}  // namespace mongo
