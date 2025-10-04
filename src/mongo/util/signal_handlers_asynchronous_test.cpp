/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/base/string_data.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_util.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/signal_handlers.h"

#include <csignal>
#include <cstdlib>
#include <string>

#include "signal_handlers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

constexpr auto kTestLogTag = "test"_sd;

class LogRotateSignalTest : public unittest::Test {
public:
    void setUp() override {
        // We don't plan on destructing the global service context in order to prevent a race
        // with the signal processing thread.
        setGlobalServiceContext(ServiceContext::make());
        static constexpr auto kNumThreadsForBarrier = 2;
        _barrier = std::make_unique<unittest::Barrier>(kNumThreadsForBarrier);

        startSignalProcessingThread(LogFileStatus::kNeedToRotateLogFile);

        logv2::addLogRotator(kTestLogTag, [&](bool, StringData, std::function<void(Status)>) {
            LOGV2(9493700, "Test log rotator called");
            _barrier->countDownAndWait();
            return Status::OK();
        });
    }

    bool checkLinesContains(auto&& logs, std::string substr) const {
        return std::count_if(logs.begin(), logs.end(), [&](const auto& log) {
            return log.find(substr) != std::string::npos;
        });
    }

    void waitUntilLogRotatorCalled() const {
        _barrier->countDownAndWait();
    }

private:
    std::unique_ptr<unittest::Barrier> _barrier;
};

// We use a death test here because the asynchronous signal processing thread runs as detached
// thread, so we kill the process in which it spawns so the thread doesn't live in other tests.
DEATH_TEST_F(LogRotateSignalTest, LogRotateSignal, "Ending LogRotateSignalTest") {
    unittest::LogCaptureGuard logs;
    kill(getpid(), SIGUSR1);
    waitUntilLogRotatorCalled();
    logs.stop();
    auto&& lines = logs.getText();
    ASSERT(checkLinesContains(lines, "Log rotation initiated"));
    ASSERT(checkLinesContains(lines, "Test log rotator called"));
    LOGV2_FATAL(9706300, "Ending LogRotateSignalTest");
}

void doSignalShutdownTest(int sig) {
    // We expect a clean exit for asynchronously handled signals. Clean exit calls into shutdown
    // tasks, so we log to indicate the signal was handled as expected, and invariant to finish
    // the DEATH_TEST.
    registerShutdownTask([] {
        LOGV2(9570500, "Shutdown called");
        invariant(false);
    });
    startSignalProcessingThread();
    kill(getpid(), sig);
    waitForShutdown();
}

DEATH_TEST(AsynchronousSignalTest, ShutdownHup, "Shutdown called") {
    doSignalShutdownTest(SIGHUP);
}
DEATH_TEST(AsynchronousSignalTest, ShutdownInt, "Shutdown called") {
    doSignalShutdownTest(SIGINT);
}
DEATH_TEST(AsynchronousSignalTest, ShutdownTerm, "Shutdown called") {
    doSignalShutdownTest(SIGTERM);
}
DEATH_TEST(AsynchronousSignalTest, ShutdownXcpu, "Shutdown called") {
    doSignalShutdownTest(SIGXCPU);
}

}  // namespace
}  // namespace mongo
