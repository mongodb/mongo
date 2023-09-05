/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include <csignal>
#include <exception>
#include <fmt/format.h>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {
using namespace fmt::literals;

// Tests of signals that should be ignored raise each signal twice, to ensure that the handler isn't
// reset.
#define IGNORED_SIGNAL(SIGNUM)           \
    TEST(IgnoredSignalTest, SIGNUM##_) { \
        ASSERT_EQ(0, raise(SIGNUM));     \
        ASSERT_EQ(0, raise(SIGNUM));     \
    }

#define FATAL_SIGNAL(SIGNUM)                                                                   \
    DEATH_TEST(FatalSignalTest, SIGNUM##_, str::stream() << "Got signal: " << SIGNUM << " ") { \
        ASSERT_EQ(0, raise(SIGNUM));                                                           \
    }

#ifdef __linux__
// The si_code field is always SI_TKILL when using raise
#define DUMP_SIGINFO(SIGNUM)                                                                    \
    DEATH_TEST(DumpSiginfoTest, SIGNUM##_, "Dumping siginfo (si_code={}): "_format(SI_TKILL)) { \
        ASSERT_EQ(0, raise(SIGNUM));                                                            \
    }
#else
#define DUMP_SIGINFO(SIGNUM)
#endif

IGNORED_SIGNAL(SIGUSR2)
IGNORED_SIGNAL(SIGHUP)
IGNORED_SIGNAL(SIGPIPE)
FATAL_SIGNAL(SIGQUIT)
FATAL_SIGNAL(SIGILL)
DUMP_SIGINFO(SIGILL)
FATAL_SIGNAL(SIGABRT)

#if !__has_feature(address_sanitizer)
// These signals trip the leak sanitizer
FATAL_SIGNAL(SIGSEGV)
DUMP_SIGINFO(SIGSEGV)
FATAL_SIGNAL(SIGBUS)
DUMP_SIGINFO(SIGBUS)
FATAL_SIGNAL(SIGFPE)
DUMP_SIGINFO(SIGFPE)
#endif

DEATH_TEST(FatalTerminateTest,
           TerminateIsFatalWithoutException,
           "terminate() called. No exception") {
    std::terminate();
}

DEATH_TEST(FatalTerminateTest,
           TerminateIsFatalWithDBException,
           "terminate() called. An exception is active") {
    try {
        uasserted(28720, "Fatal DBException occurrence");
    } catch (...) {
        std::terminate();
    }
}

DEATH_TEST(FatalTerminateTest,
           TerminateIsFatalWithDoubleException,
           "terminate() called. An exception is active") {
    class ThrowInDestructor {
    public:
        ~ThrowInDestructor() {
            uasserted(28721, "Fatal second exception");
        }
    } tid;

    // This is a workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=83400. We should delete
    // this variable once we are on a compiler that doesn't require it.
    volatile bool workaroundGCCBug = true;  // NOLINT
    if (workaroundGCCBug)
        uasserted(28719, "Non-fatal first exception");
}

}  // namespace
}  // namespace mongo
