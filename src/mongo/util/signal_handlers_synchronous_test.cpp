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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <cmath>
#include <csignal>
#include <cstdlib>

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/signal_handlers_synchronous.h"

namespace {
using namespace mongo;

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

IGNORED_SIGNAL(SIGUSR2)
IGNORED_SIGNAL(SIGHUP)
IGNORED_SIGNAL(SIGPIPE)
FATAL_SIGNAL(SIGQUIT)
FATAL_SIGNAL(SIGABRT)
FATAL_SIGNAL(SIGILL)

#if not defined(__has_feature)
#define __has_feature(X) 0
#endif

#if !__has_feature(address_sanitizer)
// These signals trip the leak sanitizer
FATAL_SIGNAL(SIGSEGV)
FATAL_SIGNAL(SIGBUS)
FATAL_SIGNAL(SIGFPE)
#endif

DEATH_TEST(FatalTerminateTest,
           TerminateIsFatalWithoutException,
           "terminate() called. No exception") {
    std::terminate();
}

DEATH_TEST(FatalTerminateTest,
           TerminateIsFatalWithDBException,
           " terminate() called. An exception is active") {
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
    uasserted(28719, "Non-fatal first exception");
}

}  // namespace
