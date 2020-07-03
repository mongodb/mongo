/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/watchdog/watchdog.h"

#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

class TestPeriodicThread : public WatchdogPeriodicThread {
public:
    TestPeriodicThread(Milliseconds period) : WatchdogPeriodicThread(period, "testPeriodic") {}

    void run(OperationContext* opCtx) final {
        {
            stdx::lock_guard<Latch> lock(_mutex);
            ++_counter;
        }

        if (_counter == _wait) {
            _condvar.notify_all();
        }
    }

    void setSignalOnCount(int c) {
        _wait = c;
    }

    void waitForCount() {
        invariant(_wait != 0);

        stdx::unique_lock<Latch> lock(_mutex);
        while (_counter < _wait) {
            _condvar.wait(lock);
        }
    }

    void resetState() final {}

    std::uint32_t getCounter() {
        {
            stdx::lock_guard<Latch> lock(_mutex);
            return _counter;
        }
    }

private:
    std::uint32_t _counter{0};

    Mutex _mutex = MONGO_MAKE_LATCH("TestPeriodicThread::_mutex");
    stdx::condition_variable _condvar;
    std::uint32_t _wait{0};
};

class PeriodicThreadTest : public ServiceContextTest {};

// Tests:
// 1. Make sure it runs at least N times
// 2. Make sure it responds to stop after being paused
// 3. Make sure it can be resumed
// 4. Make sure the period can be changed like from 1 minute -> 1 milli

// Positive: Make sure periodic thread runs at least N times and stops correctly
TEST_F(PeriodicThreadTest, Basic) {

    TestPeriodicThread testThread(Milliseconds(5));

    testThread.setSignalOnCount(5);

    testThread.start();

    testThread.waitForCount();

    testThread.shutdown();

    // Check the counter after it is shutdown and make sure it does not change.
    std::uint32_t lastCounter = testThread.getCounter();

    // This is racey but it should only produce false negatives
    sleepmillis(100);

    ASSERT_EQ(lastCounter, testThread.getCounter());
}

// Positive: Make sure it stops after being paused
TEST_F(PeriodicThreadTest, PauseAndStop) {

    TestPeriodicThread testThread(Milliseconds(5));
    testThread.setSignalOnCount(5);

    testThread.start();

    testThread.waitForCount();

    // Stop the thread by setting a -1 duration
    testThread.setPeriod(Milliseconds(-1));

    // Check the counter after it is shutdown and make sure it does not change.
    std::uint32_t pauseCounter = testThread.getCounter();

    // This is racey but it should only produce false negatives
    sleepmillis(100);

    // We could have had one more run of the loop as we paused - allow for that case
    // but no other runs of the thread.
    ASSERT_GTE(pauseCounter + 1, testThread.getCounter());

    testThread.shutdown();

    // Check the counter after it is shutdown and make sure it does not change.
    std::uint32_t stopCounter = testThread.getCounter();

    // This is racey but it should only produce false negatives
    sleepmillis(100);

    ASSERT_EQ(stopCounter, testThread.getCounter());
}

// Positive: Make sure it can be paused and resumed
TEST_F(PeriodicThreadTest, PauseAndResume) {

    TestPeriodicThread testThread(Milliseconds(5));
    testThread.setSignalOnCount(5);

    testThread.start();

    testThread.waitForCount();

    // Stop the thread by setting a -1 duration
    testThread.setPeriod(Milliseconds(-1));

    // Check the counter after it is shutdown and make sure it does not change.
    std::uint32_t pauseCounter = testThread.getCounter();

    // This is racey but it should only produce false negatives
    sleepmillis(100);

    // We could have had one more run of the loop as we paused - allow for that case
    // but no other runs of the thread.
    ASSERT_GTE(pauseCounter + 1, testThread.getCounter());

    // Make sure we can resume the thread again
    std::uint32_t baseCounter = testThread.getCounter();
    testThread.setSignalOnCount(baseCounter + 5);

    testThread.setPeriod(Milliseconds(7));

    testThread.waitForCount();

    testThread.shutdown();
}

/**
 * Simple class to ensure we run checks.
 */
class TestCounterCheck : public WatchdogCheck {
public:
    void run(OperationContext* opCtx) final {
        {
            stdx::lock_guard<Latch> lock(_mutex);
            ++_counter;
        }

        if (_counter == _wait) {
            _condvar.notify_all();
        }
    }

    std::string getDescriptionForLogging() final {
        return "test";
    }

    void setSignalOnCount(int c) {
        _wait = c;
    }

    void waitForCount() {
        invariant(_wait != 0);

        stdx::unique_lock<Latch> lock(_mutex);
        while (_counter < _wait) {
            _condvar.wait(lock);
        }
    }

    std::uint32_t getCounter() {
        {
            stdx::lock_guard<Latch> lock(_mutex);
            return _counter;
        }
    }

private:
    std::uint32_t _counter{0};

    Mutex _mutex = MONGO_MAKE_LATCH("TestCounterCheck::_mutex");
    stdx::condition_variable _condvar;
    std::uint32_t _wait{0};
};

class WatchdogCheckThreadTest : public ServiceContextTest {};

// Positive: Make sure check thread runs at least N times and stops correctly
TEST_F(WatchdogCheckThreadTest, Basic) {
    auto counterCheck = std::make_unique<TestCounterCheck>();
    auto counterCheckPtr = counterCheck.get();

    std::vector<std::unique_ptr<WatchdogCheck>> checks;
    checks.push_back(std::move(counterCheck));

    WatchdogCheckThread testThread(std::move(checks), Milliseconds(5));

    counterCheckPtr->setSignalOnCount(5);

    testThread.start();

    counterCheckPtr->waitForCount();

    testThread.shutdown();

    // Check the counter after it is shutdown and make sure it does not change.
    std::uint32_t lastCounter = counterCheckPtr->getCounter();

    // This is racey but it should only produce false negatives
    sleepmillis(100);

    ASSERT_EQ(lastCounter, counterCheckPtr->getCounter());
}

/**
 * A class that models the behavior of Windows' manual reset Event object.
 */
class ManualResetEvent {
public:
    void set() {
        stdx::lock_guard<Latch> lock(_mutex);

        _set = true;
        _condvar.notify_one();
    }

    void wait() {
        stdx::unique_lock<Latch> lock(_mutex);

        _condvar.wait(lock, [this]() { return _set; });
    }

private:
    bool _set{false};

    Mutex _mutex = MONGO_MAKE_LATCH("ManualResetEvent::_mutex");
    stdx::condition_variable _condvar;
};


class WatchdogMonitorThreadTest : public ServiceContextTest {};

// Positive: Make sure monitor thread signals death if the check thread never starts
TEST_F(WatchdogMonitorThreadTest, Basic) {
    ManualResetEvent deathEvent;
    WatchdogDeathCallback deathCallback = [&deathEvent]() {
        LOGV2(23431, "Death signalled");
        deathEvent.set();
    };

    auto counterCheck = std::make_unique<TestCounterCheck>();

    std::vector<std::unique_ptr<WatchdogCheck>> checks;
    checks.push_back(std::move(counterCheck));

    WatchdogCheckThread checkThread(std::move(checks), Milliseconds(5));

    WatchdogMonitorThread monitorThread(&checkThread, deathCallback, Milliseconds(5));

    monitorThread.start();

    deathEvent.wait();

    monitorThread.shutdown();
}

/**
 * Sleep after doing a few checks to replicate a hung check.
 */
class SleepyCheck : public WatchdogCheck {
public:
    void run(OperationContext* opCtx) final {
        ++_counter;

        if (_counter >= 6) {
            sleepFor(Seconds(5));
        }
    }

    std::string getDescriptionForLogging() final {
        return "test";
    }

private:
    std::uint32_t _counter{0};
};

// Positive: Make sure monitor thread signals death if the thread does not make progress
TEST_F(WatchdogMonitorThreadTest, SleepyHungCheck) {
    ManualResetEvent deathEvent;
    WatchdogDeathCallback deathCallback = [&deathEvent]() {
        LOGV2(23432, "Death signalled");
        deathEvent.set();
    };

    auto sleepyCheck = std::make_unique<SleepyCheck>();

    std::vector<std::unique_ptr<WatchdogCheck>> checks;
    checks.push_back(std::move(sleepyCheck));

    WatchdogCheckThread checkThread(std::move(checks), Milliseconds(1));

    WatchdogMonitorThread monitorThread(&checkThread, deathCallback, Milliseconds(100));

    checkThread.start();

    monitorThread.start();

    deathEvent.wait();

    monitorThread.shutdown();

    checkThread.shutdown();
}

class WatchdogMonitorTest : public ServiceContextTest {};

// Positive: Make sure watchdog monitor signals death if a check is unresponsive
TEST_F(WatchdogMonitorTest, SleepyHungCheck) {
    ManualResetEvent deathEvent;
    WatchdogDeathCallback deathCallback = [&deathEvent]() {
        LOGV2(23433, "Death signalled");
        deathEvent.set();
    };

    auto sleepyCheck = std::make_unique<SleepyCheck>();

    std::vector<std::unique_ptr<WatchdogCheck>> checks;
    checks.push_back(std::move(sleepyCheck));

    WatchdogMonitor monitor(std::move(checks), Milliseconds(1), Milliseconds(5), deathCallback);

    monitor.start();

    deathEvent.wait();

    monitor.shutdown();
}

// Positive: Make sure watchdog monitor terminates the process if a check is unresponsive
DEATH_TEST_F(WatchdogMonitorTest, Death, "") {
    auto sleepyCheck = std::make_unique<SleepyCheck>();

    std::vector<std::unique_ptr<WatchdogCheck>> checks;
    checks.push_back(std::move(sleepyCheck));

    WatchdogMonitor monitor(
        std::move(checks), Milliseconds(1), Milliseconds(100), watchdogTerminate);

    monitor.start();

    // In TSAN builds, we need to wait enough time for death to be triggered
    sleepsecs(100);
}

// Positive: Make sure the monitor can be paused and resumed, and it does not trigger death
TEST_F(WatchdogMonitorTest, PauseAndResume) {

    WatchdogDeathCallback deathCallback = []() {
        LOGV2(23434, "Death signalled, it should not have been");
        invariant(false);
    };

    auto counterCheck = std::make_unique<TestCounterCheck>();
    auto counterCheckPtr = counterCheck.get();

    std::vector<std::unique_ptr<WatchdogCheck>> checks;
    checks.push_back(std::move(counterCheck));

    WatchdogMonitor monitor(std::move(checks), Milliseconds(1), Milliseconds(1001), deathCallback);

    counterCheckPtr->setSignalOnCount(5);

    monitor.start();

    counterCheckPtr->waitForCount();

    // Pause the monitor
    monitor.setPeriod(Milliseconds(-1));

    // Check the counter after it is shutdown and make sure it does not change.
    std::uint32_t pauseCounter = counterCheckPtr->getCounter();

    // This is racey but it should only produce false negatives
    sleepmillis(100);

    // We could have had one more run of the loop as we paused - allow for that case
    // but no other runs of the thread.
    ASSERT_GTE(pauseCounter + 1, counterCheckPtr->getCounter());

    // Resume the monitor
    std::uint32_t baseCounter = counterCheckPtr->getCounter();
    counterCheckPtr->setSignalOnCount(baseCounter + 5);

    // Restart the monitor with a different interval.
    monitor.setPeriod(Milliseconds(1007));

    counterCheckPtr->waitForCount();

    monitor.shutdown();

    // Check the counter after it is shutdown and make sure it does not change.
    std::uint32_t lastCounter = counterCheckPtr->getCounter();

    // This is racey but it should only produce false negatives
    sleepmillis(100);

    ASSERT_EQ(lastCounter, counterCheckPtr->getCounter());
}

class DirectoryCheckTest : public ServiceContextTest {};

// Positive: Do a sanity check that directory check passes
TEST_F(DirectoryCheckTest, Basic) {
    unittest::TempDir tempdir("watchdog_testpath");

    DirectoryCheck check(tempdir.path());

    auto opCtx = makeOperationContext();
    check.run(opCtx.get());
}

}  // namespace mongo
