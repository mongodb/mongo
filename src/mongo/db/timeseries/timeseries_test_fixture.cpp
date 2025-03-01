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

#include <numeric>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo::timeseries {
void TimeseriesTestFixture::setUp() {
    CatalogTestFixture::setUp();

    _opCtx = operationContext();
    _bucketCatalog = &bucket_catalog::GlobalBucketCatalog::get(_opCtx->getServiceContext());

    for (auto&& [ns, uuid] : std::initializer_list<std::pair<NamespaceString*, UUID*>>{
             {&_ns1, &_uuid1}, {&_ns2, &_uuid2}, {&_ns3, &_uuid3}}) {
        ASSERT_OK(createCollection(
            _opCtx,
            ns->dbName(),
            BSON("create" << ns->coll() << "timeseries" << _makeTimeseriesOptionsForCreate())));
        AutoGetCollection autoColl(_opCtx, ns->makeTimeseriesBucketsNamespace(), MODE_IS);
        *uuid = autoColl.getCollection()->uuid();
    }
}

void TimeseriesTestFixture::tearDown() {
    // Validate that all tracked execution stats adds up to what is being tracked globally.
    bucket_catalog::ExecutionStats accumulated;
    for (auto&& execStats : _bucketCatalog->executionStats) {

        addCollectionExecutionGauges(accumulated, *execStats.second);
    }

    // Compare as BSON, this allows us to avoid enumerating all the individual stats in this
    // function and makes it robust to adding further stats in the future.
    auto execStatsToBSON = [](const bucket_catalog::ExecutionStats& stats) {
        BSONObjBuilder builder;
        appendExecutionStatsToBuilder(stats, builder);
        return builder.obj();
    };

    bucket_catalog::ExecutionStats global;

    // Filter out the gauges only
    addCollectionExecutionGauges(global, _bucketCatalog->globalExecutionStats);

    ASSERT_EQ(0, execStatsToBSON(global).woCompare(execStatsToBSON(accumulated)));

    CatalogTestFixture::tearDown();
}

std::vector<NamespaceString> TimeseriesTestFixture::getNamespaceStrings() {
    return {_ns1, _ns2, _ns3};
}

BSONObj TimeseriesTestFixture::_makeTimeseriesOptionsForCreate() const {
    return BSON("timeField" << _timeField << "metaField" << _metaField);
}

TimeseriesOptions TimeseriesTestFixture::_getTimeseriesOptions(const NamespaceString& ns) const {
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    return *autoColl->getTimeseriesOptions();
}

const CollatorInterface* TimeseriesTestFixture::_getCollator(const NamespaceString& ns) const {
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    return autoColl->getDefaultCollator();
}

// _generateMeasurementsWithRolloverReason enables us to easily get measurement vectors that have
// the input RolloverReason.
// Input conditions:
// numMeasurements is the number of measurements that should be returned in the measurement vector.
// Default: gTimeseriesBucketMaxCount
// 1 <= numMeasurements <= gTimeseriesBucketMaxCount    if kNone
// 2 <= numMeasurements <= gTimeseriesBucketMaxCount    if kSchemaChange,
//                                                         kTimeForward,
//                                                         kTimeBackward
// cannot set numMeasurements                           otherwise
//
// idxWithDiffMeasurement is the index where we change the record in a measurement vector to
// simulate a specific rollover reason. This is only used for kSchemaChange, kTimeForward,
// kTimeBackward.
// Default: numMeasurements - 1
// 1 <= idxWithDiffMeasurement <= numMeasurements       if kSchemaChange,
//                                                         kTimeForward,
//                                                         kTimeBackward
// cannot set idxWithDiffMeasurement                    otherwise
//
// metaValue and timeValue are what we set as the meta value and time value for a measurement, and
// have the defaults _metaValue and Date_t::now() respectively.
std::vector<BSONObj> TimeseriesTestFixture::_generateMeasurementsWithRolloverReason(
    const MeasurementsWithRolloverReasonOptions& options) const {
    std::vector<BSONObj> batchOfMeasurements;
    const bucket_catalog::RolloverReason reason = options.reason;
    size_t numMeasurements = options.numMeasurements;
    size_t idxWithDiffMeasurement = options.idxWithDiffMeasurement;
    StringData metaValue = options.metaValue;
    Date_t timeValue = options.timeValue;

    // We don't want to enable specifying the number of measurements for kCount, kSize, and
    // kCachePressure because these RolloverReasons depend on a specific number of measurements.
    invariant(numMeasurements == static_cast<size_t>(gTimeseriesBucketMaxCount) ||
              (reason == bucket_catalog::RolloverReason::kTimeForward ||
               reason == bucket_catalog::RolloverReason::kTimeBackward ||
               reason == bucket_catalog::RolloverReason::kSchemaChange ||
               reason == bucket_catalog::RolloverReason::kNone));

    // Besides kNone, we depend on numMeasurements > 2 so that we can make the output vector have
    // the appropriate RolloverReason.
    invariant(numMeasurements >= 2 || (reason == bucket_catalog::RolloverReason::kNone));

    // If the user inputs numMeasurements > gTimeseriesBucketMaxCount, we may not properly be
    // simulating the RolloverReason we want to because this measurements vector is now eligible
    // for RolloverReason::kCount.
    invariant(numMeasurements <= static_cast<size_t>(gTimeseriesBucketMaxCount));

    // We should only be setting the idxWithDiffMeasurement if we have the rollover reasons
    // kTimeForward, kTimeBackward, or kSchemaChange.
    invariant(idxWithDiffMeasurement == static_cast<size_t>(numMeasurements - 1) ||
              (reason == bucket_catalog::RolloverReason::kTimeForward ||
               reason == bucket_catalog::RolloverReason::kTimeBackward ||
               reason == bucket_catalog::RolloverReason::kSchemaChange));

    // We need to ensure that the idxWithDiffMeasurement isn't the first element. Otherwise, we may
    // not create a vector that causes the input RolloverReason.
    invariant(idxWithDiffMeasurement >= 1 && idxWithDiffMeasurement <= numMeasurements);

    switch (reason) {
        case bucket_catalog::RolloverReason::kNone:
            for (size_t i = 0; i < numMeasurements; i++) {
                batchOfMeasurements.emplace_back(
                    BSON(_timeField << timeValue << _metaField << metaValue));
            }
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kCount:
            for (auto i = 0; i < 2 * gTimeseriesBucketMaxCount; i++) {
                batchOfMeasurements.emplace_back(
                    BSON(_timeField << timeValue << _metaField << metaValue));
            }
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kTimeForward:
            for (size_t i = 0; i < idxWithDiffMeasurement; i++) {
                batchOfMeasurements.emplace_back(
                    BSON(_timeField << timeValue << _metaField << metaValue));
            }
            for (size_t i = idxWithDiffMeasurement; i < numMeasurements; i++) {
                batchOfMeasurements.emplace_back(
                    BSON(_timeField << timeValue + Hours(2) << _metaField << metaValue));
            }
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kSchemaChange:
            for (size_t i = 0; i < idxWithDiffMeasurement; i++) {
                batchOfMeasurements.emplace_back(BSON(_timeField << timeValue << _metaField
                                                                 << metaValue << "deathGrips"
                                                                 << "isOnline"));
            }
            // We want to guarantee that this measurement with different schema is at the
            // end of the BatchedInsertContext, so we make its time greater than the rest
            // of the measurements.
            for (size_t i = idxWithDiffMeasurement; i < numMeasurements; i++) {
                batchOfMeasurements.emplace_back(BSON(_timeField << timeValue + Seconds(1)
                                                                 << _metaField << metaValue
                                                                 << "deathGrips" << 100));
            }
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kTimeBackward:
            for (size_t i = 0; i < idxWithDiffMeasurement; i++) {
                batchOfMeasurements.emplace_back(
                    BSON(_timeField << timeValue << _metaField << metaValue));
            }
            for (size_t i = idxWithDiffMeasurement; i < numMeasurements; i++) {
                batchOfMeasurements.emplace_back(
                    BSON(_timeField << timeValue - Hours(1) << _metaField << metaValue));
            }
            return batchOfMeasurements;
        // kCachePressure and kSize are caused by the same measurements, but we have kCachePressure
        // when the cacheDerivedBucketSize < kLargeMeasurementsMaxBucketSize.
        // We can simulate this by lowering the _storageCacheSizeBytes or increasing the number of
        // active buckets.
        // In this layer, it is hard to simulate kCachePressure because we would need to decrease
        // the storageCacheSizeBytes or increase the number of active buckets to > 50.
        //
        // Note that we will need less large measurements to trigger kCachePressure because we use
        // _storageCacheSizeBytes to determine if we want to keep the bucket open due to large
        // measurements.
        case bucket_catalog::RolloverReason::kCachePressure:
            for (auto i = 0; i < 4; i++) {
                batchOfMeasurements.emplace_back(BSON(
                    _timeField << timeValue << _metaField << metaValue << "big_field" << _bigStr));
            }
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kSize:
            for (auto i = 0; i < 125; i++) {
                batchOfMeasurements.emplace_back(BSON(
                    _timeField << timeValue << _metaField << metaValue << "big_field" << _bigStr));
            }
            return batchOfMeasurements;
    }
    return batchOfMeasurements;
}

std::vector<BSONObj> TimeseriesTestFixture::_getFlattenedVector(
    const std::vector<std::vector<BSONObj>>& vectors) {
    size_t totalSize = std::accumulate(
        vectors.begin(), vectors.end(), size_t(0), [](size_t sum, const std::vector<BSONObj>& vec) {
            return sum + vec.size();
        });
    std::vector<BSONObj> result;
    result.reserve(totalSize);  // Reserve the total size to avoid multiple allocations
    // Use a range-based for loop to insert elements
    for (const auto& vec : vectors) {
        result.insert(result.end(), vec.begin(), vec.end());
    }
    return result;
}

uint64_t TimeseriesTestFixture::_getStorageCacheSizeBytes() const {
    return _opCtx->getServiceContext()->getStorageEngine()->getEngine()->getCacheSizeMB() * 1024 *
        1024;
}
}  // namespace mongo::timeseries
