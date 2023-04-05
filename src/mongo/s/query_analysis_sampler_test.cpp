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

#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/stats/counters.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/refresh_query_analyzer_configuration_cmd_gen.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/tick_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace analyze_shard_key {
namespace {

using QuerySamplingOptions = OperationContext::QuerySamplingOptions;

const auto smoothingFactor = gQueryAnalysisQueryStatsSmoothingFactor;

class QueryAnalysisSamplerRateLimiterTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        getServiceContext()->setTickSource(std::make_unique<TickSourceMock<Nanoseconds>>());
    }

protected:
    void advanceTime(Nanoseconds millis) {
        dynamic_cast<TickSourceMock<Nanoseconds>*>(getServiceContext()->getTickSource())
            ->advance(millis);
    }

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl");
    const UUID collUuid = UUID::gen();
};

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

TEST_F(QueryAnalysisSamplerRateLimiterTest, MicrosecondResolution) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 1e6);
    ASSERT_EQ(rateLimiter.getRate(), 1e6);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 1e6);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Microseconds(1));
    // The number of tokens available in the bucket right after the refill is 0 + 1.
    ASSERT(rateLimiter.tryConsume());
    ASSERT_FALSE(rateLimiter.tryConsume());
}

TEST_F(QueryAnalysisSamplerRateLimiterTest, NanosecondsResolution) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    auto rateLimiter =
        QueryAnalysisSampler::SampleRateLimiter(getServiceContext(), nss, collUuid, 1e9);
    ASSERT_EQ(rateLimiter.getRate(), 1e9);
    ASSERT_EQ(rateLimiter.getBurstCapacity(), 1e9);
    // There are no token available in the bucket initially.
    ASSERT_FALSE(rateLimiter.tryConsume());

    advanceTime(Nanoseconds(1));
    // The number of tokens available in the bucket right after the refill is 0 + 1.
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
        getServiceContext()->setTickSource(std::make_unique<TickSourceMock<Nanoseconds>>());
        getServiceContext()->setPeriodicRunner(std::move(runner));

        // Reset the counters since each test assumes that the count starts at 0.
        globalOpCounters.resetForTest();
    }

    void tearDown() override {
        ShardingTestFixture::tearDown();
        setMongos(_originalIsMongos);
        serverGlobalParams.clusterRole = ClusterRole::None;
    }

    void setUpRole(ClusterRole role, bool isReplEnabled = true) {
        setMongos(false);
        serverGlobalParams.clusterRole = role;

        auto replCoord = [&] {
            if (isReplEnabled) {
                return std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext());
            }
            repl::ReplSettings replSettings;
            // The empty string "disables" replication.
            replSettings.setReplSetString("");
            return std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext(),
                                                                      replSettings);
        }();
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));
    }

protected:
    void advanceTime(Nanoseconds millis) {
        dynamic_cast<TickSourceMock<Nanoseconds>*>(getServiceContext()->getTickSource())
            ->advance(millis);
    }

    Date_t now() {
        return getServiceContext()->getFastClockSource()->now();
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

    const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

    const NamespaceString nss0 =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl0");
    const NamespaceString nss1 =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl1");
    const NamespaceString nss2 =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl2");

    const UUID collUuid0 = UUID::gen();
    const UUID collUuid1 = UUID::gen();
    const UUID collUuid2 = UUID::gen();

private:
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagAnalyzeShardKey", true};
    bool _originalIsMongos;
};

TEST_F(QueryAnalysisSamplerTest, CanGetOnShardServer) {
    setUpRole(ClusterRole::ShardServer);
    QueryAnalysisSampler::get(operationContext());
}

TEST_F(QueryAnalysisSamplerTest, CanGetOnStandaloneReplicaSet) {
    setUpRole(ClusterRole::None);
    QueryAnalysisSampler::get(operationContext());
}

TEST_F(QueryAnalysisSamplerTest, CanGetOnConfigServer) {
    setUpRole(ClusterRole::ConfigServer);
    QueryAnalysisSampler::get(operationContext());
}

DEATH_TEST_F(QueryAnalysisSamplerTest, CannotGetOnStandaloneMongod, "invariant") {
    setUpRole(ClusterRole::None, false /* isReplEnabled */);
    QueryAnalysisSampler::get(operationContext());
}

DEATH_TEST_F(QueryAnalysisSamplerTest, CannotGetIfFeatureFlagNotEnabled, "invariant") {
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagAnalyzeShardKey",
                                                                false};
    QueryAnalysisSampler::get(operationContext());
}

class QueryAnalysisSamplerQueryStatsTest : public QueryAnalysisSamplerTest {
protected:
    void testInitialCount() {
        auto& sampler = QueryAnalysisSampler::get(operationContext());
        sampler.refreshQueryStatsForTest();

        auto queryStats = sampler.getQueryStatsForTest();
        ASSERT_EQ(queryStats.getLastTotalCount(), 0);
        auto lastAvgCount = queryStats.getLastAvgCount();
        ASSERT(lastAvgCount);
        ASSERT_EQ(*lastAvgCount, 0);
    }

    void testInsertsTrackedByOpCounters(bool shouldCount) {
        auto& sampler = QueryAnalysisSampler::get(operationContext());
        auto numInserts = 3;
        globalOpCounters.gotInserts(numInserts);
        sampler.refreshQueryStatsForTest();

        auto queryStats = sampler.getQueryStatsForTest();
        ASSERT_EQ(queryStats.getLastTotalCount(), shouldCount ? numInserts : 0);
        auto lastAvgCount = queryStats.getLastAvgCount();
        ASSERT(lastAvgCount);
        ASSERT_EQ(*lastAvgCount, shouldCount ? numInserts : 0);
    }

    void testUpdatesTrackedByOpCounters(bool shouldCount) {
        auto& sampler = QueryAnalysisSampler::get(operationContext());
        globalOpCounters.gotUpdate();
        sampler.refreshQueryStatsForTest();

        auto queryStats = sampler.getQueryStatsForTest();
        ASSERT_EQ(queryStats.getLastTotalCount(), shouldCount ? 1 : 0);
        auto lastAvgCount = queryStats.getLastAvgCount();
        ASSERT(lastAvgCount);
        ASSERT_EQ(*lastAvgCount, shouldCount ? 1 : 0);
    }

    void testDeletesTrackedByOpCounters(bool shouldCount) {
        auto& sampler = QueryAnalysisSampler::get(operationContext());
        globalOpCounters.gotDelete();
        sampler.refreshQueryStatsForTest();

        auto queryStats = sampler.getQueryStatsForTest();
        ASSERT_EQ(queryStats.getLastTotalCount(), shouldCount ? 1 : 0);
        auto lastAvgCount = queryStats.getLastAvgCount();
        ASSERT(lastAvgCount);
        ASSERT_EQ(*lastAvgCount, shouldCount ? 1 : 0);
    }

    void testFindAndModify(bool shouldCount) {
        auto& sampler = QueryAnalysisSampler::get(operationContext());
        sampler.gotCommand("findAndModify");
        sampler.refreshQueryStatsForTest();

        auto queryStats = sampler.getQueryStatsForTest();
        ASSERT_EQ(queryStats.getLastTotalCount(), shouldCount ? 1 : 0);
        auto lastAvgCount = queryStats.getLastAvgCount();
        ASSERT(lastAvgCount);
        ASSERT_EQ(*lastAvgCount, shouldCount ? 1 : 0);
    }

    void testQueriesTrackedByOpCounters(bool shouldCount) {
        auto& sampler = QueryAnalysisSampler::get(operationContext());
        globalOpCounters.gotQuery();
        sampler.refreshQueryStatsForTest();

        auto queryStats = sampler.getQueryStatsForTest();
        ASSERT_EQ(queryStats.getLastTotalCount(), shouldCount ? 1 : 0);
        auto lastAvgCount = queryStats.getLastAvgCount();
        ASSERT(lastAvgCount);
        ASSERT_EQ(*lastAvgCount, shouldCount ? 1 : 0);
    }

    void testCommandsTrackedByOpCounters(bool shouldCount) {
        auto& sampler = QueryAnalysisSampler::get(operationContext());
        globalOpCounters.gotCommand();
        sampler.refreshQueryStatsForTest();

        auto queryStats = sampler.getQueryStatsForTest();
        ASSERT_EQ(queryStats.getLastTotalCount(), shouldCount ? 1 : 0);
        auto lastAvgCount = queryStats.getLastAvgCount();
        ASSERT(lastAvgCount);
        ASSERT_EQ(*lastAvgCount, shouldCount ? 1 : 0);
    }

    void testAggregates(bool shouldCount) {
        auto& sampler = QueryAnalysisSampler::get(operationContext());
        sampler.gotCommand("aggregate");
        sampler.refreshQueryStatsForTest();

        auto queryStats = sampler.getQueryStatsForTest();
        ASSERT_EQ(queryStats.getLastTotalCount(), shouldCount ? 1 : 0);
        auto lastAvgCount = queryStats.getLastAvgCount();
        ASSERT(lastAvgCount);
        ASSERT_EQ(*lastAvgCount, shouldCount ? 1 : 0);
    }

    void testCounts(bool shouldCount) {
        auto& sampler = QueryAnalysisSampler::get(operationContext());
        sampler.gotCommand("count");
        sampler.refreshQueryStatsForTest();

        auto queryStats = sampler.getQueryStatsForTest();
        ASSERT_EQ(queryStats.getLastTotalCount(), shouldCount ? 1 : 0);
        auto lastAvgCount = queryStats.getLastAvgCount();
        ASSERT(lastAvgCount);
        ASSERT_EQ(*lastAvgCount, shouldCount ? 1 : 0);
    }

    void testDistincts(bool shouldCount) {
        auto& sampler = QueryAnalysisSampler::get(operationContext());
        sampler.gotCommand("distinct");
        sampler.refreshQueryStatsForTest();

        auto queryStats = sampler.getQueryStatsForTest();
        ASSERT_EQ(queryStats.getLastTotalCount(), shouldCount ? 1 : 0);
        auto lastAvgCount = queryStats.getLastAvgCount();
        ASSERT(lastAvgCount);
        ASSERT_EQ(*lastAvgCount, shouldCount ? 1 : 0);
    }

    void testNestedAggregates(bool shouldCount) {
        auto& sampler = QueryAnalysisSampler::get(operationContext());
        globalOpCounters.gotNestedAggregate();
        ;
        sampler.refreshQueryStatsForTest();

        auto queryStats = sampler.getQueryStatsForTest();
        ASSERT_EQ(queryStats.getLastTotalCount(), shouldCount ? 1 : 0);
        auto lastAvgCount = queryStats.getLastAvgCount();
        ASSERT(lastAvgCount);
        ASSERT_EQ(*lastAvgCount, shouldCount ? 1 : 0);
    }
};

TEST_F(QueryAnalysisSamplerQueryStatsTest, InitialCount) {
    testInitialCount();
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsMongos_NotCountInsertsTrackedByOpCounters) {
    testInsertsTrackedByOpCounters(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsMongos_CountUpdatesTrackedByOpCounters) {
    testUpdatesTrackedByOpCounters(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsMongos_CountDeletesTrackedByOpCounters) {
    testDeletesTrackedByOpCounters(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsMongos_CountFindAndModify) {
    testFindAndModify(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsMongos_CountQueriesTrackedByOpCounters) {
    testQueriesTrackedByOpCounters(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsMongos_NotCountCommandsTrackedByOpCounters) {
    testCommandsTrackedByOpCounters(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsMongos_CountAggregates) {
    testAggregates(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsMongos_CountCounts) {
    testCounts(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsMongos_CountDistincts) {
    testDistincts(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsMongos_NotCountNestedAggregatesTrackedByOpCounters) {
    testNestedAggregates(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsReplSetMongod_NotCountInsertsTrackedByOpCounters) {
    setUpRole(ClusterRole::None);
    testInsertsTrackedByOpCounters(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsReplSetMongod_CountUpdatesTrackedByOpCounters) {
    setUpRole(ClusterRole::None);
    testUpdatesTrackedByOpCounters(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsReplSetMongod_CountFindAndModify) {
    setUpRole(ClusterRole::None);
    testFindAndModify(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsReplSetMongod_CountDeletesTrackedByOpCounters) {
    setUpRole(ClusterRole::None);
    testDeletesTrackedByOpCounters(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsReplSetMongod_CountQueriesTrackedByOpCounters) {
    setUpRole(ClusterRole::None);
    testQueriesTrackedByOpCounters(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsReplSetMongod_NotCountCommandsTrackedByOpCounters) {
    setUpRole(ClusterRole::None);
    testCommandsTrackedByOpCounters(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsReplSetMongod_CountAggregates) {
    setUpRole(ClusterRole::None);
    testAggregates(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsReplSetMongod_CountCounts) {
    setUpRole(ClusterRole::None);
    testCounts(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsReplSetMongod_CountDistincts) {
    setUpRole(ClusterRole::None);
    testDistincts(true /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsReplSetMongod_NotCountNestedAggregatesTrackedByOpCounters) {
    setUpRole(ClusterRole::None);
    testNestedAggregates(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsShardSvrMongod_NotCountInsertsTrackedByOpCounters) {
    setUpRole(ClusterRole::ShardServer);
    testInsertsTrackedByOpCounters(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsShardSvrMongod_NotCountUpdatesTrackedByOpCounters) {
    setUpRole(ClusterRole::ShardServer);
    testUpdatesTrackedByOpCounters(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsShardSvrMongod_NotCountDeletesTrackedByOpCounters) {
    setUpRole(ClusterRole::ShardServer);
    testDeletesTrackedByOpCounters(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsShardSvrMongod_NotCountFindAndModify) {
    setUpRole(ClusterRole::ShardServer);
    testFindAndModify(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsShardSvrMongod_NotCountQueriesTrackedByOpCounters) {
    setUpRole(ClusterRole::ShardServer);
    testQueriesTrackedByOpCounters(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsShardSvrMongod_NotCountCommandsTrackedByOpCounters) {
    setUpRole(ClusterRole::ShardServer);
    testCommandsTrackedByOpCounters(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsShardSvrMongod_NotCountAggregates) {
    setUpRole(ClusterRole::ShardServer);
    testAggregates(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsShardSvrMongod_NotCountCounts) {
    setUpRole(ClusterRole::ShardServer);
    testCounts(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest, RefreshQueryStatsShardSvrMongod_NotCountDistincts) {
    setUpRole(ClusterRole::ShardServer);
    testDistincts(false /* shouldCount */);
}

TEST_F(QueryAnalysisSamplerQueryStatsTest,
       RefreshQueryStatsShardSvrMongod_CountNestedAggregatesTrackedByOpCounters) {
    setUpRole(ClusterRole::ShardServer);
    testNestedAggregates(true /* shouldCount */);
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
    auto startTime = now();
    refreshedConfigurations1.push_back(
        CollectionQueryAnalyzerConfiguration{nss0, collUuid0, 1, startTime});
    refreshedConfigurations1.push_back(
        CollectionQueryAnalyzerConfiguration{nss1, collUuid1, 0.5, startTime});
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
    globalOpCounters.gotUpdate();
    globalOpCounters.gotDelete();
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
    refreshedConfigurations2.push_back(
        CollectionQueryAnalyzerConfiguration{nss1, collUuid1, 1.5, startTime});
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
    globalOpCounters.gotQuery();
    sampler.gotCommand("findandmodify");
    sampler.gotCommand("aggregate");
    sampler.gotCommand("count");
    sampler.gotCommand("distinct");
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

TEST_F(QueryAnalysisSamplerTest, TryGenerateSampleIdExternalClient) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    transport::TransportLayerMock transportLayer;
    std::shared_ptr<transport::Session> session = transportLayer.createSession();
    auto client =
        getGlobalServiceContext()->makeClient("TryGenerateSampleIdExternalClient", session);
    auto opCtxHolder = client->makeOperationContext();
    auto opCtx = opCtxHolder.get();

    auto& sampler = QueryAnalysisSampler::get(opCtx);

    std::vector<CollectionQueryAnalyzerConfiguration> configurations;
    auto startTime = now();
    configurations.push_back(CollectionQueryAnalyzerConfiguration{nss0, collUuid0, 1, startTime});
    configurations.push_back(CollectionQueryAnalyzerConfiguration{nss1, collUuid1, 0.5, startTime});
    setUpConfigurations(&sampler, configurations);

    // Cannot sample if time has not elapsed.
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss1, SampledCommandNameEnum::kFind));

    advanceTime(Milliseconds(1000));
    // The number of tokens available in the bucket for rateLimiter0 right after the refill is 0
    // + 1.0.
    ASSERT(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));
    // The number of tokens available in the bucket for rateLimiter1 right after the refill is 0 +
    // 0.5.
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss1, SampledCommandNameEnum::kFind));
    // This collection doesn't have sampling enabled.
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss2, SampledCommandNameEnum::kFind));

    advanceTime(Milliseconds(1000));
    // The number of tokens available in the bucket for rateLimiter0 right after the refill is 0
    // + 1.0.
    ASSERT(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));
    // The number of tokens available in the bucket for rateLimiter1 right after the refill is 0.5 +
    // 0.5.
    ASSERT(sampler.tryGenerateSampleId(opCtx, nss1, SampledCommandNameEnum::kFind));
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss1, SampledCommandNameEnum::kFind));
    // This collection doesn't have sampling enabled.
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss2, SampledCommandNameEnum::kFind));

    advanceTime(Milliseconds(1000));
    // The number of tokens available in the bucket for rateLimiter0 right after the refill is 0
    // + 1.0. Cannot sample since the client has explicitly opted out of query sampling.
    opCtx->setQuerySamplingOptions(QuerySamplingOptions::kOptOut);
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));
}

TEST_F(QueryAnalysisSamplerTest, TryGenerateSampleIdInternalClient) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    // Note how this client does not have a network session.
    auto client = getGlobalServiceContext()->makeClient("TryGenerateSampleIdInternalClient");
    auto opCtxHolder = client->makeOperationContext();
    auto opCtx = opCtxHolder.get();

    auto& sampler = QueryAnalysisSampler::get(opCtx);

    std::vector<CollectionQueryAnalyzerConfiguration> configurations;
    auto startTime = now();
    configurations.push_back(CollectionQueryAnalyzerConfiguration{nss0, collUuid0, 1, startTime});
    setUpConfigurations(&sampler, configurations);

    advanceTime(Milliseconds(1000));
    // The number of tokens available in the bucket for rateLimiter0 right after the refill is 0
    // + 1.0. Cannot sample since the client is internal.
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));

    opCtx->setQuerySamplingOptions(QuerySamplingOptions::kOptIn);
    // Can sample now since the client has explicitly opted into query sampling.
    ASSERT(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));
    // Cannot sample since there are no tokens left.
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));

    advanceTime(Milliseconds(1000));
    opCtx->setQuerySamplingOptions(QuerySamplingOptions::kOptOut);
    // The number of tokens available in the bucket for rateLimiter0 right after the refill is 0
    // + 1.0. Cannot sample since the client has explicitly opted into query sampling.
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));
}

TEST_F(QueryAnalysisSamplerTest, RefreshConfigurationsNewCollectionUuid) {
    const RAIIServerParameterControllerForTest burstMultiplierController{
        "queryAnalysisSamplerBurstMultiplier", 1};

    transport::TransportLayerMock transportLayer;
    std::shared_ptr<transport::Session> session = transportLayer.createSession();
    auto client =
        getGlobalServiceContext()->makeClient("RefreshConfigurationsNewCollectionUuid", session);
    auto opCtxHolder = client->makeOperationContext();
    auto opCtx = opCtxHolder.get();

    auto& sampler = QueryAnalysisSampler::get(opCtx);
    auto startTime = now();

    std::vector<CollectionQueryAnalyzerConfiguration> oldConfigurations;
    oldConfigurations.push_back(
        CollectionQueryAnalyzerConfiguration{nss0, collUuid0, 2, startTime});
    setUpConfigurations(&sampler, oldConfigurations);

    auto oldRateLimiters = sampler.getRateLimitersForTest();
    ASSERT_EQ(oldRateLimiters.size(), oldConfigurations.size());

    auto oldIt = oldRateLimiters.find(oldConfigurations[0].getNs());
    ASSERT(oldIt != oldRateLimiters.end());
    ASSERT_EQ(oldIt->second.getCollectionUuid(), oldConfigurations[0].getCollectionUuid());
    ASSERT_EQ(oldIt->second.getRate(), oldConfigurations[0].getSampleRate());

    advanceTime(Milliseconds(1000));
    // The number of tokens available in the bucket right after the refill is 0 + 2.
    ASSERT(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));

    // Force the sampler to refresh and return a different collection uuid and sample rate this
    // time.
    auto queryStats = sampler.getQueryStatsForTest();
    std::vector<CollectionQueryAnalyzerConfiguration> newConfigurations;
    newConfigurations.push_back(
        CollectionQueryAnalyzerConfiguration{nss0, UUID::gen(), 1.5, startTime});
    auto future = stdx::async(stdx::launch::async, [&] {
        expectConfigurationRefreshReturnSuccess(*queryStats.getLastAvgCount(), newConfigurations);
    });
    sampler.refreshConfigurationsForTest(opCtx);
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
    ASSERT_FALSE(sampler.tryGenerateSampleId(opCtx, nss0, SampledCommandNameEnum::kFind));
}

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
