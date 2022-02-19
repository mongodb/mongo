/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <fmt/format.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/integer_histogram.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using namespace fmt::literals;

constexpr auto kOpTimeRemaining = "remainingOperationTimeEstimatedSecs"_sd;
constexpr auto kOplogApplierApplyBatchLatencyMillis = "oplogApplierApplyBatchLatencyMillis";
constexpr auto kCollClonerFillBatchForInsertLatencyMillis =
    "collClonerFillBatchForInsertLatencyMillis";

class ReshardingMetricsTest : public ServiceContextTest {
public:
    enum OpReportType {
        CumulativeReport,
        CurrentOpReportDonorRole,
        CurrentOpReportRecipientRole,
        CurrentOpReportCoordinatorRole
    };

    void setUp() override {
        auto clockSource = std::make_unique<ClockSourceMock>();
        _clockSource = clockSource.get();
        getGlobalServiceContext()->setFastClockSource(std::move(clockSource));
    }

    auto getMetrics() {
        return ReshardingMetrics::get(getGlobalServiceContext());
    }

    void startOperation(ReshardingMetrics::Role role) {
        getMetrics()->onStart(role, getGlobalServiceContext()->getFastClockSource()->now());
    }

    void stepUpOperation(ReshardingMetrics::Role role) {
        getMetrics()->onStepUp(role);
    }

    void stepDownOperation(ReshardingMetrics::Role role) {
        getMetrics()->onStepDown(role);
    }

    void completeOperation(ReshardingMetrics::Role role, ReshardingOperationStatusEnum opStatus) {
        getMetrics()->onCompletion(
            role, opStatus, getGlobalServiceContext()->getFastClockSource()->now());
    }

    // Timer step in milliseconds
    static constexpr auto kTimerStep = 100;

    void advanceTime(Milliseconds step = Milliseconds{kTimerStep}) {
        _clockSource->advance(step);
    }

    auto getWasReshardingEverAttempted() {
        return getMetrics()->wasReshardingEverAttempted();
    }

    auto getReport(OpReportType reportType) {
        BSONObjBuilder bob;
        if (reportType == OpReportType::CumulativeReport) {
            getMetrics()->serializeCumulativeOpMetrics(&bob);
        } else if (reportType == OpReportType::CurrentOpReportDonorRole) {
            getMetrics()->serializeCurrentOpMetrics(&bob, ReshardingMetrics::Role::kDonor);
        } else if (reportType == OpReportType::CurrentOpReportRecipientRole) {
            getMetrics()->serializeCurrentOpMetrics(&bob, ReshardingMetrics::Role::kRecipient);
        } else {
            getMetrics()->serializeCurrentOpMetrics(&bob, ReshardingMetrics::Role::kCoordinator);
        }
        return bob.obj();
    }

    void checkMetrics(std::string tag, int expectedValue, OpReportType reportType) {
        const auto report = getReport(reportType);
        checkMetrics(report, std::move(tag), expectedValue);
    }

    void checkMetrics(std::string tag,
                      int expectedValue,
                      std::string errMsg,
                      OpReportType reportType) {
        const auto report = getReport(reportType);
        checkMetrics(report, std::move(tag), expectedValue, std::move(errMsg));
    }

    void checkMetrics(const BSONObj& report,
                      std::string tag,
                      int expectedValue,
                      std::string errMsg = "Unexpected value") const {
        ASSERT_EQ(report.getIntField(tag), expectedValue)
            << fmt::format("{}: {}", errMsg, report.toString());
    };

    void appendExpectedHistogramResult(BSONObjBuilder* bob,
                                       std::string tag,
                                       const std::vector<int64_t>& latencies) {
        IntegerHistogram<kLatencyHistogramBucketsCount> hist(tag, latencyHistogramBuckets);
        for (size_t i = 0; i < latencies.size(); i++) {
            hist.increment(latencies[i]);
        }

        BSONObjBuilder histogramBuilder;
        hist.append(histogramBuilder, false);
        BSONObj histogram = histogramBuilder.obj();
        bob->appendElements(histogram);
    }

private:
    ClockSourceMock* _clockSource;
};

// TODO Re-enable once underlying invariants are re-enabled
/*
DEATH_TEST_F(ReshardingMetricsTest, RunOnCompletionBeforeOnStart, "No operation is in progress") {
    completeOperation(ReshardingMetrics::Role::kRecipient,
        ReshardingOperationStatusEnum::kSuccess);
}

DEATH_TEST_F(ReshardingMetricsTest,
             RunOnStepUpAfterOnStartInvariants,
             "Another operation is in progress") {
    startOperation(ReshardingMetrics::Role::kRecipient);
    stepUpOperation(ReshardingMetrics::Role::kRecipient);
}

DEATH_TEST_F(ReshardingMetricsTest,
             RunOnCompletionAfterOnStepDownInvariants,
             "No operation is in progress") {
    startOperation(ReshardingMetrics::Role::kRecipient);
    stepDownOperation(ReshardingMetrics::Role::kRecipient);
    completeOperation(ReshardingMetrics::Role::kRecipient,
        ReshardingOperationStatusEnum::kSuccess);
}
*/

TEST_F(ReshardingMetricsTest, RunOnCompletionTwiceIsSafe) {
    startOperation(ReshardingMetrics::Role::kCoordinator);
    completeOperation(ReshardingMetrics::Role::kCoordinator,
                      ReshardingOperationStatusEnum::kSuccess);
    completeOperation(ReshardingMetrics::Role::kCoordinator,
                      ReshardingOperationStatusEnum::kSuccess);
}

TEST_F(ReshardingMetricsTest, RunOnStepDownAfterOnCompletionIsSafe) {
    startOperation(ReshardingMetrics::Role::kRecipient);
    completeOperation(ReshardingMetrics::Role::kRecipient, ReshardingOperationStatusEnum::kSuccess);
    stepDownOperation(ReshardingMetrics::Role::kRecipient);
}

DEATH_TEST_F(ReshardingMetricsTest, CoordinatorThenDonor, "Another operation is in progress") {
    startOperation(ReshardingMetrics::Role::kCoordinator);
    startOperation(ReshardingMetrics::Role::kDonor);
}

DEATH_TEST_F(ReshardingMetricsTest, DonorThenCoordinator, "Another operation is in progress") {
    startOperation(ReshardingMetrics::Role::kDonor);
    startOperation(ReshardingMetrics::Role::kCoordinator);
}

DEATH_TEST_F(ReshardingMetricsTest, CoordinatorThenRecipient, "Another operation is in progress") {
    startOperation(ReshardingMetrics::Role::kCoordinator);
    startOperation(ReshardingMetrics::Role::kRecipient);
}

DEATH_TEST_F(ReshardingMetricsTest, RecipientThenCoordinator, "Another operation is in progress") {
    startOperation(ReshardingMetrics::Role::kRecipient);
    startOperation(ReshardingMetrics::Role::kCoordinator);
}

TEST_F(ReshardingMetricsTest, DonorAndRecipientCombinationIsSafe) {
    startOperation(ReshardingMetrics::Role::kRecipient);
    startOperation(ReshardingMetrics::Role::kDonor);
    completeOperation(ReshardingMetrics::Role::kRecipient, ReshardingOperationStatusEnum::kSuccess);
    completeOperation(ReshardingMetrics::Role::kDonor, ReshardingOperationStatusEnum::kSuccess);
}

TEST_F(ReshardingMetricsTest, DonorAndRecipientStepdownIsSafe) {
    startOperation(ReshardingMetrics::Role::kDonor);
    startOperation(ReshardingMetrics::Role::kRecipient);
    stepDownOperation(ReshardingMetrics::Role::kRecipient);
    stepDownOperation(ReshardingMetrics::Role::kDonor);
}

TEST_F(ReshardingMetricsTest, OperationStatus) {
    startOperation(ReshardingMetrics::Role::kCoordinator);
    const auto report = getReport(OpReportType::CurrentOpReportCoordinatorRole);
    ASSERT_EQ(report.getStringField("opStatus"),
              ReshardingOperationStatus_serializer(ReshardingOperationStatusEnum::kRunning));
    completeOperation(ReshardingMetrics::Role::kCoordinator,
                      ReshardingOperationStatusEnum::kSuccess);
}

TEST_F(ReshardingMetricsTest, TestOperationStatus) {
    const auto kNumSuccessfulOps = 3;
    const auto kNumFailedOps = 5;
    const auto kNumCanceledOps = 7;

    for (auto i = 0; i < kNumSuccessfulOps; i++) {
        startOperation(ReshardingMetrics::Role::kRecipient);
        completeOperation(ReshardingMetrics::Role::kRecipient,
                          ReshardingOperationStatusEnum::kSuccess);
    }

    for (auto i = 0; i < kNumFailedOps; i++) {
        startOperation(ReshardingMetrics::Role::kRecipient);
        completeOperation(ReshardingMetrics::Role::kRecipient,
                          ReshardingOperationStatusEnum::kFailure);
    }

    for (auto i = 0; i < kNumCanceledOps; i++) {
        startOperation(ReshardingMetrics::Role::kRecipient);
        completeOperation(ReshardingMetrics::Role::kRecipient,
                          ReshardingOperationStatusEnum::kCanceled);
    }

    checkMetrics("countReshardingSuccessful", kNumSuccessfulOps, OpReportType::CumulativeReport);
    checkMetrics("countReshardingFailures", kNumFailedOps, OpReportType::CumulativeReport);
    checkMetrics("countReshardingCanceled", kNumCanceledOps, OpReportType::CumulativeReport);

    const auto total = kNumSuccessfulOps + kNumFailedOps + kNumCanceledOps;
    checkMetrics("countReshardingOperations", total, OpReportType::CumulativeReport);
    startOperation(ReshardingMetrics::Role::kRecipient);
    checkMetrics("countReshardingOperations", total + 1, OpReportType::CumulativeReport);
}

TEST_F(ReshardingMetricsTest, TestElapsedTime) {
    startOperation(ReshardingMetrics::Role::kDonor);
    const auto elapsedTime = 1;
    advanceTime(Seconds(elapsedTime));
    checkMetrics(
        "totalOperationTimeElapsedSecs", elapsedTime, OpReportType::CurrentOpReportDonorRole);
}

TEST_F(ReshardingMetricsTest, TestDonorAndRecipientMetrics) {
    startOperation(ReshardingMetrics::Role::kRecipient);
    startOperation(ReshardingMetrics::Role::kDonor);
    const auto elapsedTime = 1;

    advanceTime(Seconds(elapsedTime));

    // Update metrics for donor
    const auto kWritesDuringCriticalSection = 7;
    getMetrics()->setDonorState(DonorStateEnum::kPreparingToBlockWrites);
    getMetrics()->enterCriticalSection(getGlobalServiceContext()->getFastClockSource()->now());
    getMetrics()->onWriteDuringCriticalSection(kWritesDuringCriticalSection);
    advanceTime(Seconds(elapsedTime));

    // Update metrics for recipient
    const auto kDocumentsToCopy = 50;
    const auto kBytesToCopy = 740;
    const auto kCopyProgress = 50;
    getMetrics()->setRecipientState(RecipientStateEnum::kCreatingCollection);
    getMetrics()->setDocumentsToCopy(kDocumentsToCopy, kBytesToCopy);
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->startCopyingDocuments(getGlobalServiceContext()->getFastClockSource()->now());
    getMetrics()->onDocumentsCopied(kDocumentsToCopy * kCopyProgress / 100,
                                    kBytesToCopy * kCopyProgress / 100);
    advanceTime(Seconds(elapsedTime));

    const auto currentDonorOpReport = getReport(OpReportType::CurrentOpReportDonorRole);
    const auto currentRecipientOpReport = getReport(OpReportType::CurrentOpReportRecipientRole);
    completeOperation(ReshardingMetrics::Role::kDonor, ReshardingOperationStatusEnum::kSuccess);
    completeOperation(ReshardingMetrics::Role::kRecipient, ReshardingOperationStatusEnum::kSuccess);

    checkMetrics(currentRecipientOpReport, "totalCopyTimeElapsedSecs", elapsedTime);
    checkMetrics(currentRecipientOpReport, "bytesCopied", kBytesToCopy * kCopyProgress / 100);
    checkMetrics(
        currentRecipientOpReport, "documentsCopied", kDocumentsToCopy * kCopyProgress / 100);
    checkMetrics(currentDonorOpReport, "totalCriticalSectionTimeElapsedSecs", elapsedTime * 2);
    checkMetrics(
        currentDonorOpReport, "countWritesDuringCriticalSection", kWritesDuringCriticalSection);

    const auto cumulativeReportAfterCompletion = getReport(OpReportType::CumulativeReport);
    checkMetrics(
        cumulativeReportAfterCompletion, "bytesCopied", kBytesToCopy * kCopyProgress / 100);
    checkMetrics(
        cumulativeReportAfterCompletion, "documentsCopied", kDocumentsToCopy * kCopyProgress / 100);
    checkMetrics(cumulativeReportAfterCompletion,
                 "countWritesDuringCriticalSection",
                 kWritesDuringCriticalSection);
}

TEST_F(ReshardingMetricsTest, CumulativeOpMetricsAreRetainedAfterCompletion) {
    auto constexpr kTag = "documentsCopied";
    startOperation(ReshardingMetrics::Role::kRecipient);
    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->startCopyingDocuments(getGlobalServiceContext()->getFastClockSource()->now());
    getMetrics()->onDocumentsCopied(kDocumentsToCopy, kBytesToCopy);
    advanceTime();
    completeOperation(ReshardingMetrics::Role::kRecipient, ReshardingOperationStatusEnum::kFailure);
    advanceTime();

    checkMetrics(kTag,
                 kDocumentsToCopy,
                 "Cumulative metrics are not retained",
                 OpReportType::CumulativeReport);

    startOperation(ReshardingMetrics::Role::kRecipient);
    checkMetrics(
        kTag, kDocumentsToCopy, "Cumulative metrics are reset", OpReportType::CumulativeReport);
}

TEST_F(ReshardingMetricsTest, CumulativeOpMetricsAreRetainedAfterCancellation) {
    auto constexpr kTag = "documentsCopied";
    startOperation(ReshardingMetrics::Role::kRecipient);
    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->startCopyingDocuments(getGlobalServiceContext()->getFastClockSource()->now());
    getMetrics()->onDocumentsCopied(kDocumentsToCopy, kBytesToCopy);
    advanceTime();
    completeOperation(ReshardingMetrics::Role::kRecipient,
                      ReshardingOperationStatusEnum::kCanceled);
    advanceTime();

    checkMetrics(kTag,
                 kDocumentsToCopy,
                 "Cumulative metrics are not retained",
                 OpReportType::CumulativeReport);

    startOperation(ReshardingMetrics::Role::kRecipient);
    checkMetrics(
        kTag, kDocumentsToCopy, "Cumulative metrics are reset", OpReportType::CumulativeReport);
}

TEST_F(ReshardingMetricsTest, CurrentOpMetricsAreResetAfterCompletion) {
    auto constexpr kTag = "documentsCopied";
    startOperation(ReshardingMetrics::Role::kRecipient);
    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->startCopyingDocuments(getGlobalServiceContext()->getFastClockSource()->now());
    getMetrics()->onDocumentsCopied(kDocumentsToCopy, kBytesToCopy);
    checkMetrics(kTag,
                 kDocumentsToCopy,
                 "Current metrics are not set",
                 OpReportType::CurrentOpReportRecipientRole);
    advanceTime();
    completeOperation(ReshardingMetrics::Role::kRecipient, ReshardingOperationStatusEnum::kSuccess);
    advanceTime();

    startOperation(ReshardingMetrics::Role::kRecipient);
    checkMetrics(
        kTag, 0, "Current metrics are not reset", OpReportType::CurrentOpReportRecipientRole);
}

TEST_F(ReshardingMetricsTest, CurrentOpMetricsAreNotRetainedAfterCompletion) {
    auto constexpr kTag = "documentsCopied";
    startOperation(ReshardingMetrics::Role::kRecipient);
    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->startCopyingDocuments(getGlobalServiceContext()->getFastClockSource()->now());
    getMetrics()->onDocumentsCopied(kDocumentsToCopy, kBytesToCopy);
    checkMetrics(kTag,
                 kDocumentsToCopy,
                 "Current metrics are not set",
                 OpReportType::CurrentOpReportRecipientRole);
    advanceTime();
    completeOperation(ReshardingMetrics::Role::kRecipient, ReshardingOperationStatusEnum::kFailure);
    advanceTime();

    ASSERT_FALSE(getReport(OpReportType::CurrentOpReportRecipientRole)[kTag].ok());
}

TEST_F(ReshardingMetricsTest, CurrentOpMetricsAreNotRetainedAfterStepDown) {
    auto constexpr kTag = "documentsCopied";
    startOperation(ReshardingMetrics::Role::kRecipient);
    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->startCopyingDocuments(getGlobalServiceContext()->getFastClockSource()->now());
    getMetrics()->onDocumentsCopied(kDocumentsToCopy, kBytesToCopy);
    checkMetrics(kTag,
                 kDocumentsToCopy,
                 "Current metrics are not set",
                 OpReportType::CurrentOpReportRecipientRole);
    advanceTime();
    stepDownOperation(ReshardingMetrics::Role::kRecipient);
    advanceTime();

    ASSERT_FALSE(getReport(OpReportType::CurrentOpReportRecipientRole)[kTag].ok());
}

TEST_F(ReshardingMetricsTest, CumulativeOpMetricsAreSetAndReset) {
    const auto kExpectedMin = Milliseconds(1);
    const auto kExpectedMax = Milliseconds(8);
    auto constexpr kMinOpTime = "minShardRemainingOperationTimeEstimatedMillis";
    auto constexpr kMaxOpTime = "maxShardRemainingOperationTimeEstimatedMillis";
    startOperation(ReshardingMetrics::Role::kCoordinator);
    getMetrics()->setCoordinatorState(CoordinatorStateEnum::kBlockingWrites);

    getMetrics()->setMinRemainingOperationTime(kExpectedMin);
    checkMetrics(kMinOpTime,
                 durationCount<Milliseconds>(kExpectedMin),
                 "Cumulative metrics minimum time remaining is not set",
                 OpReportType::CumulativeReport);

    getMetrics()->setMaxRemainingOperationTime(kExpectedMax);
    checkMetrics(kMaxOpTime,
                 durationCount<Milliseconds>(kExpectedMax),
                 "Cumulative metrics maximum time remaining is not set",
                 OpReportType::CumulativeReport);

    advanceTime();
    completeOperation(ReshardingMetrics::Role::kCoordinator,
                      ReshardingOperationStatusEnum::kSuccess);
    advanceTime();
    checkMetrics(kMinOpTime,
                 0,
                 "Cumulative metrics minimum time remaining is not reset",
                 OpReportType::CumulativeReport);

    checkMetrics(kMaxOpTime,
                 0,
                 "Cumulative metrics maximum time remaining is not reset",
                 OpReportType::CumulativeReport);
}

TEST_F(ReshardingMetricsTest, EstimatedRemainingOperationTime) {
    auto constexpr kTag = "remainingOperationTimeEstimatedSecs";
    const auto elapsedTime = 1;

    startOperation(ReshardingMetrics::Role::kRecipient);
    checkMetrics(kTag, -1, OpReportType::CurrentOpReportRecipientRole);

    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCreatingCollection);
    getMetrics()->setDocumentsToCopy(kDocumentsToCopy, kBytesToCopy);
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->startCopyingDocuments(getGlobalServiceContext()->getFastClockSource()->now());
    getMetrics()->onDocumentsCopied(kDocumentsToCopy / 2, kBytesToCopy / 2);
    advanceTime(Seconds(elapsedTime));
    // Since 50% of the data is copied, the remaining copy time equals the elapsed copy time, which
    // is equal to `elapsedTime` seconds.
    checkMetrics(kTag, elapsedTime + 2 * elapsedTime, OpReportType::CurrentOpReportRecipientRole);

    const auto kOplogEntriesFetched = 4;
    const auto kOplogEntriesApplied = 2;
    getMetrics()->setRecipientState(RecipientStateEnum::kApplying);
    getMetrics()->endCopyingDocuments(getGlobalServiceContext()->getFastClockSource()->now());
    getMetrics()->startApplyingOplogEntries(getGlobalServiceContext()->getFastClockSource()->now());
    getMetrics()->onOplogEntriesFetched(kOplogEntriesFetched);
    getMetrics()->onOplogEntriesApplied(kOplogEntriesApplied);
    advanceTime(Seconds(elapsedTime));
    // So far, the time to apply oplog entries equals `elapsedTime` seconds.
    checkMetrics(kTag,
                 elapsedTime * (kOplogEntriesFetched / kOplogEntriesApplied - 1),
                 OpReportType::CurrentOpReportRecipientRole);
}

TEST_F(ReshardingMetricsTest, CurrentOpReportForDonor) {
    const auto kDonorState = DonorStateEnum::kPreparingToBlockWrites;
    startOperation(ReshardingMetrics::Role::kDonor);
    advanceTime(Seconds(2));
    getMetrics()->setDonorState(kDonorState);
    getMetrics()->enterCriticalSection(getGlobalServiceContext()->getFastClockSource()->now());
    advanceTime(Seconds(3));

    const ReshardingMetrics::ReporterOptions options(
        ReshardingMetrics::Role::kDonor,
        UUID::parse("12345678-1234-1234-1234-123456789abc").getValue(),
        NamespaceString("db", "collection"),
        BSON("id" << 1),
        true);

    const auto expected =
        fromjson(fmt::format("{{ type: \"op\","
                             "desc: \"ReshardingDonorService {0}\","
                             "op: \"command\","
                             "ns: \"{1}\","
                             "originatingCommand: {{ reshardCollection: \"{1}\","
                             "key: {2},"
                             "unique: {3},"
                             "collation: {{ locale: \"simple\" }} }},"
                             "totalOperationTimeElapsedSecs: 5,"
                             "countWritesDuringCriticalSection: 0,"
                             "totalCriticalSectionTimeElapsedSecs : 3,"
                             "donorState: \"{4}\","
                             "opStatus: \"running\" }}",
                             options.id.toString(),
                             options.nss.toString(),
                             options.shardKey.toString(),
                             options.unique ? "true" : "false",
                             DonorState_serializer(kDonorState)));

    const auto report = getMetrics()->reportForCurrentOp(options);
    ASSERT_BSONOBJ_EQ(expected, report);
}

TEST_F(ReshardingMetricsTest, CurrentOpReportForRecipient) {
    const auto kRecipientState = RecipientStateEnum::kCloning;

    constexpr auto kDocumentsToCopy = 500;
    constexpr auto kDocumentsCopied = kDocumentsToCopy * 0.5;
    static_assert(kDocumentsToCopy >= kDocumentsCopied);

    constexpr auto kBytesToCopy = 8192;
    constexpr auto kBytesCopied = kBytesToCopy * 0.5;
    static_assert(kBytesToCopy >= kBytesCopied);

    constexpr auto kDelayBeforeCloning = Seconds(2);
    startOperation(ReshardingMetrics::Role::kRecipient);
    advanceTime(kDelayBeforeCloning);

    constexpr auto kTimeSpentCloning = Seconds(3);
    getMetrics()->setRecipientState(RecipientStateEnum::kCreatingCollection);
    getMetrics()->setDocumentsToCopy(kDocumentsToCopy, kBytesToCopy);
    getMetrics()->setRecipientState(kRecipientState);
    getMetrics()->startCopyingDocuments(getGlobalServiceContext()->getFastClockSource()->now());
    advanceTime(kTimeSpentCloning);
    getMetrics()->onDocumentsCopied(kDocumentsCopied, kBytesCopied);

    const auto kTimeToCopyRemainingSeconds =
        durationCount<Seconds>(kTimeSpentCloning) * (kBytesToCopy / kBytesCopied - 1);
    const auto kRemainingOperationTimeSeconds =
        durationCount<Seconds>(kTimeSpentCloning) + 2 * kTimeToCopyRemainingSeconds;

    const ReshardingMetrics::ReporterOptions options(
        ReshardingMetrics::Role::kRecipient,
        UUID::parse("12345678-1234-1234-1234-123456789def").getValue(),
        NamespaceString("db", "collection"),
        BSON("id" << 1),
        false);

    BSONObj expectedPrefix =
        fromjson(fmt::format("{{ type: \"op\","
                             "desc: \"ReshardingRecipientService {0}\","
                             "op: \"command\","
                             "ns: \"{1}\","
                             "originatingCommand: {{ reshardCollection: \"{1}\","
                             "key: {2},"
                             "unique: {3},"
                             "collation: {{ locale: \"simple\" }} }},"
                             "totalOperationTimeElapsedSecs: {4},"
                             "remainingOperationTimeEstimatedSecs: {5},"
                             "approxDocumentsToCopy: {6},"
                             "documentsCopied: {7},"
                             "approxBytesToCopy: {8},"
                             "bytesCopied: {9},"
                             "totalCopyTimeElapsedSecs: {10},"
                             "oplogEntriesFetched: 0,"
                             "oplogEntriesApplied: 0,"
                             "totalApplyTimeElapsedSecs: 0,"
                             "recipientState: \"{11}\","
                             "opStatus: \"running\" }}",
                             options.id.toString(),
                             options.nss.toString(),
                             options.shardKey.toString(),
                             options.unique ? "true" : "false",
                             durationCount<Seconds>(kDelayBeforeCloning + kTimeSpentCloning),
                             kRemainingOperationTimeSeconds,
                             kDocumentsToCopy,
                             kDocumentsCopied,
                             kBytesToCopy,
                             kBytesCopied,
                             durationCount<Seconds>(kTimeSpentCloning),
                             RecipientState_serializer(kRecipientState)));

    BSONObjBuilder expectedBuilder(std::move(expectedPrefix));

    // Append histogram latency data.
    appendExpectedHistogramResult(&expectedBuilder, kOplogApplierApplyBatchLatencyMillis, {});
    appendExpectedHistogramResult(&expectedBuilder, kCollClonerFillBatchForInsertLatencyMillis, {});

    BSONObj expected = expectedBuilder.done();

    const auto report = getMetrics()->reportForCurrentOp(options);
    ASSERT_BSONOBJ_EQ(expected, report);
}

TEST_F(ReshardingMetricsTest, TestHistogramMetricsForRecipient) {
    const std::vector<int64_t> applyLatencies_1{3, 427, 0, 6004, 320, 10056, 12300, 105, 70};
    const std::vector<int64_t> applyLatencies_2{800, 20, 5, 1025, 10567};
    const std::vector<int64_t> insertLatencies_1{120, 7, 110, 50, 0, 16500, 77000, 667, 7980};
    const std::vector<int64_t> insertLatencies_2{12450, 2400, 760, 57, 2};

    const auto combineLatencies = [](std::vector<int64_t>* allLatencies,
                                     const std::vector<int64_t>& latencies_1,
                                     const std::vector<int64_t>& latencies_2) {
        allLatencies->insert(allLatencies->end(), latencies_1.begin(), latencies_1.end());
        allLatencies->insert(allLatencies->end(), latencies_2.begin(), latencies_2.end());
    };

    std::vector<int64_t> allApplyLatencies;
    combineLatencies(&allApplyLatencies, applyLatencies_1, applyLatencies_2);
    std::vector<int64_t> allInsertLatencies;
    combineLatencies(&allInsertLatencies, insertLatencies_1, insertLatencies_2);


    const ReshardingMetrics::ReporterOptions options(
        ReshardingMetrics::Role::kRecipient,
        UUID::parse("12345678-1234-1234-1234-123456789def").getValue(),
        NamespaceString("db", "collection"),
        BSON("id" << 1),
        false);

    // Test that all histogram metrics appear in both currentOp and cumulativeOp.
    const size_t kNumTests = 4;
    std::vector<int64_t> testLatencies[kNumTests] = {
        applyLatencies_1, applyLatencies_2, insertLatencies_1, insertLatencies_2};
    std::vector<int64_t> expectedLatencies[kNumTests] = {
        applyLatencies_1, allApplyLatencies, insertLatencies_1, allInsertLatencies};
    OpReportType testReportTypes[kNumTests] = {OpReportType::CurrentOpReportRecipientRole,
                                               OpReportType::CumulativeReport,
                                               OpReportType::CurrentOpReportRecipientRole,
                                               OpReportType::CumulativeReport};
    std::string histogramTag[kNumTests] = {kOplogApplierApplyBatchLatencyMillis,
                                           kOplogApplierApplyBatchLatencyMillis,
                                           kCollClonerFillBatchForInsertLatencyMillis,
                                           kCollClonerFillBatchForInsertLatencyMillis};

    auto testLatencyHistogram = [&](std::vector<int64_t> latencies,
                                    OpReportType reportType,
                                    std::vector<int64_t> expectedLatencies,
                                    std::string histogramTag) {
        LOGV2(57700,
              "TestHistogramMetricsForRecipient test case",
              "reportType"_attr = reportType,
              "histogramTag"_attr = histogramTag);

        startOperation(ReshardingMetrics::Role::kRecipient);

        RecipientStateEnum state = (histogramTag.compare(kOplogApplierApplyBatchLatencyMillis) == 0
                                        ? RecipientStateEnum::kApplying
                                        : RecipientStateEnum::kCloning);
        getMetrics()->setRecipientState(state);

        for (size_t i = 0; i < latencies.size(); i++) {
            if (histogramTag.compare(kOplogApplierApplyBatchLatencyMillis) == 0) {
                getMetrics()->onOplogApplierApplyBatch(Milliseconds(latencies[i]));
            } else if (histogramTag.compare(kCollClonerFillBatchForInsertLatencyMillis) == 0) {
                getMetrics()->onCollClonerFillBatchForInsert(Milliseconds(latencies[i]));
            } else {
                MONGO_UNREACHABLE;
            }
        }

        const auto report = getReport(reportType);
        const auto buckets = report[histogramTag];

        BSONObjBuilder expectedBuilder;
        appendExpectedHistogramResult(&expectedBuilder, histogramTag, expectedLatencies);
        const auto expectedHist = expectedBuilder.done();
        const auto expectedBuckets = expectedHist[histogramTag];

        ASSERT_EQ(buckets.woCompare(expectedBuckets), 0);

        completeOperation(ReshardingMetrics::Role::kRecipient,
                          ReshardingOperationStatusEnum::kFailure);
    };

    for (size_t i = 0; i < kNumTests; i++) {
        testLatencyHistogram(
            testLatencies[i], testReportTypes[i], expectedLatencies[i], histogramTag[i]);
    }
}

TEST_F(ReshardingMetricsTest, CurrentOpReportForCoordinator) {
    const auto kCoordinatorState = CoordinatorStateEnum::kInitializing;
    const auto kSomeDuration = Seconds(10);

    startOperation(ReshardingMetrics::Role::kCoordinator);
    getMetrics()->setCoordinatorState(kCoordinatorState);
    advanceTime(kSomeDuration);

    const ReshardingMetrics::ReporterOptions options(
        ReshardingMetrics::Role::kCoordinator,
        UUID::parse("12345678-1234-1234-1234-123456789cba").getValue(),
        NamespaceString("db", "collection"),
        BSON("id" << 1),
        false);

    const auto expected =
        fromjson(fmt::format("{{ type: \"op\","
                             "desc: \"ReshardingCoordinatorService {0}\","
                             "op: \"command\","
                             "ns: \"{1}\","
                             "originatingCommand: {{ reshardCollection: \"{1}\","
                             "key: {2},"
                             "unique: {3},"
                             "collation: {{ locale: \"simple\" }} }},"
                             "totalOperationTimeElapsedSecs: {4},"
                             "coordinatorState: \"{5}\","
                             "opStatus: \"running\" }}",
                             options.id.toString(),
                             options.nss.toString(),
                             options.shardKey.toString(),
                             options.unique ? "true" : "false",
                             durationCount<Seconds>(kSomeDuration),
                             CoordinatorState_serializer(kCoordinatorState)));

    const auto report = getMetrics()->reportForCurrentOp(options);
    ASSERT_BSONOBJ_EQ(expected, report);
}

TEST_F(ReshardingMetricsTest, EstimatedRemainingOperationTimeCloning) {
    // Copy N docs @ timePerDoc. Check the progression of the estimated time remaining.
    auto m = getMetrics();
    m->onStart(ReshardingMetrics::Role::kRecipient,
               getGlobalServiceContext()->getFastClockSource()->now());
    auto timePerDocument = Seconds(2);
    int64_t bytesPerDocument = 1024;
    int64_t documentsToCopy = 409;
    int64_t bytesToCopy = bytesPerDocument * documentsToCopy;
    m->setRecipientState(RecipientStateEnum::kCreatingCollection);
    m->setDocumentsToCopy(documentsToCopy, bytesToCopy);
    m->setRecipientState(RecipientStateEnum::kCloning);
    m->startCopyingDocuments(getGlobalServiceContext()->getFastClockSource()->now());
    auto remainingTime = 2 * timePerDocument * documentsToCopy;
    double maxAbsRelErr = 0;
    for (int64_t copied = 0; copied < documentsToCopy; ++copied) {
        double output =
            getReport(OpReportType::CurrentOpReportRecipientRole)[kOpTimeRemaining].Number();
        if (copied == 0) {
            ASSERT_EQ(output, -1);
        } else {
            ASSERT_GTE(output, 0);
            auto expected = durationCount<Seconds>(remainingTime);
            // Check that error is pretty small (it should get better as the operation progresses)
            double absRelErr = std::abs((output - expected) / expected);
            ASSERT_LT(absRelErr, 0.05)
                << "output={}, expected={}, copied={}"_format(output, expected, copied);
            maxAbsRelErr = std::max(maxAbsRelErr, absRelErr);
        }
        m->onDocumentsCopied(1, bytesPerDocument);
        advanceTime(timePerDocument);
        remainingTime -= timePerDocument;
    }
    LOGV2_DEBUG(
        5422700, 3, "Max absolute relative error observed", "maxAbsRelErr"_attr = maxAbsRelErr);
}

TEST_F(ReshardingMetricsTest, EstimatedRemainingOperationTimeApplying) {
    // Perform N ops @ timePerOp. Check the progression of the estimated time remaining.
    auto m = getMetrics();
    m->onStart(ReshardingMetrics::Role::kRecipient,
               getGlobalServiceContext()->getFastClockSource()->now());
    m->setRecipientState(RecipientStateEnum::kApplying);
    m->startApplyingOplogEntries(getGlobalServiceContext()->getFastClockSource()->now());

    // 1 extra millisecond here because otherwise an error of just 1ms will round this down to the
    // next second.
    auto timePerOp = Milliseconds(1001);
    int64_t fetched = 10000;
    m->onOplogEntriesFetched(fetched);
    auto remainingTime = timePerOp * fetched;
    double maxAbsRelErr = 0;
    for (int64_t applied = 0; applied < fetched; ++applied) {
        double output =
            getReport(OpReportType::CurrentOpReportRecipientRole)[kOpTimeRemaining].Number();
        if (applied == 0) {
            ASSERT_EQ(output, -1);
        } else {
            auto expected = durationCount<Seconds>(remainingTime);
            // Check that error is pretty small (it should get better as the operation progresses)
            double absRelErr = std::abs((output - expected) / expected);
            ASSERT_LT(absRelErr, 0.05)
                << "output={}, expected={}, applied={}"_format(output, expected, applied);
            maxAbsRelErr = std::max(maxAbsRelErr, absRelErr);
        }
        advanceTime(timePerOp);
        m->onOplogEntriesApplied(1);
        remainingTime -= timePerOp;
    }
    LOGV2_DEBUG(
        5422701, 3, "Max absolute relative error observed", "maxAbsRelErr"_attr = maxAbsRelErr);
}

TEST_F(ReshardingMetricsTest, CumulativeOpMetricsAccumulate) {
    auto constexpr kTag = "documentsCopied";
    startOperation(ReshardingMetrics::Role::kRecipient);
    const auto kDocumentsToCopy1 = 2;
    const auto kBytesToCopy1 = 200;

    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy1, kBytesToCopy1);
    completeOperation(ReshardingMetrics::Role::kRecipient, ReshardingOperationStatusEnum::kFailure);

    startOperation(ReshardingMetrics::Role::kRecipient);
    const auto kDocumentsToCopy2 = 3;
    const auto kBytesToCopy2 = 400;

    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy2, kBytesToCopy2);
    completeOperation(ReshardingMetrics::Role::kRecipient, ReshardingOperationStatusEnum::kFailure);

    checkMetrics(kTag,
                 kDocumentsToCopy1 + kDocumentsToCopy2,
                 "Cumulative metrics are not accumulated",
                 OpReportType::CumulativeReport);
}

TEST_F(ReshardingMetricsTest, TestWasReshardingEverAttemptedStartComplete) {
    ASSERT_FALSE(getWasReshardingEverAttempted());

    startOperation(ReshardingMetrics::Role::kRecipient);
    ASSERT_TRUE(getWasReshardingEverAttempted());

    completeOperation(ReshardingMetrics::Role::kRecipient, ReshardingOperationStatusEnum::kSuccess);
    ASSERT_TRUE(getWasReshardingEverAttempted());
}

TEST_F(ReshardingMetricsTest, TestWasReshardingEverAttemptedStartStepDownStepUp) {
    ASSERT_FALSE(getWasReshardingEverAttempted());

    startOperation(ReshardingMetrics::Role::kRecipient);
    ASSERT_TRUE(getWasReshardingEverAttempted());

    stepDownOperation(ReshardingMetrics::Role::kRecipient);
    ASSERT_TRUE(getWasReshardingEverAttempted());

    stepUpOperation(ReshardingMetrics::Role::kRecipient);
    ASSERT_TRUE(getWasReshardingEverAttempted());
}

TEST_F(ReshardingMetricsTest, TestWasReshardingEverAttemptedStepUpStepDown) {
    ASSERT_FALSE(getWasReshardingEverAttempted());

    stepUpOperation(ReshardingMetrics::Role::kRecipient);
    ASSERT_TRUE(getWasReshardingEverAttempted());

    stepDownOperation(ReshardingMetrics::Role::kRecipient);
    ASSERT_TRUE(getWasReshardingEverAttempted());
}

TEST_F(ReshardingMetricsTest, TestWasReshardingEverAttemptedStepUpComplete) {
    ASSERT_FALSE(getWasReshardingEverAttempted());

    stepUpOperation(ReshardingMetrics::Role::kRecipient);
    ASSERT_TRUE(getWasReshardingEverAttempted());

    completeOperation(ReshardingMetrics::Role::kRecipient, ReshardingOperationStatusEnum::kSuccess);
    ASSERT_TRUE(getWasReshardingEverAttempted());
}

TEST_F(ReshardingMetricsTest, TestOnStepUpWithDonorMetrics) {
    ASSERT_FALSE(getWasReshardingEverAttempted());

    getMetrics()->onStepUp(DonorStateEnum::kUnused, ReshardingDonorMetrics());

    ASSERT_TRUE(getWasReshardingEverAttempted());
}

}  // namespace
}  // namespace mongo
