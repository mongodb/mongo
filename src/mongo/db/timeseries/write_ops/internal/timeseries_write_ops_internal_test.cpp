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

#include "mongo/bson/json.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/write_ops/internal/timeseries_write_ops_internal.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops.h"
#include "mongo/unittest/unittest.h"

namespace mongo::timeseries::write_ops::internal {
namespace {
static constexpr uint64_t kDefaultStorageCacheSizeBytes = 1024 * 1024 * 1024;
static constexpr uint64_t kLimitedStorageCacheSizeBytes = 1024;

class TimeseriesWriteOpsInternalTest : public CatalogTestFixture {
public:
    explicit TimeseriesWriteOpsInternalTest(Options options = {})
        : CatalogTestFixture(options.useReplSettings(true)) {}

protected:
    void setUp() override;

    virtual BSONObj _makeTimeseriesOptionsForCreate() const;

    std::vector<BSONObj> _generateMeasurementsWithRolloverReason(
        bucket_catalog::RolloverReason reason) const;

    TimeseriesOptions _getTimeseriesOptions(const NamespaceString& ns) const;

    const CollatorInterface* _getCollator(const NamespaceString& ns) const;

    uint64_t _getStorageCacheSizeBytes() const;

    void _testBuildBatchedInsertContextWithMetaField(
        std::vector<BSONObj>& userMeasurementsBatch,
        stdx::unordered_map<std::string, std::vector<size_t>>& metaFieldValueToCorrectIndexOrderMap,
        stdx::unordered_set<size_t>& expectedIndicesWithErrors) const;

    void _testBuildBatchedInsertContextWithoutMetaField(
        std::vector<BSONObj>& userMeasurementsBatch,
        std::vector<size_t>& correctIndexOrder,
        stdx::unordered_set<size_t>& expectedIndicesWithErrors) const;

    void _testStageInsertBatch(std::vector<BSONObj> batchOfMeasurements,
                               std::vector<size_t> numWriteBatches) const;

    OperationContext* _opCtx;
    bucket_catalog::BucketCatalog* _bucketCatalog;
    NamespaceString _ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_internal_test", "ts");

    StringData _timeField = "time";
    StringData _metaField = "meta";
    StringData _metaValue = "a";
    StringData _metaValue2 = "b";
    uint64_t _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;

    UUID _uuid = UUID::gen();

    // Strings used to simulate kSize/kCachePressure rollover reason.
    std::string _bigStr = std::string(1000, 'a');

    // Should not be called
    CompressAndWriteBucketFunc _compressBucket = nullptr;
};

void TimeseriesWriteOpsInternalTest::setUp() {
    CatalogTestFixture::setUp();

    _opCtx = operationContext();
    _bucketCatalog = &bucket_catalog::GlobalBucketCatalog::get(_opCtx->getServiceContext());

    ASSERT_OK(createCollection(
        _opCtx,
        _ns.dbName(),
        BSON("create" << _ns.coll() << "timeseries" << _makeTimeseriesOptionsForCreate())));
    AutoGetCollection autoColl(_opCtx, _ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    _uuid = autoColl.getCollection()->uuid();
}

BSONObj TimeseriesWriteOpsInternalTest::_makeTimeseriesOptionsForCreate() const {
    return BSON("timeField" << _timeField << "metaField" << _metaField);
}

TimeseriesOptions TimeseriesWriteOpsInternalTest::_getTimeseriesOptions(
    const NamespaceString& ns) const {
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    return *autoColl->getTimeseriesOptions();
}

const CollatorInterface* TimeseriesWriteOpsInternalTest::_getCollator(
    const NamespaceString& ns) const {
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    return autoColl->getDefaultCollator();
}

uint64_t TimeseriesWriteOpsInternalTest::_getStorageCacheSizeBytes() const {
    return _opCtx->getServiceContext()->getStorageEngine()->getEngine()->getCacheSizeMB() * 1024 *
        1024;
}

// _generateMeasurementsWithRolloverReason enables us to easily get measurement vectors that have
// the input RolloverReason.
// We require that when we call with kCachePressure, _storageCacheSizeBytes =
// kLimitedStorageCacheSizeBytes (must be set in the unit test) so that we properly trigger
// kCachePressure. Otherwise, _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes.
std::vector<BSONObj> TimeseriesWriteOpsInternalTest::_generateMeasurementsWithRolloverReason(
    const bucket_catalog::RolloverReason reason) const {
    invariant((_storageCacheSizeBytes == kDefaultStorageCacheSizeBytes &&
               reason != bucket_catalog::RolloverReason::kCachePressure) ||
              (_storageCacheSizeBytes == kLimitedStorageCacheSizeBytes &&
               reason == bucket_catalog::RolloverReason::kCachePressure));
    std::vector<BSONObj> batchOfMeasurements;
    switch (reason) {
        case bucket_catalog::RolloverReason::kNone:
            for (auto i = 0; i < gTimeseriesBucketMaxCount; i++) {
                batchOfMeasurements.emplace_back(
                    BSON(_timeField << Date_t::now() << _metaField << _metaValue));
            }
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kCount:
            for (auto i = 0; i < 2 * gTimeseriesBucketMaxCount; i++) {
                batchOfMeasurements.emplace_back(
                    BSON(_timeField << Date_t::now() << _metaField << _metaValue));
            }
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kTimeForward:
            for (auto i = 0; i < (gTimeseriesBucketMaxCount - 1); i++) {
                batchOfMeasurements.emplace_back(
                    BSON(_timeField << Date_t::now() << _metaField << _metaValue));
            }
            batchOfMeasurements.emplace_back(
                BSON(_timeField << Date_t::now() + Hours(2) << _metaField << _metaValue));
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kSchemaChange:
            for (auto i = 0; i < (gTimeseriesBucketMaxCount - 1); i++) {
                batchOfMeasurements.emplace_back(BSON(_timeField << Date_t::now() << _metaField
                                                                 << _metaValue << "deathGrips"
                                                                 << "isOnline"));
            }
            // We want to guarantee that this measurement with different schema is at the
            // end of the BatchedInsertContext, so we make it's time greater than the rest
            // of the measurements.
            batchOfMeasurements.emplace_back(BSON(_timeField << Date_t::now() + Seconds(1)
                                                             << _metaField << _metaValue
                                                             << "deathGrips" << 100));
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kTimeBackward:
            for (auto i = 0; i < (gTimeseriesBucketMaxCount - 1); i++) {
                batchOfMeasurements.emplace_back(
                    BSON(_timeField << Date_t::now() << _metaField << _metaValue));
            }
            batchOfMeasurements.emplace_back(
                BSON(_timeField << Date_t::now() - Hours(1) << _metaField << _metaValue));
            return batchOfMeasurements;
        // kCachePressure and kSize are caused by the same measurements, but we have kCachePressure
        // when the cacheDerivedBucketSize < kLargeMeasurementsMaxBucketSize.
        // We can simulate this by lowering the _storageCacheSizeBytes or increasing the number of
        // active buckets.
        //
        // Note that we will need less large measurements to trigger kCachePressure because we use
        // _storageCacheSizeBytes to determine if we want to keep the bucket open due to large
        // measurements.
        case bucket_catalog::RolloverReason::kCachePressure:
            for (auto i = 0; i < 4; i++) {
                batchOfMeasurements.emplace_back(BSON(_timeField << Date_t::now() << _metaField
                                                                 << _metaValue << "big_field"
                                                                 << _bigStr));
            }
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kSize:
            for (auto i = 0; i < 125; i++) {
                batchOfMeasurements.emplace_back(BSON(_timeField << Date_t::now() << _metaField
                                                                 << _metaValue << "big_field"
                                                                 << _bigStr));
            }
            return batchOfMeasurements;
    }
    return batchOfMeasurements;
}

void TimeseriesWriteOpsInternalTest::_testBuildBatchedInsertContextWithMetaField(
    std::vector<BSONObj>& userMeasurementsBatch,
    stdx::unordered_map<std::string, std::vector<size_t>>& metaFieldValueToCorrectIndexOrderMap,
    stdx::unordered_set<size_t>& expectedIndicesWithErrors) const {
    AutoGetCollection bucketsColl(_opCtx, _ns.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    tracking::Context trackingContext;
    timeseries::bucket_catalog::ExecutionStatsController stats;
    std::vector<timeseries::write_ops::internal::WriteStageErrorAndIndex> errorsAndIndices;

    auto batchedInsertContextVector =
        timeseries::write_ops::internal::buildBatchedInsertContextsWithMetaField(
            *_bucketCatalog,
            bucketsColl->uuid(),
            bucketsColl->getTimeseriesOptions().get(),
            userMeasurementsBatch,
            stats,
            trackingContext,
            errorsAndIndices);

    ASSERT_EQ(batchedInsertContextVector.size(), metaFieldValueToCorrectIndexOrderMap.size());

    // Check that all of the tuples in each BatchedInsertContext have the correct order and
    // measurement.
    for (size_t i = 0; i < batchedInsertContextVector.size(); i++) {
        auto insertBatchContext = batchedInsertContextVector[i];
        ASSERT_EQ(insertBatchContext.key.metadata.getMetaField().get(), _metaField);
        auto metaFieldValue = insertBatchContext.key.metadata.element().String();
        ASSERT_EQ(insertBatchContext.measurementsTimesAndIndices.size(),
                  metaFieldValueToCorrectIndexOrderMap[metaFieldValue].size());

        for (size_t j = 0; j < insertBatchContext.measurementsTimesAndIndices.size(); j++) {
            auto tuple = insertBatchContext.measurementsTimesAndIndices[j];
            auto measurement = std::get<BSONObj>(tuple);
            ASSERT_EQ(measurement[_metaField].String(), metaFieldValue);
            auto index = std::get<size_t>(tuple);
            ASSERT_EQ(index, metaFieldValueToCorrectIndexOrderMap[metaFieldValue][j]);
            ASSERT_EQ(userMeasurementsBatch[index].woCompare(measurement), 0);
        }
    }

    ASSERT_EQ(errorsAndIndices.size(), expectedIndicesWithErrors.size());

    // If we expected to see errors, check that the Statuses have the correct error code and
    // that there is a one-to-one mapping between indices that we expected to see errors for and
    // the indices that we did see errors for.
    for (size_t i = 0; i < errorsAndIndices.size(); i++) {
        auto writeStageErrorAndIndex = errorsAndIndices[i];
        ASSERT_EQ(writeStageErrorAndIndex.error.code(), ErrorCodes::BadValue);
        auto index = writeStageErrorAndIndex.index;
        ASSERT(expectedIndicesWithErrors.contains(index));
        expectedIndicesWithErrors.erase(index);
    }
    ASSERT(expectedIndicesWithErrors.empty());
};

void TimeseriesWriteOpsInternalTest::_testBuildBatchedInsertContextWithoutMetaField(
    std::vector<BSONObj>& userMeasurementsBatch,
    std::vector<size_t>& correctIndexOrder,
    stdx::unordered_set<size_t>& expectedIndicesWithErrors) const {
    AutoGetCollection autoColl(_opCtx, _ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();
    tracking::Context trackingContext;
    timeseries::bucket_catalog::ExecutionStatsController stats;
    std::vector<timeseries::write_ops::internal::WriteStageErrorAndIndex> errorsAndIndices;

    auto batchedInsertContextVector =
        timeseries::write_ops::internal::buildBatchedInsertContextsNoMetaField(
            *_bucketCatalog,
            bucketsColl->uuid(),
            bucketsColl->getTimeseriesOptions().get(),
            userMeasurementsBatch,
            stats,
            trackingContext,
            errorsAndIndices);

    // Since we are inserting measurements with metadata values, all of the measurements should
    // fit into one batch. The only exception here will be when all of the measurements are
    // malformed, in which case we should have an empty vector.
    if (expectedIndicesWithErrors.size() == userMeasurementsBatch.size()) {
        ASSERT_EQ(batchedInsertContextVector.size(), 0);
    } else {
        ASSERT_EQ(batchedInsertContextVector.size(), 1);
        auto batchedInsertContext = batchedInsertContextVector.front();
        ASSERT_EQ(batchedInsertContext.key.metadata.getMetaField(), boost::none);

        // Check that all of the tuples in the BatchedInsertContext have the correct order and
        // measurement.
        for (size_t i = 0; i < batchedInsertContext.measurementsTimesAndIndices.size(); i++) {
            auto tuple = batchedInsertContext.measurementsTimesAndIndices[i];
            ASSERT_EQ(correctIndexOrder[i], std::get<size_t>(tuple));
            ASSERT_EQ(
                userMeasurementsBatch[correctIndexOrder[i]].woCompare(std::get<BSONObj>(tuple)), 0);
        }
    }

    ASSERT_EQ(errorsAndIndices.size(), expectedIndicesWithErrors.size());

    // If we expected to see errors, check that the Statuses have the correct error code and
    // that there is a one-to-one mapping between indices that we expected to see errors for and
    // the indices that we did see errors for.
    for (size_t i = 0; i < errorsAndIndices.size(); i++) {
        auto writeStageErrorAndIndex = errorsAndIndices[i];
        ASSERT_EQ(writeStageErrorAndIndex.error.code(), ErrorCodes::BadValue);
        auto index = writeStageErrorAndIndex.index;
        ASSERT(expectedIndicesWithErrors.contains(index));
        expectedIndicesWithErrors.erase(index);
    }
    ASSERT(expectedIndicesWithErrors.empty());
};

// numWriteBatches is the size of the writeBatches that should be generated from the
// batchedInsertContext at the same index.
// numWriteBatches.size() == batchedInsertContexts.size()
// sum(numWriteBatches) == the total number of buckets that should be written to from the input
// batchOfMeasurements.
// Example with RolloverReason::kSchemaChange and two distinct meta fields:
// batchOfMeasurements = [
//                         {_timeField: Date_t::now(), _metaField: "a",  "deathGrips": "isOnline"},
//                         {_timeField: Date_t::now(), _metaField: "a",  "deathGrips": "isOffline"},
//                         {_timeField: Date_t::now() + Second(1), _metaField: "a",  "deathGrips":
//                         100},
//                         {_timeField: Date_t::now(), _metaField: "b",  "deathGrips": "isOffline"},
//                       ]
// The batchedInsertContexts = [
//                      {bucketKey: [...], stripeNumber: 0, options: [...], stats: [...],
//                          measurementsTimesAndIndices: [
//                              {_timeField: Date_t::now(), _metaField: "a",  "deathGrips":
//                              "isOnline"},
//                              {_timeField: Date_t::now(), _metaField: "a",  "deathGrips":
//                              "isOffline"},
//                              {_timeField: Date_t::now() + Second(1), _metaField: "a",
//                              "deathGrips": 100},
//                          ]},
//                       {bucketKey: [...], stripeNumber: 1, options: [...], stats: [...],
//                          measurementsTimesAndIndices: [
//                              {_timeField: Date_t::now(), _metaField: "b",  "deathGrips":
//                              "isOffline"},
//                          ]},
//                     ]
// Note: we simplified the measurementsTimesAndIndices vector to consist of measurement's BSONObj
// and not measurement's BatchedInsertTuple.
//
// numWriteBatches = [2, 1]
//
// We are accessing {_metaField: "a"}/batchedInsertContexts[0] and write the first two elements into
// one bucket.
// We detect a schema change with the "deathGrips" field for the last measurement in
// batchedInsertContexts[0].measurementsTimesAndIndices and write this measurement to a second
// bucket.
// This means for {_metaField: "a"}/batchedInsertContexts[0], we have two distinct write
// batches (numWriteBatches[0]).
//
// We then write one measurement to a third bucket because we have a distinct {_metaField:
// "b"} in batchedInsertContexts[1] (that doesn't hash to the same stripe).
// This means for {_metaField: "b"}/batchedInsertContexts[1], we have one distinct write batch
// (numWriteBatches[1]).

// If we are attempting to trigger kCachePressure, we must call this function with
// _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes. Otherwise, _storageCacheSizeBytes
// = kDefaultStorageCacheSizeBytes.
void TimeseriesWriteOpsInternalTest::_testStageInsertBatch(
    std::vector<BSONObj> batchOfMeasurements, std::vector<size_t> numWriteBatches) const {
    AutoGetCollection autoColl(_opCtx, _ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();
    auto timeseriesOptions = _getTimeseriesOptions(_ns);
    std::vector<timeseries::write_ops::internal::WriteStageErrorAndIndex> errorsAndIndices;

    auto batchedInsertContexts = write_ops::internal::buildBatchedInsertContexts(
        *_bucketCatalog, _uuid, timeseriesOptions, batchOfMeasurements, errorsAndIndices);
    ASSERT(errorsAndIndices.empty());

    ASSERT_EQ(batchedInsertContexts.size(), numWriteBatches.size());
    size_t numMeasurements = 0;
    for (size_t i = 0; i < batchedInsertContexts.size(); i++) {
        numMeasurements += batchedInsertContexts[i].measurementsTimesAndIndices.size();
    }
    ASSERT_EQ(numMeasurements, batchOfMeasurements.size());

    for (size_t i = 0; i < batchedInsertContexts.size(); i++) {
        auto writeBatches =
            write_ops::internal::stageInsertBatch(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl,
                                                  _opCtx->getOpID(),
                                                  nullptr /*comparator*/,
                                                  _storageCacheSizeBytes,
                                                  nullptr /*compressAndWriteBucketFunc*/,
                                                  batchedInsertContexts[i]);
        ASSERT_EQ(writeBatches.size(), numWriteBatches[i]);
    }
}

TEST_F(TimeseriesWriteOpsInternalTest, BuildBatchedInsertContextsOneBatchNoMetafield) {
    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:05:00.000Z"}, "x":1})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:04:00.000Z"}, "x":2})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:02:00.000Z"}, "x":3})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:03:00.000Z"}, "x":4})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:01:00.000Z"}, "x":5})"),
    };
    std::vector<size_t> correctIndexOrder{4, 2, 3, 1, 0};
    stdx::unordered_set<size_t> expectedIndicesWithErrors;
    _testBuildBatchedInsertContextWithoutMetaField(
        userMeasurementsBatch, correctIndexOrder, expectedIndicesWithErrors);
}

TEST_F(TimeseriesWriteOpsInternalTest, BuildBatchedInsertContextsOneBatchWithMetafield) {
    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:05:00.000Z"}, "meta":"a", "x":1})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:04:00.000Z"}, "meta":"a", "x":2})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:02:00.000Z"}, "meta":"a", "x":3})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:03:00.000Z"}, "meta":"a", "x":4})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:01:00.000Z"}, "meta":"a", "x":5})"),
    };

    stdx::unordered_map<std::string, std::vector<size_t>> metaFieldValueToCorrectIndexOrderMap;
    metaFieldValueToCorrectIndexOrderMap.try_emplace("a",
                                                     std::initializer_list<size_t>{4, 2, 3, 1, 0});
    stdx::unordered_set<size_t> expectedIndicesWithErrors;
    _testBuildBatchedInsertContextWithMetaField(
        userMeasurementsBatch, metaFieldValueToCorrectIndexOrderMap, expectedIndicesWithErrors);
}

TEST_F(TimeseriesWriteOpsInternalTest, BuildBatchedInsertContextsMultipleBatchesWithMetafield) {
    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:01:00.000Z"}, "meta":"b", "x":1})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:04:00.000Z"}, "meta":"c", "x":2})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:05:00.000Z"}, "meta":"a", "x":3})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:07:00.000Z"}, "meta":"a", "x":4})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:03:00.000Z"}, "meta":"b", "x":5})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:02:00.000Z"}, "meta":"c", "x":7})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:06:00.000Z"}, "meta":"a", "x":6})"),
    };
    stdx::unordered_map<std::string, std::vector<size_t>> metaFieldValueToCorrectIndexOrderMap;
    metaFieldValueToCorrectIndexOrderMap.try_emplace("a", std::initializer_list<size_t>{2, 6, 3});
    metaFieldValueToCorrectIndexOrderMap.try_emplace("b", std::initializer_list<size_t>{0, 4});
    metaFieldValueToCorrectIndexOrderMap.try_emplace("c", std::initializer_list<size_t>{5, 1});
    stdx::unordered_set<size_t> expectedIndicesWithErrors;
    _testBuildBatchedInsertContextWithMetaField(
        userMeasurementsBatch, metaFieldValueToCorrectIndexOrderMap, expectedIndicesWithErrors);
}

TEST_F(TimeseriesWriteOpsInternalTest,
       BuildBatchedInsertContextsNoMetaReportsMalformedMeasurements) {
    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"x":1})"),  // Malformed measurement, missing time field
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:04:00.000Z"}, "x":2})"),
        mongo::fromjson(R"({"x":3})"),  // Malformed measurement, missing time field
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:03:00.000Z"}, "x":4})"),
        mongo::fromjson(R"({"x":5})"),  // Malformed measurement, missing time field
    };
    std::vector<size_t> correctIndexOrder{3, 1};
    stdx::unordered_set<size_t> expectedIndicesWithErrors{0, 2, 4};
    _testBuildBatchedInsertContextWithoutMetaField(
        userMeasurementsBatch, correctIndexOrder, expectedIndicesWithErrors);
}

TEST_F(TimeseriesWriteOpsInternalTest,
       BuildBatchedInsertContextsWithMetaReportsMalformedMeasurements) {
    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:05:00.000Z"}, "meta":"a", "x":1})"),
        mongo::fromjson(R"({"meta":"a", "x":2})"),  // Malformed measurement, missing time field
        mongo::fromjson(R"({"x":3,"meta":"a"})"),   // Malformed measurement, missing time field
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:03:00.000Z"}, "meta":"b", "x":4})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:01:00.000Z"}, "meta":"a", "x":5})"),
    };
    stdx::unordered_map<std::string, std::vector<size_t>> metaFieldValueToCorrectIndexOrderMap;
    metaFieldValueToCorrectIndexOrderMap.try_emplace(_metaValue.toString(),
                                                     std::initializer_list<size_t>{4, 0});
    metaFieldValueToCorrectIndexOrderMap.try_emplace(_metaValue2.toString(),
                                                     std::initializer_list<size_t>{3});
    stdx::unordered_set<size_t> expectedIndicesWithErrors{1, 2};
    _testBuildBatchedInsertContextWithMetaField(
        userMeasurementsBatch, metaFieldValueToCorrectIndexOrderMap, expectedIndicesWithErrors);
}

TEST_F(TimeseriesWriteOpsInternalTest, BuildBatchedInsertContextsAllMeasurementsErrorNoMeta) {
    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"x":2})"),  // Malformed measurement, missing time field
        mongo::fromjson(R"({"x":3})"),  // Malformed measurement, missing time field
    };
    std::vector<size_t> correctIndexOrder;
    stdx::unordered_set<size_t> expectedIndicesWithErrors{0, 1};
    _testBuildBatchedInsertContextWithoutMetaField(
        userMeasurementsBatch, correctIndexOrder, expectedIndicesWithErrors);
};

TEST_F(TimeseriesWriteOpsInternalTest, BuildBatchedInsertContextsAllMeasurementsErrorWithMeta) {
    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"meta":"a", "x":2})"),  // Malformed measurement, missing time field
        mongo::fromjson(R"({"meta":"a", "x":3})"),  // Malformed measurement, missing time field
    };
    stdx::unordered_map<std::string, std::vector<size_t>> metaFieldValueToCorrectIndexOrderMap;
    stdx::unordered_set<size_t> expectedIndicesWithErrors{0, 1};
    _testBuildBatchedInsertContextWithMetaField(
        userMeasurementsBatch, metaFieldValueToCorrectIndexOrderMap, expectedIndicesWithErrors);
};

TEST_F(TimeseriesWriteOpsInternalTest, StageInsertBatchFillsUpSingleBucket) {
    std::vector<BSONObj> batchOfMeasurements =
        _generateMeasurementsWithRolloverReason(bucket_catalog::RolloverReason::kNone);
    std::vector<size_t> numWriteBatches{1};
    _testStageInsertBatch(batchOfMeasurements, numWriteBatches);
}

TEST_F(TimeseriesWriteOpsInternalTest, StageInsertBatchHandlesRolloverReasonkCount) {
    auto batchOfMeasurementsWithCount =
        _generateMeasurementsWithRolloverReason(bucket_catalog::RolloverReason::kCount);

    // batchOfMeasurements will be 2 * gTimeseriesBucketMaxCount measurements with all the
    // measurements having the same meta field and time, which means we should have two buckets.
    std::vector<size_t> numWriteBatches{2};
    _testStageInsertBatch(batchOfMeasurementsWithCount, numWriteBatches);
}

TEST_F(TimeseriesWriteOpsInternalTest, StageInsertBatchHandlesRolloverReasonkTimeForward) {
    auto batchOfMeasurementsWithTimeForward =
        _generateMeasurementsWithRolloverReason(bucket_catalog::RolloverReason::kTimeForward);

    // The last measurement will be outside of the bucket range, which will mean it should be in a
    // different bucket.
    std::vector<size_t> numWriteBatches{2};
    _testStageInsertBatch(batchOfMeasurementsWithTimeForward, numWriteBatches);
}

TEST_F(TimeseriesWriteOpsInternalTest, StageInsertBatchHandlesRolloverReasonkSchemaChange) {
    auto batchOfMeasurementsWithSchemaChange =
        _generateMeasurementsWithRolloverReason(bucket_catalog::RolloverReason::kSchemaChange);

    // The last measurement in the batch will have a int value rather than a string for field
    // "deathGrips", which means it will be in a different bucket.
    std::vector<size_t> numWriteBatches{2};
    _testStageInsertBatch(batchOfMeasurementsWithSchemaChange, numWriteBatches);
}

TEST_F(TimeseriesWriteOpsInternalTest, InsertBatchHandlesRolloverReasonkSize) {
    auto batchOfMeasurementsWithSize =
        _generateMeasurementsWithRolloverReason(bucket_catalog::RolloverReason::kSize);

    // The last measurement will exceed the size that the bucket can store, which will mean it
    // should be in a different bucket.
    std::vector<size_t> numWriteBatches{2};
    _testStageInsertBatch(batchOfMeasurementsWithSize, numWriteBatches);
}

TEST_F(TimeseriesWriteOpsInternalTest, InsertBatchHandlesRolloverReasonkCachePressure) {
    // Artificially lower _storageCacheSizeBytes so we can simulate kCachePressure.
    _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes;

    auto batchOfMeasurementsWithSize =
        _generateMeasurementsWithRolloverReason(bucket_catalog::RolloverReason::kCachePressure);

    // The last measurement will exceed the size that the bucket can store. Coupled with the lowered
    // cache size, we will trigger kCachePressure, so the measurement will be in a different bucket.
    std::vector<size_t> numWriteBatches{2};
    _testStageInsertBatch(batchOfMeasurementsWithSize, numWriteBatches);

    // Reset _storageCacheSizeBytes back to a representative value.
    _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;
}

TEST_F(TimeseriesWriteOpsInternalTest, PrepareInsertsToBucketsSimpleOneFullBucket) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_internal_test", "ts");

    auto tsOptions = _getTimeseriesOptions(_ns);
    std::vector<BSONObj> userBatch;
    for (auto i = 0; i < gTimeseriesBucketMaxCount; i++) {
        userBatch.emplace_back(BSON(_timeField << Date_t::now() << _metaField << _metaValue));
    }

    std::vector<WriteStageErrorAndIndex> errorsAndIndices;

    AutoGetCollection autoColl(_opCtx, _ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl,
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns),
                                                  _getStorageCacheSizeBytes(),
                                                  _compressBucket,
                                                  userBatch,
                                                  errorsAndIndices);

    ASSERT_TRUE(swWriteBatches.isOK());
    ASSERT_TRUE(errorsAndIndices.empty());

    auto& writeBatches = swWriteBatches.getValue();
    ASSERT_EQ(writeBatches.size(), 1);

    for (size_t i = 0; i < writeBatches.size(); i++) {
        ASSERT_EQ(writeBatches[i]->isReopened, false);
        ASSERT_EQ(writeBatches[i]->bucketIsSortedByTime, true);
        ASSERT_EQ(writeBatches[i]->opId, _opCtx->getOpID());
    }
}

TEST_F(TimeseriesWriteOpsInternalTest, PrepareInsertsToBucketsMultipleBucketsOneMeta) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_internal_test", "ts");

    auto tsOptions = _getTimeseriesOptions(_ns);
    std::vector<BSONObj> userBatch;
    for (auto i = 0; i < 2 * gTimeseriesBucketMaxCount; i++) {
        userBatch.emplace_back(BSON(_timeField << Date_t::now() << _metaField << _metaValue));
    }
    std::vector<WriteStageErrorAndIndex> errorsAndIndices;

    AutoGetCollection autoColl(_opCtx, _ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl,
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns),
                                                  _getStorageCacheSizeBytes(),
                                                  _compressBucket,
                                                  userBatch,
                                                  errorsAndIndices);

    ASSERT_TRUE(swWriteBatches.isOK());
    ASSERT_TRUE(errorsAndIndices.empty());

    auto& writeBatches = swWriteBatches.getValue();
    ASSERT_EQ(writeBatches.size(), 2);

    for (size_t i = 0; i < writeBatches.size(); i++) {
        ASSERT_EQ(writeBatches[i]->isReopened, false);
        ASSERT_EQ(writeBatches[i]->bucketIsSortedByTime, true);
        ASSERT_EQ(writeBatches[i]->opId, _opCtx->getOpID());
    }
}

TEST_F(TimeseriesWriteOpsInternalTest, PrepareInsertsToBucketsMultipleBucketsMultipleMetas) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_internal_test", "ts");

    auto tsOptions = _getTimeseriesOptions(_ns);
    std::vector<BSONObj> userBatch;
    for (auto i = 0; i < gTimeseriesBucketMaxCount; i++) {
        userBatch.emplace_back(BSON(_timeField << Date_t::now() << _metaField << _metaValue));
    }
    for (auto i = 0; i < gTimeseriesBucketMaxCount; i++) {
        userBatch.emplace_back(BSON(_timeField << Date_t::now() << _metaField << "m"));
    }
    std::vector<WriteStageErrorAndIndex> errorsAndIndices;

    AutoGetCollection autoColl(_opCtx, _ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl,
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns),
                                                  _getStorageCacheSizeBytes(),
                                                  _compressBucket,
                                                  userBatch,
                                                  errorsAndIndices);

    ASSERT_TRUE(swWriteBatches.isOK());
    ASSERT_TRUE(errorsAndIndices.empty());

    auto& writeBatches = swWriteBatches.getValue();
    ASSERT_EQ(writeBatches.size(), 2);

    for (size_t i = 0; i < writeBatches.size(); i++) {
        ASSERT_EQ(writeBatches[i]->isReopened, false);
        ASSERT_EQ(writeBatches[i]->bucketIsSortedByTime, true);
        ASSERT_EQ(writeBatches[i]->opId, _opCtx->getOpID());
    }
}

TEST_F(TimeseriesWriteOpsInternalTest,
       PrepareInsertsToBucketsMultipleBucketsMultipleMetasInterleaved) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_internal_test", "ts");

    auto tsOptions = _getTimeseriesOptions(_ns);
    std::vector<BSONObj> userBatch;
    for (auto i = 0; i < 2 * gTimeseriesBucketMaxCount; i++) {
        userBatch.emplace_back(BSON(_timeField << Date_t::now() << _metaField
                                               << (i % 2 == 0 ? _metaValue : _metaValue2)));
    }
    std::vector<WriteStageErrorAndIndex> errorsAndIndices;

    AutoGetCollection autoColl(_opCtx, _ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl,
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns),
                                                  _getStorageCacheSizeBytes(),
                                                  _compressBucket,
                                                  userBatch,
                                                  errorsAndIndices);

    ASSERT_TRUE(swWriteBatches.isOK());
    ASSERT_TRUE(errorsAndIndices.empty());

    auto& writeBatches = swWriteBatches.getValue();
    ASSERT_EQ(writeBatches.size(), 2);

    for (size_t i = 0; i < writeBatches.size(); i++) {
        ASSERT_EQ(writeBatches[i]->isReopened, false);
        ASSERT_EQ(writeBatches[i]->bucketIsSortedByTime, true);
        ASSERT_EQ(writeBatches[i]->opId, _opCtx->getOpID());
    }
}

TEST_F(TimeseriesWriteOpsInternalTest, PrepareInsertsBadMeasurementsAll) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_internal_test", "ts");

    auto tsOptions = _getTimeseriesOptions(_ns);
    std::vector<WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"meta":"a", "x":2})"),  // Malformed measurement, missing time field
        mongo::fromjson(R"({"meta":"a", "x":3})"),  // Malformed measurement, missing time field
    };

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl,
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns),
                                                  _getStorageCacheSizeBytes(),
                                                  _compressBucket,
                                                  userMeasurementsBatch,
                                                  errorsAndIndices);

    ASSERT_FALSE(swWriteBatches.isOK());
    ASSERT_EQ(errorsAndIndices.size(), 2);
}

TEST_F(TimeseriesWriteOpsInternalTest, PrepareInsertsBadMeasurementsSome) {
    NamespaceString ns = NamespaceString::createNamespaceString_forTest(
        "db_timeseries_write_ops_internal_test", "ts");

    auto tsOptions = _getTimeseriesOptions(_ns);
    std::vector<WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> userMeasurementsBatch{
        mongo::fromjson(R"({"meta":"a", "x":2})"),  // Malformed measurement, missing time field
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:05:00.000Z"}, "meta":"a", "x":3})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:02:00.000Z"}, "meta":"a", "x":3})"),
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T10:03:00.000Z"}, "meta":"b", "x":3})"),
        mongo::fromjson(R"({"meta":"b", "x":3})"),  // Malformed measurement, missing time field
        mongo::fromjson(R"({"time":{"$date":"2025-01-30T09:02:00.000Z"}, "meta":"b", "x":3})"),
    };

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl,
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns),
                                                  _getStorageCacheSizeBytes(),
                                                  _compressBucket,
                                                  userMeasurementsBatch,
                                                  errorsAndIndices);

    ASSERT_FALSE(swWriteBatches.isOK());
    ASSERT_EQ(errorsAndIndices.size(), 2);

    ASSERT_EQ(errorsAndIndices[0].index, 0);
    ASSERT_EQ(errorsAndIndices[1].index, 4);
}

}  // namespace
}  // namespace mongo::timeseries::write_ops::internal
