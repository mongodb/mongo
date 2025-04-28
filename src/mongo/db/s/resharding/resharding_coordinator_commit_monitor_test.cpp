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

class CoordinatorCommitMonitorTest : public ConfigServerTestFixture {
public:
    std::shared_ptr<CoordinatorCommitMonitor> getCommitMonitor() {
        invariant(_commitMonitor);
        return _commitMonitor;
    }

    void cancelMonitor() {
        _cancellationSource->cancel();
    }

    // Reports a remaining operation time larger than the commit threshold, thus indicating that the
    // coordinator should not engage the critical section yet.
    void respondWithNotReadyToCommit() {
        auto threshold = Milliseconds(gRemainingReshardingOperationTimeThresholdMillis.load());
        mockCommandForRecipients(threshold + Milliseconds(1));
    }

    // Reports a remaining operation time smaller than the commit threshold, which indicates that
    // the coordinator can engage the critical section to block writes.
    void respondWithReadyToCommit() {
        auto threshold = Milliseconds(gRemainingReshardingOperationTimeThresholdMillis.load());
        mockCommandForRecipients(threshold - Milliseconds(1));
    }

    using Callback = unique_function<void()>;
    void runOnMockingNextResponse(Callback callback) {
        invariant(!_runOnMockingNextResponse.has_value());
        _runOnMockingNextResponse = std::move(callback);
    }

protected:
    void setUp() override;
    void tearDown() override;

    void mockCommandForRecipients(Milliseconds remainingOperationTime);

    void mockOmitRemainingMillisForRecipients();

    void mockOmitRemainingMillisForOneRecipient();

    void mockRemaingOperationTimesCommandForRecipients(
        CoordinatorCommitMonitor::RemainingOperationTimes remainingOperationTimes);

    void mockRemaingOperationTimesCommandForRecipients(
        std::vector<Milliseconds> remainingOperationTimes,
        std::vector<boost::optional<Milliseconds>> replicationLags);

private:
    const NamespaceString _ns = NamespaceString::createNamespaceString_forTest("test.test");
    const std::vector<ShardId> _recipientShards = {{"shardOne"}, {"shardTwo"}};

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
    for (auto& recipient : _recipientShards) {
        shards.push_back(ShardType(recipient.toString(), hostNameForShard(recipient)));
    }
    setupShards(shards);
    shardRegistry()->reload(operationContext());

    for (auto& recipient : _recipientShards) {
        HostAndPort host(hostNameForShard(recipient.toString()));
        RemoteCommandTargeterMock::get(
            shardRegistry()->getShard(operationContext(), recipient).getValue()->getTargeter())
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
                                                                _recipientShards,
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

void CoordinatorCommitMonitorTest::mockCommandForRecipients(Milliseconds remainingOperationTime) {
    auto func = [&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        LOGV2(5392005, "Mocking command response", "command"_attr = request.cmdObj);

        if (_runOnMockingNextResponse) {
            (*_runOnMockingNextResponse)();
            _runOnMockingNextResponse = boost::none;
        }

        ShardsvrReshardingOperationTimeResponse response;
        response.setRemainingMillis(remainingOperationTime);
        return response.toBSON();
    };

    std::for_each(
        _recipientShards.begin(), _recipientShards.end(), [&](const ShardId&) { onCommand(func); });
}

void CoordinatorCommitMonitorTest::mockOmitRemainingMillisForRecipients() {
    // Omit remainingMillis from all shard responses.
    std::for_each(_recipientShards.begin(), _recipientShards.end(), [this](const ShardId&) {
        onCommand([](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            // Return an empty BSON object.
            return BSONObj();
        });
    });
}

void CoordinatorCommitMonitorTest::mockOmitRemainingMillisForOneRecipient() {
    // Omit remainingMillis from a single recipient.
    for (const auto& shard : _recipientShards) {
        onCommand([&](const executor::RemoteCommandRequest&) -> StatusWith<BSONObj> {
            if (shard == _recipientShards.front()) {
                // Return an empty BSON object.
                return BSONObj();
            }
            ShardsvrReshardingOperationTimeResponse response;
            auto threshold = Milliseconds(gRemainingReshardingOperationTimeThresholdMillis.load());
            response.setRemainingMillis(threshold - Milliseconds(1));
            return response.toBSON();
        });
    }
}

void CoordinatorCommitMonitorTest::mockRemaingOperationTimesCommandForRecipients(
    CoordinatorCommitMonitor::RemainingOperationTimes remainingOperationTimes) {
    bool useMin = true;
    auto func = [&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        LOGV2(5727600, "Mocking command response", "command"_attr = request.cmdObj);

        if (_runOnMockingNextResponse) {
            (*_runOnMockingNextResponse)();
            _runOnMockingNextResponse = boost::none;
        }

        ShardsvrReshardingOperationTimeResponse response;
        if (useMin) {
            useMin = false;
            response.setRemainingMillis(remainingOperationTimes.min);
        } else {
            response.setRemainingMillis(remainingOperationTimes.max);
        }
        return response.toBSON();
    };

    std::for_each(
        _recipientShards.begin(), _recipientShards.end(), [&](const ShardId&) { onCommand(func); });
}

void CoordinatorCommitMonitorTest::mockRemaingOperationTimesCommandForRecipients(
    std::vector<Milliseconds> remainingOperationTimes,
    std::vector<boost::optional<Milliseconds>> replicationLags) {
    ASSERT_EQ(_recipientShards.size(), remainingOperationTimes.size());
    ASSERT_EQ(_recipientShards.size(), replicationLags.size());

    for (size_t i = 0; i < _recipientShards.size(); i++) {
        onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            LOGV2(10393201, "Mocking command response", "command"_attr = request.cmdObj);

            if (_runOnMockingNextResponse) {
                (*_runOnMockingNextResponse)();
                _runOnMockingNextResponse = boost::none;
            }

            ShardsvrReshardingOperationTimeResponse response;
            response.setRemainingMillis(remainingOperationTimes[i]);
            response.setMajorityReplicationLagMillis(replicationLags[i]);
            return response.toBSON();
        });
    }
}


TEST_F(CoordinatorCommitMonitorTest, ComputesMinAndMaxRemainingTimesReplicationLagNotAvailable) {
    auto minTimeMillis = 1;
    auto maxTimeMillis = 8;

    CoordinatorCommitMonitor::RemainingOperationTimes remainingOpTimes = {
        Milliseconds(minTimeMillis), Milliseconds(maxTimeMillis)};

    for (bool accountForReplLag : {true, false}) {
        LOGV2(10393202,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "accountForReplLag"_attr = accountForReplLag);

        RAIIServerParameterControllerForTest batchSize{
            "reshardingRemainingTimeEstimateAccountsForReplicationLag", accountForReplLag};

        auto future = launchAsync([this] {
            ThreadClient tc(getServiceContext()->getService());
            return getCommitMonitor()->queryRemainingOperationTimeForRecipients();
        });

        mockRemaingOperationTimesCommandForRecipients(remainingOpTimes);

        auto newRemainingOpTimes = future.default_timed_get();
        ASSERT_EQUALS(newRemainingOpTimes.min, remainingOpTimes.min);
        ASSERT_EQUALS(newRemainingOpTimes.max, remainingOpTimes.max);
    }
}

TEST_F(CoordinatorCommitMonitorTest, ComputesMinAndMaxRemainingTimesReplicationLagFullyAvailable) {
    std::vector<Milliseconds> remainingOpTimes{Milliseconds{1}, Milliseconds{2}};
    std::vector<boost::optional<Milliseconds>> replicationLags{Milliseconds{100}, Milliseconds{10}};

    for (bool accountForReplLag : {true, false}) {
        LOGV2(10393203,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "accountForReplLag"_attr = accountForReplLag);

        RAIIServerParameterControllerForTest batchSize{
            "reshardingRemainingTimeEstimateAccountsForReplicationLag", accountForReplLag};

        auto future = launchAsync([this] {
            ThreadClient tc(getServiceContext()->getService());
            return getCommitMonitor()->queryRemainingOperationTimeForRecipients();
        });

        mockRemaingOperationTimesCommandForRecipients(remainingOpTimes, replicationLags);

        auto newRemainingOpTimes = future.default_timed_get();
        ASSERT_EQUALS(newRemainingOpTimes.min,
                      accountForReplLag ? Milliseconds{12} : Milliseconds{1});
        ASSERT_EQUALS(newRemainingOpTimes.max,
                      accountForReplLag ? Milliseconds{101} : Milliseconds{2});
    }
}

TEST_F(CoordinatorCommitMonitorTest,
       ComputesMinAndMaxRemainingTimesReplicationLagPartiallyAvailable) {
    std::vector<Milliseconds> remainingOpTimes{Milliseconds{1}, Milliseconds{2}};
    std::vector<boost::optional<Milliseconds>> replicationLags{boost::none, Milliseconds{10}};

    for (bool accountForReplLag : {true, false}) {
        LOGV2(10393204,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "accountForReplLag"_attr = accountForReplLag);

        RAIIServerParameterControllerForTest batchSize{
            "reshardingRemainingTimeEstimateAccountsForReplicationLag", accountForReplLag};

        auto future = launchAsync([this] {
            ThreadClient tc(getServiceContext()->getService());
            return getCommitMonitor()->queryRemainingOperationTimeForRecipients();
        });

        mockRemaingOperationTimesCommandForRecipients(remainingOpTimes, replicationLags);

        auto newRemainingOpTimes = future.default_timed_get();
        ASSERT_EQUALS(newRemainingOpTimes.min, Milliseconds{1});
        ASSERT_EQUALS(newRemainingOpTimes.max,
                      accountForReplLag ? Milliseconds{12} : Milliseconds{2});
    }
}

TEST_F(CoordinatorCommitMonitorTest, UnblocksWhenRecipientsWithinCommitThreshold) {
    auto future = getCommitMonitor()->waitUntilRecipientsAreWithinCommitThreshold();

    // Indicate that recipients are not within the commit threshold to trigger a retry.
    respondWithNotReadyToCommit();
    ASSERT(!future.isReady());

    // Indicate that recipients are within the commit threshold.
    respondWithReadyToCommit();
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
        respondWithReadyToCommit();
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
    }

    ASSERT(!future.isReady());
    respondWithReadyToCommit();
    future.get();
}

TEST_F(CoordinatorCommitMonitorTest, BlocksWhenRemainingMillisIsOmitted) {
    auto future = getCommitMonitor()->waitUntilRecipientsAreWithinCommitThreshold();

    mockOmitRemainingMillisForRecipients();
    ASSERT(!future.isReady());

    // If even a single shard omits remainingMillis, we cannot begin the critical section.
    mockOmitRemainingMillisForOneRecipient();
    ASSERT(!future.isReady());

    respondWithReadyToCommit();
    future.get();
}

TEST_F(CoordinatorCommitMonitorTest,
       BlocksWhenRemainingMillisPlusReplicationLagNotWithinCommitThreshold) {
    // Not set the reshardingRemainingTimeEstimateAccountsForReplicationLag to test that the
    // default is true.
    auto future = getCommitMonitor()->waitUntilRecipientsAreWithinCommitThreshold();
    auto threshold = gRemainingReshardingOperationTimeThresholdMillis.load();

    // replicationLag > threshold.
    std::vector<Milliseconds> remainingOpTimes0{
        Milliseconds{0},
        Milliseconds{0},
    };
    std::vector<boost::optional<Milliseconds>> replicationLags0{
        Milliseconds{0},
        Milliseconds{threshold + 1},
    };
    mockRemaingOperationTimesCommandForRecipients(remainingOpTimes0, replicationLags0);
    ASSERT(!future.isReady());

    // remainingTime + replicationLag > threshold.
    std::vector<Milliseconds> remainingOpTimes1{
        Milliseconds{1},
        Milliseconds{0},
    };
    std::vector<boost::optional<Milliseconds>> replicationLags1{
        Milliseconds{threshold},
        Milliseconds{0},
    };
    mockRemaingOperationTimesCommandForRecipients(remainingOpTimes1, replicationLags1);
    ASSERT(!future.isReady());

    // remainingTime + replicationLag < threshold.
    std::vector<Milliseconds> remainingOpTimes2{
        Milliseconds{0},
        Milliseconds{0},
    };
    std::vector<boost::optional<Milliseconds>> replicationLags2{
        Milliseconds{0},
        Milliseconds{threshold - 1},
    };
    mockRemaingOperationTimesCommandForRecipients(remainingOpTimes2, replicationLags2);

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

    std::vector<Milliseconds> remainingOpTimes{
        Milliseconds{0},
        Milliseconds{thresholdBefore + 1},
    };
    std::vector<boost::optional<Milliseconds>> replicationLags{
        Milliseconds{0},
        Milliseconds{0},
    };
    auto thresholdAfter = thresholdBefore + 2;

    RAIIServerParameterControllerForTest threshold{
        "remainingReshardingOperationTimeThresholdMillis", thresholdAfter};
    mockRemaingOperationTimesCommandForRecipients(remainingOpTimes, replicationLags);

    // If the commit monitor doesn't detect the new threshold, the wait below would hang.
    future.get();
}

}  // namespace
}  // namespace resharding
}  // namespace mongo
