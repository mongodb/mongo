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
#include "mongo/client/retry_strategy_server_parameters_gen.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/sharding_environment/client/shard.h"
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

constexpr StringData kNamespaceName = "unittests.shard_remote_test";
const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

struct TestShardInfo {
    ShardId id;
    std::vector<HostAndPort> hosts;
};

std::array kTestShards = {
    TestShardInfo{
        .id = ShardId("FakeShard1"),
        .hosts = {HostAndPort("FakeShard1Host", 12345), HostAndPort("FakeShard1Host2", 12345)}},
    TestShardInfo{
        .id = ShardId("FakeShard2"),
        .hosts = {HostAndPort("FakeShard2Host", 12345), HostAndPort("FakeShard2Host2", 12345)}},
    TestShardInfo{
        .id = ShardId("FakeShard3"),
        .hosts = {HostAndPort("FakeShard3Host", 12345), HostAndPort("FakeShard3Host2", 12345)}},
};

class ShardRemoteTest : public ShardingTestFixture {
protected:
    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        std::vector<ShardType> shards;

        for (const auto& shard : kTestShards) {
            ShardType shardType;
            auto host = ConnectionString::forReplicaSet(shard.id.toString(), shard.hosts);
            shardType.setName(shard.id.toString());
            shardType.setHost(host.toString());
            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(host);
            targeter->setFindHostsReturnValue(shard.hosts);

            targeterFactory()->addTargeterToReturn(host, std::move(targeter));
        }

        setupShards(shards);
    }

    void runDummyCommandOnShard(ShardId shardId,
                                Shard::RetryPolicy retryPolicy = Shard::RetryPolicy::kNoRetry) {
        auto shard = unittest::assertGet(shardRegistry()->getShard(operationContext(), shardId));
        auto result = uassertStatusOK(
            shard->runCommand(operationContext(),
                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                              DatabaseName::createDatabaseName_forTest(boost::none, "unusedDb"),
                              BSON("unused" << "cmd"),
                              retryPolicy));
        uassertStatusOK(result.commandStatus);
    }

    void runDummyCommandOnShardWithMaxTimeMS(
        ShardId shardId,
        Milliseconds maxTimeMS,
        Shard::RetryPolicy retryPolicy = Shard::RetryPolicy::kNoRetry) {
        auto shard = unittest::assertGet(shardRegistry()->getShard(operationContext(), shardId));
        auto result = uassertStatusOK(
            shard->runCommand(operationContext(),
                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                              DatabaseName::createDatabaseName_forTest(boost::none, "unusedDb"),
                              BSON("unused" << "cmd"),
                              maxTimeMS,
                              retryPolicy));
        uassertStatusOK(result.commandStatus);
    }

    void runExhaustiveRetryBackoffTest(auto networkCommand) {
        for (int i = 0; i < kMaxCommandExecutions; ++i) {
            onCommand(networkCommand);

            if (i < kDefaultClientMaxRetryAttemptsDefault) {
                ASSERT_GT(advanceUntilReadyRequest(), Milliseconds{0});
            }
        }
    }

    inline static auto errorLabelsSystemOverloaded =
        std::vector{std::string{ErrorLabel::kSystemOverloadedError}};

    static constexpr auto kSystemOverloadedErrorCode = ErrorCodes::IngressRequestRateLimitExceeded;
    static constexpr int kMaxCommandExecutions = kDefaultClientMaxRetryAttemptsDefault + 1;
    static constexpr std::uint32_t kKnownGoodSeed = 0xc0ffee;

    inline static auto kConfigShard = ShardId("config");
    FailPointEnableBlock _{"returnMaxBackoffDelay"};
};

class ShardRetryabilityTest : public ShardRemoteTest {
protected:
    inline static const std::array retryableErrorLabel = {std::string{ErrorLabel::kRetryableError}};
    inline static const std::array retryableWriteLabel = {std::string{ErrorLabel::kRetryableWrite}};

    void setUp() override {
        ShardRemoteTest::setUp();
        _shard =
            uassertStatusOK(shardRegistry()->getShard(operationContext(), kTestShards.front().id));
    }

    void tearDown() override {
        _shard = nullptr;
        ShardRemoteTest::tearDown();
    }

    std::shared_ptr<Shard> _shard;
};

TEST_F(ShardRemoteTest, TargeterMarksHostAsDownWhenConfigStepdown) {
    ASSERT_EQ(0UL, configTargeter()->getAndClearMarkedDownHosts().size());
    auto future = launchAsync([&] { runDummyCommandOnShard(kConfigShard); });

    auto error = Status(ErrorCodes::PrimarySteppedDown, "Config stepped down");
    onCommand([&](const executor::RemoteCommandRequest& request) { return error; });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::PrimarySteppedDown);
    ASSERT_EQ(1UL, configTargeter()->getAndClearMarkedDownHosts().size());
}

TEST_F(ShardRemoteTest, GridSetRetryBudgetCapacityServerParameter) {
    auto firstShard = kTestShards.front().id;
    auto firstShardHostAndPort = kTestShards.front().hosts.front();

    auto shard = uassertStatusOK(shardRegistry()->getShard(operationContext(), firstShard));
    auto& retryBudget = shard->getRetryBudget_forTest();
    auto retryStrategy = Shard::RetryStrategy{*shard, Shard::RetryPolicy::kIdempotent};

    auto initialBalance = retryBudget.getBalance_forTest();

    {
        auto _ = RAIIServerParameterControllerForTest{"shardRetryTokenBucketCapacity",
                                                      retryBudget.getBalance_forTest() + 1};
        retryStrategy.recordSuccess(firstShardHostAndPort);
        ASSERT_GT(retryBudget.getBalance_forTest(), initialBalance);
    }
}

TEST_F(ShardRemoteTest, GridSetRetryBudgetReturnRateServerParameter) {
    auto firstShard = kTestShards.front().id;
    auto firstShardHostAndPort = kTestShards.front().hosts.front();

    auto shard = uassertStatusOK(shardRegistry()->getShard(operationContext(), firstShard));
    auto& retryBudget = shard->getRetryBudget_forTest();
    auto retryStrategy = Shard::RetryStrategy{*shard, Shard::RetryPolicy::kIdempotent};

    auto initialBalance = retryBudget.getBalance_forTest();
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
        ASSERT_EQ(retryBudget.getBalance_forTest(), initialBalance - 1 + kReturnRate);
    }
}

TEST_F(ShardRemoteTest, ShardRetryStrategy) {
    constexpr auto backoff = Milliseconds{100};
    auto firstShard = kTestShards.front().id;
    auto firstShardHostAndPort = kTestShards.front().hosts.front();

    auto shardState = getShardState(firstShard);
    auto& [retryBudget, stats] = *shardState;

    auto shard = uassertStatusOK(shardRegistry()->getShard(operationContext(), firstShard));
    auto retryStrategy = Shard::RetryStrategy{*shard, Shard::RetryPolicy::kIdempotent};

    auto initialBalance = retryBudget.getBalance_forTest();
    auto error = Status(ErrorCodes::PrimarySteppedDown, "Interrupted at shutdown");

    ASSERT(retryStrategy.recordFailureAndEvaluateShouldRetry(
        error, firstShardHostAndPort, errorLabelsSystemOverloaded));
    ASSERT_LT(retryBudget.getBalance_forTest(), initialBalance);
    ASSERT_NE(std::ranges::find(retryStrategy.getTargetingMetadata().deprioritizedServers,
                                firstShardHostAndPort),
              retryStrategy.getTargetingMetadata().deprioritizedServers.end());

    ASSERT_EQ(stats.numOperationsAttempted.loadRelaxed(), 1);
    ASSERT_EQ(stats.numOperationsRetriedAtLeastOnceDueToOverload.loadRelaxed(), 1);
    ASSERT_EQ(stats.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded.loadRelaxed(), 0);
    ASSERT_EQ(stats.numOverloadErrorsReceived.loadRelaxed(), 1);
    ASSERT_EQ(stats.numRetriesDueToOverloadAttempted.loadRelaxed(), 1);
    ASSERT_EQ(stats.totalBackoffTimeMillis.loadRelaxed(), 0);

    ASSERT(retryStrategy.recordFailureAndEvaluateShouldRetry(
        error, firstShardHostAndPort, errorLabelsSystemOverloaded));

    ASSERT_EQ(stats.numOverloadErrorsReceived.loadRelaxed(), 2);
    ASSERT_EQ(stats.numRetriesDueToOverloadAttempted.loadRelaxed(), 2);

    retryStrategy.recordBackoff(backoff);
    retryStrategy.recordSuccess(firstShardHostAndPort);

    ASSERT_EQ(stats.numOperationsAttempted.loadRelaxed(), 1);
    ASSERT_EQ(stats.numOperationsRetriedAtLeastOnceDueToOverload.loadRelaxed(), 1);
    ASSERT_EQ(stats.numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded.loadRelaxed(), 1);
    ASSERT_EQ(stats.numOverloadErrorsReceived.loadRelaxed(), 2);
    ASSERT_EQ(stats.numRetriesDueToOverloadAttempted.loadRelaxed(), 2);
    ASSERT_EQ(stats.totalBackoffTimeMillis.loadRelaxed(), backoff.count());
}

TEST_F(ShardRemoteTest, RunCommandResponseErrorOverloaded) {
    auto future =
        launchAsync([&] { runDummyCommandOnShard(kConfigShard, Shard::RetryPolicy::kIdempotent); });

    runExhaustiveRetryBackoffTest([](const executor::RemoteCommandRequest&) {
        return createErrorSystemOverloaded(kSystemOverloadedErrorCode);
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, kSystemOverloadedErrorCode);
}

TEST_F(ShardRemoteTest, RunCommandResponseErrorOverloadedWithDeadline) {
    auto _ = Interruptible::DeadlineGuard{*operationContext(),
                                          clockSource()->now() + Milliseconds{200},
                                          ErrorCodes::ExceededTimeLimit};

    auto future =
        launchAsync([&] { runDummyCommandOnShard(kConfigShard, Shard::RetryPolicy::kIdempotent); });

    ASSERT_THROWS_CODE((runExhaustiveRetryBackoffTest([](const executor::RemoteCommandRequest&) {
                           return createErrorSystemOverloaded(kSystemOverloadedErrorCode);
                       })),
                       DBException,
                       ErrorCodes::ExceededTimeLimit);

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::ExceededTimeLimit);
}

TEST_F(ShardRemoteTest, RunExhaustiveCursorCommandErrorOverloadedRetry) {
    auto future = launchAsync([&] {
        auto shard =
            unittest::assertGet(shardRegistry()->getShard(operationContext(), kConfigShard));
        auto result = uassertStatusOK(shard->runExhaustiveCursorCommand(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::createDatabaseName_forTest(boost::none, "unusedDb"),
            BSON("unused" << "cmd"),
            Minutes{1}));
    });

    runExhaustiveRetryBackoffTest([](const executor::RemoteCommandRequest&) {
        return createErrorSystemOverloaded(kSystemOverloadedErrorCode);
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, kSystemOverloadedErrorCode);
}

TEST_F(ShardRemoteTest, RunAggregationWithResultErrorOverloadedRetry) {
    auto future = launchAsync([&] {
        auto shard =
            unittest::assertGet(shardRegistry()->getShard(operationContext(), kConfigShard));
        auto result = uassertStatusOK(shard->runAggregationWithResult(
            operationContext(),
            AggregateCommandRequest(NamespaceString::createNamespaceString_forTest(kNamespaceName),
                                    std::vector<mongo::BSONObj>()),
            Shard::RetryPolicy::kIdempotent));
    });

    runExhaustiveRetryBackoffTest([](const executor::RemoteCommandRequest&) {
        return createErrorSystemOverloaded(kSystemOverloadedErrorCode);
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, kSystemOverloadedErrorCode);
}

TEST_F(ShardRemoteTest, RunExhaustiveCursorCommandErrorOverloadedRetryTimeLimited) {
    constexpr auto shortButSignificantDeadline = Milliseconds{200};
    auto _ = Interruptible::DeadlineGuard{*operationContext(),
                                          clockSource()->now() + shortButSignificantDeadline,
                                          ErrorCodes::ExceededTimeLimit};

    auto future = launchAsync([&] {
        auto shard =
            unittest::assertGet(shardRegistry()->getShard(operationContext(), kConfigShard));
        auto result = uassertStatusOK(shard->runExhaustiveCursorCommand(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::createDatabaseName_forTest(boost::none, "unusedDb"),
            BSON("unused" << "cmd"),
            Minutes{1}));
    });

    ASSERT_THROWS_CODE((runExhaustiveRetryBackoffTest([](const executor::RemoteCommandRequest&) {
                           return createErrorSystemOverloaded(kSystemOverloadedErrorCode);
                       })),
                       DBException,
                       ErrorCodes::ExceededTimeLimit);

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::ExceededTimeLimit);
}

TEST_F(ShardRemoteTest, RunCommandResponseIndefiniteErrorOverloaded) {
    auto future = launchAsync([&] {
        auto shard =
            unittest::assertGet(shardRegistry()->getShard(operationContext(), kConfigShard));
        auto result = uassertStatusOK(shard->runCommandWithIndefiniteRetries(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::createDatabaseName_forTest(boost::none, "unusedDb"),
            BSON("unused" << "cmd"),
            Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(result.commandStatus);
    });

    for (int i = 0; i < kMaxCommandExecutions * 4; ++i) {
        onCommand([](const executor::RemoteCommandRequest&) {
            return createErrorSystemOverloaded(kSystemOverloadedErrorCode);
        });

        ASSERT_GT(advanceUntilReadyRequest(), Milliseconds{0});
    }

    onCommand([](const executor::RemoteCommandRequest&) {
        return Status{ErrorCodes::CommandFailed, "command failed"};
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::CommandFailed);
}

TEST_F(ShardRemoteTest, TargeterMarksHostAsDownWhenConfigShuttingDown) {
    ASSERT_EQ(0UL, configTargeter()->getAndClearMarkedDownHosts().size());
    auto future = launchAsync([&] { runDummyCommandOnShard(kConfigShard); });

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

    auto kConfigShard = ShardId("config");
    auto shard = unittest::assertGet(shardRegistry()->getShard(operationContext(), kConfigShard));
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
    auto timeoutMs = 100;

    ASSERT_EQ(0UL, configTargeter()->getAndClearMarkedDownHosts().size());
    auto future = launchAsync(
        [&] { runDummyCommandOnShardWithMaxTimeMS(kConfigShard, Milliseconds(timeoutMs)); });

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
    ASSERT_EQ(0UL, configTargeter()->getAndClearMarkedDownHosts().size());
    auto future = launchAsync([&] { runDummyCommandOnShard(kConfigShard); });

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

TEST_F(ShardRemoteTest, SystemOverloadedTargetingDeprioritizedServers) {
    FailPointEnableBlock _{"setBackoffDelayForTesting", BSON("backoffDelayMs" << 0)};

    auto firstShard = kTestShards.front().id;
    auto firstShardHosts = kTestShards.front().hosts;

    auto future = launchAsync([&] {
        auto shard = unittest::assertGet(shardRegistry()->getShard(operationContext(), firstShard));
        auto result = uassertStatusOK(shard->runCommandWithIndefiniteRetries(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::createDatabaseName_forTest(boost::none, "unusedDb"),
            BSON("unused" << "cmd"),
            Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(result.commandStatus);
    });

    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, firstShardHosts[0]);
        return createErrorSystemOverloaded(ErrorCodes::IngressRequestRateLimitExceeded);
    });

    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, firstShardHosts[1]);
        return Status{ErrorCodes::CommandFailed, "Error"};
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::CommandFailed);
}

TEST_F(ShardRetryabilityTest, RetryableErrorRemoteNoRetry) {
    ASSERT_FALSE(_shard->remoteIsRetriableError(
        Status{ErrorCodes::CommandFailed, "error"}, {}, Shard::RetryPolicy::kNoRetry));

    ASSERT_FALSE(_shard->remoteIsRetriableError(
        Status{ErrorCodes::WriteConcernTimeout, "error"}, {}, Shard::RetryPolicy::kNoRetry));

    ASSERT_FALSE(_shard->remoteIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                                retryableErrorLabel,
                                                Shard::RetryPolicy::kNoRetry));

    ASSERT_FALSE(_shard->remoteIsRetriableError(Status{ErrorCodes::WriteConcernTimeout, "error"},
                                                retryableWriteLabel,
                                                Shard::RetryPolicy::kNoRetry));
}

TEST_F(ShardRetryabilityTest, RetryableErrorRemoteIdempotent) {
    ASSERT(_shard->remoteIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                          retryableErrorLabel,
                                          Shard::RetryPolicy::kIdempotent));

    ASSERT(_shard->remoteIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                          retryableWriteLabel,
                                          Shard::RetryPolicy::kIdempotent));

    ASSERT(_shard->remoteIsRetriableError(Status{ErrorCodes::WriteConcernTimeout, "error"},
                                          retryableWriteLabel,
                                          Shard::RetryPolicy::kIdempotent));

    ASSERT_FALSE(_shard->remoteIsRetriableError(
        Status{ErrorCodes::CommandFailed, "error"}, {}, Shard::RetryPolicy::kIdempotent));

    ASSERT_FALSE(_shard->remoteIsRetriableError(
        Status{ErrorCodes::WriteConcernTimeout, "error"}, {}, Shard::RetryPolicy::kIdempotent));

    ASSERT(_shard->remoteIsRetriableError(
        Status{ErrorCodes::BalancerInterrupted, "error"}, {}, Shard::RetryPolicy::kIdempotent));
}

TEST_F(ShardRetryabilityTest, RetryableErrorRemoteIdempotentOrCursorInvalidated) {
    ASSERT(_shard->remoteIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                          retryableErrorLabel,
                                          Shard::RetryPolicy::kIdempotentOrCursorInvalidated));

    ASSERT(_shard->remoteIsRetriableError(Status{ErrorCodes::WriteConcernTimeout, "error"},
                                          retryableWriteLabel,
                                          Shard::RetryPolicy::kIdempotentOrCursorInvalidated));

    ASSERT_FALSE(
        _shard->remoteIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                       {},
                                       Shard::RetryPolicy::kIdempotentOrCursorInvalidated));

    ASSERT_FALSE(
        _shard->remoteIsRetriableError(Status{ErrorCodes::WriteConcernTimeout, "error"},
                                       {},
                                       Shard::RetryPolicy::kIdempotentOrCursorInvalidated));

    ASSERT(_shard->remoteIsRetriableError(Status{ErrorCodes::CursorNotFound, "error"},
                                          {},
                                          Shard::RetryPolicy::kIdempotentOrCursorInvalidated));

    ASSERT(_shard->remoteIsRetriableError(Status{ErrorCodes::BalancerInterrupted, "error"},
                                          {},
                                          Shard::RetryPolicy::kIdempotentOrCursorInvalidated));
}

TEST_F(ShardRetryabilityTest, RetryableErrorRemoteNotIdempotent) {
    ASSERT_FALSE(_shard->remoteIsRetriableError(
        Status{ErrorCodes::CommandFailed, "error"}, {}, Shard::RetryPolicy::kNotIdempotent));

    ASSERT(_shard->remoteIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                          retryableErrorLabel,
                                          Shard::RetryPolicy::kNotIdempotent));

    ASSERT_FALSE(_shard->remoteIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                                retryableWriteLabel,
                                                Shard::RetryPolicy::kNotIdempotent));

    ASSERT(_shard->remoteIsRetriableError(
        Status{ErrorCodes::PrimarySteppedDown, "error"}, {}, Shard::RetryPolicy::kNotIdempotent));
}

TEST_F(ShardRetryabilityTest, RetryableErrorRemoteStrictlyNotIdempotent) {
    ASSERT_FALSE(_shard->remoteIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                                {},
                                                Shard::RetryPolicy::kStrictlyNotIdempotent));

    ASSERT(_shard->remoteIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                          retryableErrorLabel,
                                          Shard::RetryPolicy::kStrictlyNotIdempotent));

    ASSERT_FALSE(_shard->remoteIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                                retryableWriteLabel,
                                                Shard::RetryPolicy::kStrictlyNotIdempotent));

    ASSERT_FALSE(_shard->remoteIsRetriableError(Status{ErrorCodes::PrimarySteppedDown, "error"},
                                                {},
                                                Shard::RetryPolicy::kStrictlyNotIdempotent));
}

TEST_F(ShardRetryabilityTest, RetryableErrorLocalNoRetry) {
    ASSERT_FALSE(_shard->localIsRetriableError(
        Status{ErrorCodes::CommandFailed, "error"}, {}, Shard::RetryPolicy::kNoRetry));

    ASSERT_FALSE(_shard->localIsRetriableError(
        Status{ErrorCodes::WriteConcernTimeout, "error"}, {}, Shard::RetryPolicy::kNoRetry));

    ASSERT_FALSE(_shard->localIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                               retryableErrorLabel,
                                               Shard::RetryPolicy::kNoRetry));

    ASSERT_FALSE(_shard->localIsRetriableError(Status{ErrorCodes::WriteConcernTimeout, "error"},
                                               retryableWriteLabel,
                                               Shard::RetryPolicy::kNoRetry));
}

TEST_F(ShardRetryabilityTest, RetryableErrorLocalIdempotent) {
    ASSERT(_shard->localIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                         retryableErrorLabel,
                                         Shard::RetryPolicy::kIdempotent));

    ASSERT(_shard->localIsRetriableError(Status{ErrorCodes::WriteConcernTimeout, "error"},
                                         retryableWriteLabel,
                                         Shard::RetryPolicy::kIdempotent));

    ASSERT(_shard->localIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                         retryableWriteLabel,
                                         Shard::RetryPolicy::kIdempotent));

    ASSERT_FALSE(_shard->localIsRetriableError(
        Status{ErrorCodes::CommandFailed, "error"}, {}, Shard::RetryPolicy::kIdempotent));

    ASSERT(_shard->localIsRetriableError(
        Status{ErrorCodes::WriteConcernTimeout, "error"}, {}, Shard::RetryPolicy::kIdempotent));
}

TEST_F(ShardRetryabilityTest, RetryableErrorLocalIdempotentOrCursorInvalidated) {
    ASSERT(_shard->localIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                         retryableErrorLabel,
                                         Shard::RetryPolicy::kIdempotentOrCursorInvalidated));

    ASSERT(_shard->localIsRetriableError(Status{ErrorCodes::WriteConcernTimeout, "error"},
                                         retryableWriteLabel,
                                         Shard::RetryPolicy::kIdempotentOrCursorInvalidated));

    ASSERT_FALSE(_shard->localIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                               {},
                                               Shard::RetryPolicy::kIdempotentOrCursorInvalidated));

    ASSERT(_shard->localIsRetriableError(Status{ErrorCodes::WriteConcernTimeout, "error"},
                                         {},
                                         Shard::RetryPolicy::kIdempotentOrCursorInvalidated));

    ASSERT(_shard->localIsRetriableError(Status{ErrorCodes::CursorNotFound, "error"},
                                         {},
                                         Shard::RetryPolicy::kIdempotentOrCursorInvalidated));
}

TEST_F(ShardRetryabilityTest, RetryableErrorLocalNotIdempotent) {
    ASSERT_FALSE(_shard->localIsRetriableError(
        Status{ErrorCodes::CommandFailed, "error"}, {}, Shard::RetryPolicy::kNotIdempotent));

    ASSERT_FALSE(_shard->localIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                               retryableWriteLabel,
                                               Shard::RetryPolicy::kNotIdempotent));

    ASSERT(_shard->localIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                         retryableErrorLabel,
                                         Shard::RetryPolicy::kNotIdempotent));

    ASSERT_FALSE(_shard->localIsRetriableError(
        Status{ErrorCodes::PrimarySteppedDown, "error"}, {}, Shard::RetryPolicy::kNotIdempotent));
}

TEST_F(ShardRetryabilityTest, RetryableErrorLocalStrictlyNotIdempotent) {
    ASSERT_FALSE(_shard->localIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                               {},
                                               Shard::RetryPolicy::kStrictlyNotIdempotent));

    ASSERT_FALSE(_shard->localIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                               retryableWriteLabel,
                                               Shard::RetryPolicy::kStrictlyNotIdempotent));

    ASSERT(_shard->localIsRetriableError(Status{ErrorCodes::CommandFailed, "error"},
                                         retryableErrorLabel,
                                         Shard::RetryPolicy::kStrictlyNotIdempotent));

    ASSERT_FALSE(_shard->localIsRetriableError(Status{ErrorCodes::PrimarySteppedDown, "error"},
                                               {},
                                               Shard::RetryPolicy::kStrictlyNotIdempotent));
}

}  // namespace
}  // namespace mongo
