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


#include "signal_handlers.h"
#include <csignal>
#include <cstdlib>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log_util.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/signal_handlers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
using namespace fmt::literals;

constexpr auto kTestLogTag = "test"_sd;

class LogRotateSignalTest : public ServiceContextTest {
public:
    void setUp() override {
        static constexpr auto kNumThreadsForBarrier = 2;
        _barrier = std::make_unique<unittest::Barrier>(kNumThreadsForBarrier);

        ServiceContextTest::setUp();
        startSignalProcessingThread(LogFileStatus::kNeedToRotateLogFile);

        logv2::addLogRotator(kTestLogTag, [&](bool, StringData, std::function<void(Status)>) {
            LOGV2(9493700, "Test log rotator called");
            _barrier->countDownAndWait();
            return Status::OK();
        });
    }

    bool checkCapturedTextFormatLogMessagesForSubstr(std::string substr) const {
        auto logs = getCapturedTextFormatLogMessages();
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

TEST_F(LogRotateSignalTest, LogRotateSignal) {
    startCapturingLogMessages();
    kill(getpid(), SIGUSR1);
    waitUntilLogRotatorCalled();
    stopCapturingLogMessages();
    ASSERT(checkCapturedTextFormatLogMessagesForSubstr("Log rotation initiated"));
    ASSERT(checkCapturedTextFormatLogMessagesForSubstr("Test log rotator called"));
}

#define TEST_SIGNAL_CLEAN_EXIT(SIGNUM)                                 \
    DEATH_TEST(AsynchronousSignalTest, SIGNUM##_, "Received signal") { \
        registerShutdownTask([&] { invariant(false); });               \
        startSignalProcessingThread();                                 \
        kill(getpid(), SIGNUM);                                        \
        waitForShutdown();                                             \
    }

TEST_SIGNAL_CLEAN_EXIT(SIGHUP);
TEST_SIGNAL_CLEAN_EXIT(SIGINT);
TEST_SIGNAL_CLEAN_EXIT(SIGTERM);
TEST_SIGNAL_CLEAN_EXIT(SIGXCPU);

}  // namespace
}  // namespace mongo
