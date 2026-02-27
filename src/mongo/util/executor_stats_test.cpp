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


#include "mongo/util/executor_stats.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source_mock.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

class ExecutorStatsTest : public unittest::Test {
public:
    void setUp() override {
        _stats = std::make_unique<ExecutorStats>(&_tickSource);
    }

    void tearDown() override {
        _stats.reset();
    }

    auto getScheduled() const {
        return getStats().getField("scheduled").numberLong();
    }

    auto getExecuted() const {
        return getStats().getField("executed").numberLong();
    }

    auto getWaitTime() const {
        return getStats().getObjectField("waitTime").getOwned();
    }

    auto getRunTime() const {
        return getStats().getObjectField("runTime").getOwned();
    }

    auto wrapTask(ExecutorStats::Task&& task) {
        return _stats->wrapTask(std::move(task));
    }

    void advanceTime(Nanoseconds step) {
        _tickSource.advance(step);
    }

    BSONObj getStats() const {
        BSONObjBuilder bob;
        _stats->serialize(&bob);
        return bob.obj();
    }

    BSONObj getServerStatusStats() const {
        BSONObjBuilder bob;
        _stats->serialize(&bob, true /* forServerStatus */);
        return bob.obj();
    }

    auto getAverageWaitTimeMicros() const {
        return getServerStatusStats().getField("averageWaitTimeMicros").numberDouble();
    }

    auto getAverageRunTimeMicros() const {
        return getServerStatusStats().getField("averageRunTimeMicros").numberDouble();
    }

    const Histogram<Microseconds>& getWaiting() const {
        return _stats->waiting_forTest();
    }

    const Histogram<Microseconds>& getRunning() const {
        return _stats->running_forTest();
    }

    void runTimingTests(const Histogram<Microseconds>& histogram,
                        std::function<size_t(Microseconds, Microseconds)> test) {
        // For each bucket in `histogram`, invoke `test` with the bounds of the bucket.
        // `test` will cause side effects on our `ExecutorStats` such that the
        // corresponding bucket will be incremented by a known amount, and no
        // other buckets will be incremented.
        struct Bucket {
            size_t i;
            Microseconds lowerBound;
            Microseconds upperBound;
        };
        Microseconds lower{0};
        std::vector<Bucket> buckets;
        for (Microseconds upper : histogram.getPartitions()) {
            buckets.push_back({.i = buckets.size(), .lowerBound = lower, .upperBound = upper});
            lower = upper;
        }
        // "100 seconds" arbitrarily standing in for "infinitely long."
        buckets.push_back({.i = buckets.size(), .lowerBound = lower, .upperBound = Seconds(100)});

        for (const auto& [i, lowerBound, upperBound] : buckets) {
            // Runs the test and captures stats before and after.
            // For each entry in `inputs`, the following calls into `test` and provides `min` and
            // `max` delays. The test function returns the number of tasks it schedules and runs.
            const auto countsBefore = histogram.getCounts();
            const auto numTasks = test(lowerBound, upperBound);
            const auto countsAfter = histogram.getCounts();

            // Verify stats by inspecting the before and after states. The expectation is for the
            // bucket corresponding to `bucket.tag` to increase by `numTasks`, and for all other
            // buckets to remain unchanged.
            std::string errMsg = fmt::format(
                "bucket {} with lower bound {} and upper bound {} did not have the expected value.",
                i,
                lowerBound,
                upperBound);
            ASSERT_EQ(countsAfter[i], numTasks) << errMsg;

            for (size_t j = 0; j < countsAfter.size(); ++j) {
                if (j == i)
                    continue;
                std::string errMsg = fmt::format(
                    "bucket {} with lower bound {} and upper bound{} was expected not to have "
                    "changed.",
                    i,
                    lowerBound,
                    upperBound);
                ASSERT_EQ(countsBefore[j], countsAfter[j]) << errMsg;
            }
        }

        LOGV2(5985801, "Execution stats", "stats"_attr = getStats());
    }

private:
    TickSourceMock<Nanoseconds> _tickSource;

    std::unique_ptr<ExecutorStats> _stats;
};

TEST_F(ExecutorStatsTest, ScheduledAndExecutedMetrics) {
    /**
     * Wrap a total of three tasks and run them to ensure both `scheduled` and `executed`
     * metrics are properly incremented.
     */
    auto t1 = wrapTask([](Status) {});
    ASSERT_EQ(getScheduled(), 1);
    ASSERT_EQ(getExecuted(), 0);
    t1(Status::OK());
    ASSERT_EQ(getScheduled(), 1);
    ASSERT_EQ(getExecuted(), 1);

    {
        auto t2 = wrapTask([](Status) {});
        ASSERT_EQ(getScheduled(), 2);
        auto t3 = wrapTask([](Status) {});
        ASSERT_EQ(getScheduled(), 3);
        ASSERT_EQ(getExecuted(), 1);

        t3(Status::OK());
        ASSERT_EQ(getExecuted(), 2);
    }

    ASSERT_EQ(getScheduled(), 3);
    ASSERT_EQ(getExecuted(), 2);
}


TEST_F(ExecutorStatsTest, WaitTime) {
    /**
     * Schedules some tasks separated by delays.
     * The delays start at `min`, never exceed `max`, and are incrementally increased by `step`.
     */
    auto test = [this](Nanoseconds min, Nanoseconds max) {
        const auto numTasks = 10;
        const auto step = (max - min) / numTasks;
        ASSERT_GT(step, step.zero());

        size_t scheduledTasks = 0;
        for (auto delay = min; delay < max; delay += step, scheduledTasks++) {
            auto task = wrapTask([](Status) {});
            advanceTime(delay);
            task(Status::OK());
        }

        return scheduledTasks;
    };

    runTimingTests(getWaiting(), test);
}

TEST_F(ExecutorStatsTest, RunTime) {
    /**
     * Schedules some tasks separated by delays.
     * follow the same semantics as mentioned earlier in `ExecutorStatsTest::WaitTime`.
     */
    auto test = [this](Nanoseconds min, Nanoseconds max) {
        const auto numTasks = 10;
        const auto step = (max - min) / numTasks;
        ASSERT_GT(step, step.zero());

        size_t scheduledTasks = 0;
        for (auto delay = min; delay < max; delay += step, scheduledTasks++) {
            auto task = wrapTask([this, delay](Status) { advanceTime(delay); });
            task(Status::OK());
        }

        return scheduledTasks;
    };

    runTimingTests(getRunning(), test);
}

TEST_F(ExecutorStatsTest, SlowTaskExecutorWaitTimeProfilingLog) {
    // Make the tasks wait time 51 Millis exceed the default slow wait time threshold of 50 Millis
    Milliseconds delay = Milliseconds{51};
    unittest::LogCaptureGuard logs;

    auto task = wrapTask([](Status) {});
    advanceTime(delay);
    task(Status::OK());

    logs.stop();
    // Check for the slow wait time log message
    ASSERT_EQUALS(1, logs.countTextContaining("Task exceeded the slow wait time threshold"));
}

TEST_F(ExecutorStatsTest, MovingAverageZeroWithNoTasks) {
    ASSERT_EQ(getAverageWaitTimeMicros(), 0);
    ASSERT_EQ(getAverageRunTimeMicros(), 0);
}

TEST_F(ExecutorStatsTest, MovingAverageSingleTaskWaitTime) {
    auto task = wrapTask([](Status) {});
    advanceTime(Milliseconds(100));
    task(Status::OK());

    // First sample sets the average directly: 100ms = 100000us.
    ASSERT_APPROX_EQUAL(getAverageWaitTimeMicros(), 100000.0, 1.0);
}

TEST_F(ExecutorStatsTest, MovingAverageSingleTaskRunTime) {
    auto task = wrapTask([this](Status) { advanceTime(Milliseconds(50)); });
    task(Status::OK());

    // First sample sets the average directly: 50ms = 50000us.
    ASSERT_APPROX_EQUAL(getAverageRunTimeMicros(), 50000.0, 1.0);
}

TEST_F(ExecutorStatsTest, ServerStatusSerializationContainsAveragesNotHistograms) {
    auto task = wrapTask([this](Status) { advanceTime(Milliseconds(10)); });
    advanceTime(Milliseconds(50));
    task(Status::OK());

    auto obj = getServerStatusStats();

    ASSERT_TRUE(obj.hasField("scheduled"));
    ASSERT_TRUE(obj.hasField("executed"));
    ASSERT_TRUE(obj.hasField("averageWaitTimeMicros"));
    ASSERT_TRUE(obj.hasField("averageRunTimeMicros"));
    ASSERT_FALSE(obj.hasField("waitTime"));
    ASSERT_FALSE(obj.hasField("runTime"));

    ASSERT_EQ(obj.getField("scheduled").numberLong(), 1);
    ASSERT_EQ(obj.getField("executed").numberLong(), 1);
    ASSERT_GT(obj.getField("averageWaitTimeMicros").numberDouble(), 0);
    ASSERT_GT(obj.getField("averageRunTimeMicros").numberDouble(), 0);
}

}  // namespace mongo
