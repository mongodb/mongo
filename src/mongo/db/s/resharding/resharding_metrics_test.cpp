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
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <fmt/format.h>
#include <initializer_list>
#include <ratio>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/s/metrics/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/metrics/sharding_data_transform_metrics.h"
#include "mongo/db/s/metrics/sharding_data_transform_metrics_test_fixture.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/shard_id.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/clock_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using TimedPhase = ReshardingMetrics::TimedPhase;
constexpr auto kRunningTime = Seconds(12345);
constexpr auto kResharding = "resharding";
const auto kShardKey = BSON("newKey" << 1);

class ReshardingMetricsTest : public ShardingDataTransformMetricsTestFixture {

public:
    virtual std::unique_ptr<ShardingDataTransformCumulativeMetrics> initializeCumulativeMetrics()
        override {
        return std::make_unique<ReshardingCumulativeMetrics>();
    }

    std::unique_ptr<ReshardingMetrics> createInstanceMetrics(ClockSource* clockSource,
                                                             UUID instanceId = UUID::gen(),
                                                             Role role = Role::kDonor) {
        return std::make_unique<ReshardingMetrics>(instanceId,
                                                   BSON("y" << 1),
                                                   kTestNamespace,
                                                   role,
                                                   clockSource->now(),
                                                   clockSource,
                                                   getCumulativeMetrics());
    }

    virtual StringData getRootSectionName() override {
        return kResharding;
    }

    const UUID& getSourceCollectionId() {
        static UUID id = UUID::gen();
        return id;
    }

    template <typename T>
    BSONObj getReportFromStateDocument(T document) {
        auto metrics =
            ReshardingMetrics::initializeFrom(document, getClockSource(), getCumulativeMetrics());
        return metrics->reportForCurrentOp();
    }

    auto makeRecipientMetricsWithAmbiguousTimeRemaining() {
        auto doc = createRecipientDocument(RecipientStateEnum::kApplying, UUID::gen());
        ReshardingRecipientMetrics metricsDoc;
        ReshardingMetricsTimeInterval interval;
        interval.setStart(getClockSource()->now());
        metricsDoc.setOplogApplication(interval);
        doc.setMetrics(metricsDoc);
        return ReshardingMetrics::initializeFrom(doc, getClockSource(), getCumulativeMetrics());
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
                                              kTestNamespace.db_forTest(), getSourceCollectionId()),
                                          kShardKey};
        metadata.setStartTime(getClockSource()->now() - kRunningTime);
        return metadata;
    }

    void verifyCommonCurrentOpFields(const BSONObj& report) {
        ASSERT_EQ(report.getStringField("type"), "op");
        ASSERT_EQ(report.getStringField("op"), "command");
        auto originalCommand = report.getObjectField("originatingCommand");
        ASSERT_EQ(originalCommand.getStringField("reshardCollection"),
                  kTestNamespace.toString_forTest());
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
            ReshardingMetrics::initializeFrom(doc, getClockSource(), getCumulativeMetrics());

        clock->advance(kInterval);
        auto report = metrics->reportForCurrentOp();
        ASSERT_EQ(report.getIntField(fieldName), getExpectedDuration());

        clock->advance(kInterval);
        report = metrics->reportForCurrentOp();
        ASSERT_EQ(report.getIntField(fieldName), getExpectedDuration());
    }

    void createMetricsAndAssertIncrementsCumulativeMetricsField(
        const std::function<void(ReshardingMetrics*)>& mutate,
        Section section,
        StringData fieldName) {
        auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
        assertIncrementsCumulativeMetricsField(
            metrics.get(),
            [&](auto base) { mutate(dynamic_cast<ReshardingMetrics*>(base)); },
            section,
            fieldName);
    }
};

TEST_F(ReshardingMetricsTest, ReportForCurrentOpShouldHaveReshardingMetricsDescription) {
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
    ReshardingMetrics::ExternallyTrackedRecipientFields external;
    external.documentCountCopied = kDocsCopied;
    external.documentBytesCopied = kBytesCopied;

    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    metrics->restoreExternallyTrackedRecipientFields(external);
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

TEST_F(ReshardingMetricsTest, RestoresOngoingBuildIndexTimeFromRecipientStateDocument) {
    RAIIServerParameterControllerForTest controller("featureFlagReshardingImprovements", true);
    doRestoreOngoingPhaseTest<ReshardingRecipientMetrics, ReshardingRecipientDocument>(
        [this] { return createRecipientDocument(RecipientStateEnum::kBuildingIndex, UUID::gen()); },
        [this](auto& doc, auto interval) { doc.setIndexBuildTime(std::move(interval)); },
        "totalIndexBuildTimeElapsedSecs");
}

TEST_F(ReshardingMetricsTest, RestoresFinishedBuildIndexTimeFromRecipientStateDocument) {
    RAIIServerParameterControllerForTest controller("featureFlagReshardingImprovements", true);
    doRestoreCompletedPhaseTest<ReshardingRecipientMetrics, ReshardingRecipientDocument>(
        [this] { return createRecipientDocument(RecipientStateEnum::kApplying, UUID::gen()); },
        [this](auto& doc, auto interval) { doc.setIndexBuildTime(std::move(interval)); },
        "totalIndexBuildTimeElapsedSecs");
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

    ReshardingMetrics::ExternallyTrackedRecipientFields external;
    external.accumulateFrom(progressDoc);

    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    metrics->restoreExternallyTrackedRecipientFields(external);
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
    metrics->onOplogEntriesFetched(50);

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesFetched"), 50);
}

TEST_F(ReshardingMetricsTest, RecipientRestoreFetchedOplogEntries) {
    ReshardingMetrics::ExternallyTrackedRecipientFields external;

    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesFetched"), 0);

    external.oplogEntriesFetched = 100;
    metrics->restoreExternallyTrackedRecipientFields(external);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesFetched"), 100);

    external.oplogEntriesFetched = 50;
    metrics->restoreExternallyTrackedRecipientFields(external);
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
    metrics->setDocumentsToProcessCounts(0, kOpsPerIncrement * 4);
    metrics->onOplogEntriesFetched(kOpsPerIncrement * 4);

    // During cloning.
    metrics->setStartFor(TimedPhase::kCloning, getClockSource()->now());
    metrics->onDocumentsProcessed(0, kOpsPerIncrement, Milliseconds(1));
    clock->advance(kIncrement);
    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("remainingOperationTimeEstimatedSecs"),
              kExpectedTotal - kIncrementSecs);

    metrics->onDocumentsProcessed(0, kOpsPerIncrement * 2, Milliseconds(1));
    clock->advance(kIncrement * 2);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("remainingOperationTimeEstimatedSecs"),
              kExpectedTotal - (kIncrementSecs * 3));

    // During applying.
    metrics->onDocumentsProcessed(0, kOpsPerIncrement, Milliseconds(1));
    clock->advance(kIncrement);
    metrics->setEndFor(TimedPhase::kCloning, getClockSource()->now());
    metrics->setStartFor(TimedPhase::kApplying, getClockSource()->now());
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
    metrics->setEndFor(TimedPhase::kApplying, getClockSource()->now());
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("remainingOperationTimeEstimatedSecs"), 0);
}

TEST_F(ReshardingMetricsTest, RecipientRestoreAppliedOplogEntries) {
    ReshardingMetrics::ExternallyTrackedRecipientFields external;

    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 0);

    external.oplogEntriesApplied = 120;
    metrics->restoreExternallyTrackedRecipientFields(external);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 120);

    external.oplogEntriesApplied = 30;
    metrics->restoreExternallyTrackedRecipientFields(external);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 30);
}

TEST_F(ReshardingMetricsTest, CurrentOpReportsCopyingTime) {
    runTimeReportTest<ReshardingMetrics>(
        "CurrentOpReportsCopyingTime",
        {Role::kRecipient, Role::kCoordinator},
        "totalCopyTimeElapsedSecs",
        [this](ReshardingMetrics* metrics) {
            metrics->setStartFor(TimedPhase::kCloning, getClockSource()->now());
        },
        [this](ReshardingMetrics* metrics) {
            metrics->setEndFor(TimedPhase::kCloning, getClockSource()->now());
        });
}

TEST_F(ReshardingMetricsTest, CurrentOpReportsBuildIndexTime) {
    RAIIServerParameterControllerForTest controller("featureFlagReshardingImprovements", true);
    runTimeReportTest<ReshardingMetrics>(
        "CurrentOpReportsBuildIndexTime",
        {Role::kRecipient},
        "totalIndexBuildTimeElapsedSecs",
        [this](ReshardingMetrics* metrics) {
            metrics->setStartFor(TimedPhase::kBuildingIndex, getClockSource()->now());
        },
        [this](ReshardingMetrics* metrics) {
            metrics->setEndFor(TimedPhase::kBuildingIndex, getClockSource()->now());
        });
}

TEST_F(ReshardingMetricsTest, CurrentOpReportsApplyingTime) {
    runTimeReportTest<ReshardingMetrics>(
        "CurrentOpReportsApplyingTime",
        {Role::kRecipient, Role::kCoordinator},
        "totalApplyTimeElapsedSecs",
        [this](ReshardingMetrics* metrics) {
            metrics->setStartFor(TimedPhase::kApplying, getClockSource()->now());
        },
        [this](ReshardingMetrics* metrics) {
            metrics->setEndFor(TimedPhase::kApplying, getClockSource()->now());
        });
}

TEST_F(ReshardingMetricsTest, CurrentOpReportsCriticalSectionTime) {
    runTimeReportTest<ReshardingMetrics>(
        "CurrentOpReportsCriticalSectionTime",
        {Role::kDonor, Role::kCoordinator},
        "totalCriticalSectionTimeElapsedSecs",
        [this](ReshardingMetrics* metrics) {
            metrics->setStartFor(TimedPhase::kCriticalSection, getClockSource()->now());
        },
        [this](ReshardingMetrics* metrics) {
            metrics->setEndFor(TimedPhase::kCriticalSection, getClockSource()->now());
        });
}

TEST_F(ReshardingMetricsTest, RecipientEstimatesNoneOnNewInstance) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    ASSERT_EQ(metrics->getHighEstimateRemainingTimeMillis(), boost::none);
}

TEST_F(ReshardingMetricsTest,
       RecipientEstimatesNoneBeforeExternalFieldsRestoredForRestoredInstance) {
    auto metrics = makeRecipientMetricsWithAmbiguousTimeRemaining();
    ASSERT_EQ(metrics->getHighEstimateRemainingTimeMillis(), boost::none);
}

TEST_F(ReshardingMetricsTest, RecipientEstimatesAfterExternalFieldsRestoredForRestoredInstance) {
    auto metrics = makeRecipientMetricsWithAmbiguousTimeRemaining();
    metrics->restoreExternallyTrackedRecipientFields(
        ReshardingMetrics::ExternallyTrackedRecipientFields{});
    ASSERT_EQ(metrics->getHighEstimateRemainingTimeMillis(), Milliseconds{0});
}

TEST_F(ReshardingMetricsTest, CurrentOpDoesNotReportRecipientEstimateIfNotSet) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    auto report = metrics->reportForCurrentOp();
    ASSERT_FALSE(report.hasField("remainingOperationTimeEstimatedSecs"));
}

TEST_F(ReshardingMetricsTest, OnInsertAppliedIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onInsertApplied(); }, Section::kActive, "insertsApplied");
}

TEST_F(ReshardingMetricsTest, OnUpdateAppliedIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onUpdateApplied(); }, Section::kActive, "updatesApplied");
}

TEST_F(ReshardingMetricsTest, OnDeleteAppliedIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onDeleteApplied(); }, Section::kActive, "deletesApplied");
}

TEST_F(ReshardingMetricsTest, OnOplogFetchedIncrementsCumulativeMetricsFetchedCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onOplogEntriesFetched(1); },
        Section::kActive,
        "oplogEntriesFetched");
}

TEST_F(ReshardingMetricsTest,
       OnBatchRetrievedDuringOplogFetchingIncrementsCumulativeMetricsFetchedBatchCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onBatchRetrievedDuringOplogFetching(Milliseconds{0}); },
        Section::kLatencies,
        "oplogFetchingTotalRemoteBatchesRetrieved");
}

TEST_F(ReshardingMetricsTest,
       OnBatchRetrievedDuringOplogFetchingIncrementsCumulativeMetricsFetchedBatchTime) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onBatchRetrievedDuringOplogFetching(Milliseconds{1}); },
        Section::kLatencies,
        "oplogFetchingTotalRemoteBatchRetrievalTimeMillis");
}

TEST_F(ReshardingMetricsTest, OnLocalFetchingInsertIncrementsCumulativeMetricsFetchedBatchCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onLocalInsertDuringOplogFetching(Milliseconds{0}); },
        Section::kLatencies,
        "oplogFetchingTotalLocalInserts");
}

TEST_F(ReshardingMetricsTest, OnLocalFetchingInsertIncrementsCumulativeMetricsFetchedBatchTime) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onLocalInsertDuringOplogFetching(Milliseconds{1}); },
        Section::kLatencies,
        "oplogFetchingTotalLocalInsertTimeMillis");
}

TEST_F(ReshardingMetricsTest, OnOplogAppliedIncrementsCumulativeMetricsAppliedCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onOplogEntriesApplied(1); },
        Section::kActive,
        "oplogEntriesApplied");
}

TEST_F(ReshardingMetricsTest, OnApplyingBatchRetrievedIncrementsCumulativeMetricsBatchCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onBatchRetrievedDuringOplogApplying(Milliseconds{0}); },
        Section::kLatencies,
        "oplogApplyingTotalLocalBatchesRetrieved");
}

TEST_F(ReshardingMetricsTest, OnApplyingBatchRetrievedIncrementsCumulativeMetricsBatchTime) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onBatchRetrievedDuringOplogApplying(Milliseconds{1}); },
        Section::kLatencies,
        "oplogApplyingTotalLocalBatchRetrievalTimeMillis");
}

TEST_F(ReshardingMetricsTest, OnApplyingBatchAppliedIncrementsCumulativeMetricsBatchCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onOplogLocalBatchApplied(Milliseconds{0}); },
        Section::kLatencies,
        "oplogApplyingTotalLocalBatchesApplied");
}

TEST_F(ReshardingMetricsTest, OnApplyingBatchAppliedIncrementsCumulativeMetricsBatchTime) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onOplogLocalBatchApplied(Milliseconds{1}); },
        Section::kLatencies,
        "oplogApplyingTotalLocalBatchApplyTimeMillis");
}

TEST_F(ReshardingMetricsTest, OnStateTransitionFromNoneInformsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) {
            metrics->onStateTransition(boost::none, CoordinatorStateEnum::kApplying);
        },
        Section::kCurrentInSteps,
        "countInstancesInCoordinatorState4Applying");
}

TEST_F(ReshardingMetricsTest, OnStateTransitionToNoneInformsCumulativeMetrics) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    auto state = CoordinatorStateEnum::kApplying;
    metrics->onStateTransition(boost::none, state);
    assertDecrementsCumulativeMetricsField(
        metrics.get(),
        [=](auto base) {
            auto reshardingMetrics = dynamic_cast<ReshardingMetrics*>(base);
            reshardingMetrics->onStateTransition(state, boost::none);
        },
        Section::kCurrentInSteps,
        "countInstancesInCoordinatorState4Applying");
}

TEST_F(ReshardingMetricsTest, OnStateTransitionInformsCumulativeMetrics) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    auto initialState = CoordinatorStateEnum::kApplying;
    auto nextState = CoordinatorStateEnum::kBlockingWrites;
    metrics->onStateTransition(boost::none, initialState);
    assertAltersCumulativeMetrics(
        metrics.get(),
        [=](auto base) {
            auto reshardingMetrics = dynamic_cast<ReshardingMetrics*>(base);
            reshardingMetrics->onStateTransition(initialState, nextState);
        },
        [this](auto reportBefore, auto reportAfter) {
            auto before = getReportSection(reportBefore, Section::kCurrentInSteps);
            ASSERT_EQ(before.getIntField("countInstancesInCoordinatorState4Applying"), 1);
            ASSERT_EQ(before.getIntField("countInstancesInCoordinatorState5BlockingWrites"), 0);
            auto after = getReportSection(reportAfter, Section::kCurrentInSteps);
            ASSERT_EQ(after.getIntField("countInstancesInCoordinatorState4Applying"), 0);
            ASSERT_EQ(after.getIntField("countInstancesInCoordinatorState5BlockingWrites"), 1);
            return true;
        });
}

TEST_F(ReshardingMetricsTest, onSameKeyResharding) {
    RAIIServerParameterControllerForTest controller("featureFlagReshardingImprovements", true);
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getBoolField("isSameKeyResharding"), false);
    metrics->setIsSameKeyResharding(true);

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getBoolField("isSameKeyResharding"), true);
}

TEST_F(ReshardingMetricsTest, onIndexBuild) {
    RAIIServerParameterControllerForTest controller("featureFlagReshardingImprovements", true);
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("indexesToBuild"), 0);
    ASSERT_EQ(report.getIntField("indexesBuilt"), 0);
    metrics->setIndexesToBuild(2);
    metrics->setIndexesBuilt(1);

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("indexesToBuild"), 2);
    ASSERT_EQ(report.getIntField("indexesBuilt"), 1);
}

}  // namespace
}  // namespace mongo
