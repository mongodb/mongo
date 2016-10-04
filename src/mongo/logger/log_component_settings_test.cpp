/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/logger/log_component.h"
#include "mongo/logger/log_component_settings.h"
#include "mongo/logger/logger.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"

#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::logger;

// This test checks that calling "shouldLog" and "setMinimumLoggedSeverity" concurrently doesn't
// cause an invariant failure, i.e. that these methods are thread-safe.
TEST(SERVER25981Test, SetSeverityShouldLogAndClear) {
    unittest::Barrier startupBarrier(4);
    AtomicBool running(true);

    stdx::thread shouldLogThread([&]() {
        startupBarrier.countDownAndWait();
        while (running.load()) {
            logger::globalLogDomain()->shouldLog(LogComponent::kDefault,
                                                 logger::LogSeverity::Debug(3));
        }
    });

    stdx::thread setMinimumLoggedSeverityThreadA([&]() {
        startupBarrier.countDownAndWait();
        while (running.load()) {
            logger::globalLogDomain()->setMinimumLoggedSeverity(LogComponent::kDefault,
                                                                logger::LogSeverity::Debug(1));
        }
    });

    stdx::thread setMinimumLoggedSeverityThreadB([&]() {
        startupBarrier.countDownAndWait();
        while (running.load()) {
            logger::globalLogDomain()->setMinimumLoggedSeverity(LogComponent::kDefault,
                                                                logger::LogSeverity::Log());
        }
    });

    stdx::thread clearMinimumLoggedSeverityThread([&]() {
        startupBarrier.countDownAndWait();
        while (running.load()) {
            logger::globalLogDomain()->clearMinimumLoggedSeverity(LogComponent::kDefault);
        }
    });

    mongo::sleepmillis(4000);
    running.store(false);
    shouldLogThread.join();
    setMinimumLoggedSeverityThreadA.join();
    setMinimumLoggedSeverityThreadB.join();
    clearMinimumLoggedSeverityThread.join();
}

}  // namespace
