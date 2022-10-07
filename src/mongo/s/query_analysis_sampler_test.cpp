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

#include "mongo/s/query_analysis_sampler.h"

#include "mongo/db/stats/counters.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/periodic_runner_factory.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace analyze_shard_key {
namespace {

const auto smoothingFactor = gQueryAnalysisQueryStatsSmoothingFactor;

TEST(QueryAnalysisSamplerQueryStatsTest, RefreshBasic) {
    // The per-second counts after: [].
    auto queryStats = QueryAnalysisSampler::QueryStats();

    ASSERT_EQ(queryStats.getLastTotalCount(), 0);
    ASSERT_FALSE(queryStats.getLastAvgCount());

    // The per-second counts after: [1].
    queryStats.refreshTotalCount(1);

    ASSERT_EQ(queryStats.getLastTotalCount(), 1);
    double expectedAvgCount1 = 1;
    auto actualAvgCount1 = queryStats.getLastAvgCount();
    ASSERT(actualAvgCount1);
    ASSERT_APPROX_EQUAL(
        *actualAvgCount1, expectedAvgCount1, std::numeric_limits<double>::epsilon());

    // The per-second counts after: [1, 2].
    queryStats.refreshTotalCount(3);

    ASSERT_EQ(queryStats.getLastTotalCount(), 3);
    auto expectedAvgCount2 = (1 - smoothingFactor) * expectedAvgCount1 + smoothingFactor * 2;
    auto actualAvgCount2 = queryStats.getLastAvgCount();
    ASSERT(actualAvgCount2);
    ASSERT_APPROX_EQUAL(
        *actualAvgCount2, expectedAvgCount2, std::numeric_limits<double>::epsilon());

    // The per-second counts after: [1, 2, 0].
    queryStats.refreshTotalCount(3);

    ASSERT_EQ(queryStats.getLastTotalCount(), 3);
    auto expectedAvgCount3 = (1 - smoothingFactor) * expectedAvgCount2 + smoothingFactor * 0;
    auto actualAvgCount3 = queryStats.getLastAvgCount();
    ASSERT(actualAvgCount3);
    ASSERT_APPROX_EQUAL(
        *actualAvgCount3, expectedAvgCount3, std::numeric_limits<double>::epsilon());

    // The per-second counts after: [1, 2, 0, 100].
    queryStats.refreshTotalCount(103);

    ASSERT_EQ(queryStats.getLastTotalCount(), 103);
    auto expectedAvgCount4 = (1 - smoothingFactor) * expectedAvgCount3 + smoothingFactor * 100;
    auto actualAvgCount4 = queryStats.getLastAvgCount();
    ASSERT(actualAvgCount4);
    ASSERT_APPROX_EQUAL(
        *actualAvgCount4, expectedAvgCount4, std::numeric_limits<double>::epsilon());
}

TEST(QueryAnalysisSamplerQueryStatsTest, RefreshZeroInitialCount) {
    // The per-second counts after: [].
    auto queryStats = QueryAnalysisSampler::QueryStats();

    ASSERT_EQ(queryStats.getLastTotalCount(), 0);
    ASSERT_FALSE(queryStats.getLastAvgCount());

    // The per-second counts after: [0].
    queryStats.refreshTotalCount(0);

    ASSERT_EQ(queryStats.getLastTotalCount(), 0);
    auto avgCount = queryStats.getLastAvgCount();
    ASSERT(avgCount);
    ASSERT_EQ(*avgCount, 0);
}

DEATH_TEST_REGEX(QueryAnalysisSamplerQueryStatsTest,
                 TotalCountCannotDecrease,
                 "Invariant failure.*Total number of queries cannot decrease") {
    auto queryStats = QueryAnalysisSampler::QueryStats();

    ASSERT_EQ(queryStats.getLastTotalCount(), 0);
    ASSERT_FALSE(queryStats.getLastAvgCount());

    // The per-second counts after: [1].
    queryStats.refreshTotalCount(1);

    ASSERT_EQ(queryStats.getLastTotalCount(), 1);
    auto avgCount = queryStats.getLastAvgCount();
    ASSERT(avgCount);
    ASSERT_EQ(*avgCount, 1);

    queryStats.refreshTotalCount(0);
}

class QueryAnalysisSamplerTest : public ShardingTestFixture {
public:
    void setUp() override {
        ShardingTestFixture::setUp();
        _originalIsMongos = isMongos();
        setMongos(true);

        // Set up a periodic runner.
        auto runner = makePeriodicRunner(getServiceContext());
        getServiceContext()->setPreciseClockSource(std::make_unique<ClockSourceMock>());
        getServiceContext()->setPeriodicRunner(std::move(runner));

        // Reset the counters since each test assumes that the count starts at 0.
        globalOpCounters.resetForTest();
    }

    void tearDown() override {
        ShardingTestFixture::tearDown();
        setMongos(_originalIsMongos);
    }

private:
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagAnalyzeShardKey", true};
    bool _originalIsMongos;
};

DEATH_TEST_F(QueryAnalysisSamplerTest, CannotGetIfFeatureFlagNotEnabled, "invariant") {
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagAnalyzeShardKey",
                                                                false};
    QueryAnalysisSampler::get(operationContext());
}

DEATH_TEST_F(QueryAnalysisSamplerTest, CannotGetIfNotMongos, "invariant") {
    setMongos(false);
    QueryAnalysisSampler::get(operationContext());
}

TEST_F(QueryAnalysisSamplerTest, RefreshQueryStats_InitialCount) {
    auto& sampler = QueryAnalysisSampler::get(operationContext());
    sampler.refreshQueryStatsForTest();

    auto queryStats = sampler.getQueryStatsForTest();
    ASSERT_EQ(queryStats.getLastTotalCount(), 0);
    auto lastAvgCount = queryStats.getLastAvgCount();
    ASSERT(lastAvgCount);
    ASSERT_EQ(*lastAvgCount, 0);
}

TEST_F(QueryAnalysisSamplerTest, RefreshQueryStats_CountQueries) {
    auto& sampler = QueryAnalysisSampler::get(operationContext());
    globalOpCounters.gotQuery();
    sampler.refreshQueryStatsForTest();

    auto queryStats = sampler.getQueryStatsForTest();
    ASSERT_EQ(queryStats.getLastTotalCount(), 1);
    auto lastAvgCount = queryStats.getLastAvgCount();
    ASSERT(lastAvgCount);
    ASSERT_EQ(*lastAvgCount, 1);
}

TEST_F(QueryAnalysisSamplerTest, RefreshQueryStats_CountInserts) {
    auto& sampler = QueryAnalysisSampler::get(operationContext());
    auto numInserts = 3;
    globalOpCounters.gotInserts(numInserts);
    sampler.refreshQueryStatsForTest();

    auto queryStats = sampler.getQueryStatsForTest();
    ASSERT_EQ(queryStats.getLastTotalCount(), numInserts);
    auto lastAvgCount = queryStats.getLastAvgCount();
    ASSERT(lastAvgCount);
    ASSERT_EQ(*lastAvgCount, numInserts);
}

TEST_F(QueryAnalysisSamplerTest, RefreshQueryStats_CountUpdates) {
    auto& sampler = QueryAnalysisSampler::get(operationContext());
    globalOpCounters.gotUpdate();
    sampler.refreshQueryStatsForTest();

    auto queryStats = sampler.getQueryStatsForTest();
    ASSERT_EQ(queryStats.getLastTotalCount(), 1);
    auto lastAvgCount = queryStats.getLastAvgCount();
    ASSERT(lastAvgCount);
    ASSERT_EQ(*lastAvgCount, 1);
}

TEST_F(QueryAnalysisSamplerTest, RefreshQueryStats_CountDeletes) {
    auto& sampler = QueryAnalysisSampler::get(operationContext());
    globalOpCounters.gotDelete();
    sampler.refreshQueryStatsForTest();

    auto queryStats = sampler.getQueryStatsForTest();
    ASSERT_EQ(queryStats.getLastTotalCount(), 1);
    auto lastAvgCount = queryStats.getLastAvgCount();
    ASSERT(lastAvgCount);
    ASSERT_EQ(*lastAvgCount, 1);
}

TEST_F(QueryAnalysisSamplerTest, RefreshQueryStats_CountCommands) {
    auto& sampler = QueryAnalysisSampler::get(operationContext());
    globalOpCounters.gotCommand();
    sampler.refreshQueryStatsForTest();

    auto queryStats = sampler.getQueryStatsForTest();
    ASSERT_EQ(queryStats.getLastTotalCount(), 1);
    auto lastAvgCount = queryStats.getLastAvgCount();
    ASSERT(lastAvgCount);
    ASSERT_EQ(*lastAvgCount, 1);
}

TEST_F(QueryAnalysisSamplerTest, RefreshQueryStats_Average) {
    auto& sampler = QueryAnalysisSampler::get(operationContext());

    auto queryStats0 = sampler.getQueryStatsForTest();
    ASSERT_EQ(queryStats0.getLastTotalCount(), 0);
    ASSERT_FALSE(queryStats0.getLastAvgCount());

    // The per-second counts after: [0].
    sampler.refreshQueryStatsForTest();

    auto queryStats1 = sampler.getQueryStatsForTest();
    auto expectedAvgCount1 = 0;
    auto actualAvgCount1 = queryStats1.getLastAvgCount();
    ASSERT(actualAvgCount1);
    ASSERT_EQ(*actualAvgCount1, expectedAvgCount1);

    // The per-second counts after: [0, 2].
    globalOpCounters.gotInserts(2);
    sampler.refreshQueryStatsForTest();

    auto queryStats2 = sampler.getQueryStatsForTest();
    ASSERT_EQ(queryStats2.getLastTotalCount(), 2);
    auto expectedAvgCount2 = (1 - smoothingFactor) * expectedAvgCount1 + smoothingFactor * 2;
    auto actualAvgCount2 = queryStats2.getLastAvgCount();
    ASSERT(actualAvgCount2);
    ASSERT_EQ(*actualAvgCount2, expectedAvgCount2);

    // The per-second counts after: [0, 2, 5].
    globalOpCounters.gotInserts(5);
    sampler.refreshQueryStatsForTest();

    auto queryStats3 = sampler.getQueryStatsForTest();
    ASSERT_EQ(queryStats3.getLastTotalCount(), 7);
    auto expectedAvgCount3 = (1 - smoothingFactor) * expectedAvgCount2 + smoothingFactor * 5;
    auto actualAvgCount3 = queryStats3.getLastAvgCount();
    ASSERT(actualAvgCount3);
    ASSERT_APPROX_EQUAL(
        *actualAvgCount3, expectedAvgCount3, std::numeric_limits<double>::epsilon());
}

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
