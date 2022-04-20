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

#include "mongo/db/s/resharding/resharding_metrics_new.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

constexpr auto kRunningTime = Seconds(12345);
const auto kShardKey = BSON("newKey" << 1);

class ReshardingMetricsTest : public ShardingDataTransformMetricsTestFixture {

public:
    std::unique_ptr<ReshardingMetricsNew> createInstanceMetrics(ClockSource* clockSource,
                                                                UUID instanceId = UUID::gen(),
                                                                Role role = Role::kDonor) {
        return std::make_unique<ReshardingMetricsNew>(instanceId,
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
            ReshardingMetricsNew::initializeFrom(document, getClockSource(), &_cumulativeMetrics);
        return metrics->reportForCurrentOp();
    }

    CommonReshardingMetadata createCommonReshardingMetadata(const UUID& operationId) {
        CommonReshardingMetadata metadata{
            operationId,
            kTestNamespace,
            getSourceCollectionId(),
            constructTemporaryReshardingNss(kTestNamespace.db(), getSourceCollectionId()),
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

TEST_F(ReshardingMetricsTest, RestoresFromRecipientStateDocument) {
    RecipientShardContext recipientCtx;
    auto state = RecipientStateEnum::kAwaitingFetchTimestamp;
    recipientCtx.setState(state);
    ReshardingRecipientDocument doc{std::move(recipientCtx), {ShardId{"donor1"}}, 5};
    auto opId = UUID::gen();
    doc.setCommonReshardingMetadata(createCommonReshardingMetadata(opId));
    auto report = getReportFromStateDocument(std::move(doc));

    verifyCommonCurrentOpFields(report);
    ASSERT_EQ(report.getStringField("desc"),
              "ReshardingMetricsRecipientService " + opId.toString());
    ASSERT_EQ(report.getStringField("recipientState").toString(), RecipientState_serializer(state));
}

TEST_F(ReshardingMetricsTest, RestoresFromDonorStateDocument) {
    DonorShardContext donorCtx;
    auto state = DonorStateEnum::kDonatingInitialData;
    donorCtx.setState(state);
    ReshardingDonorDocument doc{std::move(donorCtx), {ShardId{"recipient1"}}};
    auto opId = UUID::gen();
    doc.setCommonReshardingMetadata(createCommonReshardingMetadata(opId));
    auto report = getReportFromStateDocument(std::move(doc));

    verifyCommonCurrentOpFields(report);
    ASSERT_EQ(report.getStringField("desc"), "ReshardingMetricsDonorService " + opId.toString());
    ASSERT_EQ(report.getStringField("donorState").toString(), DonorState_serializer(state));
}

TEST_F(ReshardingMetricsTest, RestoresFromCoordinatorStateDocument) {
    auto state = CoordinatorStateEnum::kPreparingToDonate;
    ReshardingCoordinatorDocument doc{state, {}, {}};
    auto opId = UUID::gen();
    doc.setCommonReshardingMetadata(createCommonReshardingMetadata(opId));
    auto report = getReportFromStateDocument(std::move(doc));

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

}  // namespace
}  // namespace mongo
