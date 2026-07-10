/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/otel/metrics/instrumentation/observable_mutex_metrics.h"

#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <string_view>

namespace mongo {
namespace {

using otel::metrics::DynamicMetricNameMaker;
using otel::metrics::DynamicMetricNameTestPasskeyMaker;
using otel::metrics::OtelMetricsCapturer;

constexpr std::string_view kTestMutexTag = "testMutex";

constexpr std::string_view kTestMutexTagExTotal =
    "serverStatus.lockContentionMetrics.testMutex.exclusive.total";
constexpr std::string_view kTestMutexTagExContentions =
    "serverStatus.lockContentionMetrics.testMutex.exclusive.contentions";
constexpr std::string_view kTestMutexTagExWaitCycles =
    "serverStatus.lockContentionMetrics.testMutex.exclusive.waitCycles";
constexpr std::string_view kTestMutexTagShTotal =
    "serverStatus.lockContentionMetrics.testMutex.shared.total";
constexpr std::string_view kTestMutexTagShContentions =
    "serverStatus.lockContentionMetrics.testMutex.shared.contentions";
constexpr std::string_view kTestMutexTagShWaitCycles =
    "serverStatus.lockContentionMetrics.testMutex.shared.waitCycles";

StringMap<MutexStats> makeTagStats(std::string_view tag,
                                   uint64_t exTotal,
                                   uint64_t exContentions,
                                   uint64_t exWait,
                                   uint64_t shTotal,
                                   uint64_t shContentions,
                                   uint64_t shWait) {
    return {{std::string(tag),
             MutexStats{
                 .exclusiveAcquisitions = {exTotal, exContentions, exWait},
                 .sharedAcquisitions = {shTotal, shContentions, shWait},
             }}};
}

class ObservableMutexOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    int64_t readCounter(std::string_view name) {
        auto passkey = DynamicMetricNameTestPasskeyMaker::make();
        return capturer.readInt64Counter(DynamicMetricNameMaker::make(name, passkey));
    }

    OtelMetricsCapturer capturer;
    ObservableMutexMetrics metrics;
};

TEST_F(ObservableMutexOtelMetricsTest, CounterNamesAreScopedToTag) {
    metrics.update(makeTagStats(kTestMutexTag, 0, 0, 0, 0, 0, 0));
    ASSERT_DOES_NOT_THROW(readCounter(kTestMutexTagExTotal));

    ASSERT_THROWS_CODE(
        readCounter("serverStatus.lockContentionMetrics.newTestMutex.exclusive.total"),
        DBException,
        ErrorCodes::KeyNotFound);
}

TEST_F(ObservableMutexOtelMetricsTest, UpdateCreatesAllCountersForNewTag) {
    metrics.update(makeTagStats(kTestMutexTag, 10, 2, 300, 5, 1, 200));

    EXPECT_EQ(readCounter(kTestMutexTagExTotal), 10);
    EXPECT_EQ(readCounter(kTestMutexTagExContentions), 2);
    EXPECT_EQ(readCounter(kTestMutexTagExWaitCycles), 300);
    EXPECT_EQ(readCounter(kTestMutexTagShTotal), 5);
    EXPECT_EQ(readCounter(kTestMutexTagShContentions), 1);
    EXPECT_EQ(readCounter(kTestMutexTagShWaitCycles), 200);
}

TEST_F(ObservableMutexOtelMetricsTest, UpdatePreservesCountersWhenTagDisappearsAndReappears) {
    metrics.update(makeTagStats(kTestMutexTag, 10, 2, 300, 5, 1, 200));
    EXPECT_EQ(readCounter(kTestMutexTagExTotal), 10);

    metrics.update(StringMap<MutexStats>{});
    EXPECT_EQ(readCounter(kTestMutexTagExTotal), 10);

    metrics.update(makeTagStats(kTestMutexTag, 15, 4, 600, 10, 3, 400));
    EXPECT_EQ(readCounter(kTestMutexTagExTotal), 15);
}

TEST_F(ObservableMutexOtelMetricsTest, PositiveDeltaAccumulatesCounter) {
    metrics.update(makeTagStats(kTestMutexTag, 10, 2, 300, 5, 1, 200));
    metrics.update(makeTagStats(kTestMutexTag, 15, 4, 600, 10, 3, 400));

    EXPECT_EQ(readCounter(kTestMutexTagExTotal), 15);
    EXPECT_EQ(readCounter(kTestMutexTagExContentions), 4);
    EXPECT_EQ(readCounter(kTestMutexTagExWaitCycles), 600);
    EXPECT_EQ(readCounter(kTestMutexTagShTotal), 10);
    EXPECT_EQ(readCounter(kTestMutexTagShContentions), 3);
    EXPECT_EQ(readCounter(kTestMutexTagShWaitCycles), 400);
}

TEST_F(ObservableMutexOtelMetricsTest, NegativeDeltaHasNoEffectOnCounter) {
    metrics.update(makeTagStats(kTestMutexTag, 10, 2, 300, 5, 1, 200));
    metrics.update(makeTagStats(kTestMutexTag, 5, 1, 150, 2, 0, 100));

    EXPECT_EQ(readCounter(kTestMutexTagExTotal), 10);
    EXPECT_EQ(readCounter(kTestMutexTagExContentions), 2);
    EXPECT_EQ(readCounter(kTestMutexTagExWaitCycles), 300);
    EXPECT_EQ(readCounter(kTestMutexTagShTotal), 5);
    EXPECT_EQ(readCounter(kTestMutexTagShContentions), 1);
    EXPECT_EQ(readCounter(kTestMutexTagShWaitCycles), 200);
}

TEST_F(ObservableMutexOtelMetricsTest, DeltaAtMaxIsAppliedToCounter) {
    constexpr uint64_t kInt64Max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

    // A delta exactly at INT64_MAX is representable and must be applied.
    metrics.update(makeTagStats(kTestMutexTag, kInt64Max, 0, 0, 0, 0, 0));
    EXPECT_EQ(readCounter(kTestMutexTagExTotal), std::numeric_limits<int64_t>::max());
}

TEST_F(ObservableMutexOtelMetricsTest, DeltaExceedingMaxHasNoEffectOnCounter) {
    constexpr uint64_t kUint64Max = std::numeric_limits<uint64_t>::max();

    // A delta larger than int64_t can represent casts to a negative value and is dropped rather
    // than wrapping around and corrupting the counter.
    metrics.update(makeTagStats(kTestMutexTag, kUint64Max, 0, 0, 0, 0, 0));
    EXPECT_EQ(readCounter(kTestMutexTagExTotal), 0);
}

TEST_F(ObservableMutexOtelMetricsTest, InvalidOtelTagThrows) {
    StringMap<MutexStats> stats = makeTagStats("Invalid_Tag", 0, 0, 0, 0, 0, 0);
    ASSERT_THROWS_CODE(metrics.update(stats), DBException, ErrorCodes::InvalidOptions);
}

}  // namespace
}  // namespace mongo
