/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/util/duration.h"

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/sharding_environment/client/shard_remote.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <set>
#include <system_error>
#include <utility>

namespace mongo {
namespace {

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

const std::vector<ShardId> kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> kTestShardHosts = {HostAndPort("FakeShard1Host", 12345),
                                                  HostAndPort("FakeShard2Host", 12345),
                                                  HostAndPort("FakeShard3Host", 12345)};

class ShardRemoteTest : public ShardingTestFixture {
protected:
    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        std::vector<ShardType> shards;

        for (size_t i = 0; i < kTestShardIds.size(); i++) {
            ShardType shardType;
            shardType.setName(kTestShardIds[i].toString());
            shardType.setHost(kTestShardHosts[i].toString());
            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHosts[i]));
            targeter->setFindHostReturnValue(kTestShardHosts[i]);

            targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHosts[i]),
                                                   std::move(targeter));
        }

        setupShards(shards);
    }

    void runDummyCommandOnShard(ShardId shardId) {
        auto shard = unittest::assertGet(shardRegistry()->getShard(operationContext(), shardId));
        uassertStatusOK(shard->runCommandWithIndefiniteRetries(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::createDatabaseName_forTest(boost::none, "unusedDb"),
            BSON("unused" << "cmd"),
            Shard::RetryPolicy::kNoRetry));
    }

    void runDummyCommandOnShardWithMaxTimeMS(ShardId shardId, Milliseconds maxTimeMS) {
        auto shard = unittest::assertGet(shardRegistry()->getShard(operationContext(), shardId));
        uassertStatusOK(shard->runCommandWithIndefiniteRetries(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::createDatabaseName_forTest(boost::none, "unusedDb"),
            BSON("unused" << "cmd"),
            maxTimeMS,
            Shard::RetryPolicy::kNoRetry));
    }

    inline static auto errorLabelsSystemOverloaded =
        std::vector{std::string{ErrorLabel::kSystemOverloadedError}};
};

TEST_F(ShardRemoteTest, TargeterMarksHostAsDownWhenConfigStepdown) {
    auto targetedNode = ShardId("config");

    ASSERT_EQ(0UL, configTargeter()->getAndClearMarkedDownHosts().size());
    auto future = launchAsync([&] { runDummyCommandOnShard(targetedNode); });

    auto error = Status(ErrorCodes::PrimarySteppedDown, "Config stepped down");
    onCommand([&](const executor::RemoteCommandRequest& request) { return error; });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::PrimarySteppedDown);
    ASSERT_EQ(1UL, configTargeter()->getAndClearMarkedDownHosts().size());
}

TEST_F(ShardRemoteTest, GridSetRetryBudgetCapacityServerParameter) {
    auto targetedNode = ShardId("config");

    auto firstShard = kTestShardIds.front();
    auto firstShardHostAndPort = kTestShardHosts.front();

    auto shard = uassertStatusOK(shardRegistry()->getShard(operationContext(), firstShard));
    auto retryBudget = shard->getRetryBudget_forTest();
    auto retryStrategy = Shard::RetryStrategy{*shard, Shard::RetryPolicy::kIdempotent};

    auto initialBalance = retryBudget->getBalance_forTest();

    {
        auto _ = RAIIServerParameterControllerForTest{"shardRetryTokenBucketCapacity",
                                                      retryBudget->getBalance_forTest() + 1};
        retryStrategy.recordSuccess(firstShardHostAndPort);
        ASSERT_GT(retryBudget->getBalance_forTest(), initialBalance);
    }
}

TEST_F(ShardRemoteTest, GridSetRetryBudgetReturnRateServerParameter) {
    auto targetedNode = ShardId("config");

    auto firstShard = kTestShardIds.front();
    auto firstShardHostAndPort = kTestShardHosts.front();

    auto shard = uassertStatusOK(shardRegistry()->getShard(operationContext(), firstShard));
    auto retryBudget = shard->getRetryBudget_forTest();
    auto retryStrategy = Shard::RetryStrategy{*shard, Shard::RetryPolicy::kIdempotent};

    auto initialBalance = retryBudget->getBalance_forTest();
    auto error = Status(ErrorCodes::PrimarySteppedDown, "Interrupted at shutdown");

    constexpr auto kReturnRate = 0.5;

    {
        auto _ = RAIIServerParameterControllerForTest{"shardRetryTokenReturnRate", kReturnRate};
        // We consume some tokens in order to be able to observe the return rate.
        for (int i = 0; i < 2; ++i) {
            ASSERT(retryStrategy.recordFailureAndEvaluateShouldRetry(
                error, firstShardHostAndPort, errorLabelsSystemOverloaded));
        }

        // We test that the return rate was changed by observing how many tokens were returned by
        // recordSuccess.
        retryStrategy.recordSuccess(firstShardHostAndPort);
        ASSERT_EQ(retryBudget->getBalance_forTest(), initialBalance - 1 + kReturnRate);
    }
}

TEST_F(ShardRemoteTest, ShardRetryStrategy) {
    auto targetedNode = ShardId("config");

    auto firstShard = kTestShardIds.front();
    auto firstShardHostAndPort = kTestShardHosts.front();

    auto shard = uassertStatusOK(shardRegistry()->getShard(operationContext(), firstShard));
    auto retryBudget = shard->getRetryBudget_forTest();
    auto retryStrategy = Shard::RetryStrategy{*shard, Shard::RetryPolicy::kIdempotent};

    auto initialBalance = retryBudget->getBalance_forTest();
    auto error = Status(ErrorCodes::PrimarySteppedDown, "Interrupted at shutdown");

    ASSERT(retryStrategy.recordFailureAndEvaluateShouldRetry(
        error, firstShardHostAndPort, errorLabelsSystemOverloaded));
    ASSERT_LT(retryBudget->getBalance_forTest(), initialBalance);
    ASSERT(
        retryStrategy.getTargetingMetadata().deprioritizedServers.contains(firstShardHostAndPort));
}

TEST_F(ShardRemoteTest, TargeterMarksHostAsDownWhenConfigShuttingDown) {
    auto targetedNode = ShardId("config");

    ASSERT_EQ(0UL, configTargeter()->getAndClearMarkedDownHosts().size());
    auto future = launchAsync([&] { runDummyCommandOnShard(targetedNode); });

    auto error = Status(ErrorCodes::InterruptedAtShutdown, "Interrupted at shutdown");
    onCommand([&](const executor::RemoteCommandRequest& request) { return error; });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::InterruptedAtShutdown);
    ASSERT_EQ(1UL, configTargeter()->getAndClearMarkedDownHosts().size());
}

TEST_F(ShardRemoteTest, FindOnConfigRespectsDefaultConfigCommandTimeout) {
    // Set the timeout for config commands to 1 second.
    auto timeoutMs = 1000;
    RAIIServerParameterControllerForTest configCommandTimeout{"defaultConfigCommandTimeoutMS",
                                                              timeoutMs};

    auto configShard = ShardId("config");
    auto shard = unittest::assertGet(shardRegistry()->getShard(operationContext(), configShard));
    auto future = launchAsync([&] {
        uassertStatusOK(shard->exhaustiveFindOnConfig(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kMajorityReadConcern,
            NamespaceString::createNamespaceString_forTest("admin.bar"),
            {},
            {},
            {}));
    });

    // Assert that maxTimeMS is set to defaultConfigCommandTimeoutMS. We don't actually care about
    // the response here, so use a dummy error.
    auto error = Status(ErrorCodes::CommandFailed, "Dummy error");
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT(request.cmdObj.hasField("maxTimeMS")) << request;
        ASSERT_EQ(request.cmdObj["maxTimeMS"].Long(), timeoutMs);
        return error;
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::CommandFailed);
}

TEST_F(ShardRemoteTest, TimeoutCodeSetToMaxTimeMSExpiredWhenMaxTimeMSSet) {
    auto targetedNode = ShardId("config");
    auto timeoutMs = 100;

    ASSERT_EQ(0UL, configTargeter()->getAndClearMarkedDownHosts().size());
    auto future = launchAsync(
        [&] { runDummyCommandOnShardWithMaxTimeMS(targetedNode, Milliseconds(timeoutMs)); });

    // Assert that the timeout on the request is set to timeoutMs and the timeoutCode on the request
    // is set to maxTimeMSExpired. We don't actually care about the response here, so use a dummy
    // error.
    auto error = Status(ErrorCodes::CommandFailed, "Dummy error");
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ(request.timeout, Milliseconds(timeoutMs));
        ASSERT(request.timeoutCode);
        ASSERT_EQ(*request.timeoutCode, ErrorCodes::MaxTimeMSExpired);
        return error;
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::CommandFailed);
}

TEST_F(ShardRemoteTest, TimeoutCodeUnsetWhenMaxTimeMSNotSet) {
    auto targetedNode = ShardId("config");

    ASSERT_EQ(0UL, configTargeter()->getAndClearMarkedDownHosts().size());
    auto future = launchAsync([&] { runDummyCommandOnShard(targetedNode); });

    // Assert that the timeout on the request is set to kNoTimeout, and the timeoutCode on the
    // request is not set. We don't actually care about the response here, so use a dummy error.
    auto error = Status(ErrorCodes::CommandFailed, "Dummy error");
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ(request.timeout, executor::RemoteCommandRequest::kNoTimeout);
        ASSERT(!request.timeoutCode);
        return error;
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::CommandFailed);
}

}  // namespace
}  // namespace mongo
