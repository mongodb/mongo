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
#include "mongo/db/timeseries/timeseries_extended_range.h"
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


struct GenerateBucketOIDExtendedRangeParam {
    static GenerateBucketOIDExtendedRangeParam create(int64_t millisSinceEpoch, bool setsFlag) {
        return {.date = Date_t::fromMillisSinceEpoch(millisSinceEpoch), .setsFlag = setsFlag};
    }

    Date_t date;
    bool setsFlag{false};
};

std::ostream& operator<<(std::ostream& os, const GenerateBucketOIDExtendedRangeParam& param) {
    os << std::format("date: {}, setsFlag: {}", param.date.toString(), param.setsFlag);
    return os;
}

class GenerateBucketOIDExtendedRangeTest
    : public testing::TestWithParam<GenerateBucketOIDExtendedRangeParam> {};

TEST_P(GenerateBucketOIDExtendedRangeTest, ExtendedRangeFlagIsSetCorrectly) {
    const auto [date, expectExtendedRange] = GetParam();
    const auto options = std::invoke([] {
        TimeseriesOptions opts("time");
        opts.setGranularity(BucketGranularityEnum::Seconds);
        return opts;
    });

    const auto [oid, roundedTime] = internal::generateBucketOID(date, options);
    EXPECT_EQ(expectExtendedRange, timeseries::oidHasExtendedRangeTime(oid)) << fmt::format(
        "Input timestamp: {0:d}.{1:d}s ({2:#018x}), OID timestamp: {3:d} ({3:#010x})",
        date.toMillisSinceEpoch() / 1000,
        date.toMillisSinceEpoch() % 1000,
        date.toMillisSinceEpoch(),
        oid.getTimestamp());
}

INSTANTIATE_TEST_SUITE_P(
    GenerateBucketOIDExtendedRange,
    GenerateBucketOIDExtendedRangeTest,
    testing::Values(
        // Standard range: epoch
        GenerateBucketOIDExtendedRangeParam::create(0x0000'0000'0000'0000LL * 1000, false),
        // Standard range: 2021-01-01T00:00:00Z
        GenerateBucketOIDExtendedRangeParam::create(0x0000'0000'5FEE'6600LL * 1000, false),

        // Standard range: just under INT32_MAX (2038-01-19T03:13:00Z, multiple of 60)
        GenerateBucketOIDExtendedRangeParam::create(0x0000'0000'7FFF'FFCBLL * 1000, false),

        // Extended: 1 minute before epoch (negative timestamp)
        GenerateBucketOIDExtendedRangeParam::create(-0x3CLL * 1000, true),

        // Extended: well before epoch (~1958)
        GenerateBucketOIDExtendedRangeParam::create(-0x0000'0000'167F'0934LL * 1000, true),

        // Extended: just past INT32_MAX (0x8000'0044), truncation naturally has high bit
        GenerateBucketOIDExtendedRangeParam::create(0x0000'0000'8000'0044LL * 1000, true),

        // Extended: ~2106, truncation wraps to 0x2C — flag required to distinguish from epoch
        GenerateBucketOIDExtendedRangeParam::create(0x0000'0001'0000'002CLL * 1000, true),

        // Extended: ~2174, truncation wraps to 0x8000'0004 — high bit naturally present
        GenerateBucketOIDExtendedRangeParam::create(0x0000'0001'8000'0024LL * 1000, true),

        // Extended: ~2242, double wrap to 0x2C — flag required again
        GenerateBucketOIDExtendedRangeParam::create(0x0000'0002'0000'001CLL * 1000, true),

        // Extended: ~2242, double wrap to 0x2C — no flag
        GenerateBucketOIDExtendedRangeParam::create(0x0000'0000'7FFF'FFFFLL * 1000, false),

        // Extended: ~just past INT32_MAX, using milliseconds
        // This is floored to integer seconds based on timeseries options and the min cannot be
        // higher resolution than seconds, the flag should not be set as the control.min[timeField]
        // on the bucket will be within the standard range.
        GenerateBucketOIDExtendedRangeParam::create(0x0000'0000'7FFF'FFFFLL * 1000 + 1LL, false)));

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
