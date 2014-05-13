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

#include "mongo/util/timer.h"

#include <limits>

namespace mongo {

    // default value of 1 so that during startup initialization if referenced no division by zero
    long long Timer::_countsPerSecond = 1;

    namespace {

        // TODO: SERVER-5112, better startup-time initialization of C++ modules.
        struct AtStartup {
            AtStartup();
        } atstartuputil;

#if defined(MONGO_TIMER_IMPL_WIN32)

        AtStartup::AtStartup() {
            LARGE_INTEGER x;
            bool ok = QueryPerformanceFrequency(&x);
            verify(ok);
            Timer::_countsPerSecond = x.QuadPart;
        }

#elif defined(MONGO_TIMER_IMPL_POSIX_MONOTONIC_CLOCK)

        AtStartup::AtStartup() {
            Timer::_countsPerSecond = Timer::nanosPerSecond;

            // Make sure that the current time relative to the (unspecified) epoch isn't already too
            // big to represent as a 64-bit count of nanoseconds.
            long long maxSecs = std::numeric_limits<long long>::max() /
                Timer::nanosPerSecond;
            timespec the_time;
            fassert(16162, !clock_gettime(CLOCK_MONOTONIC, &the_time));
            fassert(16163, static_cast<long long>(the_time.tv_sec) < maxSecs);
        }

#elif defined(MONGO_TIMER_IMPL_GENERIC)

        AtStartup::AtStartup() {
            Timer::_countsPerSecond = Timer::microsPerSecond;
        }

#else
#error "Unknown mongo::Timer implementation"
#endif

    }  // namespace

}  // namespace mongo
