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
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/refresh_query_analyzer_configuration_cmd_gen.h"
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

class QueryAnalysisSamplerRateLimiterTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        getServiceContext()->setFastClockSource(
            std::make_unique<SharedClockSourceAdapter>(_mockClock));
        getServiceContext()->setPreciseClockSource(
            std::make_unique<SharedClockSourceAdapter>(_mockClock));
    }

    void advanceTime(Milliseconds millis) {
        _mockClock->advance(millis);
    }

    Date_t now() {
        return _mockClock->now();
    }

private:
    const std::shared_ptr<ClockSourceMock> _mockClock = std::make_shared<ClockSourceMock>();

protected:
    const NamespaceString nss{"testDb", "testColl"};
    const UUID collUuid = UUID::gen();
};

DEATH_TEST_F(QueryAnalysisSamplerRateLimiterTest, CannotUseZeroRate, "invariant") {
    QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 0);
}

DEATH_TEST_F(QueryAnalysisSamplerRateLimiterTest, CannotUseNegativeRate, "invariant") {
    QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, -0.5);
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, BurstMultiplierEqualToOne) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    // multiplier * rate > 1
    auto rateLimiter0 =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 5);
    ASSERT_EQ(rateLimiter0.getRate(), 5);
    ASSERT_EQ(rateLimiter0.getBurstCapacity(), 5);

    // multiplier * rate = 1
    auto rateLimiter1 =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 1);
    ASSERT_EQ(rateLimiter1.getRate(), 1);
    ASSERT_EQ(rateLimiter1.getBurstCapacity(), 1);

    // multiplier * rate < 1
    auto rateLimiter2 =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 0.1);
    ASSERT_EQ(rateLimiter2.getRate(), 0.1);
    ASSERT_EQ(rateLimiter2.getBurstCapacity(), 1);
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, BurstMultiplierGreaterThanOne) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 2.5};

    // multiplier * rate > 1
    auto rateLimiter0 =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 5);
    ASSERT_EQ(rateLimiter0.getRate(), 5);
    ASSERT_EQ(rateLimiter0.getBurstCapacity(), 12.5);

    // multiplier * rate = 1
    auto rateLimiter1 =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 0.4);
    ASSERT_EQ(rateLimiter1.getRate(), 0.4);
    ASSERT_EQ(rateLimiter1.getBurstCapacity(), 1);

    // multiplier * rate < 1
    auto rateLimiter2 =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 0.1);
    ASSERT_EQ(rateLimiter2.getRate(), 0.1);
    ASSERT_EQ(rateLimiter2.getBurstCapacity(), 1);
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, ConsumeAfterOneSecond) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 2);
    ASSERT_EQ(rateLimiter.getRate(), 2);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 2);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(1000));
    // The number of tokens available in the bucket right after the refill is 0 + 2.
    ASSERT(rateLimiter.tryConsume());
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, ConsumeAfterLessThanOneSecond) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 4);
    ASSERT_EQ(rateLimiter.getRate(), 4);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 4);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(500));
    // The number of tokens available in the bucket right after the refill is 0 + 2.
    ASSERT(rateLimiter.tryConsume());
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, ConsumeAfterMoreThanOneSecond) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 0.5);
    ASSERT_EQ(rateLimiter.getRate(), 0.5);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 1);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(2000));
    // The number of tokens available in the bucket right after the refill is 0 + 1.
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, ConsumeEpsilonAbove) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 1);
    ASSERT_EQ(rateLimiter.getRate(), 1);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 1);
    ASSERT_GTE(QueryAnalysisSampler::SampleRateLimiter::kEpsilon, 0.001);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(999));
    // The number of tokens available in the bucket right after the refill is 0 + 0.999.
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, ConsumeRemainingTokens) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 2);
    ASSERT_EQ(rateLimiter.getRate(), 2);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 2);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(700));
    // The number of tokens available in the bucket right after the refill is 0 + 1.4.
    ASSERT(rateLimiter.tryConsume());

    advanceTime(Milliseconds(800));
    // The number of tokens available in the bucket right after the refill is 0.4 + 1.6.
    ASSERT(rateLimiter.tryConsume());
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, ConsumeBurstCapacity) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 2};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 1);
    ASSERT_EQ(rateLimiter.getRate(), 1);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 2);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(2000));
    // The number of tokens available in the bucket right after the refill is 2.
    ASSERT(rateLimiter.tryConsume());
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, ConsumeAboveBurstCapacity) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 2};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 1);
    ASSERT_EQ(rateLimiter.getRate(), 1);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 2);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(3000));
    // The number of tokens available in the bucket right after the refill is 2.
    ASSERT(rateLimiter.tryConsume());
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, ConsumeBelowBurstCapacity) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 2};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 1);
    ASSERT_EQ(rateLimiter.getRate(), 1);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 2);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(1800));
    // The number of tokens available in the bucket right after the refill is 0 + 1.8.
    ASSERT(rateLimiter.tryConsume());

    advanceTime(Milliseconds(200));
    // The number of tokens available in the bucket right after the refill is 0.8 + 0.2.
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, ConsumeAfterRefresh_RateIncreased) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 2};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 0.1);
    ASSERT_EQ(rateLimiter.getRate(), 0.1);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 1);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(20000));
    // The number of tokens available in the bucket right after the refill is 2 (note that this is
    // greater than the pre-refresh capacity).
    rateLimiter.refreshRate(1);
    ASSERT_EQ(rateLimiter.getRate(), 1);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 2);
    ASSERT(rateLimiter.tryConsume());
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(2000));
    // Verify the rate limiter now has the new rate and burst capacity. The number of tokens
    // available in the bucket right after the refill is 2 (not 0.2).
    ASSERT(rateLimiter.tryConsume());
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, ConsumeAfterRefresh_RateDecreased) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 2};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 1);
    ASSERT_EQ(rateLimiter.getRate(), 1);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 2);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(2000));
    // The number of tokens available in the bucket right after the refill is 1 (note that this is
    // less than the pre-refresh capacity).
    rateLimiter.refreshRate(0.1);
    ASSERT_EQ(rateLimiter.getRate(), 0.1);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 1);
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(8000));
    // The number of tokens available in the bucket right after the refill is 0.8 (not 8).
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(12000));
    // Verify the rate limiter now has the new rate and burst capacity. The number of tokens
    // available in the bucket right after the refill is 1 (not 0.8 + 1.2).
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, ConsumeAfterRefresh_RateUnchanged) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 2};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 1);
    ASSERT_EQ(rateLimiter.getRate(), 1);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 2);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Milliseconds(1000));
    // The number of tokens available in the bucket right after the refill is 1.
    rateLimiter.refreshRate(1);
    ASSERT_EQ(rateLimiter.getRate(), 1);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 2);

    advanceTime(Milliseconds(1000));
    // The number of tokens available in the bucket right after the refill is 1 + 1.
    ASSERT(rateLimiter.tryConsume());
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
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

    void advanceTime(Milliseconds millis) {
        _mockClock->advance(millis);
    }

    Date_t now() {
        return _mockClock->now();
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

    /**
     * Makes the given sampler have the given configurations.
     */
    void setUpConfigurations(
        QueryAnalysisSampler* sampler,
        const std::vector<CollectionQueryAnalyzerConfiguration>& configurations) {
        globalOpCounters.gotQuery();
        sampler->refreshQueryStatsForTest();

        auto queryStats = sampler->getQueryStatsForTest();
        ASSERT_EQ(*queryStats.getLastAvgCount(), 1);

        // Force the sampler to refresh its configurations. This should cause the sampler to send a
        // _refreshQueryAnalyzerConfiguration command to get sent and update its configurations.
        auto future = stdx::async(stdx::launch::async, [&] {
            expectConfigurationRefreshReturnSuccess(*queryStats.getLastAvgCount(), configurations);
        });
        sampler->refreshConfigurationsForTest(operationContext());
        future.get();

        auto rateLimiters = sampler->getRateLimitersForTest();
        ASSERT_EQ(rateLimiters.size(), configurations.size());
    }

private:
    const std::shared_ptr<ClockSourceMock> _mockClock = std::make_shared<ClockSourceMock>();
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagAnalyzeShardKey", true};
    bool _originalIsMongos;

protected:
    const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

    const NamespaceString nss0{"testDb", "testColl0"};
    const NamespaceString nss1{"testDb", "testColl1"};
    const NamespaceString nss2{"testDb", "testColl2"};

    const UUID collUuid0 = UUID::gen();
    const UUID collUuid1 = UUID::gen();
    const UUID collUuid2 = UUID::gen();
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
    auto rateLimiters = sampler.getRateLimitersForTest();
    ASSERT(rateLimiters.empty());

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

    auto rateLimiters1 = sampler.getRateLimitersForTest();
    ASSERT_EQ(rateLimiters1.size(), refreshedConfigurations1.size());

    auto it0 = rateLimiters1.find(refreshedConfigurations1[0].getNs());
    ASSERT(it0 != rateLimiters1.end());
    ASSERT_EQ(it0->second.getCollectionUuid(), refreshedConfigurations1[0].getCollectionUuid());
    ASSERT_EQ(it0->second.getRate(), refreshedConfigurations1[0].getSampleRate());

    auto it1 = rateLimiters1.find(refreshedConfigurations1[1].getNs());
    ASSERT(it1 != rateLimiters1.end());
    ASSERT_EQ(it1->second.getCollectionUuid(), refreshedConfigurations1[1].getCollectionUuid());
    ASSERT_EQ(it1->second.getRate(), refreshedConfigurations1[1].getSampleRate());

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

    auto rateLimiters2 = sampler.getRateLimitersForTest();
    ASSERT_EQ(rateLimiters2.size(), refreshedConfigurations2.size());

    auto it = rateLimiters2.find(refreshedConfigurations2[0].getNs());
    ASSERT(it != rateLimiters2.end());
    ASSERT_EQ(it->second.getCollectionUuid(), refreshedConfigurations2[0].getCollectionUuid());
    ASSERT_EQ(it->second.getRate(), refreshedConfigurations2[0].getSampleRate());

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

    auto rateLimiters3 = sampler.getRateLimitersForTest();
    ASSERT(rateLimiters3.empty());
}

TEST_F(QueryAnalysisSamplerTest, ShouldSampleBasic) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    auto& sampler = QueryAnalysisSampler::get(operationContext());

    std::vector<CollectionQueryAnalyzerConfiguration> configurations;
    configurations.push_back(CollectionQueryAnalyzerConfiguration{nss0, collUuid0, 1});
    configurations.push_back(CollectionQueryAnalyzerConfiguration{nss1, collUuid1, 0.5});
    setUpConfigurations(&sampler, configurations);

    // Cannot sample if time has not elapsed.
    ASSERT_FALSE(sampler.shouldSample(nss0));
    ASSERT_FALSE(sampler.shouldSample(nss1));

    advanceTime(Milliseconds(1000));
    // The number of tokens available in the bucket for rateLimiter0 right after the refill is 0
    // + 1.0.
    ASSERT(sampler.shouldSample(nss0));
    ASSERT_FALSE(sampler.shouldSample(nss0));
    // The number of tokens available in the bucket for rateLimiter1 right after the refill is 0 +
    // 0.5.
    ASSERT_FALSE(sampler.shouldSample(nss1));
    // This collection doesn't have sampling enabled.
    ASSERT_FALSE(sampler.shouldSample(nss2));

    advanceTime(Milliseconds(1000));
    // The number of tokens available in the bucket for rateLimiter0 right after the refill is 0
    // + 1.0.
    ASSERT(sampler.shouldSample(nss0));
    ASSERT_FALSE(sampler.shouldSample(nss0));
    // The number of tokens available in the bucket for rateLimiter1 right after the refill is 0.5 +
    // 0.5.
    ASSERT(sampler.shouldSample(nss1));
    ASSERT_FALSE(sampler.shouldSample(nss1));
    // This collection doesn't have sampling enabled.
    ASSERT_FALSE(sampler.shouldSample(nss2));
}

TEST_F(QueryAnalysisSamplerTest, RefreshConfigurationsNewCollectionUuid) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    auto& sampler = QueryAnalysisSampler::get(operationContext());

    std::vector<CollectionQueryAnalyzerConfiguration> oldConfigurations;
    oldConfigurations.push_back(CollectionQueryAnalyzerConfiguration{nss0, collUuid0, 2});
    setUpConfigurations(&sampler, oldConfigurations);

    auto oldRateLimiters = sampler.getRateLimitersForTest();
    ASSERT_EQ(oldRateLimiters.size(), oldConfigurations.size());

    auto oldIt = oldRateLimiters.find(oldConfigurations[0].getNs());
    ASSERT(oldIt != oldRateLimiters.end());
    ASSERT_EQ(oldIt->second.getCollectionUuid(), oldConfigurations[0].getCollectionUuid());
    ASSERT_EQ(oldIt->second.getRate(), oldConfigurations[0].getSampleRate());

    advanceTime(Milliseconds(1000));
    // The number of tokens available in the bucket right after the refill is 0 + 2.
    ASSERT(sampler.shouldSample(nss0));

    // Force the sampler to refresh and return a different collection uuid and sample rate this
    // time.
    auto queryStats = sampler.getQueryStatsForTest();
    std::vector<CollectionQueryAnalyzerConfiguration> newConfigurations;
    newConfigurations.push_back(CollectionQueryAnalyzerConfiguration{nss0, UUID::gen(), 1.5});
    auto future = stdx::async(stdx::launch::async, [&] {
        expectConfigurationRefreshReturnSuccess(*queryStats.getLastAvgCount(), newConfigurations);
    });
    sampler.refreshConfigurationsForTest(operationContext());
    future.get();

    auto newRateLimiters = sampler.getRateLimitersForTest();
    ASSERT_EQ(newRateLimiters.size(), newConfigurations.size());

    auto newIt = newRateLimiters.find(newConfigurations[0].getNs());
    ASSERT(newIt != newRateLimiters.end());
    ASSERT_EQ(newIt->second.getCollectionUuid(), newConfigurations[0].getCollectionUuid());
    ASSERT_EQ(newIt->second.getRate(), newConfigurations[0].getSampleRate());

    // Cannot sample if time has not elapsed. There should be no tokens available in the bucket
    // right after the refill unless the one token from the previous configurations was
    // carried over, which is not the correct behavior.
    ASSERT_FALSE(sampler.shouldSample(nss0));
}

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
