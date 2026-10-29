// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto kTestLogTag = "test"sv;

class LogRotateSignalTest : public unittest::Test {
public:
    void setUp() override {
        // We don't plan on destructing the global service context in order to prevent a race
        // with the signal processing thread.
        setGlobalServiceContext(ServiceContext::make());
        static constexpr auto kNumThreadsForBarrier = 2;
        _barrier = std::make_unique<unittest::Barrier>(kNumThreadsForBarrier);

        startSignalProcessingThread(LogFileStatus::kNeedToRotateLogFile);

        logv2::addLogRotator(kTestLogTag, [&](bool, std::string_view, std::function<void(Status)>) {
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
using LogRotateSignalTestDeathTest = LogRotateSignalTest;
DEATH_TEST_F(LogRotateSignalTestDeathTest, LogRotateSignal, "Ending LogRotateSignalTest") {
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

DEATH_TEST(AsynchronousSignalTestDeathTest, ShutdownHup, "Shutdown called") {
    doSignalShutdownTest(SIGHUP);
}
DEATH_TEST(AsynchronousSignalTestDeathTest, ShutdownInt, "Shutdown called") {
    doSignalShutdownTest(SIGINT);
}
DEATH_TEST(AsynchronousSignalTestDeathTest, ShutdownTerm, "Shutdown called") {
    doSignalShutdownTest(SIGTERM);
}
DEATH_TEST(AsynchronousSignalTestDeathTest, ShutdownXcpu, "Shutdown called") {
    doSignalShutdownTest(SIGXCPU);
}

}  // namespace
}  // namespace mongo
