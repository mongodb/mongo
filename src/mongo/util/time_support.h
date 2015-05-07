// @file time_support.h

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

#pragma once

#include <iosfwd>
#include <ctime>
#include <string>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/thread/xtime.hpp>
#include <boost/version.hpp>

#include "mongo/base/status_with.h"
#include "mongo/stdx/chrono.h"

namespace mongo {

    template <typename Allocator> class StringBuilderImpl;

    using Microseconds = stdx::chrono::microseconds;
    using Milliseconds = stdx::chrono::milliseconds;
    using Seconds = stdx::chrono::seconds;
    using Minutes = stdx::chrono::minutes;
    using stdx::chrono::duration_cast;

    void time_t_to_Struct(time_t t, struct tm * buf , bool local = false );
    std::string time_t_to_String(time_t t);
    std::string time_t_to_String_short(time_t t);

    //
    // Operators for putting durations to streams.
    //

    std::ostream& operator<<(std::ostream& os, Microseconds us);
    std::ostream& operator<<(std::ostream& os, Milliseconds ms);
    std::ostream& operator<<(std::ostream& os, Seconds s);

    template <typename Allocator>
    StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Microseconds us);

    template <typename Allocator>
    StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Milliseconds ms);

    template <typename Allocator>
    StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Seconds s);

    /**
     * Convenience method for reading the count of a duration with specified units.
     *
     * Use when logging or comparing to integers, to ensure that you're using
     * the units you intend.
     *
     * E.g., log() << durationCount<Seconds>(some duration) << " seconds";
     */
    template <typename DOut, typename DIn> long long durationCount(DIn d) {
        return duration_cast<DOut>(d).count();
    }

    class Date_t {
    public:
        static Date_t max();
        static Date_t now();

        static Date_t fromMillisSinceEpoch(long long m) {
            return Date_t(m);
        }

        template <typename Duration>
        static Date_t fromDurationSinceEpoch(Duration d) {
            return fromMillisSinceEpoch(durationCount<Milliseconds>(d));
        }

        Date_t(): millis(0) {}

        void toTm (tm *buf);
        std::string toString() const;
        time_t toTimeT() const;
        int64_t asInt64() const {
            return toMillisSinceEpoch();
        }
        unsigned long long toULL() const { return static_cast<unsigned long long>(asInt64()); }
        Milliseconds toDurationSinceEpoch() const { return Milliseconds(toMillisSinceEpoch()); }
        long long toMillisSinceEpoch() const { return static_cast<long long>(millis); }
        bool isFormatable() const;

        template <typename Duration>
        Date_t& operator+=(Duration d) {
            millis += duration_cast<Milliseconds>(d).count();
            return *this;
        }

        template <typename Duration>
        Date_t& operator-=(Duration d) {
            return *this += (-d);
        }

        template <typename Duration>
        Date_t operator+(Duration d) const {
            Date_t result = *this;
            result += d;
            return result;
        }

        template <typename Duration>
        Date_t operator-(Duration d) const {
            Date_t result = *this;
            result -= d;
            return result;
        }

        Milliseconds operator-(Date_t other) const { return Milliseconds(millis - other.millis); }

        bool operator==(Date_t other) const {
            return toDurationSinceEpoch() == other.toDurationSinceEpoch();
        }

        bool operator!=(Date_t other) const { return !(*this == other); }

        bool operator<(Date_t other) const {
            return toDurationSinceEpoch() < other.toDurationSinceEpoch();
        }

        bool operator>(Date_t other) const {
            return toDurationSinceEpoch() > other.toDurationSinceEpoch();
        }

        bool operator<=(Date_t other) const {
            return !(*this > other);
        }

        bool operator>=(Date_t other) const {
            return !(*this < other);
        }

    private:
        Date_t(long long m): millis(m) {}

        long long millis;
    };

    // uses ISO 8601 dates without trailing Z
    // colonsOk should be false when creating filenames
    std::string terseCurrentTime(bool colonsOk=true);

    /**
     * Formats "time" according to the ISO 8601 extended form standard, including date,
     * and time, in the UTC timezone.
     *
     * Sample format: "2013-07-23T18:42:14Z"
     */
    std::string timeToISOString(time_t time);

    /**
     * Formats "date" according to the ISO 8601 extended form standard, including date,
     * and time with milliseconds decimal component, in the UTC timezone.
     *
     * Sample format: "2013-07-23T18:42:14.072Z"
     */
    std::string dateToISOStringUTC(Date_t date);

    /**
     * Formats "date" according to the ISO 8601 extended form standard, including date,
     * and time with milliseconds decimal component, in the local timezone.
     *
     * Sample format: "2013-07-23T18:42:14.072-05:00"
     */
    std::string dateToISOStringLocal(Date_t date);

    /**
     * Formats "date" in fixed width in the local time zone.
     *
     * Sample format: "Wed Oct 31 13:34:47.996"
     */
    std::string dateToCtimeString(Date_t date);

    /**
     * Parses a Date_t from an ISO 8601 std::string representation.
     *
     * Sample formats: "2013-07-23T18:42:14.072-05:00"
     *                 "2013-07-23T18:42:14.072Z"
     *
     * Local times are currently not supported.
     */
    StatusWith<Date_t> dateFromISOString(StringData dateString);

    /**
     * Like dateToISOStringUTC, except outputs to a std::ostream.
     */
    void outputDateAsISOStringUTC(std::ostream& os, Date_t date);

    /**
     * Like dateToISOStringLocal, except outputs to a std::ostream.
     */
    void outputDateAsISOStringLocal(std::ostream& os, Date_t date);

    /**
     * Like dateToCtimeString, except outputs to a std::ostream.
     */
    void outputDateAsCtime(std::ostream& os, Date_t date);

    boost::gregorian::date currentDate();

    // parses time of day in "hh:mm" format assuming 'hh' is 00-23
    bool toPointInTime( const std::string& str , boost::posix_time::ptime* timeOfDay );

    void sleepsecs(int s);
    void sleepmillis(long long ms);
    void sleepmicros(long long micros);

    class Backoff {
    public:

        Backoff( int maxSleepMillis, int resetAfter ) :
            _maxSleepMillis( maxSleepMillis ),
            _resetAfterMillis( maxSleepMillis + resetAfter ), // Don't reset < the max sleep
            _lastSleepMillis( 0 ),
            _lastErrorTimeMillis( 0 )
        {}

        void nextSleepMillis();

        /**
         * testing-only function. used in dbtests/basictests.cpp
         */
        int getNextSleepMillis(int lastSleepMillis, unsigned long long currTimeMillis,
                               unsigned long long lastErrorTimeMillis) const;

    private:

        // Parameters
        int _maxSleepMillis;
        int _resetAfterMillis;

        // Last sleep information
        int _lastSleepMillis;
        unsigned long long _lastErrorTimeMillis;
    };

    // DO NOT TOUCH except for testing
    void jsTimeVirtualSkew( long long skew );

    void jsTimeVirtualThreadSkew( long long skew );
    long long getJSTimeVirtualThreadSkew();

    /** Date_t is milliseconds since epoch */
     Date_t jsTime();

    /** warning this will wrap */
    unsigned curTimeMicros();
    unsigned long long curTimeMicros64();
    unsigned long long curTimeMillis64();

    // these are so that if you use one of them compilation will fail
    char *asctime(const struct tm *tm);
    char *ctime(const time_t *timep);
    struct tm *gmtime(const time_t *timep);
    struct tm *localtime(const time_t *timep);

#if defined(MONGO_BOOST_TIME_UTC_HACK) || (BOOST_VERSION >= 105000)
#define MONGO_BOOST_TIME_UTC boost::TIME_UTC_
#else
#define MONGO_BOOST_TIME_UTC boost::TIME_UTC
#endif

}  // namespace mongo

