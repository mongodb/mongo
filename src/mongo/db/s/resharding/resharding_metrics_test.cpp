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

#include "mongo/db/s/resharding/resharding_metrics.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/resharding_metrics_test_fixture.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"

#include <algorithm>
#include <initializer_list>
#include <ratio>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using TimedPhase = ReshardingMetrics::TimedPhase;
constexpr auto kRunningTime = Seconds(12345);
constexpr auto kResharding = "resharding";
const auto kShardKey = BSON("newKey" << 1);

class ReshardingMetricsTest : public ReshardingMetricsTestFixture {

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
                                                   getCumulativeMetrics(),
                                                   ReshardingMetrics::getDefaultState(role),
                                                   ReshardingProvenanceEnum::kReshardCollection);
    }

    StringData getRootSectionName() override {
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
        CommonReshardingMetadata metadata{
            operationId,
            kTestNamespace,
            getSourceCollectionId(),
            resharding::constructTemporaryReshardingNss(kTestNamespace, getSourceCollectionId()),
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
        ASSERT_EQ(originalCommand.getObjectField("collation").woCompare(BSON("locale" << "simple")),
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

    void validateEstimatedRemainingTimeIfDisableMovingAverage(
        ReshardingMetrics* metrics, boost::optional<Milliseconds> expectedEstimate) {
        _validateEstimatedRemainingTime(
            metrics, false /* enableEstimateBasedOnMovingAvg */, expectedEstimate);
    }

    void validateEstimatedRemainingTimeIfEnableMovingAverage(
        ReshardingMetrics* metrics, boost::optional<Milliseconds> expectedEstimate) {
        _validateEstimatedRemainingTime(
            metrics, true /* enableEstimateBasedOnMovingAvg */, expectedEstimate);
    }

private:
    void _validateEstimatedRemainingTime(ReshardingMetrics* metrics,
                                         bool enableEstimateBasedOnMovingAvg,
                                         boost::optional<Milliseconds> expectedEstimate) {
        LOGV2(10626601,
              "Validating estimated remaining time",
              "enableEstimateBasedOnMovingAvg"_attr = enableEstimateBasedOnMovingAvg,
              "expectedEstimate"_attr = expectedEstimate);

        const RAIIServerParameterControllerForTest estimateBasedOnMovingAvgServerParameter{
            "reshardingRemainingTimeEstimateBasedOnMovingAverage", enableEstimateBasedOnMovingAvg};

        ASSERT_EQ(metrics->getHighEstimateRemainingTimeMillis(
                      ReshardingMetrics::CalculationLogOption::Show),
                  expectedEstimate);
        auto report = metrics->reportForCurrentOp();
        if (expectedEstimate.has_value()) {
            ASSERT_EQ(report.getIntField("remainingOperationTimeEstimatedSecs"),
                      duration_cast<Seconds>(*expectedEstimate).count());
        } else {
            ASSERT_FALSE(report.hasField("remainingOperationTimeEstimatedSecs"));
        }
    };

protected:
    ShardId shardId0{"shard0"};
    ShardId shardId1{"shard1"};
};

class ReshardingMetricsWithObserverMock {
public:
    ReshardingMetricsWithObserverMock(Date_t startTime,
                                      int64_t timeRemaining,
                                      ClockSource* clockSource,
                                      ReshardingCumulativeMetrics* cumulativeMetrics)
        : _impl{UUID::gen(),
                BSON("command" << "test"),
                NamespaceString::createNamespaceString_forTest("test.source"),
                ReshardingMetrics::Role::kDonor,
                startTime,
                clockSource,
                cumulativeMetrics,
                ReshardingMetrics::getDefaultState(ReshardingMetrics::Role::kDonor),
                std::make_unique<ObserverMock>(startTime, timeRemaining),
                ReshardingProvenanceEnum::kReshardCollection} {}


private:
    ReshardingMetrics _impl;
};

TEST_F(ReshardingMetricsTest, ReportForCurrentOpShouldHaveReshardingMetricsDescription) {
    std::vector<Role> roles{Role::kCoordinator, Role::kDonor, Role::kRecipient};

    std::for_each(roles.begin(), roles.end(), [&](Role role) {
        auto instanceId = UUID::gen();
        auto metrics = createInstanceMetrics(getClockSource(), instanceId, role);
        auto report = metrics->reportForCurrentOp();

        ASSERT_EQ(report.getStringField("desc"),
                  fmt::format("ReshardingMetrics{}Service {}",
                              ReshardingMetricsCommon::getRoleName(role),
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
    ASSERT_EQ(report.getStringField("recipientState"), RecipientState_serializer(state));
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
    doRestoreOngoingPhaseTest<ReshardingRecipientMetrics, ReshardingRecipientDocument>(
        [this] { return createRecipientDocument(RecipientStateEnum::kBuildingIndex, UUID::gen()); },
        [this](auto& doc, auto interval) { doc.setIndexBuildTime(std::move(interval)); },
        "totalIndexBuildTimeElapsedSecs");
}

TEST_F(ReshardingMetricsTest, RestoresFinishedBuildIndexTimeFromRecipientStateDocument) {
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
    ASSERT_EQ(report.getStringField("donorState"), DonorState_serializer(state));
}

TEST_F(ReshardingMetricsTest, RestoresGeneralFieldsFromCoordinatorStateDocument) {
    auto state = CoordinatorStateEnum::kPreparingToDonate;
    auto opId = UUID::gen();
    auto report = getReportFromStateDocument(createCoordinatorDocument(state, opId));

    verifyCommonCurrentOpFields(report);
    ASSERT_EQ(report.getStringField("desc"),
              "ReshardingMetricsCoordinatorService " + opId.toString());
    ASSERT_EQ(report.getStringField("coordinatorState"), CoordinatorState_serializer(state));
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

TEST_F(ReshardingMetricsTest, RecipientCanRegisterDonors) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    metrics->registerDonors({shardId0});
}

DEATH_TEST_REGEX_F(ReshardingMetricsTest,
                   DonorCannotRegisterDonors,
                   "Tripwire assertion.*10626500") {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kDonor);
    metrics->registerDonors({shardId0});
}

DEATH_TEST_REGEX_F(ReshardingMetricsTest,
                   CoordinatorCannotRegisterDonors,
                   "Tripwire assertion.*10626500") {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    metrics->registerDonors({shardId0});
}

DEATH_TEST_REGEX_F(ReshardingMetricsTest,
                   CannotRegisterDonorsMultipleTimes,
                   "Tripwire assertion.*10626501") {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    metrics->registerDonors({shardId0});
    metrics->registerDonors({shardId0});
}

TEST_F(ReshardingMetricsTest, CannotGetAverageTimeToFetchForUnregisteredDonor) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    ASSERT_THROWS_CODE(metrics->getAverageTimeToFetchOplogEntries(shardId0), DBException, 10626503);
}

TEST_F(ReshardingMetricsTest, CannotUpdateAverageTimeToFetchForUnregisteredDonor) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    auto timeToFetch0 = Milliseconds(100);
    ASSERT_THROWS_CODE(metrics->updateAverageTimeToFetchOplogEntries(shardId0, timeToFetch0),
                       DBException,
                       10626502);
}

TEST_F(ReshardingMetricsTest, GetAndUpdateAverageTimeToFetchBasic) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    metrics->registerDonors({shardId0, shardId1});

    auto smoothingFactor = 0.7;
    const RAIIServerParameterControllerForTest smoothingFactorServerParameter{
        "reshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor", smoothingFactor};

    ASSERT_FALSE(metrics->getAverageTimeToFetchOplogEntries(shardId0));
    ASSERT_FALSE(metrics->getAverageTimeToFetchOplogEntries(shardId1));

    // Update the average for shard0 only.

    auto shard0TimeToFetch0 = Milliseconds(100);
    metrics->updateAverageTimeToFetchOplogEntries(shardId0, shard0TimeToFetch0);

    auto expectedShard0AvgTimeToFetch0 = shard0TimeToFetch0;
    ASSERT_EQ(metrics->getAverageTimeToFetchOplogEntries(shardId0), expectedShard0AvgTimeToFetch0);
    ASSERT_FALSE(metrics->getAverageTimeToFetchOplogEntries(shardId1));

    auto shard0TimeToFetch1 = Milliseconds(50);
    metrics->updateAverageTimeToFetchOplogEntries(shardId0, shard0TimeToFetch1);

    auto expectedShard0AvgTimeToFetch1 =
        Milliseconds((int)resharding::calculateExponentialMovingAverage(
            expectedShard0AvgTimeToFetch0.count(), shard0TimeToFetch1.count(), smoothingFactor));
    ASSERT_EQ(metrics->getAverageTimeToFetchOplogEntries(shardId0), expectedShard0AvgTimeToFetch1);
    ASSERT_FALSE(metrics->getAverageTimeToFetchOplogEntries(shardId1));

    // Update the average for shard1 only.

    auto shard1TimeToFetch0 = Milliseconds(1000);
    metrics->updateAverageTimeToFetchOplogEntries(shardId1, shard1TimeToFetch0);

    auto expectedShard1AvgTimeToFetch0 = shard1TimeToFetch0;
    ASSERT_EQ(metrics->getAverageTimeToFetchOplogEntries(shardId0), expectedShard0AvgTimeToFetch1);
    ASSERT_EQ(metrics->getAverageTimeToFetchOplogEntries(shardId1), expectedShard1AvgTimeToFetch0);

    auto shard1TimeToFetch1 = Milliseconds(5);
    metrics->updateAverageTimeToFetchOplogEntries(shardId1, shard1TimeToFetch1);

    auto expectedShard1AvgTimeToFetch1 =
        Milliseconds((int)resharding::calculateExponentialMovingAverage(
            expectedShard1AvgTimeToFetch0.count(), shard1TimeToFetch1.count(), smoothingFactor));
    ASSERT_EQ(metrics->getAverageTimeToFetchOplogEntries(shardId0), expectedShard0AvgTimeToFetch1);
    ASSERT_EQ(metrics->getAverageTimeToFetchOplogEntries(shardId1), expectedShard1AvgTimeToFetch1);
}

TEST_F(ReshardingMetricsTest, CannotGetAverageTimeToApplyForUnregisteredDonor) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    ASSERT_THROWS_CODE(metrics->getAverageTimeToApplyOplogEntries(shardId0), DBException, 10626505);
}

TEST_F(ReshardingMetricsTest, CannotUpdateAverageTimeToApplyForUnregisteredDonor) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    auto timeToApply0 = Milliseconds(100);
    ASSERT_THROWS_CODE(metrics->updateAverageTimeToApplyOplogEntries(shardId0, timeToApply0),
                       DBException,
                       10626504);
}

TEST_F(ReshardingMetricsTest, GetAndUpdateAverageTimeToApplyBasic) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    metrics->registerDonors({shardId0, shardId1});

    auto smoothingFactor = 0.8;
    const RAIIServerParameterControllerForTest smoothingFactorServerParameter{
        "reshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor", smoothingFactor};

    ASSERT_FALSE(metrics->getAverageTimeToApplyOplogEntries(shardId0));
    ASSERT_FALSE(metrics->getAverageTimeToApplyOplogEntries(shardId1));

    // Update the average for shard0 only.

    auto shard0TimeToApply0 = Milliseconds(100);
    metrics->updateAverageTimeToApplyOplogEntries(shardId0, shard0TimeToApply0);

    auto expectedShard0AvgTimeToApply0 = shard0TimeToApply0;
    ASSERT_EQ(metrics->getAverageTimeToApplyOplogEntries(shardId0), expectedShard0AvgTimeToApply0);
    ASSERT_FALSE(metrics->getAverageTimeToApplyOplogEntries(shardId1));

    auto shard0TimeToApply1 = Milliseconds(50);
    metrics->updateAverageTimeToApplyOplogEntries(shardId0, shard0TimeToApply1);

    auto expectedShard0AvgTimeToApply1 =
        Milliseconds((int)resharding::calculateExponentialMovingAverage(
            expectedShard0AvgTimeToApply0.count(), shard0TimeToApply1.count(), smoothingFactor));
    ASSERT_EQ(metrics->getAverageTimeToApplyOplogEntries(shardId0), expectedShard0AvgTimeToApply1);
    ASSERT_FALSE(metrics->getAverageTimeToApplyOplogEntries(shardId1));

    // Update the average for shard1 only.

    auto shard1TimeToApply0 = Milliseconds(1000);
    metrics->updateAverageTimeToApplyOplogEntries(shardId1, shard1TimeToApply0);

    auto expectedShard1AvgTimeToApply0 = shard1TimeToApply0;
    ASSERT_EQ(metrics->getAverageTimeToApplyOplogEntries(shardId0), expectedShard0AvgTimeToApply1);
    ASSERT_EQ(metrics->getAverageTimeToApplyOplogEntries(shardId1), expectedShard1AvgTimeToApply0);

    auto shard1TimeToApply1 = Milliseconds(5);
    metrics->updateAverageTimeToApplyOplogEntries(shardId1, shard1TimeToApply1);

    auto expectedShard1AvgTimeToApply1 =
        Milliseconds((int)resharding::calculateExponentialMovingAverage(
            expectedShard1AvgTimeToApply0.count(), shard1TimeToApply1.count(), smoothingFactor));
    ASSERT_EQ(metrics->getAverageTimeToApplyOplogEntries(shardId0), expectedShard0AvgTimeToApply1);
    ASSERT_EQ(metrics->getAverageTimeToApplyOplogEntries(shardId1), expectedShard1AvgTimeToApply1);
}

TEST_F(ReshardingMetricsTest,
       GetMaxAverageTimeToFetchAndApplyOplogEntriesBeforeRegisteringDonorsReturnsNone) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    ASSERT_FALSE(metrics->getMaxAverageTimeToFetchAndApplyOplogEntries(
        ReshardingMetrics::CalculationLogOption::Show));
}

TEST_F(ReshardingMetricsTest, GetMaxAverageTimeToFetchAndApplyOplogEntriesBasic) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    metrics->registerDonors({shardId0, shardId1});

    auto shard0TimeToFetch = Milliseconds(100);
    auto shard0TimeToApply = Milliseconds(20);
    metrics->updateAverageTimeToFetchOplogEntries(shardId0, shard0TimeToFetch);
    metrics->updateAverageTimeToApplyOplogEntries(shardId0, shard0TimeToApply);

    auto shard1TimeToFetch = Milliseconds(50);
    auto shard1TimeToApply = Milliseconds(150);
    metrics->updateAverageTimeToFetchOplogEntries(shardId1, shard1TimeToFetch);
    metrics->updateAverageTimeToApplyOplogEntries(shardId1, shard1TimeToApply);

    auto maxTimeToFetchAndApply = shard1TimeToFetch + shard1TimeToApply;
    ASSERT_EQ(metrics->getMaxAverageTimeToFetchAndApplyOplogEntries(
                  ReshardingMetrics::CalculationLogOption::Show),
              maxTimeToFetchAndApply);
    ASSERT_EQ(metrics->getMaxAverageTimeToFetchAndApplyOplogEntries(
                  ReshardingMetrics::CalculationLogOption::Hide),
              maxTimeToFetchAndApply);
}

TEST_F(ReshardingMetricsTest, GetAverageTimeToFetchOplogEntriesConcurrentlyWithRegisterDonors) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto registerThread = stdx::thread([&, this] { metrics->registerDonors({shardId0}); });
    auto getTimeToFetchThread = stdx::thread([&, this] {
        try {
            metrics->getAverageTimeToFetchOplogEntries(shardId0);
        } catch (const DBException& ex) {
            // If the getter thread runs before the register thread, the former is expected to hit a
            // validation error.
            ASSERT_EQ(ex.code(), 10626503);
        }
    });

    registerThread.join();
    getTimeToFetchThread.join();
}

TEST_F(ReshardingMetricsTest, UpdateAverageTimeToFetchOplogEntriesConcurrentlyWithRegisterDonors) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto registerThread = stdx::thread([&, this] { metrics->registerDonors({shardId0}); });
    auto updateTimeToFetchThread = stdx::thread([&, this] {
        try {
            metrics->updateAverageTimeToFetchOplogEntries(shardId0, Milliseconds(10));
        } catch (const DBException& ex) {
            // If the update thread runs before the register thread, the former is expected to hit a
            // validation error.
            ASSERT_EQ(ex.code(), 10626502);
        }
    });

    registerThread.join();
    updateTimeToFetchThread.join();
}

TEST_F(ReshardingMetricsTest, GetAverageTimeToApplyOplogEntriesConcurrentlyWithRegisterDonors) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto registerThread = stdx::thread([&, this] { metrics->registerDonors({shardId0}); });
    auto getTimeToApplyThread = stdx::thread([&, this] {
        try {
            metrics->getAverageTimeToApplyOplogEntries(shardId0);
        } catch (const DBException& ex) {
            // If the getter thread runs before the register thread, the former is expected to hit a
            // validation error.
            ASSERT_EQ(ex.code(), 10626505);
        }
    });

    registerThread.join();
    getTimeToApplyThread.join();
}

TEST_F(ReshardingMetricsTest, UpdateAverageTimeToApplyOplogEntriesConcurrentlyWithRegisterDonors) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto registerThread = stdx::thread([&, this] { metrics->registerDonors({shardId0}); });
    auto updateTimeToApplyThread = stdx::thread([&, this] {
        try {
            metrics->updateAverageTimeToApplyOplogEntries(shardId0, Milliseconds(10));
        } catch (const DBException& ex) {
            // If the update thread runs before the register thread, the former is expected to hit a
            // validation error.
            ASSERT_EQ(ex.code(), 10626504);
        }
    });

    registerThread.join();
    updateTimeToApplyThread.join();
}

TEST_F(ReshardingMetricsTest,
       GetMaxAverageTimeToFetchAndApplyOplogEntriesConcurrentlyWithRegisterDonors) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto registerThread = stdx::thread([&, this] { metrics->registerDonors({shardId0}); });
    auto getMaxThread = stdx::thread([&, this] {
        metrics->getMaxAverageTimeToFetchAndApplyOplogEntries(
            ReshardingMetrics::CalculationLogOption::Show);
    });

    registerThread.join();
    getMaxThread.join();
}

TEST_F(ReshardingMetricsTest, RecipientReportsRemainingTimeIfDisableMovingAvgCloningToDone) {
    const RAIIServerParameterControllerForTest estimateBasedOnMovingAvgServerParameter{
        "reshardingRemainingTimeEstimateBasedOnMovingAverage", false};

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

TEST_F(ReshardingMetricsTest, RecipientReportsRemainingTimeIfDisableOrEnableMovingAvg) {
    const auto& clock = getClockSource();
    const auto elapsedTimeInc = Milliseconds(1200);

    const auto smoothingFactor = 0.7;
    const RAIIServerParameterControllerForTest smoothingFactorServerParameter{
        "reshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor", smoothingFactor};

    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    ASSERT_FALSE(metrics->getHighEstimateRemainingTimeMillis());
    metrics->registerDonors({shardId0, shardId1});

    // The "applying" state has not started so the estimates are not available whether or not moving
    // average is enabled.
    boost::optional<Milliseconds> expectedEstimateIfDisableMovingAvg = boost::none;
    boost::optional<Milliseconds> expectedEstimateIfEnableMovingAvg = boost::none;
    validateEstimatedRemainingTimeIfDisableMovingAverage(metrics.get(),
                                                         expectedEstimateIfDisableMovingAvg);
    validateEstimatedRemainingTimeIfEnableMovingAverage(metrics.get(),
                                                        expectedEstimateIfEnableMovingAvg);

    metrics->setStartFor(TimedPhase::kApplying, clock->now());
    auto numOplogEntriesToApply = 0;
    auto numOplogEntriesApplied = 0;
    auto elapsedTime = Milliseconds(0);

    clock->advance(elapsedTimeInc);
    elapsedTime += elapsedTimeInc;

    // If moving average is disabled, the estimate should be 0 since the "applying" state has
    // started and the number of oplog entries fetched is 0.
    expectedEstimateIfDisableMovingAvg = Milliseconds(0);
    // If moving average is enabled, the estimate should also be 0 (based on non-moving average)
    // since there are no latency metrics yet.
    expectedEstimateIfEnableMovingAvg = expectedEstimateIfDisableMovingAvg;
    validateEstimatedRemainingTimeIfDisableMovingAverage(metrics.get(),
                                                         expectedEstimateIfDisableMovingAvg);
    validateEstimatedRemainingTimeIfEnableMovingAverage(metrics.get(),
                                                        expectedEstimateIfEnableMovingAvg);

    clock->advance(elapsedTimeInc);
    elapsedTime += elapsedTimeInc;

    auto shard0TimeToFetch0 = Milliseconds(100);
    metrics->onOplogEntriesFetched(2);
    metrics->updateAverageTimeToFetchOplogEntries(shardId0, shard0TimeToFetch0);
    numOplogEntriesToApply += 2;

    // If moving average is disabled, the estimate should be unavailable since the number of oplog
    // fetched is greater than 0 but the the number of oplog entries applied is 0.
    expectedEstimateIfDisableMovingAvg = boost::none;
    // If moving average is enabled, the estimate should also be unavailable (based on non-moving
    // average) since the latency metrics are still incomplete.
    expectedEstimateIfEnableMovingAvg = expectedEstimateIfDisableMovingAvg;
    validateEstimatedRemainingTimeIfDisableMovingAverage(metrics.get(),
                                                         expectedEstimateIfDisableMovingAvg);
    validateEstimatedRemainingTimeIfEnableMovingAverage(metrics.get(),
                                                        expectedEstimateIfEnableMovingAvg);

    auto shard0TimeToApply0 = Milliseconds(50);
    metrics->onOplogEntriesApplied(1);
    metrics->updateAverageTimeToApplyOplogEntries(shardId0, shard0TimeToApply0);
    numOplogEntriesToApply -= 1;
    numOplogEntriesApplied += 1;

    // If moving average is disabled, the estimate should be available since the numbers of oplog
    // fetched and applied are both greater than 0.
    expectedEstimateIfDisableMovingAvg =
        elapsedTime * numOplogEntriesToApply / numOplogEntriesApplied;
    // If moving average is enabled, the estimate should still be unavailable (based on non-moving
    // average) since the latency metrics are still incomplete (still missing the fetcher and
    // applier latency metrics for shard1).
    expectedEstimateIfEnableMovingAvg = expectedEstimateIfDisableMovingAvg;
    validateEstimatedRemainingTimeIfDisableMovingAverage(metrics.get(),
                                                         expectedEstimateIfDisableMovingAvg);
    validateEstimatedRemainingTimeIfEnableMovingAverage(metrics.get(),
                                                        expectedEstimateIfEnableMovingAvg);

    clock->advance(elapsedTimeInc);
    elapsedTime += elapsedTimeInc;

    auto shard1TimeToFetch0 = Milliseconds(1);
    metrics->onOplogEntriesFetched(1);
    metrics->updateAverageTimeToFetchOplogEntries(shardId1, shard1TimeToFetch0);
    numOplogEntriesToApply += 1;

    // If moving average is disabled, the estimate should be available since the numbers of oplog
    // fetched and applied are both greater than 0.
    expectedEstimateIfDisableMovingAvg =
        elapsedTime * numOplogEntriesToApply / numOplogEntriesApplied;
    // If moving average is enabled, the estimate should still be unavailable (based on non-moving
    // average) since the latency metrics are still incomplete (still missing the applier latency
    // metrics for shard1).
    expectedEstimateIfEnableMovingAvg = expectedEstimateIfDisableMovingAvg;
    validateEstimatedRemainingTimeIfDisableMovingAverage(metrics.get(),
                                                         expectedEstimateIfDisableMovingAvg);
    validateEstimatedRemainingTimeIfEnableMovingAverage(metrics.get(),
                                                        expectedEstimateIfEnableMovingAvg);

    clock->advance(elapsedTimeInc);
    elapsedTime += elapsedTimeInc;

    auto shard1TimeToApply0 = Milliseconds(5);
    metrics->onOplogEntriesApplied(1);
    metrics->updateAverageTimeToApplyOplogEntries(shardId1, shard1TimeToApply0);
    numOplogEntriesToApply -= 1;
    numOplogEntriesApplied += 1;

    // If moving average is disabled, the estimate should be available since the numbers of oplog
    // fetched and applied are both greater than 0.
    expectedEstimateIfDisableMovingAvg =
        elapsedTime * numOplogEntriesToApply / numOplogEntriesApplied;
    // If moving average is enabled, the estimate should be available since the latency metrics are
    // now complete.
    expectedEstimateIfEnableMovingAvg = shard0TimeToFetch0 + shard0TimeToApply0;
    validateEstimatedRemainingTimeIfDisableMovingAverage(metrics.get(),
                                                         expectedEstimateIfDisableMovingAvg);
    validateEstimatedRemainingTimeIfEnableMovingAverage(metrics.get(),
                                                        expectedEstimateIfEnableMovingAvg);

    clock->advance(elapsedTimeInc);
    elapsedTime += elapsedTimeInc;

    auto shard1TimeToFetch1 = Milliseconds(1000);
    metrics->onOplogEntriesFetched(1);
    metrics->updateAverageTimeToFetchOplogEntries(shardId1, shard1TimeToFetch1);
    numOplogEntriesToApply += 1;

    // If moving average is disabled, the estimate should be available since the numbers of oplog
    // fetched and applied are both greater than 0.
    expectedEstimateIfDisableMovingAvg =
        elapsedTime * numOplogEntriesToApply / numOplogEntriesApplied;
    // If moving average is enabled, the estimate should be available since the latency metrics are
    // now complete.
    expectedEstimateIfEnableMovingAvg =
        Milliseconds((int)resharding::calculateExponentialMovingAverage(
            shard1TimeToFetch0.count(), shard1TimeToFetch1.count(), smoothingFactor)) +
        shard1TimeToApply0;
    validateEstimatedRemainingTimeIfDisableMovingAverage(metrics.get(),
                                                         expectedEstimateIfDisableMovingAvg);
    validateEstimatedRemainingTimeIfEnableMovingAverage(metrics.get(),
                                                        expectedEstimateIfEnableMovingAvg);
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

TEST_F(ReshardingMetricsTest, SetTimedPhaseViaCoordinatorState) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    {
        const auto cloneStart = Date_t::fromMillisSinceEpoch(1);
        metrics->setStartFor(CoordinatorStateEnum::kCloning, cloneStart);
        ASSERT_EQ(metrics->getStartFor(TimedPhase::kCloning), cloneStart);
    }
    {
        const auto cloneEnd = Date_t::fromMillisSinceEpoch(2);
        metrics->setEndFor(CoordinatorStateEnum::kCloning, cloneEnd);
        ASSERT_EQ(metrics->getEndFor(TimedPhase::kCloning), cloneEnd);
    }
    {
        const auto applyStart = Date_t::fromMillisSinceEpoch(3);
        metrics->setStartFor(CoordinatorStateEnum::kApplying, applyStart);
        ASSERT_EQ(metrics->getStartFor(TimedPhase::kApplying), applyStart);
    }
    {
        const auto applyEnd = Date_t::fromMillisSinceEpoch(4);
        metrics->setEndFor(CoordinatorStateEnum::kApplying, applyEnd);
        ASSERT_EQ(metrics->getEndFor(TimedPhase::kApplying), applyEnd);
    }
    {
        const auto criticalSectionStart = Date_t::fromMillisSinceEpoch(5);
        metrics->setStartFor(CoordinatorStateEnum::kBlockingWrites, criticalSectionStart);
        ASSERT_EQ(metrics->getStartFor(TimedPhase::kCriticalSection), criticalSectionStart);
    }
    {
        const auto criticalSectionEnd = Date_t::fromMillisSinceEpoch(6);
        metrics->setEndFor(CoordinatorStateEnum::kBlockingWrites, criticalSectionEnd);
        ASSERT_EQ(metrics->getEndFor(TimedPhase::kCriticalSection), criticalSectionEnd);
    }
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
    for (bool enableEstimateBasedOnMovingAvg : {true, false}) {
        const RAIIServerParameterControllerForTest estimateBasedOnMovingAvgServerParameter{
            "reshardingRemainingTimeEstimateBasedOnMovingAverage", enableEstimateBasedOnMovingAvg};
        auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
        ASSERT_EQ(metrics->getHighEstimateRemainingTimeMillis(), boost::none);
    }
}

TEST_F(ReshardingMetricsTest,
       RecipientEstimatesNoneBeforeExternalFieldsRestoredForRestoredInstance) {
    for (bool enableEstimateBasedOnMovingAvg : {true, false}) {
        const RAIIServerParameterControllerForTest estimateBasedOnMovingAvgServerParameter{
            "reshardingRemainingTimeEstimateBasedOnMovingAverage", enableEstimateBasedOnMovingAvg};
        auto metrics = makeRecipientMetricsWithAmbiguousTimeRemaining();
        ASSERT_EQ(metrics->getHighEstimateRemainingTimeMillis(), boost::none);
    }
}

TEST_F(ReshardingMetricsTest, RecipientEstimatesAfterExternalFieldsRestoredForRestoredInstance) {
    for (bool enableEstimateBasedOnMovingAvg : {true, false}) {
        const RAIIServerParameterControllerForTest estimateBasedOnMovingAvgServerParameter{
            "reshardingRemainingTimeEstimateBasedOnMovingAverage", enableEstimateBasedOnMovingAvg};
        auto metrics = makeRecipientMetricsWithAmbiguousTimeRemaining();
        metrics->restoreExternallyTrackedRecipientFields(
            ReshardingMetrics::ExternallyTrackedRecipientFields{});
        ASSERT_EQ(metrics->getHighEstimateRemainingTimeMillis(), Milliseconds{0});
    }
}

TEST_F(ReshardingMetricsTest, CurrentOpDoesNotReportRecipientEstimateIfNotSet) {
    for (bool enableEstimateBasedOnMovingAvg : {true, false}) {
        const RAIIServerParameterControllerForTest estimateBasedOnMovingAvgServerParameter{
            "reshardingRemainingTimeEstimateBasedOnMovingAverage", enableEstimateBasedOnMovingAvg};
        auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
        auto report = metrics->reportForCurrentOp();
        ASSERT_FALSE(report.hasField("remainingOperationTimeEstimatedSecs"));
    }
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
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getBoolField("isSameKeyResharding"), false);
    metrics->setIsSameKeyResharding(true);

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getBoolField("isSameKeyResharding"), true);
}

TEST_F(ReshardingMetricsTest, onIndexBuild) {
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

TEST_F(ReshardingMetricsTest, RecipientReportsRemainingTimeLowElapsed) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    const auto& clock = getClockSource();
    constexpr auto timeSpentCloning = Seconds(20);
    constexpr auto timeSpentApplying = Milliseconds(50);
    metrics->onOplogEntriesFetched(500000);
    metrics->setStartFor(TimedPhase::kCloning, clock->now());

    clock->advance(timeSpentCloning);
    metrics->setEndFor(TimedPhase::kCloning, clock->now());
    metrics->setStartFor(TimedPhase::kApplying, clock->now());
    clock->advance(timeSpentApplying);
    metrics->onOplogEntriesApplied(300000);

    auto report = metrics->getHighEstimateRemainingTimeMillis();
    ASSERT_NE(report, Milliseconds{0});
}

TEST_F(ReshardingMetricsTest, OnStartedInformsCumulativeMetrics) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    assertAltersCumulativeMetrics(
        metrics.get(),
        [=](auto base) {
            auto reshardingMetrics = dynamic_cast<ReshardingMetrics*>(base);
            reshardingMetrics->onStarted();
        },
        [this](auto reportBefore, auto reportAfter) {
            BSONObjBuilder reportBuilder;
            getCumulativeMetrics()->reportForServerStatus(&reportBuilder);
            auto report = reportBuilder.done();
            ASSERT_EQ(report.getObjectField(kResharding).getIntField("countStarted"), 1);
            return true;
        });
}

TEST_F(ReshardingMetricsTest, OnSuccessInformsCumulativeMetrics) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    assertAltersCumulativeMetrics(
        metrics.get(),
        [=](auto base) {
            auto reshardingMetrics = dynamic_cast<ReshardingMetrics*>(base);
            reshardingMetrics->onStarted();
            reshardingMetrics->onSuccess();
        },
        [this](auto reportBefore, auto reportAfter) {
            BSONObjBuilder reportBuilder;
            getCumulativeMetrics()->reportForServerStatus(&reportBuilder);
            auto report = reportBuilder.done();
            ASSERT_EQ(report.getObjectField(kResharding).getIntField("countSucceeded"), 1);
            return true;
        });
}

TEST_F(ReshardingMetricsTest, OnCanceledInformsCumulativeMetrics) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    assertAltersCumulativeMetrics(
        metrics.get(),
        [=](auto base) {
            auto reshardingMetrics = dynamic_cast<ReshardingMetrics*>(base);
            reshardingMetrics->onStarted();
            reshardingMetrics->onCanceled();
        },
        [this](auto reportBefore, auto reportAfter) {
            BSONObjBuilder reportBuilder;
            getCumulativeMetrics()->reportForServerStatus(&reportBuilder);
            auto report = reportBuilder.done();
            ASSERT_EQ(report.getObjectField(kResharding).getIntField("countCanceled"), 1);
            return true;
        });
}

TEST_F(ReshardingMetricsTest, OnFailedInformsCumulativeMetrics) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    assertAltersCumulativeMetrics(
        metrics.get(),
        [=](auto base) {
            auto reshardingMetrics = dynamic_cast<ReshardingMetrics*>(base);
            reshardingMetrics->onStarted();
            reshardingMetrics->onFailure();
        },
        [this](auto reportBefore, auto reportAfter) {
            BSONObjBuilder reportBuilder;
            getCumulativeMetrics()->reportForServerStatus(&reportBuilder);
            auto report = reportBuilder.done();
            ASSERT_EQ(report.getObjectField(kResharding).getIntField("countFailed"), 1);
            return true;
        });
}

TEST_F(ReshardingMetricsTest, RegisterAndDeregisterMetrics) {
    for (auto i = 0; i < 100; i++) {
        auto metrics = createInstanceMetrics(getClockSource());
        ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), 1);
    }
    ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), 0);
}

TEST_F(ReshardingMetricsTest, RegisterAndDeregisterMetricsAtOnce) {
    {
        std::vector<std::unique_ptr<ReshardingMetrics>> registered;
        for (auto i = 0; i < 100; i++) {
            registered.emplace_back(createInstanceMetrics(getClockSource()));
            ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), registered.size());
        }
    }
    ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), 0);
}

TEST_F(ReshardingMetricsTest, RandomOperations) {
    doRandomOperationsTest<ReshardingMetricsWithObserverMock>();
}

TEST_F(ReshardingMetricsTest, RandomOperationsMultithreaded) {
    doRandomOperationsMultithreadedTest<ReshardingMetricsWithObserverMock>();
}

TEST_F(ReshardingMetricsTest, GetRoleNameShouldReturnCorrectName) {
    std::vector<std::pair<Role, std::string>> roles{
        {Role::kCoordinator, "Coordinator"},
        {Role::kDonor, "Donor"},
        {Role::kRecipient, "Recipient"},
    };

    std::for_each(roles.begin(), roles.end(), [&](auto role) {
        ASSERT_EQ(ReshardingMetricsCommon::getRoleName(role.first), role.second);
    });
}

TEST_F(ReshardingMetricsTest, DonorIncrementWritesDuringCriticalSection) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kDonor);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countWritesDuringCriticalSection"), 0);
    metrics->onWriteDuringCriticalSection();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countWritesDuringCriticalSection"), 1);
}

TEST_F(ReshardingMetricsTest, DonorIncrementReadsDuringCriticalSection) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kDonor);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countReadsDuringCriticalSection"), 0);
    metrics->onReadDuringCriticalSection();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countReadsDuringCriticalSection"), 1);
}

TEST_F(ReshardingMetricsTest, RecipientSetsDocumentsAndBytesToCopy) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("approxDocumentsToCopy"), 0);
    ASSERT_EQ(report.getIntField("approxBytesToCopy"), 0);
    metrics->setDocumentsToProcessCounts(5, 1000);

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("approxDocumentsToCopy"), 5);
    ASSERT_EQ(report.getIntField("approxBytesToCopy"), 1000);

    metrics->setDocumentsToProcessCounts(3, 750);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("approxDocumentsToCopy"), 3);
    ASSERT_EQ(report.getIntField("approxBytesToCopy"), 750);
}

TEST_F(ReshardingMetricsTest, RecipientIncrementsDocumentsAndBytesWritten) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("documentsCopied"), 0);
    ASSERT_EQ(report.getIntField("bytesCopied"), 0);
    metrics->onDocumentsProcessed(5, 1000, Milliseconds(1));

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("documentsCopied"), 5);
    ASSERT_EQ(report.getIntField("bytesCopied"), 1000);
}

TEST_F(ReshardingMetricsTest, CurrentOpReportsRunningTime) {
    auto* clock = getClockSource();
    auto metrics = createInstanceMetrics(clock, UUID::gen(), Role::kRecipient);
    constexpr auto kTimeElapsed = 15;
    clock->advance(Seconds(kTimeElapsed));
    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("totalOperationTimeElapsedSecs"), kTimeElapsed);
}

TEST_F(ReshardingMetricsTest, OnWriteToStasheddShouldIncrementCurOpFields) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countWritesToStashCollections"), 0);
    metrics->onWriteToStashedCollections();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("countWritesToStashCollections"), 1);
}

TEST_F(ReshardingMetricsTest, SetLowestOperationTimeShouldBeReflectedInCurrentOp) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    metrics->setCoordinatorLowEstimateRemainingTimeMillis(Milliseconds(2000));
    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("allShardsLowestRemainingOperationTimeEstimatedSecs"), 2);
}

TEST_F(ReshardingMetricsTest, SetHighestOperationTimeShouldBeReflectedInCurrentOp) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    metrics->setCoordinatorHighEstimateRemainingTimeMillis(Milliseconds(12000));
    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("allShardsHighestRemainingOperationTimeEstimatedSecs"), 12);
}

TEST_F(ReshardingMetricsTest, CoordinatorHighEstimateNoneIfNotSet) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    ASSERT_EQ(metrics->getHighEstimateRemainingTimeMillis(), boost::none);
}

TEST_F(ReshardingMetricsTest, CoordinatorLowEstimateNoneIfNotSet) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    ASSERT_EQ(metrics->getLowEstimateRemainingTimeMillis(), boost::none);
}

TEST_F(ReshardingMetricsTest, CurrentOpDoesNotReportCoordinatorHighEstimateIfNotSet) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    auto report = metrics->reportForCurrentOp();
    ASSERT_FALSE(report.hasField("allShardsHighestRemainingOperationTimeEstimatedSecs"));
}

TEST_F(ReshardingMetricsTest, CurrentOpDoesNotReportCoordinatorLowEstimateIfNotSet) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kCoordinator);
    auto report = metrics->reportForCurrentOp();
    ASSERT_FALSE(report.hasField("allShardsLowestRemainingOperationTimeEstimatedSecs"));
}

TEST_F(ReshardingMetricsTest, SetChunkImbalanceIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->setLastOpEndingChunkImbalance(1); },
        Section::kRoot,
        "lastOpEndingChunkImbalance");
}

TEST_F(ReshardingMetricsTest, OnReadDuringCriticalSectionIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onReadDuringCriticalSection(); },
        Section::kActive,
        "countReadsDuringCriticalSection");
}

TEST_F(ReshardingMetricsTest, OnWriteDuringCriticalSectionIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onWriteDuringCriticalSection(); },
        Section::kActive,
        "countWritesDuringCriticalSection");
}

TEST_F(ReshardingMetricsTest, OnWriteToStashCollectionsIncrementsCumulativeMetrics) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onWriteToStashedCollections(); },
        Section::kActive,
        "countWritesToStashCollections");
}

TEST_F(ReshardingMetricsTest, OnCloningRemoteBatchRetrievalIncrementsCumulativeMetricsCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onCloningRemoteBatchRetrieval(Milliseconds{0}); },
        Section::kLatencies,
        "collectionCloningTotalRemoteBatchesRetrieved");
}

TEST_F(ReshardingMetricsTest, OnCloningRemoteBatchRetrievalIncrementsCumulativeMetricsTime) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onCloningRemoteBatchRetrieval(Milliseconds{1}); },
        Section::kLatencies,
        "collectionCloningTotalRemoteBatchRetrievalTimeMillis");
}

TEST_F(ReshardingMetricsTest, OnDocumentsProcessedIncrementsCumulativeMetricsDocumentCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onDocumentsProcessed(1, 0, Milliseconds{0}); },
        Section::kActive,
        "documentsCopied");
}

TEST_F(ReshardingMetricsTest, OnDocumentsProcessedIncrementsCumulativeMetricsLocalInserts) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onDocumentsProcessed(1, 0, Milliseconds{0}); },
        Section::kLatencies,
        "collectionCloningTotalLocalInserts");
}

TEST_F(ReshardingMetricsTest, OnDocumentsProcessedIncrementsCumulativeMetricsByteCount) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onDocumentsProcessed(0, 1, Milliseconds{0}); },
        Section::kActive,
        "bytesCopied");
}

TEST_F(ReshardingMetricsTest, OnDocumentsProcessedIncrementsCumulativeMetricsLocalInsertTime) {
    createMetricsAndAssertIncrementsCumulativeMetricsField(
        [](auto metrics) { metrics->onDocumentsProcessed(0, 0, Milliseconds{1}); },
        Section::kLatencies,
        "collectionCloningTotalLocalInsertTimeMillis");
}

TEST_F(ReshardingMetricsTest, TestSetStartForIdempotency) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    auto start = metrics->getStartFor(resharding_metrics::TimedPhase::kCriticalSection);
    ASSERT_EQ(start, boost::none);

    auto now = getClockSource()->now();

    metrics->setStartFor(resharding_metrics::TimedPhase::kCriticalSection, now);
    start = metrics->getStartFor(resharding_metrics::TimedPhase::kCriticalSection);
    ASSERT_EQ(start, now);

    auto later = now + Seconds(10);

    metrics->setStartFor(resharding_metrics::TimedPhase::kCriticalSection, later);
    start = metrics->getStartFor(resharding_metrics::TimedPhase::kCriticalSection);
    ASSERT_EQ(start, now);
}

TEST_F(ReshardingMetricsTest, TestSetEndForIdempotency) {
    auto metrics = createInstanceMetrics(getClockSource(), UUID::gen(), Role::kRecipient);
    auto start = metrics->getEndFor(resharding_metrics::TimedPhase::kCriticalSection);
    ASSERT_EQ(start, boost::none);

    auto now = getClockSource()->now();

    metrics->setEndFor(resharding_metrics::TimedPhase::kCriticalSection, now);
    start = metrics->getEndFor(resharding_metrics::TimedPhase::kCriticalSection);
    ASSERT_EQ(start, now);

    auto later = now + Seconds(10);

    metrics->setEndFor(resharding_metrics::TimedPhase::kCriticalSection, later);
    start = metrics->getEndFor(resharding_metrics::TimedPhase::kCriticalSection);
    ASSERT_EQ(start, now);
}

}  // namespace
}  // namespace mongo
