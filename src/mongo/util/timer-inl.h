// @file mongo/util/timer-inl.h

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
