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

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"
#include "mongo/util/static_immortal.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

class ScopedObserverMock {
public:
    using Ptr = std::unique_ptr<ScopedObserverMock>;

    ScopedObserverMock(Date_t startTime,
                       int64_t timeRemaining,
                       ClockSource* clockSource,
                       ShardingDataTransformCumulativeMetrics* parent)
        : _mock{startTime, timeRemaining},
          _scopedOpObserver{parent->registerInstanceMetrics(&_mock)} {}

private:
    ObserverMock _mock;
    ShardingDataTransformCumulativeMetrics::UniqueScopedObserver _scopedOpObserver;
};

class ShardingDataTransformCumulativeMetricsTest : public ShardingDataTransformMetricsTestFixture {
public:
    static BSONObj getLatencySection(const ShardingDataTransformCumulativeMetrics& metrics) {
        BSONObjBuilder bob;
        metrics.reportForServerStatus(&bob);
        auto report = bob.done();
        return report.getObjectField(kTestMetricsName).getObjectField("latencies").getOwned();
    }

    static BSONObj getActiveSection(const ShardingDataTransformCumulativeMetrics& metrics) {
        BSONObjBuilder bob;
        metrics.reportForServerStatus(&bob);
        auto report = bob.done();
        return report.getObjectField(kTestMetricsName).getObjectField("active").getOwned();
    }
};

TEST_F(ShardingDataTransformCumulativeMetricsTest, AddAndRemoveMetrics) {
    auto deregister = _cumulativeMetrics.registerInstanceMetrics(getOldestObserver());
    ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 1);
    deregister.reset();
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

TEST_F(ShardingDataTransformCumulativeMetricsTest, NoServerStatusWhenNeverUsed) {
    BSONObjBuilder bob;
    _cumulativeMetrics.reportForServerStatus(&bob);
    auto report = bob.done();
    ASSERT_BSONOBJ_EQ(report, BSONObj());
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
    deregisterOldest.reset();
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
    removeOldD.reset();
    ASSERT_EQ(metrics.getObservedMetricsCount(), 3);
    ASSERT_EQ(metrics.getObservedMetricsCount(Role::kDonor), 1);
    ASSERT_EQ(metrics.getOldestOperationHighEstimateRemainingTimeMillis(Role::kDonor), 200);
    removeOldR.reset();
    ASSERT_EQ(metrics.getObservedMetricsCount(), 2);
    ASSERT_EQ(metrics.getObservedMetricsCount(Role::kRecipient), 1);
    ASSERT_EQ(metrics.getOldestOperationHighEstimateRemainingTimeMillis(Role::kRecipient), 400);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsTimeEstimates) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto recipientObserver = _cumulativeMetrics.registerInstanceMetrics(&recipient);
    auto coordinatorObserver = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

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

    _cumulativeMetrics.onSuccess();

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

    _cumulativeMetrics.onFailure();

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

    _cumulativeMetrics.onCanceled();

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

    auto activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("documentsCopied"), 0);
    ASSERT_EQ(activeSection.getIntField("bytesCopied"), 0);

    _cumulativeMetrics.onInsertsDuringCloning(140, 20763, Milliseconds(15));

    latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("collectionCloningTotalLocalInserts"), 1);
    ASSERT_EQ(latencySection.getIntField("collectionCloningTotalLocalInsertTimeMillis"), 15);

    activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("documentsCopied"), 140);
    ASSERT_EQ(activeSection.getIntField("bytesCopied"), 20763);
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

    _cumulativeMetrics.onBatchRetrievedDuringOplogApplying(Milliseconds(39));

    latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("oplogApplyingTotalLocalBatchesRetrieved"), 1);
    ASSERT_EQ(latencySection.getIntField("oplogApplyingTotalLocalBatchRetrievalTimeMillis"), 39);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsReadDuringCriticalSection) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&donor);

    auto activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("countReadsDuringCriticalSection"), 0);

    _cumulativeMetrics.onReadDuringCriticalSection();

    activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("countReadsDuringCriticalSection"), 1);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsWriteDuringCriticalSection) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&donor);

    auto activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("countWritesDuringCriticalSection"), 0);

    _cumulativeMetrics.onWriteDuringCriticalSection();

    activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("countWritesDuringCriticalSection"), 1);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsWriteToStashedCollection) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("countWritesToStashCollections"), 0);

    _cumulativeMetrics.onWriteToStashedCollections();

    activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("countWritesToStashCollections"), 1);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsBatchRetrievedDuringCloning) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("collectionCloningTotalRemoteBatchesRetrieved"), 0);
    ASSERT_EQ(latencySection.getIntField("collectionCloningTotalRemoteBatchRetrievalTimeMillis"),
              0);

    _cumulativeMetrics.onCloningTotalRemoteBatchRetrieval(Milliseconds(19));

    latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("collectionCloningTotalRemoteBatchesRetrieved"), 1);
    ASSERT_EQ(latencySection.getIntField("collectionCloningTotalRemoteBatchRetrievalTimeMillis"),
              19);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsBatchApplied) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("oplogApplyingTotalLocalBatchesApplied"), 0);
    ASSERT_EQ(latencySection.getIntField("oplogApplyingTotalLocalBatchApplyTimeMillis"), 0);

    _cumulativeMetrics.onOplogLocalBatchApplied(Milliseconds(333));

    latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("oplogApplyingTotalLocalBatchesApplied"), 1);
    ASSERT_EQ(latencySection.getIntField("oplogApplyingTotalLocalBatchApplyTimeMillis"), 333);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsInsertsApplied) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("insertsApplied"), 0);

    _cumulativeMetrics.onInsertApplied();

    activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("insertsApplied"), 1);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsUpdatesApplied) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("updatesApplied"), 0);

    _cumulativeMetrics.onUpdateApplied();

    activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("updatesApplied"), 1);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsDeletesApplied) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("deletesApplied"), 0);

    _cumulativeMetrics.onDeleteApplied();

    activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("deletesApplied"), 1);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsOplogEntriesFetched) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("oplogEntriesFetched"), 0);

    auto latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalRemoteBatchesRetrieved"), 0);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalRemoteBatchRetrievalTimeMillis"), 0);

    _cumulativeMetrics.onOplogEntriesFetched(123, Milliseconds(43));

    activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("oplogEntriesFetched"), 123);

    latencySection = getLatencySection(_cumulativeMetrics);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalRemoteBatchesRetrieved"), 1);
    ASSERT_EQ(latencySection.getIntField("oplogFetchingTotalRemoteBatchRetrievalTimeMillis"), 43);
}

TEST_F(ShardingDataTransformCumulativeMetricsTest, ReportContainsOplogEntriesApplied) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(100), 100, 100, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    auto activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("oplogEntriesApplied"), 0);

    _cumulativeMetrics.onOplogEntriesApplied(99);

    activeSection = getActiveSection(_cumulativeMetrics);
    ASSERT_EQ(activeSection.getIntField("oplogEntriesApplied"), 99);
}

class ShardingDataTransformCumulativeStateTest : public ShardingDataTransformCumulativeMetricsTest {
public:
    using CoordinatorStateEnum = ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum;
    using DonorStateEnum = ShardingDataTransformCumulativeMetrics::DonorStateEnum;
    using RecipientStateEnum = ShardingDataTransformCumulativeMetrics::RecipientStateEnum;

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

    bool checkDonorStateField(const ShardingDataTransformCumulativeMetrics& metrics,
                              boost::optional<DonorStateEnum> expectedState) {
        auto serverStatusSubObj = getStateSubObj(metrics);
        std::map<std::string, int> expectedStateFieldCount;

        auto addExpectedField = [&](DonorStateEnum stateToPopulate) {
            expectedStateFieldCount.emplace(
                ShardingDataTransformCumulativeMetrics::fieldNameFor(stateToPopulate),
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

    bool checkRecipientStateField(const ShardingDataTransformCumulativeMetrics& metrics,
                                  boost::optional<RecipientStateEnum> expectedState) {
        auto serverStatusSubObj = getStateSubObj(metrics);
        std::map<std::string, int> expectedStateFieldCount;

        auto addExpectedField = [&](RecipientStateEnum stateToPopulate) {
            expectedStateFieldCount.emplace(
                ShardingDataTransformCumulativeMetrics::fieldNameFor(stateToPopulate),
                ((expectedState && (stateToPopulate == expectedState)) ? 1 : 0));
        };

        addExpectedField(RecipientStateEnum::kAwaitingFetchTimestamp);
        addExpectedField(RecipientStateEnum::kCreatingCollection);
        addExpectedField(RecipientStateEnum::kCloning);
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
        _cumulativeMetrics.onStateTransition(prevState, nextState);
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

    ASSERT(checkCoordinateStateField(_cumulativeMetrics, CoordinatorStateEnum::kUnused));

    boost::optional<CoordinatorStateEnum> prevState;
    boost::optional<CoordinatorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<CoordinatorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _cumulativeMetrics.onStateTransition(prevState, nextState);
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

    boost::optional<CoordinatorStateEnum> initState = CoordinatorStateEnum::kUnused;
    ASSERT(checkCoordinateStateField(_cumulativeMetrics, initState));

    _cumulativeMetrics.onStateTransition(initState, {boost::none});
    ASSERT(checkCoordinateStateField(_cumulativeMetrics, initState));
}

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedSteppedDownCoordinatorStateTransitionReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock coordinator{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kCoordinator};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&coordinator);

    ASSERT(checkCoordinateStateField(_cumulativeMetrics, CoordinatorStateEnum::kUnused));

    boost::optional<CoordinatorStateEnum> prevState;
    boost::optional<CoordinatorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<CoordinatorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _cumulativeMetrics.onStateTransition(prevState, nextState);
        return checkCoordinateStateField(_cumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kUnused));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kInitializing));
    ASSERT(simulateTransitionTo(CoordinatorStateEnum::kPreparingToDonate));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedNormalDonorStateTransitionReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&donor);

    ASSERT(checkDonorStateField(_cumulativeMetrics, DonorStateEnum::kUnused));

    boost::optional<DonorStateEnum> prevState;
    boost::optional<DonorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<DonorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _cumulativeMetrics.onStateTransition(prevState, nextState);
        return checkDonorStateField(_cumulativeMetrics, nextState);
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

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedAbortedDonorStateTransitionReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&donor);

    ASSERT(checkDonorStateField(_cumulativeMetrics, DonorStateEnum::kUnused));

    boost::optional<DonorStateEnum> prevState;
    boost::optional<DonorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<DonorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _cumulativeMetrics.onStateTransition(prevState, nextState);
        return checkDonorStateField(_cumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(DonorStateEnum::kUnused));
    ASSERT(simulateTransitionTo(DonorStateEnum::kPreparingToDonate));
    ASSERT(simulateTransitionTo(DonorStateEnum::kError));
    ASSERT(simulateTransitionTo(DonorStateEnum::kDone));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedSteppedDownDonorStateFromUnusedReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&donor);

    boost::optional<DonorStateEnum> initState = DonorStateEnum::kUnused;
    ASSERT(checkDonorStateField(_cumulativeMetrics, initState));

    _cumulativeMetrics.onStateTransition(initState, {boost::none});
    ASSERT(checkDonorStateField(_cumulativeMetrics, initState));
}

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedSteppedDownDonorStateTransitionReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock donor{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kDonor};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&donor);

    ASSERT(checkDonorStateField(_cumulativeMetrics, DonorStateEnum::kUnused));

    boost::optional<DonorStateEnum> prevState;
    boost::optional<DonorStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<DonorStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _cumulativeMetrics.onStateTransition(prevState, nextState);
        return checkDonorStateField(_cumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(DonorStateEnum::kUnused));
    ASSERT(simulateTransitionTo(DonorStateEnum::kPreparingToDonate));
    ASSERT(simulateTransitionTo(DonorStateEnum::kDonatingInitialData));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedNormalRecipientStateTransitionReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    ASSERT(checkRecipientStateField(_cumulativeMetrics, RecipientStateEnum::kUnused));

    boost::optional<RecipientStateEnum> prevState;
    boost::optional<RecipientStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<RecipientStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _cumulativeMetrics.onStateTransition(prevState, nextState);
        return checkRecipientStateField(_cumulativeMetrics, nextState);
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

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedAbortedRecipientStateTransitionReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    ASSERT(checkRecipientStateField(_cumulativeMetrics, RecipientStateEnum::kUnused));

    boost::optional<RecipientStateEnum> prevState;
    boost::optional<RecipientStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<RecipientStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _cumulativeMetrics.onStateTransition(prevState, nextState);
        return checkRecipientStateField(_cumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(RecipientStateEnum::kUnused));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kAwaitingFetchTimestamp));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kError));
    ASSERT(simulateTransitionTo(boost::none));
}

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedSteppedDownRecipientStateFromUnusedReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    boost::optional<RecipientStateEnum> initState = RecipientStateEnum::kUnused;
    ASSERT(checkRecipientStateField(_cumulativeMetrics, initState));

    _cumulativeMetrics.onStateTransition(initState, {boost::none});
    ASSERT(checkRecipientStateField(_cumulativeMetrics, initState));
}

TEST_F(ShardingDataTransformCumulativeStateTest,
       SimulatedSteppedDownRecipientStateTransitionReportsStateCorrectly) {
    using Role = ShardingDataTransformMetrics::Role;
    ObserverMock recipient{Date_t::fromMillisSinceEpoch(200), 400, 300, Role::kRecipient};
    auto ignore = _cumulativeMetrics.registerInstanceMetrics(&recipient);

    ASSERT(checkRecipientStateField(_cumulativeMetrics, RecipientStateEnum::kUnused));

    boost::optional<RecipientStateEnum> prevState;
    boost::optional<RecipientStateEnum> nextState;

    auto simulateTransitionTo = [&](boost::optional<RecipientStateEnum> newState) {
        prevState = nextState;
        nextState = newState;
        _cumulativeMetrics.onStateTransition(prevState, nextState);
        return checkRecipientStateField(_cumulativeMetrics, nextState);
    };

    ASSERT(simulateTransitionTo(RecipientStateEnum::kUnused));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kAwaitingFetchTimestamp));
    ASSERT(simulateTransitionTo(RecipientStateEnum::kCreatingCollection));
    ASSERT(simulateTransitionTo(boost::none));
}

}  // namespace
}  // namespace mongo
