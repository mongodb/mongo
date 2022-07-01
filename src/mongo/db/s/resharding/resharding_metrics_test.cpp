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

#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics_test_fixture.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

constexpr auto kRunningTime = Seconds(12345);
const auto kShardKey = BSON("newKey" << 1);

class ReshardingMetricsTest : public ShardingDataTransformMetricsTestFixture {

public:
    std::unique_ptr<ReshardingMetrics> createInstanceMetrics(ClockSource* clockSource,
                                                             UUID instanceId = UUID::gen(),
                                                             Role role = Role::kDonor) {
        return std::make_unique<ReshardingMetrics>(instanceId,
                                                   BSON("y" << 1),
                                                   kTestNamespace,
                                                   role,
                                                   clockSource->now(),
                                                   clockSource,
                                                   &_cumulativeMetrics);
    }

    const UUID& getSourceCollectionId() {
        static UUID id = UUID::gen();
        return id;
    }

    template <typename T>
    BSONObj getReportFromStateDocument(T document) {
        auto metrics =
            ReshardingMetrics::initializeFrom(document, getClockSource(), &_cumulativeMetrics);
        return metrics->reportForCurrentOp();
    }

    ReshardingRecipientDocument createRecipientDocument(RecipientStateEnum state,
                                                        const UUID& operationId) {
        RecipientShardContext recipientCtx;
        recipientCtx.setState(state);
        ReshardingRecipientDocument doc{std::move(recipientCtx), {ShardId{"donor1"}}, 5};
        doc.setCommonReshardingMetadata(createCommonReshardingMetadata(operationId));
        return doc;
    }

    ReshardingDonorDocument createDonorDocument(DonorStateEnum state, const UUID& operationId) {
        DonorShardContext donorCtx;
        donorCtx.setState(state);
        ReshardingDonorDocument doc{std::move(donorCtx), {ShardId{"recipient1"}}};
        doc.setCommonReshardingMetadata(createCommonReshardingMetadata(operationId));
        return doc;
    }

    ReshardingCoordinatorDocument createCoordinatorDocument(CoordinatorStateEnum state,
                                                            const UUID& operationId) {
        ReshardingCoordinatorDocument doc{state, {}, {}};
        doc.setCommonReshardingMetadata(createCommonReshardingMetadata(operationId));
        return doc;
    }

    CommonReshardingMetadata createCommonReshardingMetadata(const UUID& operationId) {
        CommonReshardingMetadata metadata{operationId,
                                          kTestNamespace,
                                          getSourceCollectionId(),
                                          resharding::constructTemporaryReshardingNss(
                                              kTestNamespace.db(), getSourceCollectionId()),
                                          kShardKey};
        metadata.setStartTime(getClockSource()->now() - kRunningTime);
        return metadata;
    }

    void verifyCommonCurrentOpFields(const BSONObj& report) {
        ASSERT_EQ(report.getStringField("type"), "op");
        ASSERT_EQ(report.getStringField("op"), "command");
        auto originalCommand = report.getObjectField("originatingCommand");
        ASSERT_EQ(originalCommand.getStringField("reshardCollection"), kTestNamespace.toString());
        ASSERT_EQ(originalCommand.getObjectField("key").woCompare(kShardKey), 0);
        ASSERT_EQ(originalCommand.getStringField("unique"), "false");
        ASSERT_EQ(originalCommand.getObjectField("collation")
                      .woCompare(BSON("locale"
                                      << "simple")),
                  0);
        ASSERT_EQ(report.getIntField("totalOperationTimeElapsedSecs"), kRunningTime.count());
    }

    template <typename MetricsDocument, typename Document>
    void doRestoreOngoingPhaseTest(
        const std::function<Document()>& createDocument,
        const std::function<void(MetricsDocument&, ReshardingMetricsTimeInterval)>& setInterval,
        const std::string& fieldName) {
        doRestorePhaseTestImpl<MetricsDocument>(createDocument, setInterval, fieldName, false);
    }

    template <typename MetricsDocument, typename Document>
    void doRestoreCompletedPhaseTest(
        const std::function<Document()>& createDocument,
        const std::function<void(MetricsDocument&, ReshardingMetricsTimeInterval)>& setInterval,
        const std::string& fieldName) {
        doRestorePhaseTestImpl<MetricsDocument>(createDocument, setInterval, fieldName, true);
    }

    template <typename MetricsDocument, typename Document>
    void doRestorePhaseTestImpl(
        const std::function<Document()>& createDocument,
        const std::function<void(MetricsDocument&, ReshardingMetricsTimeInterval)>& setInterval,
        const std::string& fieldName,
        bool completed) {
        constexpr auto kInterval = Milliseconds{5000};
        auto clock = getClockSource();
        const auto start = clock->now();
        boost::optional<long long> finishedPhaseDuration;

        auto getExpectedDuration = [&] {
            if (finishedPhaseDuration) {
                return *finishedPhaseDuration;
            }
            return durationCount<Seconds>(clock->now() - start);
        };

        ReshardingMetricsTimeInterval interval;
        interval.setStart(start);
        if (completed) {
            clock->advance(kInterval);
            interval.setStop(clock->now());
            finishedPhaseDuration = durationCount<Seconds>(kInterval);
        }
        MetricsDocument metricsDoc;
        setInterval(metricsDoc, std::move(interval));
        auto doc = createDocument();
        doc.setMetrics(metricsDoc);

        auto metrics =
            ReshardingMetrics::initializeFrom(doc, getClockSource(), &_cumulativeMetrics);

        clock->advance(kInterval);
        auto report = metrics->reportForCurrentOp();
        ASSERT_EQ(report.getIntField(fieldName), getExpectedDuration());

        clock->advance(kInterval);
        report = metrics->reportForCurrentOp();
        ASSERT_EQ(report.getIntField(fieldName), getExpectedDuration());
    }
};


TEST_F(ReshardingMetricsTest, ReportForCurrentOpShouldHaveGlobalIndexDescription) {
    std::vector<Role> roles{Role::kCoordinator, Role::kDonor, Role::kRecipient};

    std::for_each(roles.begin(), roles.end(), [&](Role role) {
        auto instanceId = UUID::gen();
        auto metrics = createInstanceMetrics(getClockSource(), instanceId, role);
        auto report = metrics->reportForCurrentOp();

        ASSERT_EQ(report.getStringField("desc").toString(),
                  fmt::format("ReshardingMetrics{}Service {}",
                              ShardingDataTransformMetrics::getRoleName(role),
                              instanceId.toString()));
    });
}

TEST_F(ReshardingMetricsTest, RestoresGeneralFieldsFromRecipientStateDocument) {
    auto state = RecipientStateEnum::kAwaitingFetchTimestamp;
    auto opId = UUID::gen();
    auto report = getReportFromStateDocument(createRecipientDocument(state, opId));

    verifyCommonCurrentOpFields(report);
    ASSERT_EQ(report.getStringField("desc"),
              "ReshardingMetricsRecipientService " + opId.toString());
    ASSERT_EQ(report.getStringField("recipientState").toString(), RecipientState_serializer(state));
}

TEST_F(ReshardingMetricsTest, RestoresByteAndDocumentCountsFromRecipientStateDocument) {
    constexpr auto kDocsToCopy = 100;
    constexpr auto kBytesToCopy = 1000;
    constexpr auto kDocsCopied = 101;
    constexpr auto kBytesCopied = 1001;
    ReshardingRecipientMetrics metrics;
    metrics.setApproxDocumentsToCopy(kDocsToCopy);
    metrics.setApproxBytesToCopy(kBytesToCopy);
    metrics.setFinalBytesCopiedCount(kBytesCopied);
    metrics.setFinalDocumentsCopiedCount(kDocsCopied);
    auto doc = createRecipientDocument(RecipientStateEnum::kApplying, UUID::gen());
    doc.setMetrics(metrics);
    auto report = getReportFromStateDocument(std::move(doc));

    ASSERT_EQ(report.getIntField("approxDocumentsToCopy"), kDocsToCopy);
    ASSERT_EQ(report.getIntField("approxBytesToCopy"), kBytesToCopy);
    ASSERT_EQ(report.getIntField("documentsCopied"), kDocsCopied);
    ASSERT_EQ(report.getIntField("bytesCopied"), kBytesCopied);
}

TEST_F(ReshardingMetricsTest, RestoresByteAndDocumentCountsDuringCloning) {
    constexpr auto kDocsCopied = 50;
    constexpr auto kBytesCopied = 500;

    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    metrics->restoreDocumentsCopied(kDocsCopied, kBytesCopied);
    auto report = metrics->reportForCurrentOp();

    ASSERT_EQ(report.getIntField("documentsCopied"), kDocsCopied);
    ASSERT_EQ(report.getIntField("bytesCopied"), kBytesCopied);
}

TEST_F(ReshardingMetricsTest, RestoresOngoingCloningTimeFromRecipientStateDocument) {
    doRestoreOngoingPhaseTest<ReshardingRecipientMetrics, ReshardingRecipientDocument>(
        [this] { return createRecipientDocument(RecipientStateEnum::kCloning, UUID::gen()); },
        [this](auto& doc, auto interval) { doc.setDocumentCopy(std::move(interval)); },
        "totalCopyTimeElapsedSecs");
}

TEST_F(ReshardingMetricsTest, RestoresFinishedCloningTimeFromRecipientStateDocument) {
    doRestoreCompletedPhaseTest<ReshardingRecipientMetrics, ReshardingRecipientDocument>(
        [this] { return createRecipientDocument(RecipientStateEnum::kApplying, UUID::gen()); },
        [this](auto& doc, auto interval) { doc.setDocumentCopy(std::move(interval)); },
        "totalCopyTimeElapsedSecs");
}

TEST_F(ReshardingMetricsTest, RestoresOngoingApplyingTimeFromRecipientStateDocument) {
    doRestoreOngoingPhaseTest<ReshardingRecipientMetrics, ReshardingRecipientDocument>(
        [this] { return createRecipientDocument(RecipientStateEnum::kApplying, UUID::gen()); },
        [this](auto& doc, auto interval) { doc.setOplogApplication(std::move(interval)); },
        "totalApplyTimeElapsedSecs");
}

TEST_F(ReshardingMetricsTest, RestoresFinishedApplyingTimeFromRecipientStateDocument) {
    doRestoreCompletedPhaseTest<ReshardingRecipientMetrics, ReshardingRecipientDocument>(
        [this] {
            return createRecipientDocument(RecipientStateEnum::kStrictConsistency, UUID::gen());
        },
        [this](auto& doc, auto interval) { doc.setOplogApplication(std::move(interval)); },
        "totalApplyTimeElapsedSecs");
}

TEST_F(ReshardingMetricsTest, RestoresGeneralFieldsFromDonorStateDocument) {
    auto state = DonorStateEnum::kDonatingInitialData;
    auto opId = UUID::gen();
    auto report = getReportFromStateDocument(createDonorDocument(state, opId));

    verifyCommonCurrentOpFields(report);
    ASSERT_EQ(report.getStringField("desc"), "ReshardingMetricsDonorService " + opId.toString());
    ASSERT_EQ(report.getStringField("donorState").toString(), DonorState_serializer(state));
}

TEST_F(ReshardingMetricsTest, RestoresGeneralFieldsFromCoordinatorStateDocument) {
    auto state = CoordinatorStateEnum::kPreparingToDonate;
    auto opId = UUID::gen();
    auto report = getReportFromStateDocument(createCoordinatorDocument(state, opId));

    verifyCommonCurrentOpFields(report);
    ASSERT_EQ(report.getStringField("desc"),
              "ReshardingMetricsCoordinatorService " + opId.toString());
    ASSERT_EQ(report.getStringField("coordinatorState").toString(),
              CoordinatorState_serializer(state));
}

TEST_F(ReshardingMetricsTest, RestoresFromReshardingApplierProgressDocument) {
    ReshardingOplogApplierProgress progressDoc;
    progressDoc.setInsertsApplied(123);
    progressDoc.setUpdatesApplied(456);
    progressDoc.setDeletesApplied(789);
    progressDoc.setWritesToStashCollections(800);

    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    metrics->accumulateFrom(progressDoc);
    auto report = metrics->reportForCurrentOp();

    ASSERT_EQ(report.getIntField("insertsApplied"), 123);
    ASSERT_EQ(report.getIntField("updatesApplied"), 456);
    ASSERT_EQ(report.getIntField("deletesApplied"), 789);
    ASSERT_EQ(report.getIntField("countWritesToStashCollections"), 800);
}

TEST_F(ReshardingMetricsTest, RestoresOngoingCloningTimeFromCoordinatorStateDocument) {
    doRestoreOngoingPhaseTest<ReshardingCoordinatorMetrics, ReshardingCoordinatorDocument>(
        [this] { return createCoordinatorDocument(CoordinatorStateEnum::kCloning, UUID::gen()); },
        [this](auto& doc, auto interval) { doc.setDocumentCopy(std::move(interval)); },
        "totalCopyTimeElapsedSecs");
}

TEST_F(ReshardingMetricsTest, RestoresFinishedCloningTimeFromCoordinatorStateDocument) {
    doRestoreCompletedPhaseTest<ReshardingCoordinatorMetrics, ReshardingCoordinatorDocument>(
        [this] { return createCoordinatorDocument(CoordinatorStateEnum::kApplying, UUID::gen()); },
        [this](auto& doc, auto interval) { doc.setDocumentCopy(std::move(interval)); },
        "totalCopyTimeElapsedSecs");
}

TEST_F(ReshardingMetricsTest, RestoresOngoingApplyingTimeFromCoordinatorStateDocument) {
    doRestoreOngoingPhaseTest<ReshardingCoordinatorMetrics, ReshardingCoordinatorDocument>(
        [this] { return createCoordinatorDocument(CoordinatorStateEnum::kApplying, UUID::gen()); },
        [this](auto& doc, auto interval) { doc.setOplogApplication(std::move(interval)); },
        "totalApplyTimeElapsedSecs");
}

TEST_F(ReshardingMetricsTest, RestoresFinishedApplyingTimeFromCoordinatorStateDocument) {
    doRestoreCompletedPhaseTest<ReshardingCoordinatorMetrics, ReshardingCoordinatorDocument>(
        [this] {
            return createCoordinatorDocument(CoordinatorStateEnum::kBlockingWrites, UUID::gen());
        },
        [this](auto& doc, auto interval) { doc.setOplogApplication(std::move(interval)); },
        "totalApplyTimeElapsedSecs");
}

TEST_F(ReshardingMetricsTest, OnInsertAppliedShouldIncrementInsertsApplied) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("insertsApplied"), 0);
    metrics->onInsertApplied();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("insertsApplied"), 1);
}

TEST_F(ReshardingMetricsTest, OnUpdateAppliedShouldIncrementUpdatesApplied) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("updatesApplied"), 0);
    metrics->onUpdateApplied();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("updatesApplied"), 1);
}

TEST_F(ReshardingMetricsTest, OnDeleteAppliedShouldIncrementDeletesApplied) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("deletesApplied"), 0);
    metrics->onDeleteApplied();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("deletesApplied"), 1);
}

TEST_F(ReshardingMetricsTest, OnOplogsEntriesAppliedShouldIncrementOplogsEntriesApplied) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 0);
    metrics->onOplogEntriesApplied(100);

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 100);
}

TEST_F(ReshardingMetricsTest, RecipientIncrementFetchedOplogEntries) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesFetched"), 0);
    metrics->onOplogEntriesFetched(50, Milliseconds(1));

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesFetched"), 50);
}

TEST_F(ReshardingMetricsTest, RecipientRestoreFetchedOplogEntries) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesFetched"), 0);

    metrics->restoreOplogEntriesFetched(100);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesFetched"), 100);

    metrics->restoreOplogEntriesFetched(50);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesFetched"), 50);
}

TEST_F(ReshardingMetricsTest, RecipientReportsRemainingTime) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
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

TEST_F(ReshardingMetricsTest, RecipientRestoreAppliedOplogEntries) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 0);

    metrics->restoreOplogEntriesApplied(120);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 120);

    metrics->restoreOplogEntriesApplied(30);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 30);
}

TEST_F(ReshardingMetricsTest, CurrentOpReportsApplyingTime) {
    runTimeReportTest<ReshardingMetrics>(
        "CurrentOpReportsApplyingTime",
        {Role::kRecipient, Role::kCoordinator},
        "totalApplyTimeElapsedSecs",
        [](ReshardingMetrics* metrics) { metrics->onApplyingBegin(); },
        [](ReshardingMetrics* metrics) { metrics->onApplyingEnd(); });
}

}  // namespace
}  // namespace mongo
