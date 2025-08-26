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


#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/db/s/resharding/resharding_metrics_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

constexpr auto kResharding = "resharding";

class ReshardingCumulativeMetricsTest : public ReshardingMetricsTestFixture {
protected:
    void setUp() override {
        ReshardingMetricsTestFixture::setUp();
        _reshardingCumulativeMetrics =
            static_cast<ReshardingCumulativeMetrics*>(_cumulativeMetrics.get());
    }

    StringData getRootSectionName() override {
        return kResharding;
    }

    template <typename T>
    StringData fieldNameFor(T state) {
        auto maybeFieldName = ReshardingCumulativeMetrics::fieldNameFor(state);
        invariant(maybeFieldName.has_value());
        return *maybeFieldName;
    }

    BSONObj getStateSubObj(const ReshardingCumulativeMetrics* metrics) {
        BSONObjBuilder bob;
        metrics->reportForServerStatus(&bob);
        auto report = bob.done();
        return report.getObjectField(kResharding).getObjectField("currentInSteps").getOwned();
    }

    bool checkCoordinateStateField(const ReshardingCumulativeMetrics* metrics,
                                   boost::optional<CoordinatorStateEnum> expectedState) {
        auto serverStatusSubObj = getStateSubObj(metrics);
        std::map<std::string, int> expectedStateFieldCount;

        auto addExpectedField = [&](CoordinatorStateEnum stateToPopulate) {
            expectedStateFieldCount.emplace(
                fieldNameFor(stateToPopulate),
                ((expectedState && (stateToPopulate == expectedState)) ? 1 : 0));
        };

        addExpectedField(CoordinatorStateEnum::kInitializing);
        addExpectedField(CoordinatorStateEnum::kPreparingToDonate);
        addExpectedField(CoordinatorStateEnum::kCloning);
        addExpectedField(CoordinatorStateEnum::kApplying);
        addExpectedField(CoordinatorStateEnum::kBlockingWrites);
        addExpectedField(CoordinatorStateEnum::kAborting);
        addExpectedField(CoordinatorStateEnum::kCommitting);

        for (const auto& fieldNameAndState : expectedStateFieldCount) {
            const auto actualValue = serverStatusSubObj.getIntField(fieldNameAndState.first);
            if (actualValue != fieldNameAndState.second) {
                LOGV2_DEBUG(6438600,
                            0,
                            "coordinator state field value does not match expected value",
                            "field"_attr = fieldNameAndState.first,
                            "serverStatus"_attr = serverStatusSubObj);
                return false;
            }
        }

        return true;
    }

    bool checkDonorStateField(const ReshardingCumulativeMetrics* metrics,
                              boost::optional<DonorStateEnum> expectedState) {
        auto serverStatusSubObj = getStateSubObj(metrics);
        std::map<std::string, int> expectedStateFieldCount;

        auto addExpectedField = [&](DonorStateEnum stateToPopulate) {
            expectedStateFieldCount.emplace(
                fieldNameFor(stateToPopulate),
                ((expectedState && (stateToPopulate == expectedState)) ? 1 : 0));
        };

        addExpectedField(DonorStateEnum::kPreparingToDonate);
        addExpectedField(DonorStateEnum::kDonatingInitialData);
        addExpectedField(DonorStateEnum::kDonatingOplogEntries);
        addExpectedField(DonorStateEnum::kPreparingToBlockWrites);
        addExpectedField(DonorStateEnum::kError);
        addExpectedField(DonorStateEnum::kBlockingWrites);
        addExpectedField(DonorStateEnum::kDone);

        for (const auto& fieldNameAndState : expectedStateFieldCount) {
            const auto actualValue = serverStatusSubObj.getIntField(fieldNameAndState.first);
            if (actualValue != fieldNameAndState.second) {
                LOGV2_DEBUG(6438701,
                            0,
                            "Donor state field value does not match expected value",
                            "field"_attr = fieldNameAndState.first,
                            "serverStatus"_attr = serverStatusSubObj);
                return false;
            }
        }

        return true;
    }

    bool checkRecipientStateField(const ReshardingCumulativeMetrics* metrics,
                                  boost::optional<RecipientStateEnum> expectedState) {
        auto serverStatusSubObj = getStateSubObj(metrics);
        std::map<std::string, int> expectedStateFieldCount;

        auto addExpectedField = [&](RecipientStateEnum stateToPopulate) {
            expectedStateFieldCount.emplace(
                fieldNameFor(stateToPopulate),
                ((expectedState && (stateToPopulate == expectedState)) ? 1 : 0));
        };

        addExpectedField(RecipientStateEnum::kAwaitingFetchTimestamp);
        addExpectedField(RecipientStateEnum::kCreatingCollection);
        addExpectedField(RecipientStateEnum::kCloning);
        addExpectedField(RecipientStateEnum::kBuildingIndex);
        addExpectedField(RecipientStateEnum::kApplying);
        addExpectedField(RecipientStateEnum::kError);
        addExpectedField(RecipientStateEnum::kStrictConsistency);
        addExpectedField(RecipientStateEnum::kDone);

        for (const auto& fieldNameAndState : expectedStateFieldCount) {
            const auto actualValue = serverStatusSubObj.getIntField(fieldNameAndState.first);
            if (actualValue != fieldNameAndState.second) {
                LOGV2_DEBUG(6438901,
                            0,
                            "Recipient state field value does not match expected value",
                            "field"_attr = fieldNameAndState.first,
                            "serverStatus"_attr = serverStatusSubObj);
                return false;
            }
        }

        return true;
    }

    ReshardingCumulativeMetrics* _reshardingCumulativeMetrics;
};

class ScopedObserverMock {
public:
    using Ptr = std::unique_ptr<ScopedObserverMock>;

    ScopedObserverMock(Date_t startTime,
                       int64_t timeRemaining,
                       ClockSource* clockSource,
                       ReshardingCumulativeMetrics* parent)
        : _mock{startTime, timeRemaining},
          _scopedOpObserver{parent->registerInstanceMetrics(&_mock)} {}

private:
    ObserverMock _mock;
    ReshardingCumulativeMetrics::UniqueScopedObserver _scopedOpObserver;
};


TEST_F(ReshardingCumulativeMetricsTest, ReportContainsInsertsDuringFetching) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    auto latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("oplogFetchingTotalLocalInserts"), 0);
    ASSERT_EQ(latencies.getIntField("oplogFetchingTotalLocalInsertTimeMillis"), 0);

    _reshardingCumulativeMetrics->onLocalInsertDuringOplogFetching(Milliseconds(17));

    latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("oplogFetchingTotalLocalInserts"), 1);
    ASSERT_EQ(latencies.getIntField("oplogFetchingTotalLocalInsertTimeMillis"), 17);
}


TEST_F(ReshardingCumulativeMetricsTest, ReportContainsBatchRetrievedDuringApplying) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    auto latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("oplogApplyingTotalLocalBatchesRetrieved"), 0);
    ASSERT_EQ(latencies.getIntField("oplogApplyingTotalLocalBatchRetrievalTimeMillis"), 0);

    _reshardingCumulativeMetrics->onBatchRetrievedDuringOplogApplying(Milliseconds(39));

    latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("oplogApplyingTotalLocalBatchesRetrieved"), 1);
    ASSERT_EQ(latencies.getIntField("oplogApplyingTotalLocalBatchRetrievalTimeMillis"), 39);
}


TEST_F(ReshardingCumulativeMetricsTest, ReportContainsBatchApplied) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    auto latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("oplogApplyingTotalLocalBatchesApplied"), 0);
    ASSERT_EQ(latencies.getIntField("oplogApplyingTotalLocalBatchApplyTimeMillis"), 0);

    _reshardingCumulativeMetrics->onOplogLocalBatchApplied(Milliseconds(333));

    latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("oplogApplyingTotalLocalBatchesApplied"), 1);
    ASSERT_EQ(latencies.getIntField("oplogApplyingTotalLocalBatchApplyTimeMillis"), 333);
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsInsertsApplied) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    auto active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("insertsApplied"), 0);

    _reshardingCumulativeMetrics->onInsertApplied();

    active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("insertsApplied"), 1);
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsUpdatesApplied) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    auto active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("updatesApplied"), 0);

    _reshardingCumulativeMetrics->onUpdateApplied();

    active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("updatesApplied"), 1);
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsDeletesApplied) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    auto active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("deletesApplied"), 0);

    _reshardingCumulativeMetrics->onDeleteApplied();

    active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("deletesApplied"), 1);
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsOplogEntriesFetched) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    auto active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("oplogEntriesFetched"), 0);

    auto latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("oplogFetchingTotalRemoteBatchesRetrieved"), 0);
    ASSERT_EQ(latencies.getIntField("oplogFetchingTotalRemoteBatchRetrievalTimeMillis"), 0);

    _reshardingCumulativeMetrics->onOplogEntriesFetched(123);
    _reshardingCumulativeMetrics->onBatchRetrievedDuringOplogFetching(Milliseconds(43));

    active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("oplogEntriesFetched"), 123);

    latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("oplogFetchingTotalRemoteBatchesRetrieved"), 1);
    ASSERT_EQ(latencies.getIntField("oplogFetchingTotalRemoteBatchRetrievalTimeMillis"), 43);
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsOplogEntriesApplied) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    auto active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("oplogEntriesApplied"), 0);

    _reshardingCumulativeMetrics->onOplogEntriesApplied(99);

    active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("oplogEntriesApplied"), 99);
}

TEST_F(ReshardingCumulativeMetricsTest,
       SimulatedNormalCoordinatorStateTransitionReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&coordinator);

    ASSERT(checkCoordinateStateField(_reshardingCumulativeMetrics, CoordinatorStateEnum::kUnused));

    boost::optional<CoordinatorStateEnum> prevState;
    boost::optional<CoordinatorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<CoordinatorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _reshardingCumulativeMetrics->onStateTransition(prevState, nextState);
        return checkCoordinateStateField(_reshardingCumulativeMetrics, nextState);
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

TEST_F(ReshardingCumulativeMetricsTest,
       SimulatedAbortedCoordinatorStateTransitionReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&coordinator);

    ASSERT(checkCoordinateStateField(_reshardingCumulativeMetrics, CoordinatorStateEnum::kUnused));

    boost::optional<CoordinatorStateEnum> prevState;
    boost::optional<CoordinatorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<CoordinatorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _reshardingCumulativeMetrics->onStateTransition(prevState, nextState);
        return checkCoordinateStateField(_reshardingCumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kUnused));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kInitializing));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kPreparingToDonate));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kAborting));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ReshardingCumulativeMetricsTest,
       SimulatedSteppedDownCoordinatorStateFromUnusedReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&coordinator);

    boost::optional<CoordinatorStateEnum> initState = CoordinatorStateEnum::kUnused;
    ASSERT(checkCoordinateStateField(_reshardingCumulativeMetrics, initState));

    _reshardingCumulativeMetrics->onStateTransition(initState, {boost::none});
    ASSERT(checkCoordinateStateField(_reshardingCumulativeMetrics, initState));
}

TEST_F(ReshardingCumulativeMetricsTest,
       SimulatedSteppedDownCoordinatorStateTransitionReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&coordinator);

    ASSERT(checkCoordinateStateField(_reshardingCumulativeMetrics, CoordinatorStateEnum::kUnused));

    boost::optional<CoordinatorStateEnum> prevState;
    boost::optional<CoordinatorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<CoordinatorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _reshardingCumulativeMetrics->onStateTransition(prevState, nextState);
        return checkCoordinateStateField(_reshardingCumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kUnused));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kInitializing));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kPreparingToDonate));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ReshardingCumulativeMetricsTest, SimulatedNormalDonorStateTransitionReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&donor);

    ASSERT(checkDonorStateField(_reshardingCumulativeMetrics, DonorStateEnum::kUnused));

    boost::optional<DonorStateEnum> prevState;
    boost::optional<DonorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<DonorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _reshardingCumulativeMetrics->onStateTransition(prevState, nextState);
        return checkDonorStateField(_reshardingCumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(DonorStateEnum::kUnused));
    ASSERT(simulateTransitionTo(DonorStateEnum::kPreparingToDonate));
    ASSERT(simulateTransitionTo(DonorStateEnum::kDonatingInitialData));
    ASSERT(simulateTransitionTo(DonorStateEnum::kDonatingOplogEntries));
    ASSERT(simulateTransitionTo(DonorStateEnum::kPreparingToBlockWrites));
    ASSERT(simulateTransitionTo(DonorStateEnum::kBlockingWrites));
    ASSERT(simulateTransitionTo(DonorStateEnum::kDone));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ReshardingCumulativeMetricsTest, SimulatedAbortedDonorStateTransitionReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&donor);

    ASSERT(checkDonorStateField(_reshardingCumulativeMetrics, DonorStateEnum::kUnused));

    boost::optional<DonorStateEnum> prevState;
    boost::optional<DonorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<DonorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _reshardingCumulativeMetrics->onStateTransition(prevState, nextState);
        return checkDonorStateField(_reshardingCumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(DonorStateEnum::kUnused));
    ASSERT(simulateTransitionTo(DonorStateEnum::kPreparingToDonate));
    ASSERT(simulateTransitionTo(DonorStateEnum::kError));
    ASSERT(simulateTransitionTo(DonorStateEnum::kDone));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ReshardingCumulativeMetricsTest,
       SimulatedSteppedDownDonorStateFromUnusedReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&donor);

    boost::optional<DonorStateEnum> initState = DonorStateEnum::kUnused;
    ASSERT(checkDonorStateField(_reshardingCumulativeMetrics, initState));

    _reshardingCumulativeMetrics->onStateTransition(initState, {boost::none});
    ASSERT(checkDonorStateField(_reshardingCumulativeMetrics, initState));
}

TEST_F(ReshardingCumulativeMetricsTest,
       SimulatedSteppedDownDonorStateTransitionReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&donor);

    ASSERT(checkDonorStateField(_reshardingCumulativeMetrics, DonorStateEnum::kUnused));

    boost::optional<DonorStateEnum> prevState;
    boost::optional<DonorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<DonorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _reshardingCumulativeMetrics->onStateTransition(prevState, nextState);
        return checkDonorStateField(_reshardingCumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(DonorStateEnum::kUnused));
    ASSERT(simulateTransitionTo(DonorStateEnum::kPreparingToDonate));
    ASSERT(simulateTransitionTo(DonorStateEnum::kDonatingInitialData));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ReshardingCumulativeMetricsTest,
       SimulatedNormalRecipientStateTransitionReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    ASSERT(checkRecipientStateField(_reshardingCumulativeMetrics, RecipientStateEnum::kUnused));

    boost::optional<RecipientStateEnum> prevState;
    boost::optional<RecipientStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<RecipientStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _reshardingCumulativeMetrics->onStateTransition(prevState, nextState);
        return checkRecipientStateField(_reshardingCumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(RecipientStateEnum::kUnused));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kAwaitingFetchTimestamp));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kCreatingCollection));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kCloning));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kApplying));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kStrictConsistency));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kDone));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ReshardingCumulativeMetricsTest,
       SimulatedAbortedRecipientStateTransitionReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    ASSERT(checkRecipientStateField(_reshardingCumulativeMetrics, RecipientStateEnum::kUnused));

    boost::optional<RecipientStateEnum> prevState;
    boost::optional<RecipientStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<RecipientStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _reshardingCumulativeMetrics->onStateTransition(prevState, nextState);
        return checkRecipientStateField(_reshardingCumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(RecipientStateEnum::kUnused));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kAwaitingFetchTimestamp));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kError));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ReshardingCumulativeMetricsTest,
       SimulatedSteppedDownRecipientStateFromUnusedReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    boost::optional<RecipientStateEnum> initState = RecipientStateEnum::kUnused;
    ASSERT(checkRecipientStateField(_reshardingCumulativeMetrics, initState));

    _reshardingCumulativeMetrics->onStateTransition(initState, {boost::none});
    ASSERT(checkRecipientStateField(_reshardingCumulativeMetrics, initState));
}

TEST_F(ReshardingCumulativeMetricsTest,
       SimulatedSteppedDownRecipientStateTransitionReportsStateCorrectly) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kRecipient};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&recipient);

    ASSERT(checkRecipientStateField(_reshardingCumulativeMetrics, RecipientStateEnum::kUnused));

    boost::optional<RecipientStateEnum> prevState;
    boost::optional<RecipientStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<RecipientStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _reshardingCumulativeMetrics->onStateTransition(prevState, nextState);
        return checkRecipientStateField(_reshardingCumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(RecipientStateEnum::kUnused));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kAwaitingFetchTimestamp));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kCreatingCollection));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsRunCount) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countStarted"), 0);
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSameKeyStarted"), 0);
    }

    auto uuidOne = UUID::gen();
    auto uuidTwo = UUID::gen();

    _reshardingCumulativeMetrics->onStarted(false /*isSameKeyResharding*/, uuidOne);
    _reshardingCumulativeMetrics->onStarted(true /*isSameKeyResharding*/, uuidTwo);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countStarted"), 2);
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSameKeyStarted"), 1);
    }
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsSucceededCount) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics->registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSucceeded"), 0);
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSameKeySucceeded"), 0);
    }

    auto uuidOne = UUID::gen();
    auto uuidTwo = UUID::gen();

    _reshardingCumulativeMetrics->onStarted(false, uuidOne);
    _reshardingCumulativeMetrics->onStarted(true, uuidTwo);

    _reshardingCumulativeMetrics->onSuccess(false /*isSameKeyResharding*/, uuidOne);
    _reshardingCumulativeMetrics->onSuccess(true /*isSameKeyResharding*/, uuidTwo);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSucceeded"), 2);
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSameKeySucceeded"), 1);
    }
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsFailedCount) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countFailed"), 0);
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSameKeyFailed"), 0);
    }

    auto uuidOne = UUID::gen();
    auto uuidTwo = UUID::gen();

    _reshardingCumulativeMetrics->onStarted(false, uuidOne);
    _reshardingCumulativeMetrics->onStarted(true, uuidTwo);

    _reshardingCumulativeMetrics->onFailure(false /*isSameKeyResharding*/, uuidOne);
    _reshardingCumulativeMetrics->onFailure(true /*isSameKeyResharding*/, uuidTwo);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countFailed"), 2);
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSameKeyFailed"), 1);
    }
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsCanceledCount) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countCanceled"), 0);
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSameKeyCanceled"), 0);
    }

    auto uuidOne = UUID::gen();
    auto uuidTwo = UUID::gen();

    _reshardingCumulativeMetrics->onStarted(false, uuidOne);
    _reshardingCumulativeMetrics->onStarted(true, uuidTwo);

    _reshardingCumulativeMetrics->onCanceled(false /*isSameKeyResharding*/, uuidOne);
    _reshardingCumulativeMetrics->onCanceled(true /*isSameKeyResharding*/, uuidTwo);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countCanceled"), 2);
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSameKeyCanceled"), 1);
    }
}

TEST_F(ReshardingCumulativeMetricsTest, RepeatedCallsToOnStartedDoesNotIncrementCount) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countStarted"), 0);
    }

    auto uuid = UUID::gen();

    _reshardingCumulativeMetrics->onStarted(true /*isSameKeyResharding*/, uuid);
    _reshardingCumulativeMetrics->onStarted(true /*isSameKeyResharding*/, uuid);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countStarted"), 1);
    }
}

TEST_F(ReshardingCumulativeMetricsTest, RepeatedCallsToOnFailedDoesNotIncrementCount) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countFailed"), 0);
    }

    auto uuid = UUID::gen();

    _reshardingCumulativeMetrics->onStarted(true, uuid);

    _reshardingCumulativeMetrics->onFailure(true /*isSameKeyResharding*/, uuid);
    _reshardingCumulativeMetrics->onFailure(true /*isSameKeyResharding*/, uuid);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countFailed"), 1);
    }
}

TEST_F(ReshardingCumulativeMetricsTest, RepeatedCallsToOnSuccessDoesNotIncrementCount) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSucceeded"), 0);
    }

    auto uuid = UUID::gen();

    _reshardingCumulativeMetrics->onStarted(true, uuid);

    _reshardingCumulativeMetrics->onSuccess(true /*isSameKeyResharding*/, uuid);
    _reshardingCumulativeMetrics->onSuccess(true /*isSameKeyResharding*/, uuid);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSucceeded"), 1);
    }
}

TEST_F(ReshardingCumulativeMetricsTest, RepeatedCallsToOnCanceledDoesNotIncrementCount) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _reshardingCumulativeMetrics->registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countCanceled"), 0);
    }

    auto uuid = UUID::gen();

    _reshardingCumulativeMetrics->onStarted(true, uuid);

    _reshardingCumulativeMetrics->onCanceled(true /*isSameKeyResharding*/, uuid);
    _reshardingCumulativeMetrics->onCanceled(true /*isSameKeyResharding*/, uuid);

    {
        BSONObjBuilder bob;
        _reshardingCumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("countCanceled"), 1);
    }
}

TEST_F(ReshardingCumulativeMetricsTest, AddAndRemoveMetrics) {
    auto deregister = _cumulativeMetrics->registerInstanceMetrics(getOldestObserver());
    ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), 1);
    deregister.reset();
    ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), 0);
}

TEST_F(ReshardingCumulativeMetricsTest, MetricsReportsOldestWhenInsertedFirst) {
    auto deregisterOldest = _cumulativeMetrics->registerInstanceMetrics(getOldestObserver());
    auto deregisterYoungest = _cumulativeMetrics->registerInstanceMetrics(getYoungestObserver());
    ASSERT_EQ(_cumulativeMetrics->getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              kOldestTimeLeft);
}

TEST_F(ReshardingCumulativeMetricsTest, MetricsReportsOldestWhenInsertedLast) {
    auto deregisterYoungest = _cumulativeMetrics->registerInstanceMetrics(getYoungestObserver());
    auto deregisterOldest = _cumulativeMetrics->registerInstanceMetrics(getOldestObserver());
    ASSERT_EQ(_cumulativeMetrics->getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              kOldestTimeLeft);
}

TEST_F(ReshardingCumulativeMetricsTest, NoServerStatusWhenNeverUsed) {
    BSONObjBuilder bob;
    _cumulativeMetrics->reportForServerStatus(&bob);
    auto report = bob.done();
    ASSERT_BSONOBJ_EQ(report, BSONObj());
}

TEST_F(ReshardingCumulativeMetricsTest, RemainingTimeReportsMinusOneWhenEmpty) {
    ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), 0);
    ASSERT_EQ(_cumulativeMetrics->getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              -1);
}

TEST_F(ReshardingCumulativeMetricsTest, UpdatesOldestWhenOldestIsRemoved) {
    auto deregisterYoungest = _cumulativeMetrics->registerInstanceMetrics(getYoungestObserver());
    auto deregisterOldest = _cumulativeMetrics->registerInstanceMetrics(getOldestObserver());
    ASSERT_EQ(_cumulativeMetrics->getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              kOldestTimeLeft);
    deregisterOldest.reset();
    ASSERT_EQ(_cumulativeMetrics->getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              kYoungestTimeLeft);
}

TEST_F(ReshardingCumulativeMetricsTest, InsertsTwoWithSameStartTime) {
    auto deregisterOldest = _cumulativeMetrics->registerInstanceMetrics(getOldestObserver());
    ObserverMock sameAsOldest{kOldestTime, kOldestTimeLeft};
    auto deregisterOldest2 = _cumulativeMetrics->registerInstanceMetrics(&sameAsOldest);
    ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), 2);
    ASSERT_EQ(_cumulativeMetrics->getOldestOperationHighEstimateRemainingTimeMillis(
                  ObserverMock::kDefaultRole),
              kOldestTimeLeft);
}

TEST_F(ReshardingCumulativeMetricsTest, StillReportsOldestAfterRandomOperations) {
    doRandomOperationsTest<ScopedObserverMock>();
}

TEST_F(ReshardingCumulativeMetricsTest, StillReportsOldestAfterRandomOperationsMultithreaded) {
    doRandomOperationsMultithreadedTest<ScopedObserverMock>();
}

TEST_F(ReshardingCumulativeMetricsTest, ReportsOldestByRole) {
    using Role = ReshardingMetricsCommon::Role;
    auto& metrics = _cumulativeMetrics;
    ObserverMock oldDonor{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kDonor};
    ObserverMock youngDonor{Date_t::fromMillisSinceEpoch(200), 200, 200, Role::kDonor};
    ObserverMock oldRecipient{Date_t::fromMillisSinceEpoch(300), 300, 300, Role::kRecipient};
    ObserverMock youngRecipient{Date_t::fromMillisSinceEpoch(400), 400, 400, Role::kRecipient};
    auto removeOldD = metrics->registerInstanceMetrics(&oldDonor);
    auto removeYoungD = metrics->registerInstanceMetrics(&youngDonor);
    auto removeOldR = metrics->registerInstanceMetrics(&oldRecipient);
    auto removeYoungR = metrics->registerInstanceMetrics(&youngRecipient);

    ASSERT_EQ(metrics->getObservedMetricsCount(), 4);
    ASSERT_EQ(metrics->getObservedMetricsCount(Role::kDonor), 2);
    ASSERT_EQ(metrics->getObservedMetricsCount(Role::kRecipient), 2);
    ASSERT_EQ(metrics->getOldestOperationHighEstimateRemainingTimeMillis(Role::kDonor), 100);
    ASSERT_EQ(metrics->getOldestOperationHighEstimateRemainingTimeMillis(Role::kRecipient), 300);
    removeOldD.reset();
    ASSERT_EQ(metrics->getObservedMetricsCount(), 3);
    ASSERT_EQ(metrics->getObservedMetricsCount(Role::kDonor), 1);
    ASSERT_EQ(metrics->getOldestOperationHighEstimateRemainingTimeMillis(Role::kDonor), 200);
    removeOldR.reset();
    ASSERT_EQ(metrics->getObservedMetricsCount(), 2);
    ASSERT_EQ(metrics->getObservedMetricsCount(Role::kRecipient), 1);
    ASSERT_EQ(metrics->getOldestOperationHighEstimateRemainingTimeMillis(Role::kRecipient), 400);
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsTimeEstimates) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto recipientObserver = _cumulativeMetrics->registerInstanceMetrics(&recipient);
    auto coordinatorObserver = _cumulativeMetrics->registerInstanceMetrics(&coordinator);

    BSONObjBuilder bob;
    _cumulativeMetrics->reportForServerStatus(&bob);
    auto report = bob.done();
    auto section = report.getObjectField(kResharding).getObjectField("oldestActive");
    ASSERT_EQ(section.getIntField("recipientRemainingOperationTimeEstimatedMillis"), 100);
    ASSERT_EQ(
        section.getIntField("coordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis"),
        400);
    ASSERT_EQ(
        section.getIntField("coordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis"),
        300);
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsLastChunkImbalanceCount) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics->registerInstanceMetrics(&coordinator);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("lastOpEndingChunkImbalance"), 0);
    }

    _cumulativeMetrics->setLastOpEndingChunkImbalance(111);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("lastOpEndingChunkImbalance"),
                  111);
    }

    _cumulativeMetrics->setLastOpEndingChunkImbalance(777);

    {
        BSONObjBuilder bob;
        _cumulativeMetrics->reportForServerStatus(&bob);
        auto report = bob.done();
        ASSERT_EQ(report.getObjectField(kResharding).getIntField("lastOpEndingChunkImbalance"),
                  777);
    }
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsInsertsDuringCloning) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics->registerInstanceMetrics(&recipient);

    auto latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("collectionCloningTotalLocalInserts"), 0);
    ASSERT_EQ(latencies.getIntField("collectionCloningTotalLocalInsertTimeMillis"), 0);

    auto active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("documentsCopied"), 0);
    ASSERT_EQ(active.getIntField("bytesCopied"), 0);

    _cumulativeMetrics->onInsertsDuringCloning(140, 20763, Milliseconds(15));

    latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("collectionCloningTotalLocalInserts"), 1);
    ASSERT_EQ(latencies.getIntField("collectionCloningTotalLocalInsertTimeMillis"), 15);

    active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("documentsCopied"), 140);
    ASSERT_EQ(active.getIntField("bytesCopied"), 20763);
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsReadDuringCriticalSection) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _cumulativeMetrics->registerInstanceMetrics(&donor);

    auto active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("countReadsDuringCriticalSection"), 0);

    _cumulativeMetrics->onReadDuringCriticalSection();

    active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("countReadsDuringCriticalSection"), 1);
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsWriteDuringCriticalSection) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _cumulativeMetrics->registerInstanceMetrics(&donor);

    auto active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("countWritesDuringCriticalSection"), 0);

    _cumulativeMetrics->onWriteDuringCriticalSection();

    active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("countWritesDuringCriticalSection"), 1);
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsWriteToStashedCollection) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kRecipient};
    auto ignore = _cumulativeMetrics->registerInstanceMetrics(&recipient);

    auto active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("countWritesToStashCollections"), 0);

    _cumulativeMetrics->onWriteToStashedCollections();

    active = getCumulativeMetricsReportForSection(kActive);
    ASSERT_EQ(active.getIntField("countWritesToStashCollections"), 1);
}

TEST_F(ReshardingCumulativeMetricsTest, ReportContainsBatchRetrievedDuringCloning) {
    using Role = ReshardingMetricsCommon::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics->registerInstanceMetrics(&recipient);

    auto latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("collectionCloningTotalRemoteBatchesRetrieved"), 0);
    ASSERT_EQ(latencies.getIntField("collectionCloningTotalRemoteBatchRetrievalTimeMillis"), 0);

    _cumulativeMetrics->onCloningRemoteBatchRetrieval(Milliseconds(19));

    latencies = getCumulativeMetricsReportForSection(kLatencies);
    ASSERT_EQ(latencies.getIntField("collectionCloningTotalRemoteBatchesRetrieved"), 1);
    ASSERT_EQ(latencies.getIntField("collectionCloningTotalRemoteBatchRetrievalTimeMillis"), 19);
}

}  // namespace
}  // namespace mongo
