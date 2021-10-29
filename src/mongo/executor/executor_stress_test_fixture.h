/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/counter.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace executor {

/**
 * Stress test common implementation methods.
 */
class ThreadPoolExecutorStressTestEngine {
public:
    using CopyableCallback = std::function<void(const TaskExecutor::CallbackArgs&)>;

    // Adjust periods based on statistical reports if needed.
    static constexpr auto kDurationBetweenSimpleSchedulings{Microseconds(40)};
    static constexpr auto kDurationBetweenRemoteCommands{Microseconds(10)};
    static constexpr auto kDurationBetweenMockedResponses{Microseconds(10)};
    static constexpr auto kMaxCallbacks = 100 * 1000;

    ThreadPoolExecutorStressTestEngine(std::shared_ptr<TaskExecutor> executor,
                                       boost::optional<NetworkInterfaceMock*> netMock,
                                       Milliseconds waitBeforeTermination);

    // Add various stress test threads. In each case 'count' is the count of new threads to add.

    void addSimpleSchedulingThreads(int count);

    void addRandomCancelationThreads(int count);

    void addScheduleRemoteCommandThreads(int count);

    // Waits for test termination, then sets termination flag and blocks until all threads
    // are terminated.
    void waitAndCleanup();

private:
    void _addMockNetworkResponseThread();

    static int32_t nextRandomInt32(int32_t max);

    const std::shared_ptr<TaskExecutor> _executor;
    const boost::optional<NetworkInterfaceMock*> _netMock;

    // Statistics.
    Counter64 _completedWorks;
    Counter64 _commandsSucceeded;
    Counter64 _commandsFailed;

    AtomicWord<bool> _terminate;  // Termination flag.
    Timer _timer;
    Milliseconds _waitBeforeTermination;
    boost::optional<unittest::ThreadAssertionMonitor> _threadAssertionMonitor;

    Mutex _mutex = MONGO_MAKE_LATCH("ThreadPoolExecutorMockNetStressTest::_mutex");
    std::list<stdx::thread> _threads;
    std::deque<TaskExecutor::CallbackHandle> _callbacks;
};

/**
 * Stress test suite using mock thread pool and mock Net interface.
 */
class ThreadPoolExecutorMockNetStressTest : public ThreadPoolExecutorTest {
public:
    void setUp() override {
        ThreadPoolExecutorTest::setUp();
        getExecutor().startup();
        _engine = std::make_unique<ThreadPoolExecutorStressTestEngine>(
            getExecutor().shared_from_this(), getNet(), Seconds(2));
    }

    void tearDown() override {
        waitAndCleanup();
        ThreadPoolExecutorTest::tearDown();
    }

    void addSimpleSchedulingThreads(int count) {
        _engine->addSimpleSchedulingThreads(count);
    }

    void addRandomCancelationThreads(int count) {
        _engine->addRandomCancelationThreads(count);
    }

    void addScheduleRemoteCommandThreads(int count) {
        _engine->addScheduleRemoteCommandThreads(count);
    }

    void waitAndCleanup() {
        _engine->waitAndCleanup();
    }

private:
    std::unique_ptr<ThreadPoolExecutorStressTestEngine> _engine;
};

}  // namespace executor
}  // namespace mongo
