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


#include <algorithm>
#include <boost/optional.hpp>
#include <fmt/format.h>
#include <memory>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/resharding/resharding_coordinator_commit_monitor.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"

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

private:
    const NamespaceString _ns{"test.test"};
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
    options.onCreateThread = [] { Client::initThread("executor", nullptr); };
    auto net = std::make_unique<executor::NetworkInterfaceMock>();
    return executor::makeSharedThreadPoolTestExecutor(std::move(net), std::move(options));
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

        return BSON("remainingMillis" << durationCount<Milliseconds>(remainingOperationTime));
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
            auto threshold = Milliseconds(gRemainingReshardingOperationTimeThresholdMillis.load());
            return BSON("remainingMillis"
                        << durationCount<Milliseconds>(threshold - Milliseconds(1)));
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

        if (useMin) {
            useMin = false;
            return BSON("remainingMillis"
                        << durationCount<Milliseconds>(remainingOperationTimes.min));
        } else {
            return BSON("remainingMillis"
                        << durationCount<Milliseconds>(remainingOperationTimes.max));
        }
    };

    std::for_each(
        _recipientShards.begin(), _recipientShards.end(), [&](const ShardId&) { onCommand(func); });
}

TEST_F(CoordinatorCommitMonitorTest, ComputesMinAndMaxRemainingTimes) {
    auto future = launchAsync([this] {
        ThreadClient tc(getServiceContext());
        return getCommitMonitor()->queryRemainingOperationTimeForRecipients();
    });

    auto minTimeMillis = 1;
    auto maxTimeMillis = 8;

    CoordinatorCommitMonitor::RemainingOperationTimes remainingOpTimes = {
        Milliseconds(minTimeMillis), Milliseconds(maxTimeMillis)};

    mockRemaingOperationTimesCommandForRecipients(remainingOpTimes);

    auto newRemainingOpTimes = future.default_timed_get();
    ASSERT_EQUALS(newRemainingOpTimes.min, remainingOpTimes.min);
    ASSERT_EQUALS(newRemainingOpTimes.max, remainingOpTimes.max);
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

}  // namespace
}  // namespace resharding
}  // namespace mongo
