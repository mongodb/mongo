/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/sharding_data_transform_instance_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class InstanceMetricsWithObserverMock {
public:
    InstanceMetricsWithObserverMock(int64_t startTime,
                                    int64_t timeRemaining,
                                    ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
        : _impl{cumulativeMetrics, std::make_unique<ObserverMock>(startTime, timeRemaining)} {}

private:
    ShardingDataTransformInstanceMetrics _impl;
};

class ShardingDataTransformInstanceMetricsTest : public ShardingDataTransformMetricsTestFixture {};

TEST_F(ShardingDataTransformInstanceMetricsTest, RegisterAndDeregisterMetrics) {
    for (auto i = 0; i < 100; i++) {
        ShardingDataTransformInstanceMetrics metrics(&_cumulativeMetrics);
        ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 1);
    }
    ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 0);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RegisterAndDeregisterMetricsAtOnce) {
    {
        std::vector<std::unique_ptr<ShardingDataTransformInstanceMetrics>> registered;
        for (auto i = 0; i < 100; i++) {
            registered.emplace_back(
                std::make_unique<ShardingDataTransformInstanceMetrics>(&_cumulativeMetrics));
            ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), registered.size());
        }
    }
    ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 0);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, UsesObserverToReportTimeEstimate) {
    constexpr auto kExpectedTimeLeft = 1000;
    ShardingDataTransformInstanceMetrics metrics{
        &_cumulativeMetrics, std::make_unique<ObserverMock>(0, kExpectedTimeLeft)};
    ASSERT_EQ(metrics.getRemainingTimeMillis(), kExpectedTimeLeft);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RandomOperations) {
    doRandomOperationsTest<InstanceMetricsWithObserverMock>();
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RandomOperationsMultithreaded) {
    doRandomOperationsMultithreadedTest<InstanceMetricsWithObserverMock>();
}

}  // namespace
}  // namespace mongo
