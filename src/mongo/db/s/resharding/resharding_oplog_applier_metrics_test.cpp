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

#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics_test_fixture.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

class ReshardingOplogApplierMetricsTest : public ShardingDataTransformMetricsTestFixture {
public:
    virtual std::unique_ptr<ShardingDataTransformCumulativeMetrics> initializeCumulativeMetrics()
        override {
        return std::make_unique<ReshardingCumulativeMetrics>();
    }

    std::unique_ptr<ReshardingMetrics> createInstanceMetrics() {
        return std::make_unique<ReshardingMetrics>(UUID::gen(),
                                                   kTestCommand,
                                                   kTestNamespace,
                                                   ReshardingMetrics::Role::kRecipient,
                                                   getClockSource()->now(),
                                                   getClockSource(),
                                                   _cumulativeMetrics.get());
    }
};

TEST_F(ReshardingOplogApplierMetricsTest,
       IncrementInsertOnApplierMetricsShouldAlsoIncrementInstance) {
    auto metrics = createInstanceMetrics();

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("insertsApplied"), 0);

    ReshardingOplogApplierMetrics applierMetrics(metrics.get(), boost::none);
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

    ReshardingOplogApplierMetrics applierMetrics(metrics.get(), boost::none);
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

    ReshardingOplogApplierMetrics applierMetrics(metrics.get(), boost::none);
    applierMetrics.onDeleteApplied();

    ASSERT_EQ(applierMetrics.getDeletesApplied(), 1);
    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("deletesApplied"), 1);
}

TEST_F(ReshardingOplogApplierMetricsTest, ApplierInsertProgressIncrementsIdependentlyFromInstance) {
    auto metrics = createInstanceMetrics();

    ReshardingOplogApplierProgress progressDoc;
    progressDoc.setInsertsApplied(12);
    ReshardingOplogApplierMetrics applierMetrics(metrics.get(), progressDoc);

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
    ReshardingOplogApplierMetrics applierMetrics(metrics.get(), progressDoc);

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
    ReshardingOplogApplierMetrics applierMetrics(metrics.get(), progressDoc);

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
