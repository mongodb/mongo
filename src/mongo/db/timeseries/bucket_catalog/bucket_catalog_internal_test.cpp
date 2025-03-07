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

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo::timeseries::bucket_catalog {
namespace {
const std::string testDbName = "db_bucket_catalog_internal_test";
const TimeseriesOptions kTimeseriesOptions("time");

class BucketCatalogInternalTest : public CatalogTestFixture {
protected:
    using CatalogTestFixture::setUp;

protected:
    TrackingContexts _trackingContexts;
    ExecutionStats _globalStats;
};

TEST_F(BucketCatalogInternalTest, UpdateRolloverStats) {
    auto _collectionStats = std::make_shared<bucket_catalog::ExecutionStats>();
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
}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
