/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/db/timeseries/write_ops/internal/timeseries_write_ops_internal.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo::timeseries::bucket_catalog {
namespace {
constexpr StringData kNumClosedDueToCount = "numBucketsClosedDueToCount"_sd;
constexpr StringData kNumClosedDueToTimeForward = "numBucketsClosedDueToTimeForward"_sd;
constexpr StringData kNumClosedDueToSchemaChanges = "numBucketsClosedDueToSchemaChange"_sd;
constexpr StringData kNumClosedDueToSize = "numBucketsClosedDueToSize"_sd;
constexpr StringData kNumClosedDuetoCachePressure = "numBucketsClosedDueToCachePressure"_sd;

class BucketCatalogInternalTest : public TimeseriesTestFixture {
protected:
    TrackingContexts _trackingContexts;
    ExecutionStats _globalStats;

    void _rolloverWithRolloverReason(RolloverReason reason);
    void _testRolloverWithRolloverReasonUpdatesStats(RolloverReason reason, StringData stat);
};

void BucketCatalogInternalTest::_rolloverWithRolloverReason(RolloverReason reason) {
    auto timeseriesOptions = _getTimeseriesOptions(_ns1);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts = bucket_catalog::buildBatchedInsertContexts(*_bucketCatalog,
                                                                            _uuid1,
                                                                            timeseriesOptions,
                                                                            {_measurement},
                                                                            /*startIndex=*/0,
                                                                            /*numDocsToStage=*/1,
                                                                            /*docsToRetry=*/{},
                                                                            errorsAndIndices);
    ASSERT(errorsAndIndices.empty());

    auto batchedInsertCtx = batchedInsertContexts[0];
    auto measurementTimestamp = std::get<1>(batchedInsertCtx.measurementsTimesAndIndices[0]);

    Bucket& bucket =
        internal::allocateBucket(*_bucketCatalog,
                                 *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                 WithLock::withoutLock(),
                                 batchedInsertCtx.key,
                                 batchedInsertCtx.options,
                                 measurementTimestamp,
                                 nullptr,
                                 batchedInsertCtx.stats);

    bucket.rolloverReason = reason;
    ASSERT(allCommitted(bucket));
    internal::rollover(*_bucketCatalog,
                       *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                       WithLock::withoutLock(),
                       bucket,
                       bucket.rolloverReason);
}

void BucketCatalogInternalTest::_testRolloverWithRolloverReasonUpdatesStats(RolloverReason reason,
                                                                            StringData stat) {
    ASSERT_EQ(0, _getExecutionStat(_uuid1, stat));
    _rolloverWithRolloverReason(reason);
    ASSERT_EQ(1, _getExecutionStat(_uuid1, stat));
}

TEST_F(BucketCatalogInternalTest, UpdateRolloverStats) {
    auto _collectionStats = std::make_shared<ExecutionStats>();
    ExecutionStatsController stats(_collectionStats, _globalStats);

    // Ensure that both the globalStats and collectionStats are initially all set to 0.
    ASSERT_EQ(_globalStats.numBucketsClosedDueToTimeForward.load(), 0);
    ASSERT_EQ(_globalStats.numBucketsArchivedDueToTimeBackward.load(), 0);
    ASSERT_EQ(_globalStats.numBucketsClosedDueToCount.load(), 0);
    ASSERT_EQ(_globalStats.numBucketsClosedDueToCachePressure.load(), 0);
    ASSERT_EQ(_globalStats.numBucketsClosedDueToSize.load(), 0);
    ASSERT_EQ(_globalStats.numBucketsClosedDueToSchemaChange.load(), 0);

    ASSERT_EQ(_collectionStats->numBucketsClosedDueToTimeForward.load(), 0);
    ASSERT_EQ(_collectionStats->numBucketsArchivedDueToTimeBackward.load(), 0);
    ASSERT_EQ(_collectionStats->numBucketsClosedDueToCount.load(), 0);
    ASSERT_EQ(_collectionStats->numBucketsClosedDueToCachePressure.load(), 0);
    ASSERT_EQ(_collectionStats->numBucketsClosedDueToSize.load(), 0);
    ASSERT_EQ(_collectionStats->numBucketsClosedDueToSchemaChange.load(), 0);

    internal::updateRolloverStats(stats, RolloverReason::kTimeForward);
    ASSERT_EQ(_globalStats.numBucketsClosedDueToTimeForward.load(), 1);
    ASSERT_EQ(_collectionStats->numBucketsClosedDueToTimeForward.load(), 1);

    internal::updateRolloverStats(stats, RolloverReason::kTimeBackward);
    ASSERT_EQ(_globalStats.numBucketsArchivedDueToTimeBackward.load(), 1);
    ASSERT_EQ(_collectionStats->numBucketsArchivedDueToTimeBackward.load(), 1);

    internal::updateRolloverStats(stats, RolloverReason::kCount);
    ASSERT_EQ(_globalStats.numBucketsClosedDueToCount.load(), 1);
    ASSERT_EQ(_collectionStats->numBucketsClosedDueToCount.load(), 1);

    internal::updateRolloverStats(stats, RolloverReason::kCachePressure);
    ASSERT_EQ(_globalStats.numBucketsClosedDueToCachePressure.load(), 1);
    ASSERT_EQ(_collectionStats->numBucketsClosedDueToCachePressure.load(), 1);

    internal::updateRolloverStats(stats, RolloverReason::kSize);
    ASSERT_EQ(_globalStats.numBucketsClosedDueToSize.load(), 1);
    ASSERT_EQ(_collectionStats->numBucketsClosedDueToSize.load(), 1);

    internal::updateRolloverStats(stats, RolloverReason::kSchemaChange);
    ASSERT_EQ(_globalStats.numBucketsClosedDueToSchemaChange.load(), 1);
    ASSERT_EQ(_collectionStats->numBucketsClosedDueToSchemaChange.load(), 1);
}

TEST_F(BucketCatalogInternalTest, RolloverUpdatesRolloverStats) {
    std::vector<std::tuple<RolloverReason, StringData>> rolloverReasonAndMetricPairs = {
        std::make_tuple(RolloverReason::kCount, kNumClosedDueToCount),
        std::make_tuple(RolloverReason::kTimeForward, kNumClosedDueToTimeForward),
        std::make_tuple(RolloverReason::kSchemaChange, kNumClosedDueToSchemaChanges),
        std::make_tuple(RolloverReason::kSize, kNumClosedDueToSize),
        std::make_tuple(RolloverReason::kCachePressure, kNumClosedDuetoCachePressure),
    };
    for (const auto& [reason, metric] : rolloverReasonAndMetricPairs) {
        _testRolloverWithRolloverReasonUpdatesStats(reason, metric);
    }
}
}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
