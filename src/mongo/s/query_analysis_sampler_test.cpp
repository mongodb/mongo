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
        setRemote(HostAndPort("ClientHost", 12345));

        // Set up the RemoteCommandTargeter for the config shard.
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

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

    /**
     * Asserts that the first unprocessed request corresponds to a
     * _refreshQueryAnalyzerConfiguration command and then responds to it with the given
     * configurations. If there are no unprocessed requests, blocks until there is.
     */
    void expectConfigurationRefreshReturnSuccess(
        double expectedNumQueriesExecutedPerSecond,
        std::vector<CollectionQueryAnalyzerConfiguration> refreshedConfigurations) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            auto opMsg = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);

            auto refreshRequest = RefreshQueryAnalyzerConfiguration::parse(
                IDLParserContext("QueryAnalysisSamplerTest"), opMsg.body);
            ASSERT_EQ(refreshRequest.getNumQueriesExecutedPerSecond(),
                      expectedNumQueriesExecutedPerSecond);

            RefreshQueryAnalyzerConfigurationResponse response;
            response.setConfigurations(refreshedConfigurations);
            return response.toBSON();
        });
    }

private:
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagAnalyzeShardKey", true};
    bool _originalIsMongos;

protected:
    const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

    const NamespaceString nss0{"testDb", "testColl0"};
    const NamespaceString nss1{"testDb", "testColl1"};

    const UUID collUuid0 = UUID::gen();
    const UUID collUuid1 = UUID::gen();
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

TEST_F(QueryAnalysisSamplerTest, RefreshQueryStatsAndConfigurations) {
    auto& sampler = QueryAnalysisSampler::get(operationContext());

    auto queryStats0 = sampler.getQueryStatsForTest();
    ASSERT_EQ(queryStats0.getLastTotalCount(), 0);
    ASSERT_FALSE(queryStats0.getLastAvgCount());

    // Force the sampler to refresh its configurations. This should not cause the sampler to send a
    // _refreshQueryAnalyzerConfiguration command to get sent since there is no
    // numQueriesExecutedPerSecond yet.
    sampler.refreshConfigurationsForTest(operationContext());
    auto configurations = sampler.getConfigurationsForTest();
    ASSERT(configurations.empty());

    // The per-second counts after: [0].
    sampler.refreshQueryStatsForTest();

    auto queryStats1 = sampler.getQueryStatsForTest();
    auto expectedAvgCount1 = 0;
    auto actualAvgCount1 = queryStats1.getLastAvgCount();
    ASSERT(actualAvgCount1);
    ASSERT_EQ(*actualAvgCount1, expectedAvgCount1);

    // Force the sampler to refresh its configurations. This should cause the sampler to send a
    // _refreshQueryAnalyzerConfiguration command to get sent and update its configurations even
    // though the numQueriesExecutedPerSecond is 0.
    std::vector<CollectionQueryAnalyzerConfiguration> refreshedConfigurations1;
    refreshedConfigurations1.push_back(CollectionQueryAnalyzerConfiguration{nss0, collUuid0, 1});
    refreshedConfigurations1.push_back(CollectionQueryAnalyzerConfiguration{nss1, collUuid1, 0.5});
    auto future1 = stdx::async(stdx::launch::async, [&] {
        expectConfigurationRefreshReturnSuccess(*queryStats1.getLastAvgCount(),
                                                refreshedConfigurations1);
    });
    sampler.refreshConfigurationsForTest(operationContext());
    future1.get();

    auto configurations1 = sampler.getConfigurationsForTest();
    ASSERT_EQ(configurations1.size(), refreshedConfigurations1.size());
    for (size_t i = 0; i < configurations1.size(); i++) {
        ASSERT_BSONOBJ_EQ(configurations1[i].toBSON(), refreshedConfigurations1[i].toBSON());
    }

    // The per-second counts after: [0, 2].
    globalOpCounters.gotInserts(2);
    sampler.refreshQueryStatsForTest();

    auto queryStats2 = sampler.getQueryStatsForTest();
    ASSERT_EQ(queryStats2.getLastTotalCount(), 2);
    auto expectedAvgCount2 = (1 - smoothingFactor) * expectedAvgCount1 + smoothingFactor * 2;
    auto actualAvgCount2 = queryStats2.getLastAvgCount();
    ASSERT(actualAvgCount2);
    ASSERT_EQ(*actualAvgCount2, expectedAvgCount2);

    // Force the sampler to refresh its configurations. This should cause the sampler to send a
    // _refreshQueryAnalyzerConfiguration command to get sent and update its configurations.
    std::vector<CollectionQueryAnalyzerConfiguration> refreshedConfigurations2;
    refreshedConfigurations2.push_back(CollectionQueryAnalyzerConfiguration{nss1, collUuid1, 1.5});
    auto future2 = stdx::async(stdx::launch::async, [&] {
        expectConfigurationRefreshReturnSuccess(*queryStats2.getLastAvgCount(),
                                                refreshedConfigurations2);
    });
    sampler.refreshConfigurationsForTest(operationContext());
    future2.get();

    auto configurations2 = sampler.getConfigurationsForTest();
    ASSERT_EQ(configurations2.size(), refreshedConfigurations2.size());
    for (size_t i = 0; i < configurations2.size(); i++) {
        ASSERT_BSONOBJ_EQ(configurations2[i].toBSON(), refreshedConfigurations2[i].toBSON());
    }

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

    // Force the sampler to refresh its configurations. This should cause the sampler to send a
    // _refreshQueryAnalyzerConfiguration command to get sent and update its configurations.
    auto future3 = stdx::async(stdx::launch::async, [&] {
        expectConfigurationRefreshReturnSuccess(*queryStats3.getLastAvgCount(), {});
    });
    sampler.refreshConfigurationsForTest(operationContext());
    future3.get();

    auto configurations3 = sampler.getConfigurationsForTest();
    ASSERT(configurations3.empty());
}

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
