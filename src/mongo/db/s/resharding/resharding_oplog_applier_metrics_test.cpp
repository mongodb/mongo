// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/db/s/resharding/resharding_metrics_test_fixture.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

class ReshardingOplogApplierMetricsTest : public ReshardingMetricsTestFixture {
public:
    std::unique_ptr<ReshardingMetrics> createInstanceMetrics() {
        return std::make_unique<ReshardingMetrics>(UUID::gen(),
                                                   kTestCommand,
                                                   kTestNamespace,
                                                   ReshardingMetrics::Role::kRecipient,
                                                   getClockSource()->now(),
                                                   getClockSource(),
                                                   _cumulativeMetrics.get(),
                                                   RecipientStateEnum::kApplying,
                                                   ReshardingProvenanceEnum::kReshardCollection);
    }

protected:
    ShardId donorShardId{"shard0"};
};

TEST_F(ReshardingOplogApplierMetricsTest,
       IncrementInsertOnApplierMetricsShouldAlsoIncrementInstance) {
    auto metrics = createInstanceMetrics();

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("insertsApplied"), 0);

    ReshardingOplogApplierMetrics applierMetrics(donorShardId, metrics.get(), boost::none);
    applierMetrics.onInsertApplied();

    ASSERT_EQ(applierMetrics.getInsertsApplied(), 1);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("insertsApplied"), 1);
}

TEST_F(ReshardingOplogApplierMetricsTest,
       IncrementUpdateOnApplierMetricsShouldAlsoIncrementInstance) {
    auto metrics = createInstanceMetrics();

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("updatesApplied"), 0);

    ReshardingOplogApplierMetrics applierMetrics(donorShardId, metrics.get(), boost::none);
    applierMetrics.onUpdateApplied();

    ASSERT_EQ(applierMetrics.getUpdatesApplied(), 1);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("updatesApplied"), 1);
}

TEST_F(ReshardingOplogApplierMetricsTest,
       IncrementDeleteOnApplierMetricsShouldAlsoIncrementInstance) {
    auto metrics = createInstanceMetrics();

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("deletesApplied"), 0);

    ReshardingOplogApplierMetrics applierMetrics(donorShardId, metrics.get(), boost::none);
    applierMetrics.onDeleteApplied();

    ASSERT_EQ(applierMetrics.getDeletesApplied(), 1);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("deletesApplied"), 1);
}

TEST_F(ReshardingOplogApplierMetricsTest, ApplierInsertProgressIncrementsIdependentlyFromInstance) {
    auto metrics = createInstanceMetrics();

    ReshardingOplogApplierProgress progressDoc;
    progressDoc.setInsertsApplied(12);
    ReshardingOplogApplierMetrics applierMetrics(donorShardId, metrics.get(), progressDoc);

    ASSERT_EQ(applierMetrics.getInsertsApplied(), 12);
    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("insertsApplied"), 0);

    applierMetrics.onInsertApplied();

    ASSERT_EQ(applierMetrics.getInsertsApplied(), 13);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("insertsApplied"), 1);
}

TEST_F(ReshardingOplogApplierMetricsTest, ApplierUpdateProgressIncrementsIdependentlyFromInstance) {
    auto metrics = createInstanceMetrics();

    ReshardingOplogApplierProgress progressDoc;
    progressDoc.setUpdatesApplied(34);
    ReshardingOplogApplierMetrics applierMetrics(donorShardId, metrics.get(), progressDoc);

    ASSERT_EQ(applierMetrics.getUpdatesApplied(), 34);
    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("updatesApplied"), 0);

    applierMetrics.onUpdateApplied();

    ASSERT_EQ(applierMetrics.getUpdatesApplied(), 35);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("updatesApplied"), 1);
}

TEST_F(ReshardingOplogApplierMetricsTest, ApplierDeleteProgressIncrementsIdependentlyFromInstance) {
    auto metrics = createInstanceMetrics();

    ReshardingOplogApplierProgress progressDoc;
    progressDoc.setDeletesApplied(56);
    ReshardingOplogApplierMetrics applierMetrics(donorShardId, metrics.get(), progressDoc);

    ASSERT_EQ(applierMetrics.getDeletesApplied(), 56);
    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("deletesApplied"), 0);

    applierMetrics.onDeleteApplied();

    ASSERT_EQ(applierMetrics.getDeletesApplied(), 57);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("deletesApplied"), 1);
}

}  // namespace
}  // namespace mongo
