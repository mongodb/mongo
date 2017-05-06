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
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/periodic_runner_asio.h"

namespace mongo {
namespace {

class PeriodicRunnerASIOTest : public unittest::Test {
public:
    void setUp() override {
        auto timerFactory = stdx::make_unique<executor::AsyncTimerFactoryMock>();
        _timerFactory = timerFactory.get();
        _runner = stdx::make_unique<PeriodicRunnerASIO>(std::move(timerFactory));
        _runner->startup();
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

private:
    executor::AsyncTimerFactoryMock* _timerFactory;
    std::unique_ptr<PeriodicRunnerASIO> _runner;
};

TEST_F(PeriodicRunnerASIOTest, OneJobTest) {
    int count = 0;
    Milliseconds interval{5};

    // Add a job, ensure that it runs once
    PeriodicRunner::PeriodicJob job([&count] { count++; }, interval);
    ASSERT_OK(runner()->scheduleJob(std::move(job)));

    // Ensure nothing happens until we fastForward
    ASSERT_EQ(count, 0);

    // Fast forward, we should run once, and only once
    timerFactory().fastForward(interval);
    sleepmillis(10);
    ASSERT_EQ(count, 1);

    // Fast forward again, we should run all ten times.
    for (int i = 0; i < 9; i++) {
        timerFactory().fastForward(interval);
        // Give asio time to execute
        sleepmillis(10);
    }

    ASSERT_EQ(count, 10);
}

TEST_F(PeriodicRunnerASIOTest, ScheduleAfterShutdownTest) {
    int count = 0;
    Milliseconds interval{5};

    // Schedule a job before shutdown
    PeriodicRunner::PeriodicJob job([&count] { count++; }, interval);
    ASSERT_OK(runner()->scheduleJob(std::move(job)));

    // Shut down before the job runs
    runner()->shutdown();

    // Even once we fast forward, job should not get run
    timerFactory().fastForward(interval);
    sleepmillis(10);
    ASSERT_EQ(count, 0);
}

TEST_F(PeriodicRunnerASIOTest, TwoJobsTest) {
    int countA = 0;
    int countB = 0;
    Milliseconds intervalA{5};
    Milliseconds intervalB{10};

    // Add two jobs, ensure they both run the proper number of times
    PeriodicRunner::PeriodicJob jobA([&countA] { countA++; }, intervalA);
    PeriodicRunner::PeriodicJob jobB([&countB] { countB++; }, intervalB);

    ASSERT_OK(runner()->scheduleJob(std::move(jobA)));
    ASSERT_OK(runner()->scheduleJob(std::move(jobB)));

    for (int i = 0; i < 10; i++) {
        timerFactory().fastForward(intervalA);
        sleepmillis(10);
    }

    ASSERT_EQ(countA, 10);
    ASSERT_EQ(countB, 5);
}

}  // namespace
}  // namespace mongo
