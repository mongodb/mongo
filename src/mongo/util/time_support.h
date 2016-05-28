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

#include <boost/date_time/gregorian/gregorian_types.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <ctime>
#include <iosfwd>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"

namespace mongo {

template <typename Allocator>
class StringBuilderImpl;

void time_t_to_Struct(time_t t, struct tm* buf, bool local = false);
std::string time_t_to_String_short(time_t t);

/**
 * Representation of a point in time, with millisecond resolution and capable
 * of representing all times representable by the BSON Date type.
 *
 * The epoch used for this type is the Posix Epoch (1970-01-01T00:00:00Z).
 */
class Date_t {
public:
    /**
     * The largest representable Date_t.
     *
     * TODO(schwerin): Make constexpr when supported by all compilers.
     */
    static Date_t max();

    /**
     * Reads the system clock and returns a Date_t representing the present time.
     */
    static Date_t now();

    /**
     * Returns a Date_t from an integer number of milliseconds since the epoch.
     */
    static Date_t fromMillisSinceEpoch(long long m) {
        return Date_t(m);
    }

    /**
     * Returns a Date_t from a duration since the epoch.
     */
    template <typename Duration>
    static Date_t fromDurationSinceEpoch(Duration d) {
        return fromMillisSinceEpoch(durationCount<Milliseconds>(d));
    }

    /**
     * Constructs a Date_t representing the epoch.
     */
    Date_t() = default;

    /**
     * Constructs a Date_t from a system clock time point.
     */
    explicit Date_t(stdx::chrono::system_clock::time_point tp);

    /**
     * Returns a string representation of the date.
     *
     * If isFormattable() returns true for this date, the string will be equivalent to the one
     * returned by dateToISOStringLocal(*this).  Otherwise, returns the string Date(...) where
     * ... is the string representation in base 10 of the number of milliseconds since the epoch
     * that this date represents (negative for pre-epoch).
     */
    std::string toString() const;

    /**
     * Returns a representation of this date as a C time_t.
     *
     * Raises an exception if this date is not representable as a time_t on the system.
     */
    time_t toTimeT() const;

    /**
     * DEPRECATED. This is a deprecated form of toMillisSinceEpoch().
     */
    int64_t asInt64() const {
        return toMillisSinceEpoch();
    }

    /**
     * DEPRECATED. This is a deprecated form of toMillisSinceEpoch() that casts the result to
     * unsigned long long.  It is leftover because sometimes objects of logical type Timestamp
     * get stored in BSON documents (or in-memory structures) with effective type Date_t, and
     * it is necessary to convert between the two.
     */
    unsigned long long toULL() const {
        return static_cast<unsigned long long>(toMillisSinceEpoch());
    }

    /**
     * Returns a duration representing the time since the epoch represented by this Date_t.
     */
    Milliseconds toDurationSinceEpoch() const {
        return Milliseconds(toMillisSinceEpoch());
    }

    /**
     * Returns the number of milliseconds since the epoch represented by this Date_t.
     */
    long long toMillisSinceEpoch() const {
        return static_cast<long long>(millis);
    }

    /*
     * Returns a system clock time_point representing the same point in time as this Date_t.
     * Warning: careful when using with Date_t::max() as it can have a value that is bigger than
     * time_point can store.
     */
    stdx::chrono::system_clock::time_point toSystemTimePoint() const;

    /**
     * Returns true if this Date_t is in the range of Date_ts that can be formatted as calendar
     * dates.  This property is guaranteed to be true for all dates from the epoch,
     * 1970-01-01T00:00:00.000Z, through 3000-12-31T23:59:59.000Z on 64-bit systems and through
     * 2038-01-19T03:14:07.000Z on 32-bit systems.
     */
    bool isFormattable() const;

    /**
     * Implicit conversion operator to system clock time point.  Enables use of Date_t with
     * condition_variable::wait_until.
     * Warning: careful when using with Date_t::max() as it can have a value that is bigger than
     * time_point can store.
     */
    operator stdx::chrono::system_clock::time_point() const {
        return toSystemTimePoint();
    }

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

    Milliseconds operator-(Date_t other) const {
        return Milliseconds(millis - other.millis);
    }

    bool operator==(Date_t other) const {
        return toDurationSinceEpoch() == other.toDurationSinceEpoch();
    }

    bool operator!=(Date_t other) const {
        return !(*this == other);
    }

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
    explicit Date_t(long long m) : millis(m) {}

    long long millis = 0;
};

// uses ISO 8601 dates without trailing Z
// colonsOk should be false when creating filenames
std::string terseCurrentTime(bool colonsOk = true);

/**
 * Produces a short UTC date + time approriate for file names with Z appended.
 */
std::string terseUTCCurrentTime();

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
bool toPointInTime(const std::string& str, boost::posix_time::ptime* timeOfDay);

void sleepsecs(int s);
void sleepmillis(long long ms);
void sleepmicros(long long micros);

template <typename DurationType>
void sleepFor(DurationType time) {
    sleepmicros(durationCount<Microseconds>(time));
}

class Backoff {
public:
    Backoff(int maxSleepMillis, int resetAfter)
        : _maxSleepMillis(maxSleepMillis),
          _resetAfterMillis(maxSleepMillis + resetAfter),  // Don't reset < the max sleep
          _lastSleepMillis(0),
          _lastErrorTimeMillis(0) {}

    void nextSleepMillis();

    /**
     * testing-only function. used in dbtests/basictests.cpp
     */
    int getNextSleepMillis(int lastSleepMillis,
                           unsigned long long currTimeMillis,
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
void jsTimeVirtualSkew(long long skew);

void jsTimeVirtualThreadSkew(long long skew);
long long getJSTimeVirtualThreadSkew();

/** Date_t is milliseconds since epoch */
Date_t jsTime();

unsigned long long curTimeMicros64();
unsigned long long curTimeMillis64();

// these are so that if you use one of them compilation will fail
char* asctime(const struct tm* tm);
char* ctime(const time_t* timep);
struct tm* gmtime(const time_t* timep);
struct tm* localtime(const time_t* timep);

}  // namespace mongo
