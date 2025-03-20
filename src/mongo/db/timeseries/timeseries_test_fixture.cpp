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

#include <functional>
#include <numeric>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
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
void TimeseriesTestFixture::setUpCollectionsHelper(
    std::initializer_list<std::pair<NamespaceString*, UUID*>> collectionMetadata,
    std::function<BSONObj()> makeTimeseriesOptionsForCreateFn) {
    for (auto&& [ns, uuid] : collectionMetadata) {
        ASSERT_OK(createCollection(
            _opCtx,
            ns->dbName(),
            BSON("create" << ns->coll() << "timeseries" << makeTimeseriesOptionsForCreateFn())));
        AutoGetCollection autoColl(_opCtx, ns->makeTimeseriesBucketsNamespace(), MODE_IS);
        *uuid = autoColl.getCollection()->uuid();
    }
}

// Ensure that the input collection has a meta field and that at least one measurement has a
// meta field value.
void TimeseriesTestFixture::_assertCollWithMetaField(
    const NamespaceString& ns, std::vector<BSONObj> batchOfMeasurements) const {
    // Ensure that the input collection has a meta field.
    auto tsOptions = _getTimeseriesOptions(ns);
    auto metaField = tsOptions.getMetaField();
    ASSERT(metaField);

    // Ensure that that at least one measurement has a meta field value.
    ASSERT(std::any_of(
        batchOfMeasurements.begin(), batchOfMeasurements.end(), [metaField](BSONObj measurement) {
            return measurement.hasField(*metaField);
        }));
}

// Ensure that the input collection doesn't have a meta field. We don't have to check the
// measurements because we don't have a meta field in the collection.
void TimeseriesTestFixture::_assertCollWithoutMetaField(
    const NamespaceString& ns, std::vector<BSONObj> batchOfMeasurements) const {
    auto tsOptions = _getTimeseriesOptions(ns);
    auto metaField = tsOptions.getMetaField();
    ASSERT(!metaField);
}

// Ensure that the input collection is configured with a meta field. Ensure that no measurements in
// the collection have meta field values.
void TimeseriesTestFixture::_assertNoMetaFieldsInCollWithMetaField(
    const NamespaceString& ns, std::vector<BSONObj> batchOfMeasurements) const {
    // Ensure that the input collection has a meta field.
    auto tsOptions = _getTimeseriesOptions(ns);
    auto metaField = tsOptions.getMetaField();
    ASSERT(metaField);

    // Ensure that there no measurements have meta field values.
    ASSERT(std::none_of(batchOfMeasurements.begin(),
                        batchOfMeasurements.end(),
                        [](BSONObj measurement) { return measurement.hasField(_metaField); }));
}

void TimeseriesTestFixture::setUp() {
    CatalogTestFixture::setUp();

    _opCtx = operationContext();
    _bucketCatalog = &bucket_catalog::GlobalBucketCatalog::get(_opCtx->getServiceContext());

    // Create all collections with meta fields.
    setUpCollectionsHelper(
        std::initializer_list<std::pair<NamespaceString*, UUID*>>{{&_ns1, &_uuid1},
                                                                  {&_ns2, &_uuid2}},
        [this]() { return this->_makeTimeseriesOptionsForCreate(); });

    // Create all collections without meta fields.
    setUpCollectionsHelper(
        std::initializer_list<std::pair<NamespaceString*, UUID*>>{{&_nsNoMeta, &_uuidNoMeta}},
        [this]() { return this->_makeTimeseriesOptionsForCreateNoMetaField(); });
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
    return {_ns1, _ns2, _nsNoMeta};
}

BSONObj TimeseriesTestFixture::_makeTimeseriesOptionsForCreate() const {
    return BSON("timeField" << _timeField << "metaField" << _metaField);
}

BSONObj TimeseriesTestFixture::_makeTimeseriesOptionsForCreateNoMetaField() const {
    return BSON("timeField" << _timeField);
}

TimeseriesOptions TimeseriesTestFixture::_getTimeseriesOptions(const NamespaceString& ns) const {
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    return *autoColl->getTimeseriesOptions();
}

const CollatorInterface* TimeseriesTestFixture::_getCollator(const NamespaceString& ns) const {
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    return autoColl->getDefaultCollator();
}

//_generateMeasurementWithMetaFieldType enables us to generate a simple measurement with
// metaValue and timeValue.
//
// There are  simplifications with the Array, Object, RegEx, DBRef, and CodeWScope
// BSONTypes (we use the metaValue in a String component of the BSONElement rather than
// configuring the entire BSONType output).
BSONObj TimeseriesTestFixture::_generateMeasurementWithMetaFieldType(const BSONType type,
                                                                     const Date_t timeValue) const {
    switch (type) {
        case (Undefined):
            return BSON(_timeField << timeValue << _metaField << BSONUndefined);
        case (Date): {
            StatusWith<Date_t> date = dateFromISOString("2022-06-06T15:34:00.000Z");
            ASSERT(date.isOK());
            return BSON(_timeField << timeValue << _metaField << date.getValue());
        }
        case (MinKey):
            return BSON(_timeField << timeValue << _metaField << MINKEY);
        case (MaxKey):
            return BSON(_timeField << timeValue << _metaField << MAXKEY);
        case (jstNULL):
            return BSON(_timeField << timeValue << _metaField << BSONNULL);
        case (EOO):
            return BSON(_timeField << timeValue << _metaField << BSONObj());
        case (bsonTimestamp):
            return _generateMeasurementWithMetaFieldType(type, timeValue, Timestamp(1, 2));
        case (NumberInt):
            return _generateMeasurementWithMetaFieldType(type, timeValue, 365);
        case (NumberLong):
            return _generateMeasurementWithMetaFieldType(
                type, timeValue, static_cast<long long>(0x0123456789abcdefll));
        case (NumberDecimal):
            return _generateMeasurementWithMetaFieldType(type, timeValue, Decimal128("0.30"));
        case (NumberDouble):
            return _generateMeasurementWithMetaFieldType(type, timeValue, 1.5);
        case (jstOID):
            return _generateMeasurementWithMetaFieldType(
                type, timeValue, OID("dbdbdbdbdbdbdbdbdbdbdbdb"));
        case (Bool):
            return _generateMeasurementWithMetaFieldType(type, timeValue, true);
        case (BinData):
            return _generateMeasurementWithMetaFieldType(
                type, timeValue, BSONBinData("", 0, BinDataGeneral));
        default:
            return _generateMeasurementWithMetaFieldType(type, timeValue, _metaValue);
    }
    MONGO_UNREACHABLE;
}

BSONObj TimeseriesTestFixture::_generateMeasurementWithMetaFieldType(
    const BSONType type, const Date_t timeValue, const Timestamp metaValue) const {
    invariant(type == bsonTimestamp);
    return BSON(_timeField << timeValue << _metaField << metaValue);
}

BSONObj TimeseriesTestFixture::_generateMeasurementWithMetaFieldType(const BSONType type,
                                                                     const Date_t timeValue,
                                                                     const int metaValue) const {
    invariant(type == NumberInt);
    return BSON(_timeField << timeValue << _metaField << metaValue);
}

BSONObj TimeseriesTestFixture::_generateMeasurementWithMetaFieldType(
    const BSONType type, const Date_t timeValue, const long long metaValue) const {
    invariant(type == NumberLong);
    return BSON(_timeField << timeValue << _metaField << metaValue);
}

BSONObj TimeseriesTestFixture::_generateMeasurementWithMetaFieldType(
    const BSONType type, const Date_t timeValue, const Decimal128 metaValue) const {
    invariant(type == NumberDecimal);
    return BSON(_timeField << timeValue << _metaField << metaValue);
}

BSONObj TimeseriesTestFixture::_generateMeasurementWithMetaFieldType(const BSONType type,
                                                                     const Date_t timeValue,
                                                                     const double metaValue) const {
    invariant(type == NumberDouble);
    return BSON(_timeField << timeValue << _metaField << metaValue);
}

BSONObj TimeseriesTestFixture::_generateMeasurementWithMetaFieldType(const BSONType type,
                                                                     const Date_t timeValue,
                                                                     const OID metaValue) const {
    invariant(type == jstOID);
    return BSON(_timeField << timeValue << _metaField << metaValue);
}

BSONObj TimeseriesTestFixture::_generateMeasurementWithMetaFieldType(const BSONType type,
                                                                     const Date_t timeValue,
                                                                     const bool metaValue) const {
    invariant(type == Bool);
    return BSON(_timeField << timeValue << _metaField << metaValue);
}

BSONObj TimeseriesTestFixture::_generateMeasurementWithMetaFieldType(
    const BSONType type, const Date_t timeValue, const BSONBinData metaValue) const {
    invariant(type == BinData);
    return BSON(_timeField << timeValue << _metaField << metaValue);
}

BSONObj TimeseriesTestFixture::_generateMeasurementWithMetaFieldType(
    const BSONType type, const Date_t timeValue, const StringData metaValue) const {
    switch (type) {
        // Cases where the metaValue will be a part of the returned measurement.
        case (Object):
            return BSON(_timeField << timeValue << _metaField << BSON("obj" << metaValue));
        case (Array):
            return BSON(_timeField << timeValue << _metaField << BSONArray(BSON("0" << metaValue)));
        case (RegEx):
            return BSON(_timeField << timeValue << _metaField << BSONRegEx(metaValue, metaValue));
        case (DBRef):
            return BSON(_timeField << timeValue << _metaField
                                   << BSONDBRef(metaValue, OID("dbdbdbdbdbdbdbdbdbdbdbdb")));
        case (Code):
            return BSON(_timeField << timeValue << _metaField << BSONCode(metaValue));
        case (Symbol):
            return BSON(_timeField << timeValue << _metaField << BSONSymbol(metaValue));
        case (CodeWScope):
            return BSON(_timeField << timeValue << _metaField
                                   << BSONCodeWScope(metaValue, BSON("x" << 1)));
        default:
            return BSON(_timeField << timeValue << _metaField << metaValue);
    }
    MONGO_UNREACHABLE;
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
    boost::optional<StringData> metaValue = options.metaValue;
    Date_t timeValue = options.timeValue;
    BSONType metaValueType = options.metaValueType;

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

    // We should not be setting the metaValueType if the measurements don't have a metaValue.
    invariant(metaValue != boost::none || metaValueType == String);
    auto measurement = (metaValue)
        ? _generateMeasurementWithMetaFieldType(metaValueType, timeValue, *metaValue)
        : BSON(_timeField << timeValue);

    switch (reason) {
        case bucket_catalog::RolloverReason::kNone:
            for (size_t i = 0; i < numMeasurements; i++) {
                batchOfMeasurements.emplace_back(measurement);
            }
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kCount:
            for (auto i = 0; i < 2 * gTimeseriesBucketMaxCount; i++) {
                batchOfMeasurements.emplace_back(measurement);
            }
            return batchOfMeasurements;
        case bucket_catalog::RolloverReason::kTimeForward: {
            for (size_t i = 0; i < idxWithDiffMeasurement; i++) {
                batchOfMeasurements.emplace_back(measurement);
            }
            auto kTimeForwardMeasurement = (metaValue)
                ? BSON(_timeField << timeValue + Hours(2) << _metaField << *metaValue)
                : BSON(_timeField << timeValue + Hours(2));
            for (size_t i = idxWithDiffMeasurement; i < numMeasurements; i++) {
                batchOfMeasurements.emplace_back(kTimeForwardMeasurement);
            }
            return batchOfMeasurements;
        }
        case bucket_catalog::RolloverReason::kSchemaChange: {
            auto kSchemaChangeMeasurement1 = (metaValue)
                ? BSON(_timeField << timeValue << _metaField << *metaValue << "deathGrips"
                                  << "isOnline")
                : BSON(_timeField << timeValue << "deathGrips"
                                  << "isOnline");
            for (size_t i = 0; i < idxWithDiffMeasurement; i++) {
                batchOfMeasurements.emplace_back(kSchemaChangeMeasurement1);
            }
            // We want to guarantee that this measurement with different schema is at the
            // end of the BatchedInsertContext, so we make its time greater than the rest
            // of the measurements.
            auto kSchemaChangeMeasurement2 = (metaValue)
                ? BSON(_timeField << timeValue + Seconds(1) << _metaField << *metaValue
                                  << "deathGrips" << 100)
                : BSON(_timeField << timeValue + Seconds(1) << "deathGrips" << 100);
            for (size_t i = idxWithDiffMeasurement; i < numMeasurements; i++) {
                batchOfMeasurements.emplace_back(kSchemaChangeMeasurement2);
            }
            return batchOfMeasurements;
        }
        case bucket_catalog::RolloverReason::kTimeBackward: {
            for (size_t i = 0; i < idxWithDiffMeasurement; i++) {
                batchOfMeasurements.emplace_back(measurement);
            }
            auto kTimeBackwardMeasurement = (metaValue)
                ? BSON(_timeField << timeValue - Hours(1) << _metaField << *metaValue)
                : BSON(_timeField << timeValue - Hours(1));
            for (size_t i = idxWithDiffMeasurement; i < numMeasurements; i++) {
                batchOfMeasurements.emplace_back(kTimeBackwardMeasurement);
            }
            return batchOfMeasurements;
        }
        // kCachePressure and kSize are caused by the same measurements, but we have kCachePressure
        // when the cacheDerivedBucketSize < kLargeMeasurementsMaxBucketSize.
        // We can simulate this by lowering the _storageCacheSizeBytes or increasing the number of
        // active buckets.
        //
        // Note that we will need less large measurements to trigger kCachePressure because we use
        // _storageCacheSizeBytes to determine if we want to keep the bucket open due to large
        // measurements.
        case bucket_catalog::RolloverReason::kCachePressure: {
            auto bigMeasurement = (metaValue)
                ? BSON(_timeField << timeValue << _metaField << *metaValue << "big_field"
                                  << _bigStr)
                : BSON(_timeField << timeValue << "big_field" << _bigStr);
            for (auto i = 0; i < 4; i++) {
                batchOfMeasurements.emplace_back(bigMeasurement);
            }
            return batchOfMeasurements;
        }
        case bucket_catalog::RolloverReason::kSize: {
            auto bigMeasurement = (metaValue)
                ? BSON(_timeField << timeValue << _metaField << *metaValue << "big_field"
                                  << _bigStr)
                : BSON(_timeField << timeValue << "big_field" << _bigStr);
            for (auto i = 0; i < 125; i++) {
                batchOfMeasurements.emplace_back(bigMeasurement);
            }
            return batchOfMeasurements;
        }
    }
    return batchOfMeasurements;
}

uint64_t TimeseriesTestFixture::_getStorageCacheSizeBytes() const {
    return _opCtx->getServiceContext()->getStorageEngine()->getEngine()->getCacheSizeMB() * 1024 *
        1024;
}

long long TimeseriesTestFixture::_getExecutionStat(const UUID& uuid, StringData stat) {
    BSONObjBuilder builder;
    appendExecutionStats(*_bucketCatalog, uuid, builder);

    BSONObj obj = builder.obj();
    BSONElement e = obj.getField(stat);
    return e.isNumber() ? (int)e.number() : 0;
}
}  // namespace mongo::timeseries
