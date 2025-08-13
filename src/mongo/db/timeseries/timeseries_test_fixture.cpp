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

#include "mongo/db/timeseries/timeseries_test_fixture.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/write_ops/internal/timeseries_write_ops_internal.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops.h"
#include "mongo/db/validate/collection_validation.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <numeric>
#include <utility>
#include <vector>

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

void TimeseriesTestFixture::validateCollectionsHelper(
    const std::set<NamespaceString>& collections) {
    ValidateResults validateResults;
    for (const NamespaceString& nss : collections) {
        ASSERT_OK(CollectionValidation::validate(
            _opCtx,
            nss,
            CollectionValidation::ValidationOptions{
                /*mode=*/CollectionValidation::ValidateMode::kForegroundFull,
                /*repairMode=*/CollectionValidation::RepairMode::kNone,
                /*logDiagnostics=*/false},
            &validateResults));
        ASSERT(validateResults.isValid());
    }
}

// Ensure that the input collection has a meta field and that at least one measurement has a
// meta field value.
void TimeseriesTestFixture::_assertCollWithMetaField(const NamespaceString& ns,
                                                     std::vector<BSONObj> measurements) const {
    // Ensure that the input collection has a meta field.
    auto tsOptions = _getTimeseriesOptions(ns);
    auto metaField = tsOptions.getMetaField();
    invariant(metaField);

    // Ensure that that at least one measurement has a meta field value.
    invariant(
        std::any_of(measurements.begin(), measurements.end(), [metaField](BSONObj measurement) {
            return measurement.hasField(*metaField);
        }));
}

// Ensure that the input collection doesn't have a meta field. We don't have to check the
// measurements because we don't have a meta field in the collection.
void TimeseriesTestFixture::_assertCollWithoutMetaField(const NamespaceString& ns) const {
    auto tsOptions = _getTimeseriesOptions(ns);
    auto metaField = tsOptions.getMetaField();
    invariant(!metaField);
}

void TimeseriesTestFixture::_assertNoMetaFieldsInCollWithMetaField(
    const NamespaceString& ns, std::vector<BSONObj> measurements) const {
    // Ensure that the input collection has a meta field.
    auto tsOptions = _getTimeseriesOptions(ns);
    auto metaField = tsOptions.getMetaField();
    invariant(metaField);

    // Ensure that there no measurements have meta field values.
    invariant(std::none_of(measurements.begin(), measurements.end(), [](BSONObj measurement) {
        return measurement.hasField(_metaField);
    }));
}

void TimeseriesTestFixture::setUp() {
    CatalogTestFixture::setUp();

    _opCtx = operationContext();
    _bucketCatalog = &bucket_catalog::GlobalBucketCatalog::get(_opCtx->getServiceContext());

    // Enables us to do data integrity checks on reopening and insert.
    gPerformTimeseriesCompressionIntermediateDataIntegrityCheckOnInsert.store(true);
    gPerformTimeseriesCompressionIntermediateDataIntegrityCheckOnReopening.store(true);

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

    // Validate all collections.
    validateCollectionsHelper(_collections);

    CatalogTestFixture::tearDown();
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

BSONObjBuilder TimeseriesTestFixture::_generateMeasurement(
    const boost::optional<BSONObj>& metaValue, const boost::optional<Date_t> timeValue) const {
    BSONObjBuilder builder;
    if (timeValue) {
        builder.appendDate(_timeField, *timeValue);
    }
    if (!metaValue) {
        return builder;
    }
    invariant(metaValue->hasField(_metaField));
    invariant(metaValue->nFields() == 1);
    builder.append((*metaValue).firstElement());
    return builder;
}

BSONObjBuilder TimeseriesTestFixture::_generateMeasurement(
    const boost::optional<BSONType> type, const boost::optional<Date_t> timeValue) const {
    if (!type) {
        BSONObjBuilder builder;
        if (timeValue) {
            builder.appendDate(_timeField, *timeValue);
        }
        return builder;
    }
    switch (*type) {
        case BSONType::undefined:
            return _generateMeasurement(_undefinedMeta, timeValue);
        case BSONType::minKey:
            return _generateMeasurement(_minKeyMeta, timeValue);
        case BSONType::maxKey:
            return _generateMeasurement(_maxKeyMeta, timeValue);
        case BSONType::null:
            return _generateMeasurement(_nullMeta, timeValue);
        case BSONType::eoo:
            return _generateMeasurement(_eooMeta, timeValue);
        case BSONType::date:
            return _generateMeasurement(_dateMeta, timeValue);
        case BSONType::timestamp:
            return _generateMeasurement(_bsonTimestampMeta, timeValue);
        case BSONType::numberInt:
            return _generateMeasurement(_intMeta, timeValue);
        case BSONType::numberLong:
            return _generateMeasurement(_longMeta, timeValue);
        case BSONType::numberDecimal:
            return _generateMeasurement(_decimalMeta, timeValue);
        case BSONType::numberDouble:
            return _generateMeasurement(_doubleMeta, timeValue);
        case BSONType::oid:
            return _generateMeasurement(_oidMeta, timeValue);
        case BSONType::boolean:
            return _generateMeasurement(_boolMeta, timeValue);
        case BSONType::binData:
            return _generateMeasurement(_binDataMeta, timeValue);
        case BSONType::object:
            return _generateMeasurement(_objMeta, timeValue);
        case BSONType::array:
            return _generateMeasurement(_arrayMeta, timeValue);
        case BSONType::regEx:
            return _generateMeasurement(_regexMeta, timeValue);
        case BSONType::dbRef:
            return _generateMeasurement(_dbRefMeta, timeValue);
        case BSONType::code:
            return _generateMeasurement(_codeMeta, timeValue);
        case BSONType::symbol:
            return _generateMeasurement(_symbolMeta, timeValue);
        case BSONType::codeWScope:
            return _generateMeasurement(_codeWScopeMeta, timeValue);
        default:
            return _generateMeasurement(_stringMeta, timeValue);
    }
    MONGO_UNREACHABLE;
}

std::vector<BSONObj> TimeseriesTestFixture::_generateMeasurementsWithRolloverReason(
    const MeasurementsWithRolloverReasonOptions& options) {
    std::vector<BSONObj> measurements;
    const bucket_catalog::RolloverReason reason = options.reason;
    size_t numMeasurements = options.numMeasurements;
    size_t idxWithDiffMeasurement = options.idxWithDiffMeasurement;
    Date_t timeValue = options.timeValue;
    const boost::optional<BSONObj>& metaValue = options.metaValue;
    const boost::optional<BSONType> metaValueType = options.metaValueType;

    // We don't want to enable specifying the number of measurements for kCount, kSize, and
    // kCachePressure because these RolloverReasons depend on a specific number of measurements.
    invariant(numMeasurements == static_cast<size_t>(gTimeseriesBucketMaxCount) ||
              (reason == bucket_catalog::RolloverReason::kTimeForward ||
               reason == bucket_catalog::RolloverReason::kTimeBackward ||
               reason == bucket_catalog::RolloverReason::kSchemaChange ||
               reason == bucket_catalog::RolloverReason::kNone ||
               reason == bucket_catalog::RolloverReason::kCount));

    // Besides kNone, we depend on numMeasurements > 2 so that we can make the output vector have
    // the appropriate RolloverReason.
    invariant(numMeasurements >= 2 || (reason == bucket_catalog::RolloverReason::kNone));

    // If the user inputs numMeasurements > gTimeseriesBucketMaxCount, we may not properly be
    // simulating the RolloverReason we want to because this measurements vector is now eligible
    // for RolloverReason::kCount.
    invariant(numMeasurements <= static_cast<size_t>(gTimeseriesBucketMaxCount) ||
              reason == bucket_catalog::RolloverReason::kCount);

    // We should only be setting the idxWithDiffMeasurement if we have the rollover reasons
    // kTimeForward, kTimeBackward, or kSchemaChange.
    invariant(idxWithDiffMeasurement == static_cast<size_t>(numMeasurements - 1) ||
              (reason == bucket_catalog::RolloverReason::kTimeForward ||
               reason == bucket_catalog::RolloverReason::kTimeBackward ||
               reason == bucket_catalog::RolloverReason::kSchemaChange));

    // We need to ensure that 'idxWithDiffMeasurement' is within the range of [1, numMeasurements).
    // Otherwise, we may not create a vector that leads to the input RolloverReason.
    invariant(numMeasurements == 1 ||
              (idxWithDiffMeasurement >= 1 && idxWithDiffMeasurement < numMeasurements));

    auto measurementBuilder = (metaValue) ? _generateMeasurement(metaValue, timeValue)
                                          : _generateMeasurement(metaValueType, timeValue);
    if (options.extraPayload) {
        std::string padding(options.extraPayload, 'a');
        measurementBuilder.append("padField", padding);
    }
    auto measurement = measurementBuilder.obj();

    switch (reason) {
        case bucket_catalog::RolloverReason::kNone:
            for (size_t i = 0; i < numMeasurements; i++) {
                measurements.emplace_back(measurement);
            }
            return measurements;
        case bucket_catalog::RolloverReason::kCount:
            for (size_t i = 0; i < numMeasurements; i++) {
                measurements.emplace_back(measurement);
            }
            return measurements;
        case bucket_catalog::RolloverReason::kTimeForward: {
            for (size_t i = 0; i < idxWithDiffMeasurement; i++) {
                measurements.emplace_back(measurement);
            }
            auto kTimeForwardMeasurement = (metaValue)
                ? _generateMeasurement(metaValue, timeValue + Hours(2)).obj()
                : _generateMeasurement(metaValueType, timeValue + Hours(2)).obj();
            for (size_t i = idxWithDiffMeasurement; i < numMeasurements; i++) {
                measurements.emplace_back(kTimeForwardMeasurement);
            }
            return measurements;
        }
        case bucket_catalog::RolloverReason::kSchemaChange: {
            auto kSchemaChangeMeasurementBuilder1 = (metaValue)
                ? _generateMeasurement(metaValue, timeValue)
                : _generateMeasurement(metaValueType, timeValue);
            kSchemaChangeMeasurementBuilder1.append("deathGrips", "isOnline");
            auto kSchemaChangeMeasurement1 = kSchemaChangeMeasurementBuilder1.obj();
            for (size_t i = 0; i < idxWithDiffMeasurement; i++) {
                measurements.emplace_back(kSchemaChangeMeasurement1);
            }
            // We want to guarantee that this measurement with different schema is at the
            // end of the BatchedInsertContext, so we make its time greater than the rest
            // of the measurements.
            auto kSchemaChangeMeasurementBuilder2 = (metaValue)
                ? _generateMeasurement(metaValue, timeValue + Seconds(1))
                : _generateMeasurement(metaValueType, timeValue + Seconds(1));
            kSchemaChangeMeasurementBuilder2.appendNumber("deathGrips", 100);
            auto kSchemaChangeMeasurement2 = kSchemaChangeMeasurementBuilder2.obj();
            for (size_t i = idxWithDiffMeasurement; i < numMeasurements; i++) {
                measurements.emplace_back(kSchemaChangeMeasurement2);
            }
            return measurements;
        }
        case bucket_catalog::RolloverReason::kTimeBackward: {
            for (size_t i = 0; i < idxWithDiffMeasurement; i++) {
                measurements.emplace_back(measurement);
            }
            auto kTimeBackwardMeasurement = (metaValue)
                ? _generateMeasurement(metaValue, timeValue - Hours(1)).obj()
                : _generateMeasurement(metaValueType, timeValue - Hours(1)).obj();
            for (size_t i = idxWithDiffMeasurement; i < numMeasurements; i++) {
                measurements.emplace_back(kTimeBackwardMeasurement);
            }
            return measurements;
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
            auto bigMeasurementBuilder = (metaValue)
                ? _generateMeasurement(metaValue, timeValue)
                : _generateMeasurement(metaValueType, timeValue);
            bigMeasurementBuilder.append("big_field", _bigStr);
            auto bigMeasurement = bigMeasurementBuilder.obj();
            for (auto i = 0; i < 4; i++) {
                measurements.emplace_back(bigMeasurement);
            }
            return measurements;
        }
        case bucket_catalog::RolloverReason::kSize: {
            auto bigMeasurementBuilder = (metaValue)
                ? _generateMeasurement(metaValue, timeValue)
                : _generateMeasurement(metaValueType, timeValue);
            bigMeasurementBuilder.append("big_field", _bigStr);
            auto bigMeasurement = bigMeasurementBuilder.obj();
            for (auto i = 0; i < 125; i++) {
                measurements.emplace_back(bigMeasurement);
            }
            return measurements;
        }
    }
    return measurements;
}

/**
 * Generates a bucket with the metadata from an input BatchedInsertContext.
 */
bucket_catalog::Bucket* TimeseriesTestFixture::_generateBucketWithBatch(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    bucket_catalog::BatchedInsertContext& batch,
    size_t batchIdx) const {
    auto measurementTimestamp = std::get<Date_t>(batch.measurementsTimesAndIndices[batchIdx]);
    return &bucket_catalog::internal::allocateBucket(*_bucketCatalog,
                                                     *_bucketCatalog->stripes[batch.stripeNumber],
                                                     WithLock::withoutLock(),
                                                     batch.key,
                                                     batch.options,
                                                     measurementTimestamp,
                                                     nullptr,
                                                     batch.stats);
}

void TimeseriesTestFixture::_assertBatchDoesNotRollover(const TimeseriesOptions& options,
                                                        bucket_catalog::BatchedInsertContext& batch,
                                                        bucket_catalog::Bucket* bucket) {
    // We track runningBucketSize to determine if we could rollover due to kSize or kCachePressure.
    size_t runningBucketSize = bucket->size;

    // We track runningBucketCount so we can determine if we could rollover due to kCount
    // (determineRolloverReason will only check bucket->numMeasurements, but we aren't staging
    // inserts).
    size_t runningBucketCount = bucket->numMeasurements;

    auto numActiveBuckets = _bucketCatalog->globalExecutionStats.numActiveBuckets.loadRelaxed();
    auto [effectiveMaxSize, cacheDerivedBucketMaxSize] =
        bucket_catalog::internal::getCacheDerivedBucketMaxSize(_storageCacheSizeBytes,
                                                               numActiveBuckets);
    int32_t absoluteMaxSize = std::min(bucket_catalog::Bucket::kLargeMeasurementsMaxBucketSize,
                                       cacheDerivedBucketMaxSize);
    for (size_t i = 0; i < batch.measurementsTimesAndIndices.size(); i++) {
        runningBucketCount += 1;
        ASSERT(runningBucketCount <= static_cast<size_t>(gTimeseriesBucketMaxCount));

        bucket_catalog::Bucket::NewFieldNames newFieldNamesToBeInserted;
        bucket_catalog::Sizes sizesToBeAdded;
        auto [measurement, measurementTimestamp, _] = batch.measurementsTimesAndIndices[i];
        bucket_catalog::calculateBucketFieldsAndSizeChange(_bucketCatalog->trackingContexts,
                                                           *bucket,
                                                           measurement,
                                                           options.getMetaField(),
                                                           newFieldNamesToBeInserted,
                                                           sizesToBeAdded);
        runningBucketSize += sizesToBeAdded.total();
        if (runningBucketSize > static_cast<size_t>(effectiveMaxSize)) {
            bool keepBucketOpenForLargeMeasurements =
                i < static_cast<size_t>(gTimeseriesBucketMinCount);
            ASSERT(keepBucketOpenForLargeMeasurements &&
                   runningBucketSize <= static_cast<size_t>(absoluteMaxSize));
        }

        auto rolloverReason =
            bucket_catalog::internal::determineRolloverReason(measurement,
                                                              options,
                                                              numActiveBuckets,
                                                              sizesToBeAdded,
                                                              measurementTimestamp,
                                                              _storageCacheSizeBytes,
                                                              nullptr,
                                                              *bucket,
                                                              batch.stats);
        ASSERT(rolloverReason == bucket_catalog::RolloverReason::kNone);
    }
}

void TimeseriesTestFixture::_stageInsertOneBatchIntoEligibleBucketHelper(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    bucket_catalog::BatchedInsertContext& batch,
    bucket_catalog::Bucket* bucket) {
    auto options = _getTimeseriesOptions(ns);
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();
    // Ensure that we don't rollover before staging an insert into the bucket.
    _assertBatchDoesNotRollover(options, batch, bucket);
    size_t currentPosition = 0;
    auto& stripe = *_bucketCatalog->stripes[batch.stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};
    auto writeBatch = activeBatch(_bucketCatalog->trackingContexts,
                                  *bucket,
                                  _opCtx->getOpID(),
                                  batch.stripeNumber,
                                  batch.stats);
    auto successfulInsertion = bucket_catalog::internal::stageInsertBatchIntoEligibleBucket(
        *_bucketCatalog,
        _opCtx->getOpID(),
        bucketsColl->getDefaultCollator(),
        batch,
        stripe,
        stripeLock,
        _storageCacheSizeBytes,
        *bucket,
        currentPosition,
        writeBatch);
    ASSERT_EQ(successfulInsertion, bucket_catalog::internal::StageInsertBatchResult::Success);
    ASSERT_EQ(currentPosition, batch.measurementsTimesAndIndices.size());
}

absl::InlinedVector<bucket_catalog::Bucket*, 8>
TimeseriesTestFixture::_generateBucketsWithMeasurements(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<std::pair<std::vector<BSONObj>, bucket_catalog::RolloverReason>>&
        measurementsAndRolloverReasons) {
    absl::InlinedVector<bucket_catalog::Bucket*, 8> buckets;
    auto options = _getTimeseriesOptions(ns);

    for (size_t i = 0; i < measurementsAndRolloverReasons.size(); i++) {
        std::vector<BSONObj> measurements = measurementsAndRolloverReasons[i].first;
        bucket_catalog::RolloverReason reason = measurementsAndRolloverReasons[i].second;
        // We don't enable creating buckets with kNone rollover reasons unless we are only creating
        // one bucket, because trying to call allocateBucket in debug mode with an uncleared open
        // bucket will invariant.
        //
        // We require the caller of this function to be responsible for setting/unsetting the
        // bucket's rollover reason as it is staging/committing writes to the bucket.
        ASSERT(measurementsAndRolloverReasons.size() == 1 ||
               reason != bucket_catalog::RolloverReason::kNone);
        std::vector<timeseries::bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
        auto batchedInsertContexts =
            bucket_catalog::buildBatchedInsertContexts(*_bucketCatalog,
                                                       collectionUUID,
                                                       options,
                                                       measurements,
                                                       /*startIndex=*/0,
                                                       /*numDocsToStage=*/measurements.size(),
                                                       /*docsToRetry=*/{},
                                                       errorsAndIndices);
        ASSERT(errorsAndIndices.empty());
        // We should only be creating one batch and it should be able to fit in one bucket.
        ASSERT_EQ(batchedInsertContexts.size(), 1);
        size_t numMeasurements = 0;
        for (size_t i = 0; i < batchedInsertContexts.size(); i++) {
            numMeasurements += batchedInsertContexts[i].measurementsTimesAndIndices.size();
        }
        ASSERT_EQ(numMeasurements, measurements.size());
        auto curBatch = batchedInsertContexts[0];
        bucket_catalog::Bucket* curBucket = _generateBucketWithBatch(ns, collectionUUID, curBatch);
        _stageInsertOneBatchIntoEligibleBucketHelper(ns, collectionUUID, curBatch, curBucket);
        curBucket->rolloverReason = reason;
        // Check that we have the same metadata across all the buckets we create; we know that all
        // the measurements within the same batch have the same meta field value if we didn't assert
        // in _stageInsertOneBatchIntoEligibleBucketHelper.
        //
        // Checking metadata allows us to cover both cases if we have a collection with or without
        // a meta field value.
        if (i != 0) {
            auto prevBucketMetadata = buckets[buckets.size() - 1]->key.metadata;
            ASSERT(prevBucketMetadata == curBucket->key.metadata);
        }
        buckets.push_back(std::move(curBucket));
    }
    ASSERT_EQ(measurementsAndRolloverReasons.size(), buckets.size());
    return buckets;
}

absl::InlinedVector<bucket_catalog::Bucket*, 8> TimeseriesTestFixture::_createFullBuckets(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    size_t numBuckets,
    boost::optional<BSONObj> metaValue = boost::none,
    boost::optional<BSONType> metaValueType = BSONType::string) {
    auto measurements =
        _generateMeasurementsWithRolloverReason({.reason = bucket_catalog::RolloverReason::kCount,
                                                 .metaValue = metaValue,
                                                 .metaValueType = metaValueType});
    std::vector<std::pair<std::vector<BSONObj>, bucket_catalog::RolloverReason>>
        measurementsAndRolloverReasons;
    for (size_t i = 0; i < numBuckets; i++) {
        measurementsAndRolloverReasons.push_back(
            std::make_pair(measurements, bucket_catalog::RolloverReason::kCount));
    }
    return _generateBucketsWithMeasurements(ns, collectionUUID, measurementsAndRolloverReasons);
}

uint64_t TimeseriesTestFixture::_getStorageCacheSizeBytes() const {
    return _opCtx->getServiceContext()->getStorageEngine()->getEngine()->getCacheSizeMB() * 1024 *
        1024;
}

void TimeseriesTestFixture::_addNsToValidate(const NamespaceString& ns) {
    _collections.insert(ns);
}

long long TimeseriesTestFixture::_getExecutionStat(const UUID& uuid, StringData stat) {
    BSONObjBuilder builder;
    appendExecutionStats(*_bucketCatalog, uuid, builder);

    BSONObj obj = builder.obj();
    BSONElement e = obj.getField(stat);
    return e.isNumber() ? (int)e.number() : 0;
}
}  // namespace mongo::timeseries
