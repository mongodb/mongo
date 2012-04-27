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

/**
 * Inline function implementations for timers on systems that support the
 * POSIX clock API and CLOCK_MONOTONIC clock.
 *
 * This file should only be included through timer-inl.h, which selects the
 * particular implementation based on target platform.
 */

#define MONGO_TIMER_IMPL_POSIX_MONOTONIC_CLOCK

#include <ctime>

#include "mongo/util/assert_util.h"

namespace mongo {

    unsigned long long Timer::now() const {
        timespec the_time;
        unsigned long long result;

        fassert(16160, !clock_gettime(CLOCK_MONOTONIC, &the_time));

        // Safe for 292 years after the clock epoch, even if we switch to a signed time value.  On
        // Linux, the monotonic clock's epoch is the UNIX epoch.
        result = static_cast<unsigned long long>(the_time.tv_sec);
        result *= nanosPerSecond;
        result += static_cast<unsigned long long>(the_time.tv_nsec);
        return result;
    }

}  // namespace mongo
