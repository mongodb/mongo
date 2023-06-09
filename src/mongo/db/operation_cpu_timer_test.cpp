/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/operation_cpu_timer.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

class OperationCPUTimerTest : public ServiceContextTest {
public:
    auto makeClient() const {
        return getGlobalServiceContext()->makeClient("AlternativeClient");
    }

    OperationCPUTimers* getTimers() const {
        return OperationCPUTimers::get(_opCtx.get());
    }

    std::unique_ptr<OperationCPUTimer> makeTimer() const {
        return getTimers()->makeTimer();
    }

    void setUp() {
        _opCtx = getGlobalServiceContext()->makeOperationContext(Client::getCurrent());
    }

    void busyWait(Nanoseconds delay) const {
        AtomicWord<bool> mayJoin{false};
        stdx::thread blocker([&] {
            sleepFor(delay);
            mayJoin.store(true);
        });
        while (!mayJoin.load()) {
            // Busy wait for the blocker thread.
        }
        blocker.join();
    }

    void resetOpCtx() {
        _opCtx.reset();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

#if defined(__linux__)

TEST_F(OperationCPUTimerTest, TestTimer) {
    auto timer = makeTimer();

    timer->start();
    busyWait(Microseconds(1));  // A small delay to make sure the timer advances.
    ASSERT_GT(timer->getElapsed(), Nanoseconds(0));
    timer->stop();

    const auto elapsedAfterStop = timer->getElapsed();
    busyWait(Microseconds(10));  // A relatively longer delay to ensure the timer doesn't proceed.
    const auto elapsedAfterSleep = timer->getElapsed();
    ASSERT_EQ(elapsedAfterStop, elapsedAfterSleep);
}

TEST_F(OperationCPUTimerTest, TestReset) {
    auto timer = makeTimer();

    timer->start();
    busyWait(Milliseconds(2));  // Introducing some delay for the timer to measure.
    timer->stop();
    auto elapsedAfterStop = timer->getElapsed();
    // Due to inconsistencies between the CPU time-based clock used in the timer and the
    // clock used in busyWait, the elapsed CPU time is sometimes observed as being less than the
    // time spent busy waiting. To account for that, only assert that at least 1ms of CPU time has
    // elapsed even though the thread was supposed to have busy-waited for 2ms.
    ASSERT_GTE(elapsedAfterStop, Milliseconds(1));

    timer->start();
    auto elapsedAfterReset = timer->getElapsed();
    ASSERT_LT(elapsedAfterReset, elapsedAfterStop);
}

TEST_F(OperationCPUTimerTest, TestTimerDetachAndAttachHandlers) {
    unittest::Barrier failPointsReady{2};
    stdx::thread observer([&] {
        FailPointEnableBlock fpAttach("hangCPUTimerAfterOnThreadAttach");
        {
            FailPointEnableBlock fpDetach("hangCPUTimerAfterOnThreadDetach");
            failPointsReady.countDownAndWait();
            fpDetach->waitForTimesEntered(fpDetach.initialTimesEntered() + 1);
        }
        fpAttach->waitForTimesEntered(fpAttach.initialTimesEntered() + 1);
    });

    auto timer1 = makeTimer();
    timer1->start();

    auto timer2 = makeTimer();
    timer2->start();

    failPointsReady.countDownAndWait();
    {
        auto client = makeClient();
        AlternativeClientRegion acr(client);
    }

    busyWait(Microseconds(1));  // A small delay to make sure the timers advance.

    timer1->stop();
    timer2->stop();
    observer.join();

    ASSERT_GT(timer1->getElapsed(), Nanoseconds(0));
    ASSERT_GT(timer2->getElapsed(), Nanoseconds(0));
}

TEST_F(OperationCPUTimerTest, MultipleTimers) {
    auto timer1 = makeTimer();
    timer1->start();

    {
        auto timer2 = makeTimer();
        timer2->start();

        busyWait(Microseconds(1));  // A small delay to make sure the timers advance.
        ASSERT_GT(timer1->getElapsed(), Nanoseconds(0));
        ASSERT_GT(timer2->getElapsed(), Nanoseconds(0));

        ASSERT_EQ(2, getTimers()->count());
    }

    ASSERT_EQ(1, getTimers()->count());

    timer1->stop();

    auto elapsedAfterStop = timer1->getElapsed();
    busyWait(Milliseconds(10));  // A small delay to make sure the timer could advance.
    auto elapsedAfterSleep = timer1->getElapsed();
    ASSERT_EQ(elapsedAfterSleep, elapsedAfterStop);
}

TEST_F(OperationCPUTimerTest, MultipleTimersOutOfOrder) {
    auto timer1 = makeTimer();
    timer1->start();

    auto timer2 = makeTimer();
    timer2->start();

    busyWait(Microseconds(1));  // A small delay to make sure the timers advance.
    ASSERT_GT(timer1->getElapsed(), Nanoseconds(0));
    ASSERT_GT(timer2->getElapsed(), Nanoseconds(0));

    // Note that there should be no restriction against stopping the first timer first.
    timer1->stop();

    auto elapsedAfterStop = timer1->getElapsed();
    busyWait(Milliseconds(10));  // A small delay to make sure the timer could advance.
    auto elapsedAfterSleep = timer1->getElapsed();
    ASSERT_EQ(elapsedAfterSleep, elapsedAfterStop);

    timer2->stop();
    ASSERT_GT(timer2->getElapsed(), elapsedAfterStop);
    elapsedAfterStop = timer2->getElapsed();
    busyWait(Milliseconds(10));  // A small delay to make sure the timer could advance.
    elapsedAfterSleep = timer2->getElapsed();
    ASSERT_EQ(elapsedAfterSleep, elapsedAfterStop);

    ASSERT_EQ(2, getTimers()->count());
}

TEST_F(OperationCPUTimerTest, TestOpCtxDestruction) {
    auto timer = makeTimer();
    timer->start();
    resetOpCtx();
    timer->stop();
}

DEATH_TEST_F(OperationCPUTimerTest, StopTimerBeforeStart, "Timer is not running") {
    auto timer = makeTimer();
    timer->stop();
}

DEATH_TEST_F(OperationCPUTimerTest, StartTimerMultipleTimes, "Timer has already started") {
    auto timer = makeTimer();
    timer->start();
    timer->start();
}

DEATH_TEST_F(OperationCPUTimerTest, OnAttachForAttachedTimer, "Timer has already been attached") {
    auto timer = makeTimer();
    timer->start();
    timer->onThreadAttach();
}

DEATH_TEST_F(OperationCPUTimerTest, OnDetachForDetachedTimer, "Timer is not attached") {
    auto timer = makeTimer();
    timer->start();
    auto client = Client::releaseCurrent();
    timer->onThreadDetach();
}

DEATH_TEST_F(OperationCPUTimerTest, GetElapsedForPausedTimer, "Not attached to current thread") {
    auto timer = makeTimer();
    timer->start();
    auto client = Client::releaseCurrent();
    timer->getElapsed();
}

TEST_F(OperationCPUTimerTest, TimerPausesOnBlockingSleep) {
    // This test checks if the time measured by `OperationCPUTimer` does not include the period of
    // time the operation was blocked (e.g., waiting on a condition variable). The idea is to have
    // the operation block for `kSomeDelay`, ensure the elapsed time observed by the timer is always
    // less than `kSomeDelay`, and repeat the test `kRepeats` times. To account for the sporadic
    // wake ups, the test does not fail unless the number of failures exceeds `kMaxFailures`. This
    // is just a best-effort, and the number of failures is not guaranteed to not exceed the upper
    // bound (i.e., `kMaxFailures`).
    const auto kSomeDelay = Milliseconds(1);
    const auto kRepeats = 1000;
    const auto kMaxFailureRate = 0.1;
    const auto kMaxFailures = kMaxFailureRate * kRepeats;

    auto timer = makeTimer();

    auto checkTimer = [&] {
        auto elapsed = timer->getElapsed();
        if (elapsed < kSomeDelay)
            return true;
        LOGV2_WARNING(5160101,
                      "Elapsed operation time exceeded the upper bound",
                      "elapsed"_attr = elapsed,
                      "delay"_attr = kSomeDelay);
        return false;
    };

    auto failures = 0;
    for (auto i = 0; i < kRepeats; i++) {
        timer->start();
        sleepFor(kSomeDelay);
        if (!checkTimer())
            failures++;
        timer->stop();

        stdx::condition_variable cv;
        auto mutex = MONGO_MAKE_LATCH("TimerPausesOnBlockingSleep");
        timer->start();
        stdx::unique_lock lk(mutex);
        cv.wait_for(lk, kSomeDelay.toSystemDuration(), [] { return false; });
        if (!checkTimer())
            failures++;
        timer->stop();
    }

    ASSERT_LTE(failures, kMaxFailures);
}

#else

TEST_F(OperationCPUTimerTest, TimerNotSetIfNotSupported) {
    auto timer = getTimers();
    ASSERT(timer == nullptr);
}

#endif  // defined(__linux__)

}  // namespace mongo
