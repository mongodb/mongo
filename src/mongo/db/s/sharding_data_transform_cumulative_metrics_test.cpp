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
#include "mongo/db/s/sharding_data_transform_metrics_test_fixture.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"
#include "mongo/util/static_immortal.h"

namespace mongo {
namespace {

class ScopedObserverMock {
public:
    using Ptr = std::unique_ptr<ScopedObserverMock>;

    ScopedObserverMock(Date_t startTime,
                       int64_t timeRemaining,
                       ClockSource* clockSource,
                       ShardingDataTransformCumulativeMetrics* parent)
        : _mock{startTime, timeRemaining}, _deregister{parent->registerInstanceMetrics(&_mock)} {}

    ~ScopedObserverMock() {
        if (_deregister) {
            _deregister();
        }
    }

private:
    ObserverMock _mock;
    ShardingDataTransformCumulativeMetrics::DeregistrationFunction _deregister;
};

class ShardingDataTransformCumulativeMetricsTest : public ShardingDataTransformMetricsTestFixture {
public:
    static BSONObj getLatencySection(const ShardingDataTransformCumulativeMetrics& metrics) {
        BSONObjBuilder bob;
        metrics.reportForServerStatus(&bob);
        auto report = bob.done();
        return report.getObjectField(kTestMetricsName).getObjectField("latencies").getOwned();
    }
};

TEST_F(ShardingDataTransformCumulativeMetricsTest, AddAndRemoveMetrics) {
    auto deregister = _cumulativeMetrics.registerInstanceMetrics(getOldestObserver());
    ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 1);
    deregister();
    ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 0);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, MetricsReportsOldestWhenInsertedFirst) {
    auto deregisterOldest = _cumulativeMetrics.registerInstanceMetrics(getOldestObserver());
    auto deregisterYoungest = _cumulativeMetrics.registerInstanceMetrics(getYoungestObserver());
    ASSERT_EQ(_cumulativeMetrics.getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              kOldestTimeLeft);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, MetricsReportsOldestWhenInsertedLast) {
    auto deregisterYoungest = _cumulativeMetrics.registerInstanceMetrics(getYoungestObserver());
    auto deregisterOldest = _cumulativeMetrics.registerInstanceMetrics(getOldestObserver());
    ASSERT_EQ(_cumulativeMetrics.getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              kOldestTimeLeft);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, RemainingTimeReports0WhenEmpty) {
    ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 0);
    ASSERT_EQ(_cumulativeMetrics.getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              0);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, UpdatesOldestWhenOldestIsRemoved) {
    auto deregisterYoungest = _cumulativeMetrics.registerInstanceMetrics(getYoungestObserver());
    auto deregisterOldest = _cumulativeMetrics.registerInstanceMetrics(getOldestObserver());
    ASSERT_EQ(_cumulativeMetrics.getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              kOldestTimeLeft);
    deregisterOldest();
    ASSERT_EQ(_cumulativeMetrics.getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              kYoungestTimeLeft);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, InsertsTwoWithSameStartTime) {
    auto deregisterOldest = _cumulativeMetrics.registerInstanceMetrics(getOldestObserver());
    ObserverMock sameAsOldest{kOldestTime, kOldestTimeLeft};
    auto deregisterOldest2 = _cumulativeMetrics.registerInstanceMetrics(&sameAsOldest);
    ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 2);
    ASSERT_EQ(_cumulativeMetrics.getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              kOldestTimeLeft);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, StillReportsOldestAfterRandomOperations) {
    doRandomOperationsTest<ScopedObserverMock>();
}

TEST_F(ShardingDataTransformCumulativeMetricsTest,
       StillReportsOldestAfterRandomOperationsMultithreaded) {
    doRandomOperationsMultithreadedTest<ScopedObserverMock>();
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportsOldestByRole) {
    using Role = ShardingDataTransformMetrics::Role;
    auto& metrics = _cumulativeMetrics;
    ObserverMock oldDonor{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kDonor};
    ObserverMock youngDonor{Date_t::fromMillisSinceEpoch(200), 200, 200, Role::kDonor};
    ObserverMock oldRecipient{Date_t::fromMillisSinceEpoch(300), 300, 300, Role::kRecipient};
    ObserverMock youngRecipient{Date_t::fromMillisSinceEpoch(400), 400, 400, Role::kRecipient};
    auto removeOldD = metrics.registerInstanceMetrics(&oldDonor);
    auto removeYoungD = metrics.registerInstanceMetrics(&youngDonor);
    auto removeOldR = metrics.registerInstanceMetrics(&oldRecipient);
    auto removeYoungR = metrics.registerInstanceMetrics(&youngRecipient);

    ASSERT_EQ(metrics.getObservedMetricsCount(), 4);
    ASSERT_EQ(metrics.getObservedMetricsCount(Role::kDonor), 2);
    ASSERT_EQ(metrics.getObservedMetricsCount(Role::kRecipient), 2);
    ASSERT_EQ(metrics.getOldestOperationHighEstimateRemainingTimeMillis(Role::kDonor), 100);
    ASSERT_EQ(metrics.getOldestOperationHighEstimateRemainingTimeMillis(Role::kRecipient), 300);
    removeOldD();
    ASSERT_EQ(metrics.getObservedMetricsCount(), 3);
    ASSERT_EQ(metrics.getObservedMetricsCount(Role::kDonor), 1);
    ASSERT_EQ(metrics.getOldestOperationHighEstimateRemainingTimeMillis(Role::kDonor), 200);
    removeOldR();
    ASSERT_EQ(metrics.getObservedMetricsCount(), 2);
    ASSERT_EQ(metrics.getObservedMetricsCount(Role::kRecipient), 1);
    ASSERT_EQ(metrics.getOldestOperationHighEstimateRemainingTimeMillis(Role::kRecipient), 400);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsTimeEstimates) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);
    ignore = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

    BSONObjBuilder bob;
    _cumulativeMetrics.reportForServerStatus(&bob);
    auto report = bob.done();
    auto section = report.getObjectField(kTestMetricsName).getObjectField("oldestActive");
    ASSERT_EQ(section.getIntField("recipientRemainingOperationTimeEstimatedMillis"), 100);
    ASSERT_EQ(
        section.getIntField("coordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis"),
        400);
    ASSERT_EQ(
        section.getIntField("coordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis"),
        300);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsRunCount) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics.reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kTestMetricsName).getIntField("countStarted"), 0);
    }

    _cumulativeMetrics.onStarted();

    {
        BSONObjBuilder bob;
        _cumulativeMetrics.reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kTestMetricsName).getIntField("countStarted"), 1);
    }
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsSucceededCount) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics.reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kTestMetricsName).getIntField("countSucceeded"), 0);
    }

    _cumulativeMetrics.onCompletion(ReshardingOperationStatusEnum::kSuccess);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics.reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kTestMetricsName).getIntField("countSucceeded"), 1);
    }
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsFailedCount) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics.reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kTestMetricsName).getIntField("countFailed"), 0);
    }

    _cumulativeMetrics.onCompletion(ReshardingOperationStatusEnum::kFailure);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics.reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kTestMetricsName).getIntField("countFailed"), 1);
    }
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsCanceledCount) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics.reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kTestMetricsName).getIntField("countCanceled"), 0);
    }

    _cumulativeMetrics.onCompletion(ReshardingOperationStatusEnum::kCanceled);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics.reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kTestMetricsName).getIntField("countCanceled"), 1);
    }
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsLastChunkImbalanceCount) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics.reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kTestMetricsName).getIntField("lastOpEndingChunkImbalance"),
                  0);
    }

    _cumulativeMetrics.setLastOpEndingChunkImbalance(111);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics.reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kTestMetricsName).getIntField("lastOpEndingChunkImbalance"),
                  111);
    }

    _cumulativeMetrics.setLastOpEndingChunkImbalance(777);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics.reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kTestMetricsName).getIntField("lastOpEndingChunkImbalance"),
                  777);
    }
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsInsertsDuringCloning) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("collectionCloningTotalLocalInserts"), 0);
    ASSERT_EQ(latencySection.getIntField("collectionCloningTotalLocalInsertTimeMillis"), 0);

    _cumulativeMetrics.onInsertsDuringCloning(140, Milliseconds(15));

    latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("collectionCloningTotalLocalInserts"), 140);
    ASSERT_EQ(latencySection.getIntField("collectionCloningTotalLocalInsertTimeMillis"), 15);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsBatchRetrievedDuringFetching) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalRemoteBatchesRetrieved"), 0);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalRemoteBatchRetrievalTimeMillis"), 0);

    _cumulativeMetrics.onRemoteBatchRetrievedDuringOplogFetching(200, Milliseconds(5));

    latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalRemoteBatchesRetrieved"), 200);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalRemoteBatchRetrievalTimeMillis"), 5);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsInsertsDuringFetching) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalLocalInserts"), 0);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalLocalInsertTimeMillis"), 0);

    _cumulativeMetrics.onLocalInsertDuringOplogFetching(Milliseconds(17));

    latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalLocalInserts"), 1);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalLocalInsertTimeMillis"), 17);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsBatchRetrievedDuringApplying) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("oplogApplyingTotalLocalBatchesRetrieved"), 0);
    ASSERT_EQ(latencySection.getIntField("oplogApplyingTotalLocalBatchRetrievalTimeMillis"), 0);

    _cumulativeMetrics.onBatchRetrievedDuringOplogApplying(707, Milliseconds(39));

    latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("oplogApplyingTotalLocalBatchesRetrieved"), 707);
    ASSERT_EQ(latencySection.getIntField("oplogApplyingTotalLocalBatchRetrievalTimeMillis"), 39);
}

class ShardingDataTransformCumulativeStateTest : public ShardingDataTransformCumulativeMetricsTest {
public:
    using CoordinatorStateEnum = ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum;

    BSONObj getStateSubObj(const ShardingDataTransformCumulativeMetrics& metrics) {
        BSONObjBuilder bob;
        metrics.reportForServerStatus(&bob);
        auto report = bob.done();
        return report.getObjectField(kTestMetricsName).getObjectField("currentInSteps").getOwned();
    }

    bool checkCoordinateStateField(const ShardingDataTransformCumulativeMetrics& metrics,
                                   boost::optional<CoordinatorStateEnum> expectedState) {
        auto serverStatusSubObj = getStateSubObj(metrics);
        std::map<std::string, int> expectedStateFieldCount;

        auto addExpectedField = [&](CoordinatorStateEnum stateToPopulate) {
            expectedStateFieldCount.emplace(
                ShardingDataTransformCumulativeMetrics::fieldNameFor(stateToPopulate),
                ((expectedState && (stateToPopulate == expectedState)) ? 1 : 0));
        };

        addExpectedField(CoordinatorStateEnum::kInitializing);
        addExpectedField(CoordinatorStateEnum::kPreparingToDonate);
        addExpectedField(CoordinatorStateEnum::kCloning);
        addExpectedField(CoordinatorStateEnum::kApplying);
        addExpectedField(CoordinatorStateEnum::kBlockingWrites);
        addExpectedField(CoordinatorStateEnum::kAborting);
        addExpectedField(CoordinatorStateEnum::kCommitting);

        for (const auto& expectedState : expectedStateFieldCount) {
            const auto actualValue = serverStatusSubObj.getIntField(expectedState.first);
            if (actualValue != expectedState.second) {
                LOGV2_DEBUG(6438600,
                            0,
                            "coordinator state field value does not match expected value",
                            "field"_attr = expectedState.first,
                            "serverStatus"_attr = serverStatusSubObj);
                return false;
            }
        }

        return true;
    }
};

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedNormalCoordinatorStateTransitionReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

    ASSERT(checkCoordinateStateField(_cumulativeMetrics, CoordinatorStateEnum::kUnused));

    boost::optional<CoordinatorStateEnum> prevState;
    boost::optional<CoordinatorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<CoordinatorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _cumulativeMetrics.onCoordinatorStateTransition(prevState, nextState);
        return checkCoordinateStateField(_cumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kUnused));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kInitializing));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kPreparingToDonate));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kCloning));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kBlockingWrites));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kCommitting));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kDone));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedAbortedCoordinatorStateTransitionReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

    ASSERT(checkCoordinateStateField(
        _cumulativeMetrics, ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum::kUnused));

    boost::optional<CoordinatorStateEnum> prevState;
    boost::optional<CoordinatorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<CoordinatorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _cumulativeMetrics.onCoordinatorStateTransition(prevState, nextState);
        return checkCoordinateStateField(_cumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kUnused));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kInitializing));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kPreparingToDonate));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kAborting));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedSteppedDownCoordinatorStateFromUnusedReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

    auto initState = CoordinatorStateEnum::kUnused;
    ASSERT(checkCoordinateStateField(_cumulativeMetrics, initState));

    _cumulativeMetrics.onCoordinatorStateTransition(initState, boost::none);
    ASSERT(checkCoordinateStateField(_cumulativeMetrics, initState));
}

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedSteppedDownCoordinatorStateTransitionReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

    ASSERT(checkCoordinateStateField(
        _cumulativeMetrics, ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum::kUnused));

    boost::optional<CoordinatorStateEnum> prevState;
    boost::optional<CoordinatorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<CoordinatorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _cumulativeMetrics.onCoordinatorStateTransition(prevState, nextState);
        return checkCoordinateStateField(_cumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kUnused));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kInitializing));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kPreparingToDonate));
    ASSERT(simulateTransitionTo(boost::none));
}

}  // namespace
}  // namespace mongo
