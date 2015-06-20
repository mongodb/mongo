/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/system_tick_source.h"

#include "mongo/config.h"

#include <ctime>
#include <limits>
#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#include "mongo/base/init.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {

const int64_t kMillisPerSecond = 1000;
const int64_t kMicrosPerSecond = 1000 * kMillisPerSecond;
const int64_t kNanosPerSecond = 1000 * kMicrosPerSecond;

/**
 * Internally, the timer counts platform-dependent ticks of some sort, and
 * must then convert those ticks to microseconds and their ilk.  This field
 * stores the frequency of the platform-dependent counter.
 *
 * Define ticksPerSecond before init to ensure correct relative sequencing
 * regardless of how it is initialized (static or dynamic).
 */
TickSource::Tick ticksPerSecond = kMicrosPerSecond;

// "Generic" implementation for _timerNow.
TickSource::Tick _timerNowGeneric() {
    return curTimeMicros64();
}

// Function pointer to timer implementation.
// Overridden in initTickSource() with better implementation where available.
TickSource::Tick (*_timerNow)() = &_timerNowGeneric;

SystemTickSource globalSystemTickSource;

#if defined(_WIN32)

/**
 * Windows-specific implementation of the
 * Timer class.  Windows selects the best available timer, in its estimation, for
 * measuring time at high resolution.  This may be the HPET of the TSC on x86 systems,
 * but is promised to be synchronized across processors, barring BIOS errors.
 */
TickSource::Tick timerNowWindows() {
    LARGE_INTEGER i;
    fassert(16161, QueryPerformanceCounter(&i));
    return i.QuadPart;
}

void initTickSource() {
    LARGE_INTEGER x;
    bool ok = QueryPerformanceFrequency(&x);
    verify(ok);
    ticksPerSecond = x.QuadPart;
    _timerNow = &timerNowWindows;
}

#elif defined(MONGO_CONFIG_HAVE_POSIX_MONOTONIC_CLOCK)

/**
 * Implementation for timer on systems that support the
 * POSIX clock API and CLOCK_MONOTONIC clock.
 */
TickSource::Tick timerNowPosixMonotonicClock() {
    timespec the_time;
    long long result;

    fassert(16160, !clock_gettime(CLOCK_MONOTONIC, &the_time));

    // Safe for 292 years after the clock epoch, even if we switch to a signed time value.
    // On Linux, the monotonic clock's epoch is the UNIX epoch.
    result = static_cast<long long>(the_time.tv_sec);
    result *= kNanosPerSecond;
    result += static_cast<long long>(the_time.tv_nsec);
    return result;
}

void initTickSource() {
    // If the monotonic clock is not available at runtime (sysconf() returns 0 or -1),
    // do not override the generic implementation or modify ticksPerSecond.
    if (sysconf(_SC_MONOTONIC_CLOCK) <= 0) {
        return;
    }

    ticksPerSecond = kNanosPerSecond;
    _timerNow = &timerNowPosixMonotonicClock;

    // Make sure that the current time relative to the (unspecified) epoch isn't already too
    // big to represent as a 64-bit count of nanoseconds.
    long long maxSecs = std::numeric_limits<long long>::max() / kNanosPerSecond;
    timespec the_time;
    fassert(16162, !clock_gettime(CLOCK_MONOTONIC, &the_time));
    fassert(16163, static_cast<long long>(the_time.tv_sec) < maxSecs);
}
#else
void initTickSource() {}
#endif

}  // unnamed namespace

MONGO_INITIALIZER(SystemTickSourceInit)(InitializerContext* context) {
    initTickSource();
    SystemTickSource::get();
    return Status::OK();
}

TickSource::Tick SystemTickSource::getTicks() {
    return _timerNow();
}

TickSource::Tick SystemTickSource::getTicksPerSecond() {
    return ticksPerSecond;
}

SystemTickSource* SystemTickSource::get() {
    static const auto globalSystemTickSource = stdx::make_unique<SystemTickSource>();
    return globalSystemTickSource.get();
}

}  // namespace mongo
