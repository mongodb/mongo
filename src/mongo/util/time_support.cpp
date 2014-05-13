/*    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/time_support.h"

#include <cstdio>
#include <string>
#include <iostream>
#include <boost/thread/thread.hpp>
#include <boost/thread/tss.hpp>
#include <boost/thread/xtime.hpp>

#include "mongo/base/parse_number.h"
#include "mongo/bson/util/builder.h"
#include "mongo/platform/cstdint.h"
#include "mongo/util/assert_util.h"

#ifdef _WIN32
#include <boost/date_time/filetime_functions.hpp>
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/timer.h"

// NOTE(schwerin): MSVC's _snprintf is not a drop-in replacement for C99's snprintf().  In
// particular, when the target buffer is too small, behaviors differ.  Consult the documentation
// from MSDN and form the BSD or Linux man pages before using.
#define snprintf _snprintf
#endif

namespace mongo {

namespace {
    const bool isTimeTSmall =
        (sizeof(time_t) == sizeof(int32_t)) && std::numeric_limits<time_t>::is_signed;

    // Some of the library functions we use do not support dates past this.  See SERVER-13760.
    Date_t getMaxFormatableDate() {
        StatusWith<Date_t> mfd = dateFromISOString(isTimeTSmall ?
                                                   "2038-01-19T03:14:07Z" :
                                                   "3000-12-31T23:59:59Z");
        fassert(17486, mfd.getStatus());
        return mfd.getValue();
    }
} // namespace

    const Date_t Date_t::maxFormatableDate = getMaxFormatableDate();

    // jsTime_virtual_skew is just for testing. a test command manipulates it.
    long long jsTime_virtual_skew = 0;
    boost::thread_specific_ptr<long long> jsTime_virtual_thread_skew;

    using std::string;

    void time_t_to_Struct(time_t t, struct tm * buf , bool local) {
#if defined(_WIN32)
        if ( local )
            localtime_s( buf , &t );
        else
            gmtime_s(buf, &t);
#else
        if ( local )
            localtime_r(&t, buf);
        else
            gmtime_r(&t, buf);
#endif
    }

    std::string time_t_to_String(time_t t) {
        char buf[64];
#if defined(_WIN32)
        ctime_s(buf, sizeof(buf), &t);
#else
        ctime_r(&t, buf);
#endif
        buf[24] = 0; // don't want the \n
        return buf;
    }

    std::string time_t_to_String_short(time_t t) {
        char buf[64];
#if defined(_WIN32)
        ctime_s(buf, sizeof(buf), &t);
#else
        ctime_r(&t, buf);
#endif
        buf[19] = 0;
        if( buf[0] && buf[1] && buf[2] && buf[3] )
            return buf + 4; // skip day of week
        return buf;
    }


    // uses ISO 8601 dates without trailing Z
    // colonsOk should be false when creating filenames
    string terseCurrentTime(bool colonsOk) {
        struct tm t;
        time_t_to_Struct( time(0) , &t );

        const char* fmt = (colonsOk ? "%Y-%m-%dT%H:%M:%S" : "%Y-%m-%dT%H-%M-%S");
        char buf[32];
        fassert(16226, strftime(buf, sizeof(buf), fmt, &t) == 19);
        return buf;
    }

#define MONGO_ISO_DATE_FMT_NO_TZ "%Y-%m-%dT%H:%M:%S"
    string timeToISOString(time_t time) {
        struct tm t;
        time_t_to_Struct( time, &t );

        const char* fmt = MONGO_ISO_DATE_FMT_NO_TZ "Z";
        char buf[32];
        fassert(16227, strftime(buf, sizeof(buf), fmt, &t) == 20);
        return buf;
    }

namespace {
    struct DateStringBuffer {
        static const int dataCapacity = 64;
        char data[dataCapacity];
        int size;
    };

    void _dateToISOString(Date_t date, bool local, DateStringBuffer* result) {
        invariant(date.millis <= Date_t::maxFormatableDate);
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
            if (t.tm_isdst) msTimeZone -= 3600;
            const bool tzIsWestOfUTC = msTimeZone > 0;
            const long tzOffsetSeconds = msTimeZone* (tzIsWestOfUTC ? 1 : -1);
            const long tzOffsetHoursPart = tzOffsetSeconds / 3600;
            const long tzOffsetMinutesPart = (tzOffsetSeconds / 60) % 60;
            snprintf(cur, localTzSubstrLen + 1, "%c%02ld%02ld",
                     tzIsWestOfUTC ? '-' : '+',
                     tzOffsetHoursPart,
                     tzOffsetMinutesPart);
#else
            strftime(cur, bufRemaining, "%z", &t);
#endif
            cur += localTzSubstrLen;
        }
        else {
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
        snprintf(milliSecStr, millisSubstrLen + 1, ".%03d", static_cast<int32_t>(date.asInt64() % 1000));
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
    StringData getNextToken(const StringData& currentString,
                            const StringData& terminalChars,
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
    bool isOnlyDigits(const StringData& toCheck) {
        StringData digits("0123456789");
        for (StringData::const_iterator iterator = toCheck.begin();
             iterator != toCheck.end(); iterator++) {
            if (digits.find(*iterator) == std::string::npos) {
                return false;
            }
        }
        return true;
    }

    Status parseTimeZoneFromToken(const StringData& tzStr, int* tzAdjSecs) {

        *tzAdjSecs = 0;

        if (!tzStr.empty()) {
            if (tzStr[0] == 'Z') {
                if (tzStr.size() != 1) {
                    StringBuilder sb;
                    sb << "Found trailing characters in time zone specifier:  " << tzStr;
                    return Status(ErrorCodes::BadValue, sb.str());
                }
            }
            else if (tzStr[0] == '+' || tzStr[0] == '-') {
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
                *tzAdjSecs = (-1) * ((tzAdjHours < 0 ? -1 : 1) * (tzAdjMinutes * 60) +
                                     (tzAdjHours * 60 * 60));

                // Disallow adjustiment of 24 hours or more in either direction (should be checked
                // above as the separate components of minutes and hours)
                fassert(17318, *tzAdjSecs > -86400 && *tzAdjSecs < 86400);
            }
            else {
                StringBuilder sb;
                sb << "Invalid time zone string:  \"" << tzStr
                   << "\".  Found invalid character at the beginning of time "
                   << "zone specifier: " << tzStr[0];
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
        else {
            return Status(ErrorCodes::BadValue, "Missing required time zone specifier for date");
        }

        return Status::OK();
    }

    Status parseMillisFromToken(
            const StringData& millisStr,
            int* resultMillis) {

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
            }
            else if (millisStr.size() == 1) {
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

    Status parseTmFromTokens(
            const StringData& yearStr,
            const StringData& monthStr,
            const StringData& dayStr,
            const StringData& hourStr,
            const StringData& minStr,
            const StringData& secStr,
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

    Status parseTm(const StringData& dateString,
                   std::tm* resultTm,
                   int* resultMillis,
                   int* tzAdjSecs) {
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
        }
        else if (secEnd != std::string::npos && dateString[secEnd] != '.') {
            tzStr = getNextToken(dateString, "", secEnd, &tzEnd);
        }
        else if (minEnd != std::string::npos && dateString[minEnd] != ':') {
            tzStr = getNextToken(dateString, "", minEnd, &tzEnd);
        }

        Status status = parseTmFromTokens(yearStr, monthStr, dayStr, hourStr, minStr, secStr,
                                          resultTm);
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

    StatusWith<Date_t> dateFromISOString(const StringData& dateString) {
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
            (static_cast<unsigned long long>(fileTime.dwHighDateTime) << 32) |
             fileTime.dwLowDateTime;

        // There are 11644473600 seconds between the unix epoch and the windows epoch
        // 100-nanoseconds = milliseconds * 10000
        unsigned long long epochDifference = 11644473600000 * 10000;

        // removes the diff between 1970 and 1601
        windowsTimeOffset -= epochDifference;

        // 1 milliseconds = 1000000 nanoseconds = 10000 100-nanosecond intervals
        resultMillis = windowsTimeOffset / 10000;
#else
        struct tm dateStruct = { 0 };
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

        return StatusWith<Date_t>(resultMillis);
    }

#undef MONGO_ISO_DATE_FMT_NO_TZ

    void Date_t::toTm(tm* buf) {
        time_t dtime = toTimeT();
#if defined(_WIN32)
        gmtime_s(buf, &dtime);
#else
        gmtime_r(&dtime, buf);
#endif
    }

    std::string Date_t::toString() const {
        return time_t_to_String(toTimeT());
    }

    time_t Date_t::toTimeT() const {
        verify((long long)millis >= 0); // TODO when millis is signed, delete 
        verify(((long long)millis/1000) < (std::numeric_limits<time_t>::max)());
        return millis / 1000;
    }

    boost::gregorian::date currentDate() {
        boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
        return now.date();
    }

    // parses time of day in "hh:mm" format assuming 'hh' is 00-23
    bool toPointInTime( const string& str , boost::posix_time::ptime* timeOfDay ) {
        int hh = 0;
        int mm = 0;
        if ( 2 != sscanf( str.c_str() , "%d:%d" , &hh , &mm ) ) {
            return false;
        }

        // verify that time is well formed
        if ( ( hh / 24 ) || ( mm / 60 ) ) {
            return false;
        }

        boost::posix_time::ptime res( currentDate() , boost::posix_time::hours( hh ) + boost::posix_time::minutes( mm ) );
        *timeOfDay = res;
        return true;
    }

#if defined(_WIN32)
    void sleepsecs(int s) {
        Sleep(s*1000);
    }
    void sleepmillis(long long s) {
        fassert(16228, s <= 0xffffffff );
        Sleep((DWORD) s);
    }
    void sleepmicros(long long s) {
        if ( s <= 0 )
            return;
        boost::xtime xt;
        boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
        xt.sec += (int)( s / 1000000 );
        xt.nsec += (int)(( s % 1000000 ) * 1000);
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }
        boost::thread::sleep(xt);
    }
#elif defined(__sunos__)
    void sleepsecs(int s) {
        boost::xtime xt;
        boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
        xt.sec += s;
        boost::thread::sleep(xt);
    }
    void sleepmillis(long long s) {
        boost::xtime xt;
        boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
        xt.sec += (int)( s / 1000 );
        xt.nsec += (int)(( s % 1000 ) * 1000000);
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }
        boost::thread::sleep(xt);
    }
    void sleepmicros(long long s) {
        if ( s <= 0 )
            return;
        boost::xtime xt;
        boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
        xt.sec += (int)( s / 1000000 );
        xt.nsec += (int)(( s % 1000000 ) * 1000);
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }
        boost::thread::sleep(xt);
    }
#else
    void sleepsecs(int s) {
        struct timespec t;
        t.tv_sec = s;
        t.tv_nsec = 0;
        if ( nanosleep( &t , 0 ) ) {
            std::cout << "nanosleep failed" << std::endl;
        }
    }
    void sleepmicros(long long s) {
        if ( s <= 0 )
            return;
        struct timespec t;
        t.tv_sec = (int)(s / 1000000);
        t.tv_nsec = 1000 * ( s % 1000000 );
        struct timespec out;
        if ( nanosleep( &t , &out ) ) {
            std::cout << "nanosleep failed" << std::endl;
        }
    }
    void sleepmillis(long long s) {
        sleepmicros( s * 1000 );
    }
#endif

    void Backoff::nextSleepMillis(){

        // Get the current time
        unsigned long long currTimeMillis = curTimeMillis64();

        int lastSleepMillis = _lastSleepMillis;

        if( _lastErrorTimeMillis == 0 || _lastErrorTimeMillis > currTimeMillis /* VM bugs exist */ )
            _lastErrorTimeMillis = currTimeMillis;
        unsigned long long lastErrorTimeMillis = _lastErrorTimeMillis;
        _lastErrorTimeMillis = currTimeMillis;

        lastSleepMillis = getNextSleepMillis(lastSleepMillis, currTimeMillis, lastErrorTimeMillis);

        // Store the last slept time
        _lastSleepMillis = lastSleepMillis;
        sleepmillis( lastSleepMillis );
    }

    int Backoff::getNextSleepMillis(int lastSleepMillis, unsigned long long currTimeMillis,
                                    unsigned long long lastErrorTimeMillis) const {
        // Backoff logic

        // Get the time since the last error
        unsigned long long timeSinceLastErrorMillis = currTimeMillis - lastErrorTimeMillis;

        // Makes the cast below safe
        verify( _resetAfterMillis >= 0 );

        // If we haven't seen another error recently (3x the max wait time), reset our
        // wait counter.
        if( timeSinceLastErrorMillis > (unsigned)( _resetAfterMillis ) ) lastSleepMillis = 0;

        // Makes the test below sane
        verify( _maxSleepMillis > 0 );

        // Wait a power of two millis
        if( lastSleepMillis == 0 ) lastSleepMillis = 1;
        else lastSleepMillis = std::min( lastSleepMillis * 2, _maxSleepMillis );

        return lastSleepMillis;
    }

    extern long long jsTime_virtual_skew;
    extern boost::thread_specific_ptr<long long> jsTime_virtual_thread_skew;

    // DO NOT TOUCH except for testing
    void jsTimeVirtualSkew( long long skew ){
        jsTime_virtual_skew = skew;
    }
    long long getJSTimeVirtualSkew(){
        return jsTime_virtual_skew;
    }

    void jsTimeVirtualThreadSkew( long long skew ){
        jsTime_virtual_thread_skew.reset(new long long(skew));
    }
    long long getJSTimeVirtualThreadSkew(){
        if(jsTime_virtual_thread_skew.get()){
            return *(jsTime_virtual_thread_skew.get());
        }
        else return 0;
    }

    /** Date_t is milliseconds since epoch */
    Date_t jsTime();

    /** warning this will wrap */
    unsigned curTimeMicros();

    unsigned long long curTimeMicros64();
#ifdef _WIN32 // no gettimeofday on windows
    unsigned long long curTimeMillis64() {
        boost::xtime xt;
        boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
        return ((unsigned long long)xt.sec) * 1000 + xt.nsec / 1000000;
    }
    Date_t jsTime() {
        boost::xtime xt;
        boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
        unsigned long long t = xt.nsec / 1000000;
        return ((unsigned long long) xt.sec * 1000) + t + getJSTimeVirtualSkew() + getJSTimeVirtualThreadSkew();
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
    static SimpleMutex _curTimeMicros64ReadMutex("curTimeMicros64Read");
    static SimpleMutex _curTimeMicros64ResyncMutex("curTimeMicros64Resync");

    static unsigned long long resyncTime() {
        SimpleMutex::scoped_lock lkResync(_curTimeMicros64ResyncMutex);
        unsigned long long ftOld;
        unsigned long long ftNew;
        ftOld = ftNew = getFiletime();
        do {
            ftNew = getFiletime();
        } while (ftOld == ftNew);   // wait for filetime to change

        unsigned long long newPerfCounter = getPerfCounter();

        // Make sure that we use consistent values for baseFiletime and basePerfCounter.
        //
        SimpleMutex::scoped_lock lkRead(_curTimeMicros64ReadMutex);
        baseFiletime = ftNew;
        basePerfCounter = newPerfCounter;
        resyncInterval = 60 * Timer::_countsPerSecond;
        return newPerfCounter;
    }

    unsigned long long curTimeMicros64() {

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
        SimpleMutex::scoped_lock lkRead(_curTimeMicros64ReadMutex);

        // Compute the current time in FILETIME format by adding our base FILETIME and an offset
        // from that time based on QueryPerformanceCounter.  The math is (logically) to compute the
        // fraction of a second elapsed since 'baseFiletime' by taking the difference in ticks
        // and dividing by the tick frequency, then scaling this fraction up to units of 100
        // nanoseconds to match the FILETIME format.  We do the multiplication first to avoid
        // truncation while using only integer instructions.
        //
        unsigned long long computedTime = baseFiletime +
                ((perfCounter - basePerfCounter) * 10 * 1000 * 1000) / Timer::_countsPerSecond;

        // Convert the computed FILETIME into microseconds since the Unix epoch (1/1/1970).
        //
        return boost::date_time::winapi::file_time_to_microseconds(computedTime);
    }

    unsigned curTimeMicros() {
        boost::xtime xt;
        boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
        unsigned t = xt.nsec / 1000;
        unsigned secs = xt.sec % 1024;
        return secs*1000000 + t;
    }
#else
#  include <sys/time.h>
    unsigned long long curTimeMillis64() {
        timeval tv;
        gettimeofday(&tv, NULL);
        return ((unsigned long long)tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    }
    Date_t jsTime() {
        timeval tv;
        gettimeofday(&tv, NULL);
        unsigned long long t = tv.tv_usec / 1000;
        return ((unsigned long long) tv.tv_sec * 1000) + t + getJSTimeVirtualSkew() + getJSTimeVirtualThreadSkew();
    }
    unsigned long long curTimeMicros64() {
        timeval tv;
        gettimeofday(&tv, NULL);
        return (((unsigned long long) tv.tv_sec) * 1000*1000) + tv.tv_usec;
    }
    unsigned curTimeMicros() {
        timeval tv;
        gettimeofday(&tv, NULL);
        unsigned secs = tv.tv_sec % 1024;
        return secs*1000*1000 + tv.tv_usec;
    }
#endif

}  // namespace mongo
