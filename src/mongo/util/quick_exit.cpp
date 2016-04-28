/**
*    Copyright (C) 2014 MongoDB Inc.
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

// NOTE: This file *must not* depend on any mongo symbols.

#include "mongo/platform/basic.h"

#include "mongo/config.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

// This will probably get us _exit on non-unistd platforms like Windows.
#include <cstdlib>

// NOTE: Header only dependencies are OK in this library.
#include "mongo/stdx/mutex.h"

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

#if !defined(__has_include)
#define __has_include(x) 0
#endif

#if __has_feature(address_sanitizer)

#if __has_include("sanitizer/coverage_interface.h")
// In Clang 3.7+, the coverage interface was split out into its own header file.
#include <sanitizer/coverage_interface.h>
#elif __has_include("sanitizer/common_interface_defs.h")
#include <sanitizer/common_interface_defs.h>
#endif

#include <sanitizer/lsan_interface.h>
#endif

#if defined(MONGO_CPU_PROFILER)
#include <gperftools/profiler.h>
#endif

#ifdef MONGO_GCOV
extern "C" void __gcov_flush();
#endif

namespace mongo {

namespace {
stdx::mutex* const quickExitMutex = new stdx::mutex;
}  // namespace

void quickExit(int code) {
    // Ensure that only one thread invokes the last rites here. No
    // RAII here - we never want to unlock this.
    if (quickExitMutex)
        quickExitMutex->lock();

#if defined(MONGO_CPU_PROFILER)
    ::ProfilerStop();
#endif

#ifdef MONGO_GCOV
    __gcov_flush();
#endif

#if __has_feature(address_sanitizer)
    // Always dump coverage data first because older versions of sanitizers may not write coverage
    // data before exiting with errors. The underlying issue is fixed in clang 3.6, which also
    // prevents coverage data from being written more than once via an atomic guard.
    __sanitizer_cov_dump();
    __lsan_do_leak_check();
#endif

#if defined(_WIN32) && defined(MONGO_CONFIG_DEBUG_BUILD)
    // SERVER-23860: VS 2015 Debug Builds abort during _exit for unknown reasons.
    // Bypass _exit CRT shutdown code and call ExitProcess directly instead.
    ::ExitProcess(code);
#else
    ::_exit(code);
#endif
}

}  // namespace mongo
