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

#include <boost/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/basic.h"

#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

#include "mongo/util/periodic_runner_impl.h"

#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

class Client;

namespace {

class PeriodicRunnerImplTestNoSetup : public LockerNoopServiceContextTest {
public:
    void setUp() override {
        _clockSource = std::make_unique<ClockSourceMock>();
        _runner = std::make_unique<PeriodicRunnerImpl>(getServiceContext(), _clockSource.get());
    }

    ClockSourceMock& clockSource() {
        return *_clockSource;
    }

    PeriodicRunner& runner() {
        return *_runner;
    }

private:
    std::unique_ptr<ClockSourceMock> _clockSource;
    std::unique_ptr<PeriodicRunner> _runner;
};

class PeriodicRunnerImplTest : public PeriodicRunnerImplTestNoSetup {
public:
    void setUp() override {
        PeriodicRunnerImplTestNoSetup::setUp();
    }

    auto makeStoppedJob() {
        PeriodicRunner::PeriodicJob job(
            "job", [](Client* client) {}, Seconds{1}, false);
        auto jobAnchor = runner().makeJob(std::move(job));
        jobAnchor.start();
        jobAnchor.stop();
        return jobAnchor;
    }
};

TEST_F(PeriodicRunnerImplTest, OneJobTest) {
    int count = 0;
    Milliseconds interval{5};

    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;

    // Add a job, ensure that it runs once
    PeriodicRunner::PeriodicJob job(
        "job",
        [&count, &mutex, &cv](Client*) {
            {
                stdx::unique_lock<Latch> lk(mutex);
                count++;
            }
            cv.notify_all();
        },
        interval,
        false);

    auto jobAnchor = runner().makeJob(std::move(job));
    jobAnchor.start();

    // Fast forward ten times, we should run all ten times.
    for (int i = 0; i < 10; i++) {
        clockSource().advance(interval);
        {
            stdx::unique_lock<Latch> lk(mutex);
            cv.wait(lk, [&count, &i] { return count > i; });
        }
    }

    tearDown();
}

TEST_F(PeriodicRunnerImplTest, OnePausableJobDoesNotRunWithoutStart) {
    int count = 0;
    Milliseconds interval{5};

    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;

    // Add a job, ensure that it runs once
    PeriodicRunner::PeriodicJob job(
        "job",
        [&count, &mutex, &cv](Client*) {
            {
                stdx::unique_lock<Latch> lk(mutex);
                count++;
            }
            cv.notify_all();
        },
        interval,
        false);

    auto jobAnchor = runner().makeJob(std::move(job));
    clockSource().advance(interval);
    ASSERT_EQ(count, 0);

    tearDown();
}

TEST_F(PeriodicRunnerImplTest, OnePausableJobRunsCorrectlyWithStart) {
    int count = 0;
    Milliseconds interval{5};

    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;

    // Add a job, ensure that it runs once
    PeriodicRunner::PeriodicJob job(
        "job",
        [&count, &mutex, &cv](Client*) {
            {
                stdx::unique_lock<Latch> lk(mutex);
                count++;
            }
            cv.notify_all();
        },
        interval,
        false);

    auto jobAnchor = runner().makeJob(std::move(job));
    jobAnchor.start();
    // Fast forward ten times, we should run all ten times.
    for (int i = 0; i < 10; i++) {
        {
            stdx::unique_lock<Latch> lk(mutex);
            cv.wait(lk, [&] { return count == i + 1; });
        }
        clockSource().advance(interval);
    }

    tearDown();
}

TEST_F(PeriodicRunnerImplTest, OnePausableJobPausesCorrectly) {
    bool hasExecuted = false;
    bool isPaused = false;
    Milliseconds interval{5};

    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;

    // Add a job, ensure that it runs once
    PeriodicRunner::PeriodicJob job(
        "job",
        [&](Client*) {
            {
                stdx::unique_lock<Latch> lk(mutex);
                // This will fail if pause does not work correctly.
                ASSERT_FALSE(isPaused);
                hasExecuted = true;
            }
            cv.notify_all();
        },
        interval,
        false);

    auto jobAnchor = runner().makeJob(std::move(job));
    jobAnchor.start();
    // Wait for the first execution.
    {
        stdx::unique_lock<Latch> lk(mutex);
        cv.wait(lk, [&] { return hasExecuted; });
    }

    {
        stdx::unique_lock<Latch> lk(mutex);
        isPaused = true;
        jobAnchor.pause();
    }

    // Fast forward ten times, we shouldn't run anymore. If we do, the assert inside the job will
    // fail. (Note that even if the pausing behavior were incorrect, this is racy since tearDown()
    // could happen before the job executes, leading the test to incorrectly succeed from time to
    // time.)
    for (int i = 0; i < 10; i++) {
        clockSource().advance(interval);
    }

    tearDown();
}

TEST_F(PeriodicRunnerImplTest, OnePausableJobResumesCorrectly) {
    int count = 0;
    Milliseconds interval{5};

    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;

    PeriodicRunner::PeriodicJob job(
        "job",
        [&count, &mutex, &cv](Client*) {
            {
                stdx::unique_lock<Latch> lk(mutex);
                count++;
            }
            cv.notify_all();
        },
        interval,
        false);

    auto jobAnchor = runner().makeJob(std::move(job));
    jobAnchor.start();
    // Wait for the first execution.
    {
        stdx::unique_lock<Latch> lk(mutex);
        cv.wait(lk, [&] { return count == 1; });
    }

    // Reset the count before iterating to make other conditions in the test slightly simpler to
    // follow.
    count = 0;

    int numIterationsBeforePause = 10;
    // Fast forward ten times, we should run exactly 10 times.
    for (int i = 0; i < numIterationsBeforePause; i++) {
        clockSource().advance(interval);

        {
            stdx::unique_lock<Latch> lk(mutex);
            // Wait for count to increment due to job execution.
            cv.wait(lk, [&] { return count == i + 1; });
        }
    }

    jobAnchor.pause();

    // Fast forward ten times, we shouldn't run anymore.
    for (int i = 0; i < 10; i++) {
        clockSource().advance(interval);
    }

    // Make sure we didn't run anymore while paused.
    ASSERT_EQ(count, numIterationsBeforePause);

    jobAnchor.resume();
    // Fast forward, we should run at least once.
    clockSource().advance(interval);

    // Wait for count to increase. Test will hang if resume() does not work correctly.
    {
        stdx::unique_lock<Latch> lk(mutex);
        cv.wait(lk, [&] { return count > numIterationsBeforePause; });
    }

    tearDown();
}

TEST_F(PeriodicRunnerImplTest, TwoJobsTest) {
    int countA = 0;
    int countB = 0;
    Milliseconds intervalA{5};
    Milliseconds intervalB{10};

    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;

    // Add two jobs, ensure they both run the proper number of times
    PeriodicRunner::PeriodicJob jobA(
        "job",
        [&countA, &mutex, &cv](Client*) {
            {
                stdx::unique_lock<Latch> lk(mutex);
                countA++;
            }
            cv.notify_all();
        },
        intervalA,
        false);

    PeriodicRunner::PeriodicJob jobB(
        "job",
        [&countB, &mutex, &cv](Client*) {
            {
                stdx::unique_lock<Latch> lk(mutex);
                countB++;
            }
            cv.notify_all();
        },
        intervalB,
        false);

    auto jobAnchorA = runner().makeJob(std::move(jobA));
    auto jobAnchorB = runner().makeJob(std::move(jobB));

    jobAnchorA.start();
    jobAnchorB.start();

    // Fast forward and wait for both jobs to run the right number of times
    for (int i = 0; i <= 10; i++) {
        clockSource().advance(intervalA);
        {
            stdx::unique_lock<Latch> lk(mutex);
            cv.wait(lk, [&countA, &countB, &i] { return (countA > i && countB >= i / 2); });
        }
    }

    tearDown();
}

TEST_F(PeriodicRunnerImplTest, TwoJobsDontDeadlock) {
    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;
    stdx::condition_variable doneCv;
    bool a = false;
    bool b = false;

    PeriodicRunner::PeriodicJob jobA(
        "job",
        [&](Client*) {
            stdx::unique_lock<Latch> lk(mutex);
            a = true;

            cv.notify_one();
            cv.wait(lk, [&] { return b; });
            doneCv.notify_one();
        },
        Milliseconds(1),
        false);

    PeriodicRunner::PeriodicJob jobB(
        "job",
        [&](Client*) {
            stdx::unique_lock<Latch> lk(mutex);
            b = true;

            cv.notify_one();
            cv.wait(lk, [&] { return a; });
            doneCv.notify_one();
        },
        Milliseconds(1),
        false);

    auto jobAnchorA = runner().makeJob(std::move(jobA));
    auto jobAnchorB = runner().makeJob(std::move(jobB));

    jobAnchorA.start();
    jobAnchorB.start();

    clockSource().advance(Milliseconds(1));

    {
        stdx::unique_lock<Latch> lk(mutex);
        doneCv.wait(lk, [&] { return a && b; });

        ASSERT(a);
        ASSERT(b);
    }

    tearDown();
}

TEST_F(PeriodicRunnerImplTest, ChangingIntervalWorks) {
    size_t timesCalled = 0;

    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;

    // Add a job, ensure that it runs once
    PeriodicRunner::PeriodicJob job(
        "job",
        [&](Client*) {
            {
                stdx::unique_lock<Latch> lk(mutex);
                timesCalled++;
            }
            cv.notify_one();
        },
        Milliseconds(5),
        false);

    auto jobAnchor = runner().makeJob(std::move(job));
    jobAnchor.start();
    // Wait for the first execution.
    {
        stdx::unique_lock<Latch> lk(mutex);
        cv.wait(lk, [&] { return timesCalled; });
    }

    jobAnchor.setPeriod(Milliseconds(10));
    ASSERT_EQ(jobAnchor.getPeriod(), Milliseconds(10));

    // if we change the period to a longer duration, that doesn't trigger a run
    {
        stdx::lock_guard<Latch> lk(mutex);
        ASSERT_EQ(timesCalled, 1ul);
    }

    clockSource().advance(Milliseconds(5));

    // We actually changed the period
    {
        stdx::lock_guard<Latch> lk(mutex);
        ASSERT_EQ(timesCalled, 1ul);
    }

    clockSource().advance(Milliseconds(5));

    // Now we hit the new cutoff
    {
        stdx::unique_lock<Latch> lk(mutex);
        cv.wait(lk, [&] { return timesCalled == 2ul; });
    }

    clockSource().advance(Milliseconds(5));

    // Haven't hit it
    {
        stdx::lock_guard<Latch> lk(mutex);
        ASSERT_EQ(timesCalled, 2ul);
    }

    jobAnchor.setPeriod(Milliseconds(4));
    ASSERT_EQ(jobAnchor.getPeriod(), Milliseconds(4));

    // shortening triggers the period
    {
        stdx::unique_lock<Latch> lk(mutex);
        cv.wait(lk, [&] { return timesCalled == 3ul; });
    }

    tearDown();
}

TEST_F(PeriodicRunnerImplTest, StopProperlyInterruptsOpCtx) {
    Milliseconds interval{5};
    unittest::Barrier barrier(2);
    AtomicWord<bool> killed{false};

    PeriodicRunner::PeriodicJob job(
        "job",
        [&barrier, &killed](Client* client) {
            stdx::condition_variable cv;
            auto mutex = MONGO_MAKE_LATCH();
            barrier.countDownAndWait();

            try {
                auto opCtx = client->makeOperationContext();
                stdx::unique_lock<Latch> lk(mutex);
                opCtx->waitForConditionOrInterrupt(cv, lk, [] { return false; });
            } catch (const ExceptionForCat<ErrorCategory::CancellationError>& e) {
                ASSERT_EQ(e.code(), ErrorCodes::ClientMarkedKilled);
                killed.store(true);
                return;
            }

            MONGO_UNREACHABLE;
        },
        interval,
        false);

    auto jobAnchor = runner().makeJob(std::move(job));
    jobAnchor.start();

    barrier.countDownAndWait();

    jobAnchor.stop();
    ASSERT(killed.load());

    tearDown();
}

TEST_F(PeriodicRunnerImplTest, ThrowsErrorOnceStopped) {
    auto jobAnchor = makeStoppedJob();
    ASSERT_THROWS_CODE_AND_WHAT(jobAnchor.start(),
                                AssertionException,
                                ErrorCodes::PeriodicJobIsStopped,
                                "Attempted to start an already stopped job");
    ASSERT_THROWS_CODE_AND_WHAT(jobAnchor.pause(),
                                AssertionException,
                                ErrorCodes::PeriodicJobIsStopped,
                                "Attempted to pause an already stopped job");
    ASSERT_THROWS_CODE_AND_WHAT(jobAnchor.resume(),
                                AssertionException,
                                ErrorCodes::PeriodicJobIsStopped,
                                "Attempted to resume an already stopped job");
    jobAnchor.stop();
}

}  // namespace
}  // namespace mongo
