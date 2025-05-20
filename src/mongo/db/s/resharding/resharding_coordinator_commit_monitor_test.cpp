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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include <algorithm>
#include <memory>
#include <ratio>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/metrics/sharding_data_transform_instance_metrics.h"
#include "mongo/db/s/resharding/resharding_coordinator_commit_monitor.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/request_types/resharding_operation_time_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace resharding {
namespace {

struct OperationTimeResponseFields {
    boost::optional<Milliseconds> remainingOpTime;
    boost::optional<Milliseconds> replicationLag;
};

class CoordinatorCommitMonitorTest : public ConfigServerTestFixture {
public:
    std::shared_ptr<CoordinatorCommitMonitor> getCommitMonitor() {
        invariant(_commitMonitor);
        return _commitMonitor;
    }

    void cancelMonitor() {
        _cancellationSource->cancel();
    }


    using Callback = unique_function<void()>;
    void runOnMockingNextResponse(Callback callback) {
        invariant(!_runOnMockingNextResponse.has_value());
        _runOnMockingNextResponse = std::move(callback);
    }

protected:
    ShardId recipientShardId0{"recipientShardId0"};
    ShardId recipientShardId1{"recipientShardId1"};

    void setUp() override;
    void tearDown() override;

    void mockResponsesOmitRemainingMillisForAllRecipients();

    void mockResponsesOmitRemainingMillisForOneRecipient();

    /**
     * Reports a remaining operation time larger than the commit threshold, thus indicating that the
     * coordinator should not engage the critical section yet.
     */
    void mockResponsesNotReadyToCommit();

    /**
     * Reports a remaining operation time smaller than the commit threshold, which indicates that
     * the coordinator can engage the critical section to block writes.
     */
    void mockResponsesReadyToCommit();

    void mockResponses(std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses);

    ShardsvrReshardingOperationTimeResponse makeResponse(OperationTimeResponseFields fields);

private:
    const NamespaceString _ns = NamespaceString::createNamespaceString_forTest("test.test");

    const std::vector<ShardId> _recipientShardIds = {recipientShardId0, recipientShardId1};

    std::shared_ptr<executor::ThreadPoolTaskExecutor> _futureExecutor;

    std::unique_ptr<CancellationSource> _cancellationSource;
    std::shared_ptr<CoordinatorCommitMonitor> _commitMonitor;

    boost::optional<Callback> _runOnMockingNextResponse;

    ReshardingCumulativeMetrics _cumulativeMetrics;
    std::shared_ptr<ReshardingMetrics> _metrics;
};

auto makeExecutor() {
    executor::ThreadPoolMock::Options options;
    options.onCreateThread = [] {
        Client::initThread("executor", getGlobalServiceContext()->getService());
    };
    auto net = std::make_unique<executor::NetworkInterfaceMock>();
    return executor::makeThreadPoolTestExecutor(std::move(net), std::move(options));
}

void CoordinatorCommitMonitorTest::setUp() {
    ConfigServerTestFixture::setUp();

    auto hostNameForShard = [](const ShardId& shard) -> std::string {
        return fmt::format("{}:1234", shard.toString());
    };

    std::vector<ShardType> shards;
    for (auto& recipientShardId : _recipientShardIds) {
        shards.push_back(
            ShardType(recipientShardId.toString(), hostNameForShard(recipientShardId)));
    }
    setupShards(shards);
    shardRegistry()->reload(operationContext());

    for (auto& recipientShardId : _recipientShardIds) {
        HostAndPort host(hostNameForShard(recipientShardId.toString()));
        RemoteCommandTargeterMock::get(shardRegistry()
                                           ->getShard(operationContext(), recipientShardId)
                                           .getValue()
                                           ->getTargeter())
            ->setFindHostReturnValue(std::move(host));
    }

    // The coordinator monitor uses `AsyncRequestsSender` to schedule remote commands, which
    // provides a blocking interface for fetching responses. The mocked networking, however,
    // requires yielding the executor thread after scheduling a remote command. To avoid deadlocks,
    // we only use `executor()` to schedule remote commands, and use the future executor to run
    // the future-continuation and its blocking calls.
    _futureExecutor = makeExecutor();
    _futureExecutor->startup();

    _cancellationSource = std::make_unique<CancellationSource>();

    auto clockSource = getServiceContext()->getFastClockSource();
    _metrics = std::make_shared<ReshardingMetrics>(
        UUID::gen(),
        BSON("y" << 1),
        _ns,
        ShardingDataTransformInstanceMetrics::Role::kCoordinator,
        clockSource->now(),
        clockSource,
        &_cumulativeMetrics);

    _commitMonitor = std::make_shared<CoordinatorCommitMonitor>(_metrics,
                                                                _ns,
                                                                _recipientShardIds,
                                                                _futureExecutor,
                                                                _cancellationSource->token(),
                                                                0,
                                                                Milliseconds(0));
    _commitMonitor->setNetworkExecutorForTest(executor());
}

void CoordinatorCommitMonitorTest::tearDown() {
    _commitMonitor.reset();
    _cancellationSource.reset();

    _futureExecutor->shutdown();
    _futureExecutor->join();

    ConfigServerTestFixture::tearDown();
}

void CoordinatorCommitMonitorTest::mockResponsesOmitRemainingMillisForAllRecipients() {
    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses = {
        {recipientShardId0, makeResponse({})},
        {recipientShardId1, makeResponse({})},
    };
    mockResponses(responses);
}

void CoordinatorCommitMonitorTest::mockResponsesOmitRemainingMillisForOneRecipient() {
    auto threshold = Milliseconds(gRemainingReshardingOperationTimeThresholdMillis.load());

    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses = {
        {recipientShardId0, makeResponse({.remainingOpTime = threshold - Milliseconds(1)})},

        {recipientShardId1, makeResponse({})},
    };
    mockResponses(responses);
}

void CoordinatorCommitMonitorTest::mockResponsesNotReadyToCommit() {
    auto threshold = Milliseconds(gRemainingReshardingOperationTimeThresholdMillis.load());
    auto remainingOpTime = threshold + Milliseconds(1);
    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses = {
        {recipientShardId0, makeResponse({.remainingOpTime = remainingOpTime})},

        {recipientShardId1, makeResponse({.remainingOpTime = remainingOpTime})},
    };
    mockResponses(responses);
}

void CoordinatorCommitMonitorTest::mockResponsesReadyToCommit() {
    auto threshold = Milliseconds(gRemainingReshardingOperationTimeThresholdMillis.load());
    auto remainingOpTime = threshold - Milliseconds(1);
    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses = {
        {recipientShardId0, makeResponse({.remainingOpTime = remainingOpTime})},

        {recipientShardId1, makeResponse({.remainingOpTime = remainingOpTime})},
    };
    mockResponses(responses);
}

void CoordinatorCommitMonitorTest::mockResponses(
    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses) {
    for (size_t i = 0; i < responses.size(); i++) {
        onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            LOGV2(10393201, "Mocking command response", "command"_attr = request.cmdObj);

            if (_runOnMockingNextResponse) {
                (*_runOnMockingNextResponse)();
                _runOnMockingNextResponse = boost::none;
            }

            ShardId shardId{request.target.host()};
            auto it = responses.find(shardId);
            invariant(it != responses.end());
            return it->second.toBSON();
        });
    }
}

ShardsvrReshardingOperationTimeResponse CoordinatorCommitMonitorTest::makeResponse(
    OperationTimeResponseFields fields) {
    ShardsvrReshardingOperationTimeResponse response;
    response.setRecipientRemainingMillis(fields.remainingOpTime);
    response.setMajorityReplicationLagMillis(fields.replicationLag);
    return response;
}

TEST_F(CoordinatorCommitMonitorTest, ComputesMinAndMaxRemainingTimesReplicationLagNotAvailable) {
    auto minRemainingOpTime = Milliseconds{1};
    auto maxRemainingOpTime = Milliseconds{8};
    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses = {
        {recipientShardId0,
         makeResponse({
             .remainingOpTime = minRemainingOpTime,
         })},
        {recipientShardId1,
         makeResponse({
             .remainingOpTime = maxRemainingOpTime,
         })},
    };

    for (bool accountForReplLag : {true, false}) {
        LOGV2(10393202,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "accountForReplLag"_attr = accountForReplLag);

        RAIIServerParameterControllerForTest batchSize{
            "reshardingRemainingTimeEstimateAccountsForRecipientReplicationLag", accountForReplLag};

        auto future = launchAsync([this] {
            ThreadClient tc(getServiceContext()->getService());
            return getCommitMonitor()->queryRemainingOperationTimeForRecipients();
        });

        mockResponses(responses);

        auto newRemainingOpTimes = future.default_timed_get();
        ASSERT_EQUALS(newRemainingOpTimes.min, minRemainingOpTime);
        ASSERT_EQUALS(newRemainingOpTimes.max, maxRemainingOpTime);
    }
}

TEST_F(CoordinatorCommitMonitorTest, ComputesMinAndMaxRemainingTimesReplicationLagFullyAvailable) {
    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses = {
        {recipientShardId0,
         makeResponse({.remainingOpTime = Milliseconds{1}, .replicationLag = Milliseconds{100}})},
        {recipientShardId1,
         makeResponse({.remainingOpTime = Milliseconds{2}, .replicationLag = Milliseconds{10}})},
    };

    for (bool accountForReplLag : {true, false}) {
        LOGV2(10393203,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "accountForReplLag"_attr = accountForReplLag);

        RAIIServerParameterControllerForTest batchSize{
            "reshardingRemainingTimeEstimateAccountsForRecipientReplicationLag", accountForReplLag};

        auto future = launchAsync([this] {
            ThreadClient tc(getServiceContext()->getService());
            return getCommitMonitor()->queryRemainingOperationTimeForRecipients();
        });

        mockResponses(responses);

        auto newRemainingOpTimes = future.default_timed_get();
        ASSERT_EQUALS(newRemainingOpTimes.min,
                      accountForReplLag ? Milliseconds{12} : Milliseconds{1});
        ASSERT_EQUALS(newRemainingOpTimes.max,
                      accountForReplLag ? Milliseconds{101} : Milliseconds{2});
    }
}

TEST_F(CoordinatorCommitMonitorTest,
       ComputesMinAndMaxRemainingTimesReplicationLagPartiallyAvailable) {
    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses = {
        {recipientShardId0,
         makeResponse({
             .remainingOpTime = Milliseconds{1},
         })},
        {recipientShardId1,
         makeResponse({
             .remainingOpTime = Milliseconds{2},
             .replicationLag = Milliseconds{10},
         })},
    };

    for (bool accountForReplLag : {true, false}) {
        LOGV2(10393204,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "accountForReplLag"_attr = accountForReplLag);

        RAIIServerParameterControllerForTest batchSize{
            "reshardingRemainingTimeEstimateAccountsForRecipientReplicationLag", accountForReplLag};

        auto future = launchAsync([this] {
            ThreadClient tc(getServiceContext()->getService());
            return getCommitMonitor()->queryRemainingOperationTimeForRecipients();
        });

        mockResponses(responses);

        auto newRemainingOpTimes = future.default_timed_get();
        ASSERT_EQUALS(newRemainingOpTimes.min, Milliseconds{1});
        ASSERT_EQUALS(newRemainingOpTimes.max,
                      accountForReplLag ? Milliseconds{12} : Milliseconds{2});
    }
}

TEST_F(CoordinatorCommitMonitorTest, UnblocksWhenRecipientsWithinCommitThreshold) {
    auto future = getCommitMonitor()->waitUntilRecipientsAreWithinCommitThreshold();

    // Indicate that recipients are not within the commit threshold to trigger a retry.
    mockResponsesNotReadyToCommit();
    ASSERT(!future.isReady());

    // Indicate that recipients are within the commit threshold.
    mockResponsesReadyToCommit();
    future.get();
}

TEST_F(CoordinatorCommitMonitorTest, UnblocksWhenCancellationTokenIsCancelled) {
    auto future = [&] {
        FailPointEnableBlock fp("hangBeforeQueryingRecipients");
        auto future = getCommitMonitor()->waitUntilRecipientsAreWithinCommitThreshold();
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
        // Cancels the monitor before waiting for the recipients to respond to the query. Once the
        // fail-point is disabled, the monitor should cancel pending network operations and make the
        // future ready. Thus, we do not block to run commands on behalf of the mocked network here.
        cancelMonitor();
        return future;
    }();

    ASSERT_EQ(future.getNoThrow(), ErrorCodes::CallbackCanceled);
}

TEST_F(CoordinatorCommitMonitorTest, RetriesWhenEncountersErrorsWhileQueryingRecipients) {
    auto future = getCommitMonitor()->waitUntilRecipientsAreWithinCommitThreshold();
    {
        FailPointEnableBlock fp("failQueryingRecipients");
        mockResponsesReadyToCommit();
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
    }

    ASSERT(!future.isReady());
    mockResponsesReadyToCommit();
    future.get();
}

TEST_F(CoordinatorCommitMonitorTest, BlocksWhenRemainingMillisIsOmitted) {
    auto future = getCommitMonitor()->waitUntilRecipientsAreWithinCommitThreshold();

    mockResponsesOmitRemainingMillisForAllRecipients();
    ASSERT(!future.isReady());

    // If even a single shard omits remainingMillis, we cannot begin the critical section.
    mockResponsesOmitRemainingMillisForOneRecipient();
    ASSERT(!future.isReady());

    mockResponsesReadyToCommit();
    future.get();
}

TEST_F(CoordinatorCommitMonitorTest,
       BlocksWhenRemainingMillisPlusReplicationLagNotWithinCommitThreshold) {
    // Not set the reshardingRemainingTimeEstimateAccountsForRecipientReplicationLag to test that
    // the default is true.
    auto future = getCommitMonitor()->waitUntilRecipientsAreWithinCommitThreshold();
    auto threshold = gRemainingReshardingOperationTimeThresholdMillis.load();

    // replicationLag > threshold.
    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses0 = {
        {recipientShardId0,
         makeResponse({
             .remainingOpTime = Milliseconds{0},
             .replicationLag = Milliseconds{0},
         })},
        {recipientShardId1,
         makeResponse({
             .remainingOpTime = Milliseconds{0},
             .replicationLag = Milliseconds{threshold + 1},
         })},
    };
    mockResponses(responses0);
    ASSERT(!future.isReady());

    // remainingTime + replicationLag > threshold.
    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses1 = {
        {recipientShardId0,
         makeResponse({
             .remainingOpTime = Milliseconds{1},
             .replicationLag = Milliseconds{threshold},
         })},
        {recipientShardId1,
         makeResponse({
             .remainingOpTime = Milliseconds{0},
             .replicationLag = Milliseconds{0},
         })},
    };
    mockResponses(responses1);
    ASSERT(!future.isReady());

    // remainingTime + replicationLag < threshold.
    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses2 = {
        {recipientShardId0,
         makeResponse({
             .remainingOpTime = Milliseconds{0},
             .replicationLag = Milliseconds{0},
         })},
        {recipientShardId1,
         makeResponse({
             .remainingOpTime = Milliseconds{0},
             .replicationLag = Milliseconds{threshold - 1},
         })},
    };
    mockResponses(responses2);

    future.get();
}

TEST_F(CoordinatorCommitMonitorTest, ReconfiguringThreshold) {
    auto thresholdBefore = gRemainingReshardingOperationTimeThresholdMillis.load();

    auto fp = globalFailPointRegistry().find("hangBeforeQueryingRecipients");
    auto timesEnteredBefore = fp->setMode(FailPoint::alwaysOn);

    auto future = getCommitMonitor()->waitUntilRecipientsAreWithinCommitThreshold();

    fp->waitForTimesEntered(timesEnteredBefore + 1);
    auto timesEnteredAfter = fp->setMode(FailPoint::off);
    ASSERT_EQ(timesEnteredAfter, timesEnteredBefore + 1);

    std::map<ShardId, ShardsvrReshardingOperationTimeResponse> responses = {
        {recipientShardId0,
         makeResponse({
             .remainingOpTime = Milliseconds{0},
             .replicationLag = Milliseconds{0},
         })},
        {recipientShardId1,
         makeResponse({
             .remainingOpTime = Milliseconds{thresholdBefore + 1},
             .replicationLag = Milliseconds{0},
         })},
    };
    auto thresholdAfter = thresholdBefore + 2;

    RAIIServerParameterControllerForTest threshold{
        "remainingReshardingOperationTimeThresholdMillis", thresholdAfter};

    mockResponses(responses);

    // If the commit monitor doesn't detect the new threshold, the wait below would hang.
    future.get();
}

}  // namespace
}  // namespace resharding
}  // namespace mongo
