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

#include <algorithm>
#include <fmt/format.h>
#include <functional>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/s/metrics/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/metrics/sharding_data_transform_instance_metrics.h"
#include "mongo/db/s/metrics/sharding_data_transform_metrics_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {
class ShardingDataTransformInstanceMetricsFieldNameProviderForTest
    : public ShardingDataTransformInstanceMetricsFieldNameProvider {
public:
    StringData getForDocumentsProcessed() const override {
        return kDocumentsProcessed;
    }
    StringData getForBytesWritten() const override {
        return kBytesWritten;
    }
    StringData getForApproxDocumentsToProcess() const override {
        return kApproxDocumentsToProcess;
    }
    StringData getForApproxBytesToScan() const override {
        return kApproxBytesToScan;
    }

protected:
    static constexpr auto kDocumentsProcessed = "documentsProcessed";
    static constexpr auto kBytesWritten = "bytesWritten";
    static constexpr auto kApproxDocumentsToProcess = "approxDocumentsToProcess";
    static constexpr auto kApproxBytesToScan = "approxBytesToScan";
};

class ShardingDataTransformInstanceMetricsForTest : public ShardingDataTransformInstanceMetrics {
public:
    ShardingDataTransformInstanceMetricsForTest(
        UUID instanceId,
        BSONObj shardKey,
        NamespaceString nss,
        Role role,
        Date_t startTime,
        ClockSource* clockSource,
        ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
        : ShardingDataTransformInstanceMetrics{std::move(instanceId),
                                               std::move(shardKey),
                                               std::move(nss),
                                               role,
                                               startTime,
                                               clockSource,
                                               cumulativeMetrics,
                                               std::make_unique<
                                                   ShardingDataTransformInstanceMetricsFieldNameProviderForTest>()},
          _scopedObserver(registerInstanceMetrics()) {}
    ShardingDataTransformInstanceMetricsForTest(
        UUID instanceId,
        BSONObj shardKey,
        NamespaceString nss,
        Role role,
        Date_t startTime,
        ClockSource* clockSource,
        ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
        FieldNameProviderPtr fieldNameProvider,
        ObserverPtr observer)
        : ShardingDataTransformInstanceMetrics{std::move(instanceId),
                                               std::move(shardKey),
                                               std::move(nss),
                                               role,
                                               startTime,
                                               clockSource,
                                               cumulativeMetrics,
                                               std::move(fieldNameProvider),
                                               std::move(observer)},
          _scopedObserver(registerInstanceMetrics()) {}

    boost::optional<Milliseconds> getRecipientHighEstimateRemainingTimeMillis() const {
        return boost::none;
    }

private:
    ShardingDataTransformInstanceMetrics::UniqueScopedObserver _scopedObserver;
};

class InstanceMetricsWithObserverMock {
public:
    InstanceMetricsWithObserverMock(Date_t startTime,
                                    int64_t timeRemaining,
                                    ClockSource* clockSource,
                                    ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
        : _impl{UUID::gen(),
                BSON("command"
                     << "test"),
                NamespaceString::createNamespaceString_forTest("test.source"),
                ShardingDataTransformInstanceMetrics::Role::kDonor,
                startTime,
                clockSource,
                cumulativeMetrics,
                std::make_unique<ShardingDataTransformInstanceMetricsFieldNameProviderForTest>(),
                std::make_unique<ObserverMock>(startTime, timeRemaining)} {}


private:
    ShardingDataTransformInstanceMetricsForTest _impl;
};

class ShardingDataTransformInstanceMetricsTest : public ShardingDataTransformMetricsTestFixture {
public:
    std::unique_ptr<ShardingDataTransformInstanceMetricsForTest> createInstanceMetrics(
        UUID instanceId = UUID::gen(), Role role = Role::kDonor) {
        return std::make_unique<ShardingDataTransformInstanceMetricsForTest>(
            instanceId,
            kTestCommand,
            kTestNamespace,
            role,
            getClockSource()->now(),
            getClockSource(),
            _cumulativeMetrics.get());
    }

    std::unique_ptr<ShardingDataTransformInstanceMetricsForTest> createInstanceMetrics(
        std::unique_ptr<ObserverMock> mock,
        std::unique_ptr<ShardingDataTransformInstanceMetricsFieldNameProviderForTest>
            fieldNameProvider) {
        return std::make_unique<ShardingDataTransformInstanceMetricsForTest>(
            UUID::gen(),
            kTestCommand,
            kTestNamespace,
            ShardingDataTransformInstanceMetrics::Role::kDonor,
            getClockSource()->now(),
            getClockSource(),
            _cumulativeMetrics.get(),
            std::move(fieldNameProvider),
            std::move(mock));
    }

    void createMetricsAndAssertIncrementsCumulativeMetricsField(
        const std::function<void(ShardingDataTransformInstanceMetrics*)>& mutate,
        Section section,
        StringData fieldName) {
        auto metrics = createInstanceMetrics(UUID::gen(), Role::kCoordinator);
        assertIncrementsCumulativeMetricsField(metrics.get(), mutate, section, fieldName);
    }
};

TEST_F(ShardingDataTransformInstanceMetricsTest, RegisterAndDeregisterMetrics) {
    for (auto i = 0; i < 100; i++) {
        auto metrics = createInstanceMetrics();
        ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), 1);
    }
    ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), 0);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RegisterAndDeregisterMetricsAtOnce) {
    {
        std::vector<std::unique_ptr<ShardingDataTransformInstanceMetricsForTest>> registered;
        for (auto i = 0; i < 100; i++) {
            registered.emplace_back(createInstanceMetrics());
            ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), registered.size());
        }
    }
    ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), 0);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RandomOperations) {
    doRandomOperationsTest<InstanceMetricsWithObserverMock>();
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RandomOperationsMultithreaded) {
    doRandomOperationsMultithreadedTest<InstanceMetricsWithObserverMock>();
}

TEST_F(ShardingDataTransformInstanceMetricsTest, ReportForCurrentOpShouldHaveGenericDescription) {
    std::vector<Role> roles{Role::kCoordinator, Role::kDonor, Role::kRecipient};

    std::for_each(roles.begin(), roles.end(), [&](auto role) {
        auto instanceId = UUID::gen();
        auto metrics = createInstanceMetrics(instanceId, role);
        auto report = metrics->reportForCurrentOp();
        ASSERT_EQ(report.getStringField("desc").toString(),
                  fmt::format("ShardingDataTransformMetrics{}Service {}",
                              ShardingDataTransformMetrics::getRoleName(role),
                              instanceId.toString()));
    });
}

TEST_F(ShardingDataTransformInstanceMetricsTest, GetRoleNameShouldReturnCorrectName) {
    std::vector<std::pair<Role, std::string>> roles{
        {Role::kCoordinator, "Coordinator"},
        {Role::kDonor, "Donor"},
        {Role::kRecipient, "Recipient"},
    };

    std::for_each(roles.begin(), roles.end(), [&](auto role) {
        ASSERT_EQ(ShardingDataTransformMetrics::getRoleName(role.first), role.second);
    });
}

TEST_F(ShardingDataTransformInstanceMetricsTest, DonorIncrementWritesDuringCriticalSection) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kDonor);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countWritesDuringCriticalSection"), 0);
    metrics->onWriteDuringCriticalSection();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countWritesDuringCriticalSection"), 1);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, DonorIncrementReadsDuringCriticalSection) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kDonor);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countReadsDuringCriticalSection"), 0);
    metrics->onReadDuringCriticalSection();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countReadsDuringCriticalSection"), 1);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RecipientSetsDocumentsAndBytesToCopy) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("approxDocumentsToProcess"), 0);
    ASSERT_EQ(report.getIntField("approxBytesToScan"), 0);
    metrics->setDocumentsToProcessCounts(5, 1000);

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("approxDocumentsToProcess"), 5);
    ASSERT_EQ(report.getIntField("approxBytesToScan"), 1000);

    metrics->setDocumentsToProcessCounts(3, 750);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("approxDocumentsToProcess"), 3);
    ASSERT_EQ(report.getIntField("approxBytesToScan"), 750);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RecipientIncrementsDocumentsAndBytesWritten) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("documentsProcessed"), 0);
    ASSERT_EQ(report.getIntField("bytesWritten"), 0);
    metrics->onDocumentsProcessed(5, 1000, Milliseconds(1));

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("documentsProcessed"), 5);
    ASSERT_EQ(report.getIntField("bytesWritten"), 1000);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, CurrentOpReportsRunningTime) {
    auto uuid = UUID::gen();
    auto now = getClockSource()->now();
    constexpr auto kTimeElapsed = 15;
    auto start = now - Seconds(kTimeElapsed);
    auto metrics =
        std::make_unique<ShardingDataTransformInstanceMetricsForTest>(uuid,
                                                                      kTestCommand,
                                                                      kTestNamespace,
                                                                      Role::kCoordinator,
                                                                      start,
                                                                      getClockSource(),
                                                                      _cumulativeMetrics.get());
    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("totalOperationTimeElapsedSecs"), kTimeElapsed);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, OnWriteToStasheddShouldIncrementCurOpFields) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countWritesToStashCollections"), 0);
    metrics->onWriteToStashedCollections();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countWritesToStashCollections"), 1);
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       SetLowestOperationTimeShouldBeReflectedInCurrentOp) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kCoordinator);
    metrics->setCoordinatorLowEstimateRemainingTimeMillis(Milliseconds(2000));
    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("allShardsLowestRemainingOperationTimeEstimatedSecs"), 2);
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       SetHighestOperationTimeShouldBeReflectedInCurrentOp) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kCoordinator);
    metrics->setCoordinatorHighEstimateRemainingTimeMillis(Milliseconds(12000));
    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("allShardsHighestRemainingOperationTimeEstimatedSecs"), 12);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, CoordinatorHighEstimateNoneIfNotSet) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kCoordinator);
    ASSERT_EQ(metrics->getHighEstimateRemainingTimeMillis(), boost::none);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, CoordinatorLowEstimateNoneIfNotSet) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kCoordinator);
    ASSERT_EQ(metrics->getLowEstimateRemainingTimeMillis(), boost::none);
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       CurrentOpDoesNotReportCoordinatorHighEstimateIfNotSet) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kCoordinator);
    auto report = metrics->reportForCurrentOp();
    ASSERT_FALSE(report.hasField("allShardsHighestRemainingOperationTimeEstimatedSecs"));
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       CurrentOpDoesNotReportCoordinatorLowEstimateIfNotSet) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kCoordinator);
    auto report = metrics->reportForCurrentOp();
    ASSERT_FALSE(report.hasField("allShardsLowestRemainingOperationTimeEstimatedSecs"));
}

TEST_F(ShardingDataTransformInstanceMetricsTest, OnStartedIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onStarted(); }, Section::kRoot, "countStarted");
}

TEST_F(ShardingDataTransformInstanceMetricsTest, OnSuccessIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onSuccess(); }, Section::kRoot, "countSucceeded");
}

TEST_F(ShardingDataTransformInstanceMetricsTest, OnFailureIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onFailure(); }, Section::kRoot, "countFailed");
}

TEST_F(ShardingDataTransformInstanceMetricsTest, OnCanceledIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onCanceled(); }, Section::kRoot, "countCanceled");
}

TEST_F(ShardingDataTransformInstanceMetricsTest, SetChunkImbalanceIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->setLastOpEndingChunkImbalance(1); },
        Section::kRoot,
        "lastOpEndingChunkImbalance");
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       OnReadDuringCriticalSectionIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onReadDuringCriticalSection(); },
        Section::kActive,
        "countReadsDuringCriticalSection");
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       OnWriteDuringCriticalSectionIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onWriteDuringCriticalSection(); },
        Section::kActive,
        "countWritesDuringCriticalSection");
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       OnWriteToStashCollectionsIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onWriteToStashedCollections(); },
        Section::kActive,
        "countWritesToStashCollections");
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       OnCloningRemoteBatchRetrievalIncrementsCumulativeMetricsCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onCloningRemoteBatchRetrieval(Milliseconds{0}); },
        Section::kLatencies,
        "collectionCloningTotalRemoteBatchesRetrieved");
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       OnCloningRemoteBatchRetrievalIncrementsCumulativeMetricsTime) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onCloningRemoteBatchRetrieval(Milliseconds{1}); },
        Section::kLatencies,
        "collectionCloningTotalRemoteBatchRetrievalTimeMillis");
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       OnDocumentsProcessedIncrementsCumulativeMetricsDocumentCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onDocumentsProcessed(1, 0, Milliseconds{0}); },
        Section::kActive,
        "documentsProcessed");
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       OnDocumentsProcessedIncrementsCumulativeMetricsLocalInserts) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onDocumentsProcessed(1, 0, Milliseconds{0}); },
        Section::kLatencies,
        "collectionCloningTotalLocalInserts");
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       OnDocumentsProcessedIncrementsCumulativeMetricsByteCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onDocumentsProcessed(0, 1, Milliseconds{0}); },
        Section::kActive,
        "bytesWritten");
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       OnDocumentsProcessedIncrementsCumulativeMetricsLocalInsertTime) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onDocumentsProcessed(0, 0, Milliseconds{1}); },
        Section::kLatencies,
        "collectionCloningTotalLocalInsertTimeMillis");
}

}  // namespace
}  // namespace mongo
