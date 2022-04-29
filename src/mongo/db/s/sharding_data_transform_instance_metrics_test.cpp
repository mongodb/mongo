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
#include "mongo/db/s/sharding_data_transform_instance_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

class InstanceMetricsWithObserverMock {
public:
    InstanceMetricsWithObserverMock(Date_t startTime,
                                    int64_t timeRemaining,
                                    ClockSource* clockSource,
                                    ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
        : _impl{UUID::gen(),
                BSON("command"
                     << "test"),
                NamespaceString("test.source"),
                ShardingDataTransformInstanceMetrics::Role::kDonor,
                startTime,
                clockSource,
                cumulativeMetrics,
                std::make_unique<ObserverMock>(startTime, timeRemaining)} {}


private:
    ShardingDataTransformInstanceMetrics _impl;
};

class ShardingDataTransformInstanceMetricsTest : public ShardingDataTransformMetricsTestFixture {
public:
    std::unique_ptr<ShardingDataTransformInstanceMetrics> createInstanceMetrics(
        UUID instanceId = UUID::gen(), Role role = Role::kDonor) {
        return std::make_unique<ShardingDataTransformInstanceMetrics>(instanceId,
                                                                      kTestCommand,
                                                                      kTestNamespace,
                                                                      role,
                                                                      getClockSource()->now(),
                                                                      getClockSource(),
                                                                      &_cumulativeMetrics);
    }

    std::unique_ptr<ShardingDataTransformInstanceMetrics> createInstanceMetrics(
        std::unique_ptr<ObserverMock> mock) {
        return std::make_unique<ShardingDataTransformInstanceMetrics>(
            UUID::gen(),
            kTestCommand,
            kTestNamespace,
            ShardingDataTransformInstanceMetrics::Role::kDonor,
            getClockSource()->now(),
            getClockSource(),
            &_cumulativeMetrics,
            std::move(mock));
    }

    using MetricsMutator = std::function<void(ShardingDataTransformInstanceMetrics*)>;
    void runTimeReportTest(const std::string& testName,
                           const std::initializer_list<Role>& roles,
                           const std::string& timeField,
                           const MetricsMutator& beginTimedSection,
                           const MetricsMutator& endTimedSection) {
        constexpr auto kIncrement = Milliseconds(5000);
        const auto kIncrementInSeconds = durationCount<Seconds>(kIncrement);
        for (const auto& role : roles) {
            LOGV2(6437400, "", "TestName"_attr = testName, "Role"_attr = role);
            auto uuid = UUID::gen();
            const auto& clock = getClockSource();
            auto metrics = std::make_unique<ShardingDataTransformInstanceMetrics>(
                uuid, kTestCommand, kTestNamespace, role, clock->now(), clock, &_cumulativeMetrics);

            // Reports 0 before timed section entered.
            clock->advance(kIncrement);
            auto report = metrics->reportForCurrentOp();
            ASSERT_EQ(report.getIntField(timeField), 0);

            // Reports time so far during critical section.
            beginTimedSection(metrics.get());
            clock->advance(kIncrement);
            report = metrics->reportForCurrentOp();
            ASSERT_EQ(report.getIntField(timeField), kIncrementInSeconds);
            clock->advance(kIncrement);
            report = metrics->reportForCurrentOp();
            ASSERT_EQ(report.getIntField(timeField), kIncrementInSeconds * 2);

            // Still reports total time after critical section ends.
            endTimedSection(metrics.get());
            clock->advance(kIncrement);
            report = metrics->reportForCurrentOp();
            ASSERT_EQ(report.getIntField(timeField), kIncrementInSeconds * 2);
        }
    }
};

TEST_F(ShardingDataTransformInstanceMetricsTest, RegisterAndDeregisterMetrics) {
    for (auto i = 0; i < 100; i++) {
        auto metrics = createInstanceMetrics();
        ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 1);
    }
    ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 0);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RegisterAndDeregisterMetricsAtOnce) {
    {
        std::vector<std::unique_ptr<ShardingDataTransformInstanceMetrics>> registered;
        for (auto i = 0; i < 100; i++) {
            registered.emplace_back(createInstanceMetrics());
            ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), registered.size());
        }
    }
    ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 0);
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


TEST_F(ShardingDataTransformInstanceMetricsTest, OnInsertAppliedShouldIncrementInsertsApplied) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("insertsApplied"), 0);
    metrics->onInsertApplied();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("insertsApplied"), 1);
}


TEST_F(ShardingDataTransformInstanceMetricsTest, OnUpdateAppliedShouldIncrementUpdatesApplied) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("updatesApplied"), 0);
    metrics->onUpdateApplied();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("updatesApplied"), 1);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, OnDeleteAppliedShouldIncrementDeletesApplied) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("deletesApplied"), 0);
    metrics->onDeleteApplied();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("deletesApplied"), 1);
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       OnOplogsEntriesAppliedShouldIncrementOplogsEntriesApplied) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 0);
    metrics->onOplogEntriesApplied(100);

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 100);
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

TEST_F(ShardingDataTransformInstanceMetricsTest, RecipientIncrementFetchedOplogEntries) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesFetched"), 0);
    metrics->onOplogEntriesFetched(50, Milliseconds(1));

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesFetched"), 50);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, CurrentOpReportsCriticalSectionTime) {
    runTimeReportTest(
        "CurrentOpReportsCriticalSectionTime",
        {Role::kDonor, Role::kCoordinator},
        "totalCriticalSectionTimeElapsedSecs",
        [](ShardingDataTransformInstanceMetrics* metrics) { metrics->onCriticalSectionBegin(); },
        [](ShardingDataTransformInstanceMetrics* metrics) { metrics->onCriticalSectionEnd(); });
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RecipientSetsDocumentsAndBytesToCopy) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("approxDocumentsToCopy"), 0);
    ASSERT_EQ(report.getIntField("approxBytesToCopy"), 0);
    metrics->setDocumentsToCopyCounts(5, 1000);

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("approxDocumentsToCopy"), 5);
    ASSERT_EQ(report.getIntField("approxBytesToCopy"), 1000);

    metrics->setDocumentsToCopyCounts(3, 750);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("approxDocumentsToCopy"), 3);
    ASSERT_EQ(report.getIntField("approxBytesToCopy"), 750);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RecipientIncrementsDocumentsAndBytesCopied) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("documentsCopied"), 0);
    ASSERT_EQ(report.getIntField("bytesCopied"), 0);
    metrics->onDocumentsCopied(5, 1000, Milliseconds(1));

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("documentsCopied"), 5);
    ASSERT_EQ(report.getIntField("bytesCopied"), 1000);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RecipientReportsRemainingTime) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);
    const auto& clock = getClockSource();
    constexpr auto kIncrement = Milliseconds(5000);
    constexpr auto kOpsPerIncrement = 25;
    const auto kIncrementSecs = durationCount<Seconds>(kIncrement);
    const auto kExpectedTotal = kIncrementSecs * 8;
    metrics->setDocumentsToCopyCounts(0, kOpsPerIncrement * 4);
    metrics->onOplogEntriesFetched(kOpsPerIncrement * 4, Milliseconds(1));

    // Before cloning.
    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("remainingOperationTimeEstimatedSecs"), 0);

    // During cloning.
    metrics->onCopyingBegin();
    metrics->onDocumentsCopied(0, kOpsPerIncrement, Milliseconds(1));
    clock->advance(kIncrement);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("remainingOperationTimeEstimatedSecs"),
              kExpectedTotal - kIncrementSecs);

    metrics->onDocumentsCopied(0, kOpsPerIncrement * 2, Milliseconds(1));
    clock->advance(kIncrement * 2);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("remainingOperationTimeEstimatedSecs"),
              kExpectedTotal - (kIncrementSecs * 3));

    // During applying.
    metrics->onDocumentsCopied(0, kOpsPerIncrement, Milliseconds(1));
    clock->advance(kIncrement);
    metrics->onCopyingEnd();
    metrics->onApplyingBegin();
    metrics->onOplogEntriesApplied(kOpsPerIncrement);
    clock->advance(kIncrement);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("remainingOperationTimeEstimatedSecs"),
              kExpectedTotal - (kIncrementSecs * 5));

    metrics->onOplogEntriesApplied(kOpsPerIncrement * 2);
    clock->advance(kIncrement * 2);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("remainingOperationTimeEstimatedSecs"),
              kExpectedTotal - (kIncrementSecs * 7));

    // Done.
    metrics->onOplogEntriesApplied(kOpsPerIncrement);
    clock->advance(kIncrement);
    metrics->onApplyingEnd();
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("remainingOperationTimeEstimatedSecs"), 0);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, CurrentOpReportsCopyingTime) {
    runTimeReportTest(
        "CurrentOpReportsCopyingTime",
        {Role::kRecipient, Role::kCoordinator},
        "totalCopyTimeElapsedSecs",
        [](ShardingDataTransformInstanceMetrics* metrics) { metrics->onCopyingBegin(); },
        [](ShardingDataTransformInstanceMetrics* metrics) { metrics->onCopyingEnd(); });
}

TEST_F(ShardingDataTransformInstanceMetricsTest, CurrentOpReportsApplyingTime) {
    runTimeReportTest(
        "CurrentOpReportsApplyingTime",
        {Role::kRecipient, Role::kCoordinator},
        "totalApplyTimeElapsedSecs",
        [](ShardingDataTransformInstanceMetrics* metrics) { metrics->onApplyingBegin(); },
        [](ShardingDataTransformInstanceMetrics* metrics) { metrics->onApplyingEnd(); });
}

TEST_F(ShardingDataTransformInstanceMetricsTest, CurrentOpReportsRunningTime) {
    auto uuid = UUID::gen();
    auto now = getClockSource()->now();
    constexpr auto kTimeElapsed = 15;
    auto start = now - Seconds(kTimeElapsed);
    auto metrics = std::make_unique<ShardingDataTransformInstanceMetrics>(uuid,
                                                                          kTestCommand,
                                                                          kTestNamespace,
                                                                          Role::kCoordinator,
                                                                          start,
                                                                          getClockSource(),
                                                                          &_cumulativeMetrics);
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

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("allShardsLowestRemainingOperationTimeEstimatedSecs"), 0);
    metrics->setCoordinatorLowEstimateRemainingTimeMillis(Milliseconds(2000));

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("allShardsLowestRemainingOperationTimeEstimatedSecs"), 2);
}

TEST_F(ShardingDataTransformInstanceMetricsTest,
       SetHighestOperationTimeShouldBeReflectedInCurrentOp) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kCoordinator);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("allShardsHighestRemainingOperationTimeEstimatedSecs"), 0);
    metrics->setCoordinatorHighEstimateRemainingTimeMillis(Milliseconds(12000));

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("allShardsHighestRemainingOperationTimeEstimatedSecs"), 12);
}

}  // namespace
}  // namespace mongo
