// @file mongo/util/timer.cpp

/*    Copyright 2009 10gen Inc.
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

#include "pch.h"  // TODO: Replace with platform/base.h after it's checked in.

#include "mongo/util/timer.h"

#include <limits>

namespace mongo {

    unsigned long long Timer::_countsPerSecond;

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
            unsigned long long maxSecs = std::numeric_limits<unsigned long long>::max() /
                Timer::nanosPerSecond;
            timespec the_time;
            fassert(16162, !clock_gettime(CLOCK_MONOTONIC, &the_time));
            fassert(16163, static_cast<unsigned long long>(the_time.tv_sec) < maxSecs);
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
