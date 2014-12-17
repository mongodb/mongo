// @file mongo/util/timer.cpp

/*    Copyright 2009 10gen Inc.
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

#include "mongo/util/timer.h"

#include <ctime>
#include <limits>
#if defined(MONGO_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

namespace mongo {

    // Set default value to reflect "generic" timer implementation.
    // Define Timer::_countsPerSecond before static initializer "atstartuputil" to ensure correct
    // relative sequencing regardless of how _countsPerSecond is initialized (static or dynamic).
    long long Timer::_countsPerSecond = Timer::microsPerSecond;

    namespace {

        // TODO: SERVER-5112, better startup-time initialization of C++ modules.
        struct AtStartup {
            AtStartup();
        } atstartuputil;

        // "Generic" implementation for Timer::now().
        long long _timerNowGeneric() {
            return curTimeMicros64();
        }

        // Function pointer to Timer::now() implementation.
        // Overridden in AtStartup() with better implementation where available.
        long long (*_timerNow)() = &_timerNowGeneric;

#if defined(_WIN32)

        /**
         * Windows-specific implementation of the
         * Timer class.  Windows selects the best available timer, in its estimation, for
         * measuring time at high resolution.  This may be the HPET of the TSC on x86 systems,
         * but is promised to be synchronized across processors, barring BIOS errors.
         */
        long long timerNowWindows() {
            LARGE_INTEGER i;
            fassert(16161, QueryPerformanceCounter(&i));
            return i.QuadPart;
        }

        AtStartup::AtStartup() {
            LARGE_INTEGER x;
            bool ok = QueryPerformanceFrequency(&x);
            verify(ok);
            Timer::_countsPerSecond = x.QuadPart;
            _timerNow = &timerNowWindows;
        }

#elif defined(MONGO_HAVE_POSIX_MONOTONIC_CLOCK)

        /**
         * Implementation for timer on systems that support the
         * POSIX clock API and CLOCK_MONOTONIC clock.
         */
        long long timerNowPosixMonotonicClock() {
            timespec the_time;
            long long result;

            fassert(16160, !clock_gettime(CLOCK_MONOTONIC, &the_time));

            // Safe for 292 years after the clock epoch, even if we switch to a signed time value.
            // On Linux, the monotonic clock's epoch is the UNIX epoch.
            result = static_cast<long long>(the_time.tv_sec);
            result *= Timer::nanosPerSecond;
            result += static_cast<long long>(the_time.tv_nsec);
            return result;
        }

        AtStartup::AtStartup() {
            // If the monotonic clock is not available at runtime (sysconf() returns 0 or -1),
            // do not override the generic implementation or modify Timer::_countsPerSecond.
            if (sysconf(_SC_MONOTONIC_CLOCK) <= 0) {
                return;
            }

            Timer::_countsPerSecond = Timer::nanosPerSecond;
            _timerNow = &timerNowPosixMonotonicClock;

            // Make sure that the current time relative to the (unspecified) epoch isn't already too
            // big to represent as a 64-bit count of nanoseconds.
            long long maxSecs = std::numeric_limits<long long>::max() /
                Timer::nanosPerSecond;
            timespec the_time;
            fassert(16162, !clock_gettime(CLOCK_MONOTONIC, &the_time));
            fassert(16163, static_cast<long long>(the_time.tv_sec) < maxSecs);
        }
#else
        AtStartup::AtStartup() { }
#endif

    }  // namespace

    long long Timer::now() const {
        return _timerNow();
    }

}  // namespace mongo
