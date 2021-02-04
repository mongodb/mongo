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

#include <fmt/format.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/uuid.h"

namespace mongo {

class ReshardingMetricsTest : public ServiceContextTest {
public:
    void setUp() {
        auto clockSource = std::make_unique<ClockSourceMock>();
        _clockSource = clockSource.get();
        getGlobalServiceContext()->setFastClockSource(std::move(clockSource));
    }

    auto getMetrics() {
        return ReshardingMetrics::get(getGlobalServiceContext());
    }

    // Timer step in milliseconds
    static constexpr auto kTimerStep = 100;

    void advanceTime(Milliseconds interval = Milliseconds(kTimerStep)) {
        _clockSource->advance(interval);
    }

    auto getReport() {
        BSONObjBuilder bob;
        getMetrics()->serialize(&bob);
        return bob.obj();
    }

    void checkMetrics(std::string tag, int expectedValue) {
        const auto report = getReport();
        checkMetrics(report, std::move(tag), std::move(expectedValue));
    }

    void checkMetrics(std::string tag, int expectedValue, std::string errMsg) {
        const auto report = getReport();
        checkMetrics(report, std::move(tag), std::move(expectedValue), std::move(errMsg));
    }

    void checkMetrics(const BSONObj& report,
                      std::string tag,
                      int expectedValue,
                      std::string errMsg = "Unexpected value") const {
        ASSERT_EQ(report.getIntField(tag), expectedValue)
            << fmt::format("{}: {}", errMsg, report.toString());
    };

private:
    ClockSourceMock* _clockSource;
};

DEATH_TEST_F(ReshardingMetricsTest, UpdateMetricsBeforeOnStart, "No operation is in progress") {
    getMetrics()->onWriteDuringCriticalSection(1);
}

DEATH_TEST_F(ReshardingMetricsTest, RunOnCompletionBeforeOnStart, "No operation is in progress") {
    getMetrics()->onCompletion(ReshardingMetrics::OperationStatus::kSucceeded);
}

TEST_F(ReshardingMetricsTest, OperationStatus) {
    auto constexpr kTag = "opStatus";
    // No operation has completed yet, so the status is unknown.
    checkMetrics(kTag, (int)ReshardingMetrics::OperationStatus::kUnknown);
    for (auto status : {ReshardingMetrics::OperationStatus::kSucceeded,
                        ReshardingMetrics::OperationStatus::kFailed,
                        ReshardingMetrics::OperationStatus::kCanceled}) {
        getMetrics()->onStart();
        checkMetrics(kTag, (int)ReshardingMetrics::OperationStatus::kUnknown);
        getMetrics()->onCompletion(status);
        checkMetrics(kTag, (int)status);
    }
}

TEST_F(ReshardingMetricsTest, TestOperationStatus) {
    const auto kNumSuccessfulOps = 3;
    const auto kNumFailedOps = 5;
    const auto kNumCanceledOps = 7;

    for (auto i = 0; i < kNumSuccessfulOps; i++) {
        getMetrics()->onStart();
        getMetrics()->onCompletion(ReshardingMetrics::OperationStatus::kSucceeded);
    }

    for (auto i = 0; i < kNumFailedOps; i++) {
        getMetrics()->onStart();
        getMetrics()->onCompletion(ReshardingMetrics::OperationStatus::kFailed);
    }

    for (auto i = 0; i < kNumCanceledOps; i++) {
        getMetrics()->onStart();
        getMetrics()->onCompletion(ReshardingMetrics::OperationStatus::kCanceled);
    }

    checkMetrics("successfulOperations", kNumSuccessfulOps);
    checkMetrics("failedOperations", kNumFailedOps);
    checkMetrics("canceledOperations", kNumCanceledOps);
}

TEST_F(ReshardingMetricsTest, TestElapsedTime) {
    getMetrics()->onStart();
    advanceTime();
    getMetrics()->onCompletion(ReshardingMetrics::OperationStatus::kSucceeded);
    checkMetrics("totalOperationTimeElapsedMillis", kTimerStep);
}

TEST_F(ReshardingMetricsTest, TestDonorAndRecipientMetrics) {
    getMetrics()->onStart();

    advanceTime();

    // Update metrics for donor
    const auto kWritesDuringCriticalSection = 7;
    getMetrics()->setDonorState(DonorStateEnum::kPreparingToMirror);
    getMetrics()->onWriteDuringCriticalSection(kWritesDuringCriticalSection);
    advanceTime();

    // Update metrics for recipient
    const auto kDocumentsToCopy = 50;
    const auto kBytesToCopy = 740;
    const auto kCopyProgress = 50;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->setDocumentsToCopy(kDocumentsToCopy, kBytesToCopy);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy * kCopyProgress / 100,
                                    kBytesToCopy * kCopyProgress / 100);
    advanceTime();

    const auto report = getReport();
    getMetrics()->onCompletion(ReshardingMetrics::OperationStatus::kSucceeded);

    checkMetrics(report, "totalCopyTimeElapsedMillis", kTimerStep);
    checkMetrics(report, "bytesCopied", kBytesToCopy * kCopyProgress / 100);
    checkMetrics(report, "documentsCopied", kDocumentsToCopy * kCopyProgress / 100);
    checkMetrics(report, "totalCriticalSectionTimeElapsedMillis", kTimerStep * 2);
    checkMetrics(report, "countWritesDuringCriticalSection", kWritesDuringCriticalSection);

    // Expected remaining time = totalCopyTimeElapsedMillis + 2 * estimated time to copy remaining
    checkMetrics(report,
                 "remainingOperationTimeEstimatedMillis",
                 kTimerStep + 2 * (100 - kCopyProgress) / kCopyProgress * kTimerStep);
}

TEST_F(ReshardingMetricsTest, MetricsAreRetainedAfterCompletion) {
    auto constexpr kTag = "totalOperationTimeElapsedMillis";

    getMetrics()->onStart();
    advanceTime();
    getMetrics()->onCompletion(ReshardingMetrics::OperationStatus::kSucceeded);
    advanceTime();

    checkMetrics(kTag, kTimerStep, "Metrics are not retained");

    getMetrics()->onStart();
    checkMetrics(kTag, 0, "Metrics are not reset");
}

TEST_F(ReshardingMetricsTest, EstimatedRemainingOperationTime) {
    auto constexpr kTag = "remainingOperationTimeEstimatedMillis";

    getMetrics()->onStart();
    checkMetrics(kTag, -1);

    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->setDocumentsToCopy(kDocumentsToCopy, kBytesToCopy);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy / 2, kBytesToCopy / 2);
    advanceTime();
    // Since 50% of the data is copied, the remaining copy time equals the elapsed copy time, which
    // is equal to `kTimerStep` milliseconds.
    checkMetrics(kTag, kTimerStep + 2 * kTimerStep);

    const auto kOplogEntriesFetched = 4;
    const auto kOplogEntriesApplied = 2;
    getMetrics()->setRecipientState(RecipientStateEnum::kApplying);
    getMetrics()->onOplogEntriesFetched(kOplogEntriesFetched);
    getMetrics()->onOplogEntriesApplied(kOplogEntriesApplied);
    advanceTime();
    // So far, the time to apply oplog entries equals `kTimerStep` milliseconds.
    checkMetrics(kTag, kTimerStep * (kOplogEntriesFetched / kOplogEntriesApplied - 1));
}

TEST_F(ReshardingMetricsTest, CurrentOpReportForDonor) {
    const auto kDonorState = DonorStateEnum::kPreparingToMirror;
    getMetrics()->onStart();
    advanceTime(Seconds(2));
    getMetrics()->setDonorState(kDonorState);
    advanceTime(Seconds(3));

    const ReshardingMetrics::ReporterOptions options(
        ReshardingMetrics::ReporterOptions::Role::kDonor,
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
                             "totalOperationTimeElapsed: 5,"
                             "remainingOperationTimeEstimated: -1,"
                             "countWritesDuringCriticalSection: 0,"
                             "totalCriticalSectionTimeElapsed : 3,"
                             "donorState: \"{4}\","
                             "opStatus: \"actively running\" }}",
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
    getMetrics()->onStart();
    advanceTime(kDelayBeforeCloning);

    constexpr auto kTimeSpentCloning = Seconds(3);
    getMetrics()->setRecipientState(kRecipientState);
    getMetrics()->setDocumentsToCopy(kDocumentsToCopy, kBytesToCopy);
    advanceTime(kTimeSpentCloning);
    getMetrics()->onDocumentsCopied(kDocumentsCopied, kBytesCopied);

    const auto kTimeToCopyRemainingSeconds =
        durationCount<Seconds>(kTimeSpentCloning) * (kBytesToCopy / kBytesCopied - 1);
    const auto kRemainingOperationTimeSeconds =
        durationCount<Seconds>(kTimeSpentCloning) + 2 * kTimeToCopyRemainingSeconds;

    const ReshardingMetrics::ReporterOptions options(
        ReshardingMetrics::ReporterOptions::Role::kRecipient,
        UUID::parse("12345678-1234-1234-1234-123456789def").getValue(),
        NamespaceString("db", "collection"),
        BSON("id" << 1),
        false);

    const auto expected =
        fromjson(fmt::format("{{ type: \"op\","
                             "desc: \"ReshardingRecipientService {0}\","
                             "op: \"command\","
                             "ns: \"{1}\","
                             "originatingCommand: {{ reshardCollection: \"{1}\","
                             "key: {2},"
                             "unique: {3},"
                             "collation: {{ locale: \"simple\" }} }},"
                             "totalOperationTimeElapsed: {4},"
                             "remainingOperationTimeEstimated: {5},"
                             "approxDocumentsToCopy: {6},"
                             "documentsCopied: {7},"
                             "approxBytesToCopy: {8},"
                             "bytesCopied: {9},"
                             "totalCopyTimeElapsed: {10},"
                             "oplogEntriesFetched: 0,"
                             "oplogEntriesApplied: 0,"
                             "totalApplyTimeElapsed: 0,"
                             "recipientState: \"{11}\","
                             "opStatus: \"actively running\" }}",
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

    const auto report = getMetrics()->reportForCurrentOp(options);
    ASSERT_BSONOBJ_EQ(expected, report);
}

}  // namespace mongo
