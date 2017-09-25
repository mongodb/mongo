/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/executor/async_timer_interface.h"
#include "mongo/executor/async_timer_mock.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/periodic_runner_asio.h"

namespace mongo {

class Client;

namespace {

class PeriodicRunnerASIOTestNoSetup : public unittest::Test {
public:
    void setUp() override {
        auto timerFactory = stdx::make_unique<executor::AsyncTimerFactoryMock>();
        _timerFactory = timerFactory.get();
        _runner = stdx::make_unique<PeriodicRunnerASIO>(std::move(timerFactory));
    }

    void tearDown() override {
        _runner->shutdown();
    }

    executor::AsyncTimerFactoryMock& timerFactory() {
        return *_timerFactory;
    }

    std::unique_ptr<PeriodicRunnerASIO>& runner() {
        return _runner;
    }

    void sleepForReschedule(int jobs) {
        while (timerFactory().jobs() < jobs) {
            sleepmillis(2);
        }
    }

protected:
    executor::AsyncTimerFactoryMock* _timerFactory;
    std::unique_ptr<PeriodicRunnerASIO> _runner;
};

class PeriodicRunnerASIOTest : public PeriodicRunnerASIOTestNoSetup {
public:
    void setUp() override {
        auto timerFactory = stdx::make_unique<executor::AsyncTimerFactoryMock>();
        _timerFactory = timerFactory.get();
        _runner = stdx::make_unique<PeriodicRunnerASIO>(std::move(timerFactory));
        auto res = _runner->startup();
        ASSERT(res.isOK());
    }
};

TEST_F(PeriodicRunnerASIOTest, OneJobTest) {
    int count = 0;
    Milliseconds interval{5};

    stdx::mutex mutex;
    stdx::condition_variable cv;

    // Add a job, ensure that it runs once
    PeriodicRunner::PeriodicJob job(
        [&count, &mutex, &cv](Client*) {
            {
                stdx::unique_lock<stdx::mutex> lk(mutex);
                count++;
            }
            cv.notify_all();
        },
        interval);

    runner()->scheduleJob(std::move(job));

    // Ensure nothing happens until we fastForward
    {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        ASSERT_EQ(count, 0);
    }

    // Fast forward ten times, we should run all ten times.
    for (int i = 0; i < 10; i++) {
        timerFactory().fastForward(interval);
        {
            stdx::unique_lock<stdx::mutex> lk(mutex);
            cv.wait(lk, [&count, &i] { return count > i; });
        }
        sleepForReschedule(2);
    }
}

TEST_F(PeriodicRunnerASIOTestNoSetup, ScheduleBeforeStartupTest) {
    int count = 0;
    Milliseconds interval{5};

    stdx::mutex mutex;
    stdx::condition_variable cv;

    // Schedule a job before startup
    PeriodicRunner::PeriodicJob job(
        [&count, &mutex, &cv](Client*) {
            {
                stdx::unique_lock<stdx::mutex> lk(mutex);
                count++;
            }
            cv.notify_all();
        },
        interval);

    runner()->scheduleJob(std::move(job));

    // Start the runner, job should still run
    ASSERT(runner()->startup().isOK());

    timerFactory().fastForward(interval);

    stdx::unique_lock<stdx::mutex> lk(mutex);
    cv.wait(lk, [&count] { return count > 0; });
}

TEST_F(PeriodicRunnerASIOTest, ScheduleAfterShutdownTest) {
    int count = 0;
    Milliseconds interval{5};

    // Schedule a job before shutdown
    PeriodicRunner::PeriodicJob job([&count](Client*) { count++; }, interval);

    runner()->scheduleJob(std::move(job));

    // Shut down before the job runs
    runner()->shutdown();

    // Even once we fast forward, job should not get run
    timerFactory().fastForward(interval);
    sleepmillis(10);
    ASSERT_EQ(count, 0);

    // Start up the runner again, this should error and should not run
    auto res = runner()->startup();
    ASSERT(!res.isOK());

    timerFactory().fastForward(interval);
    sleepmillis(10);
    ASSERT_EQ(count, 0);
}

TEST_F(PeriodicRunnerASIOTest, TwoJobsTest) {
    int countA = 0;
    int countB = 0;
    Milliseconds intervalA{5};
    Milliseconds intervalB{10};

    stdx::mutex mutex;
    stdx::condition_variable cv;

    // Add two jobs, ensure they both run the proper number of times
    PeriodicRunner::PeriodicJob jobA(
        [&countA, &mutex, &cv](Client*) {
            {
                stdx::unique_lock<stdx::mutex> lk(mutex);
                countA++;
            }
            cv.notify_all();
        },
        intervalA);

    PeriodicRunner::PeriodicJob jobB(
        [&countB, &mutex, &cv](Client*) {
            {
                stdx::unique_lock<stdx::mutex> lk(mutex);
                countB++;
            }
            cv.notify_all();
        },
        intervalB);

    runner()->scheduleJob(std::move(jobA));
    runner()->scheduleJob(std::move(jobB));

    // Fast forward and wait for both jobs to run the right number of times
    for (int i = 0; i <= 10; i++) {
        timerFactory().fastForward(intervalA);
        {
            stdx::unique_lock<stdx::mutex> lk(mutex);
            cv.wait(lk, [&countA, &countB, &i] { return (countA > i && countB >= i / 2); });
        }
        sleepForReschedule(3);
    }
}

TEST_F(PeriodicRunnerASIOTest, TwoJobsDontDeadlock) {
    stdx::mutex mutex;
    stdx::condition_variable cv;
    stdx::condition_variable doneCv;
    bool a = false;
    bool b = false;

    PeriodicRunner::PeriodicJob jobA(
        [&](Client*) {
            stdx::unique_lock<stdx::mutex> lk(mutex);
            a = true;

            cv.notify_one();
            cv.wait(lk, [&] { return b; });
            doneCv.notify_one();
        },
        Milliseconds(1));

    PeriodicRunner::PeriodicJob jobB(
        [&](Client*) {
            stdx::unique_lock<stdx::mutex> lk(mutex);
            b = true;

            cv.notify_one();
            cv.wait(lk, [&] { return a; });
            doneCv.notify_one();
        },
        Milliseconds(1));

    runner()->scheduleJob(std::move(jobA));
    runner()->scheduleJob(std::move(jobB));

    timerFactory().fastForward(Milliseconds(1));

    {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        doneCv.wait(lk, [&] { return a && b; });

        ASSERT(a);
        ASSERT(b);
    }

    tearDown();
}

}  // namespace
}  // namespace mongo
