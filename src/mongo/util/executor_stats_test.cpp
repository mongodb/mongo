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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include <fmt/format.h>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/executor_stats.h"

namespace mongo {

class ExecutorStatsTest : public unittest::Test {
public:
    void setUp() override {
        _stats = std::make_unique<ExecutorStats>(&_clkSource);
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

    void advanceTime(Milliseconds step) {
        _clkSource.advance(step);
    }

    BSONObj getStats() const {
        BSONObjBuilder bob;
        _stats->serialize(&bob);
        return bob.obj();
    }

    void runTimingTests(std::string histogramTag,
                        std::function<size_t(Microseconds, Microseconds)> test) {
        // Represents a test case, where `min` and `max` will be provided to `test`. The expected
        // behavior for `ExecutorStats` is to update the histogram bucket corresponding to `tag`.
        struct Bucket {
            std::string tag;
            Microseconds min;
            Microseconds max;
        };

        const Bucket inputs[]{
            {"0-999us", Microseconds(0), Microseconds(1000)},
            {"1-49ms", Milliseconds(1), Milliseconds(50)},
            {"50-99ms", Milliseconds(50), Milliseconds(100)},
            {"500-549ms", Milliseconds(500), Milliseconds(550)},
            {"950-999ms", Milliseconds(950), Milliseconds(1000)},
            {"1000ms+", Seconds(1), Seconds(100)},
        };

        for (const auto& bucket : inputs) {
            // Runs the test and captures stats before and after.
            // For each entry in `inputs`, the following calls into `test` and provides `min` and
            // `max` delays. The test function returns the number of tasks it schedules and runs.
            const auto beforeStats = getStats().getObjectField(histogramTag).getOwned();

            const auto numTasks = test(bucket.min, bucket.max);

            const auto afterStats = getStats().getObjectField(histogramTag).getOwned();

            // Verify stats by inspecting the before and after states. The expectation is for the
            // bucket corresponding to `bucket.tag` to increase by `numTasks`, and for all other
            // buckets to remain unchanged.
            std::string errMsg =
                fmt::format("Bad value for bucket '{}' in {}", bucket.tag, afterStats.toString());
            ASSERT_EQ(afterStats.getField(bucket.tag).numberLong(), numTasks) << errMsg;

            for (const auto& element : afterStats) {
                const auto fieldName = element.fieldName();
                if (fieldName == bucket.tag)
                    continue;
                std::string errMsg = fmt::format("Expected matching values for '{}' in {} and {}",
                                                 fieldName,
                                                 beforeStats.toString(),
                                                 afterStats.toString());
                ASSERT_EQ(beforeStats.getField(fieldName).numberLong(), element.numberLong())
                    << errMsg;
            }
        }

        LOGV2(5985801, "Execution stats", "stats"_attr = getStats());
    }

private:
    ClockSourceMock _clkSource;

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
     * Schedules a total of 100 tasks and introduces artificial delays before running each task.
     * The delays start at `min`, never exceed `max`, and are incrementally increased by `step`.
     */
    auto test = [this](Microseconds min, Microseconds max) {
        const auto numTasks = 100;
        const auto step = (max - min) / numTasks;
        ASSERT_GT(step, Microseconds{0});

        size_t scheduledTasks = 0;
        for (auto delay = min; delay < max; delay += step, scheduledTasks++) {
            auto task = wrapTask([](Status) {});
            advanceTime(duration_cast<Milliseconds>(delay));
            task(Status::OK());
        }

        return scheduledTasks;
    };

    runTimingTests("waitTime", test);
}

TEST_F(ExecutorStatsTest, RunTime) {
    /**
     * Schedules a few tasks and introduces artificial delays as running each task. These delays
     * follow the same semantics as mentioned earlier in `ExecutorStatsTest::WaitTime`.
     */
    auto test = [this](Microseconds min, Microseconds max) {
        const auto numTasks = 20;
        const auto step = (max - min) / numTasks;
        ASSERT_GT(step, Microseconds{0});

        size_t scheduledTasks = 0;
        for (auto delay = min; delay < max; delay += step, scheduledTasks++) {
            auto task = wrapTask(
                [this, delay](Status) { advanceTime(duration_cast<Milliseconds>(delay)); });
            task(Status::OK());
        }

        return scheduledTasks;
    };

    runTimingTests("runTime", test);
}

}  // namespace mongo
