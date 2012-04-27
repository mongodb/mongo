// @file timer.h

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

#pragma once

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
    class Timer /*copyable*/ {
    public:
        static const unsigned long long millisPerSecond = 1000;
        static const unsigned long long microsPerSecond = 1000 * millisPerSecond;
        static const unsigned long long nanosPerSecond = 1000 * microsPerSecond;

        Timer() { reset(); }
        int seconds() const { return (int)(micros() / 1000000); }
        int millis() const { return (int)(micros() / 1000); }
        int minutes() const { return seconds() / 60; }


        /** Get the time interval and reset at the same time.
         *  @return time in milliseconds.
         */
        inline int millisReset() {
            unsigned long long nextNow = now();
            unsigned long long deltaMicros =
                ((nextNow - _old) * microsPerSecond) / _countsPerSecond;

            _old = nextNow;
            return static_cast<int>(deltaMicros / 1000);
        }

        inline unsigned long long micros() const {
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
        static unsigned long long _countsPerSecond;

    private:
        inline unsigned long long now() const;

        unsigned long long _old;
    };
}  // namespace mongo

#include "mongo/util/timer-inl.h"
