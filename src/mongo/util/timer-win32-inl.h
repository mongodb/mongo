// @file mongo/util/timer-win32-inl.h

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
 * Inline function implementations for the Windows-specific implementation of the
 * Timer class.  Windows selects the best available timer, in its estimation, for
 * measuring time at high resolution.  This may be the HPET of the TSC on x86 systems,
 * but is promised to be synchronized across processors, barring BIOS errors.
 *
 * Do not include directly.  Include "mongo/util/timer.h".
 */

#pragma once

#define MONGO_TIMER_IMPL_WIN32

#include "mongo/util/assert_util.h"

namespace mongo {

    unsigned long long Timer::now() const {
        LARGE_INTEGER i;
        fassert(16161, QueryPerformanceCounter(&i));
        return i.QuadPart;
    }

}  // namespace mongo
