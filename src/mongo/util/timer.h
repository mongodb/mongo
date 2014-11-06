// @file timer.h

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

#include "mongo/client/export_macros.h"
#include "mongo/util/assert_util.h"

namespace mongo {
    /**
     * Time tracking object.
     *
     * Should be of reasonably high performance, though the implementations are platform-specific.
     * Each platform provides a distinct implementation of the now() method, and sets the
     * _countsPerSecond static field to the constant number of ticks per second that now() counts
     * in.  The maximum span measurable by the counter and convertible to microseconds is about 10
     * trillion ticks.  As long as there are fewer than 100 ticks per nanosecond, timer durations of
     * 2.5 years will be supported.  Since a typical tick duration will be under 10 per nanosecond,
     * if not below 1 per nanosecond, this should not be an issue.
     */
    class MONGO_CLIENT_API Timer /*copyable*/ {
    public:
        static const long long millisPerSecond = 1000;
        static const long long microsPerSecond = 1000 * millisPerSecond;
        static const long long nanosPerSecond = 1000 * microsPerSecond;

        enum ShouldStart { START = true, DONT_START = false };

        Timer(ShouldStart shouldStart = START) : _old(shouldStart ? now() : unstartedTime) {}

        bool isStarted() const { return _old != unstartedTime; }

        int seconds() const { return (int)(micros() / 1000000); }
        int millis() const { return (int)(micros() / 1000); }
        int minutes() const { return seconds() / 60; }


        /** Get the time interval and reset at the same time.
         *  @return time in milliseconds.
         */
        inline int millisReset() {
            return static_cast<int>(microsReset() / 1000);
        }

        /** Get the time interval and reset at the same time.
         *  @return time in microseconds.
         */
        inline long long microsReset() {
            dassert(isStarted());
            const long long nextNow = now();
            const long long deltaMicros =
                ((nextNow - _old) * microsPerSecond) / _countsPerSecond;

            _old = nextNow;
            return deltaMicros;
        }

        inline long long micros() const {
            dassert(isStarted());
            return ((now() - _old) * microsPerSecond) / _countsPerSecond;
        }

        inline void reset() { _old = now(); }

        /**
         * Internally, the timer counts platform-dependent ticks of some sort, and
         * must then convert those ticks to microseconds and their ilk.  This field
         * stores the frequency of the platform-dependent counter.
         *
         * This value is initialized at program startup, and never changed after.
         * It should be treated as private.
         */
        static long long _countsPerSecond;

    private:
        static const long long unstartedTime = -1;

        long long now() const;

        long long _old;
    };
}  // namespace mongo
