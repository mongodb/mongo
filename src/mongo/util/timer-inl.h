// @file mongo/util/timer-inl.h

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
 * Inline function implementations for the Timer class.  This file simply selects
 * the platform-appropriate inline functions to include.
 *
 * This file should only be included through timer-inl.h, which selects the
 * particular implementation based on target platform.
 */

#pragma once

#if defined(MONGO_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#if defined(_WIN32)

// On Windows, prefer the Windows-specific implementation, which employs QueryPerformanceCounter.
#include "mongo/util/timer-win32-inl.h"

#elif defined(_POSIX_TIMERS) and _POSIX_TIMERS > 0 and defined(_POSIX_MONOTONIC_CLOCK) and _POSIX_MONOTONIC_CLOCK > 0

// On systems that support the POSIX clock_gettime function, and the "monotonic" clock,
// use those.
#include "mongo/util/timer-posixclock-inl.h"

#else

// If all else fails, fall back to a generic implementation.  Performance may suffer.
#include "mongo/util/timer-generic-inl.h"

#endif
