/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::timeseries::bucket_catalog {
namespace {
constexpr StringData kNumActiveBuckets = "numActiveBuckets"_sd;
constexpr StringData kNumSchemaChanges = "numBucketsClosedDueToSchemaChange"_sd;
constexpr StringData kNumBucketsReopened = "numBucketsReopened"_sd;
constexpr StringData kNumArchivedDueToMemoryThreshold = "numBucketsArchivedDueToMemoryThreshold"_sd;
constexpr StringData kNumClosedDueToReopening = "numBucketsClosedDueToReopening"_sd;
constexpr StringData kNumClosedDueToTimeForward = "numBucketsClosedDueToTimeForward"_sd;
constexpr StringData kNumClosedDueToMemoryThreshold = "numBucketsClosedDueToMemoryThreshold"_sd;

class BucketCatalogTest : public TimeseriesTestFixture {
protected:
    class RunBackgroundTaskAndWaitForFailpoint {
        stdx::thread _taskThread;

    public:
        RunBackgroundTaskAndWaitForFailpoint(const std::string& failpointName,
                                             std::function<void()>&& fn);
        ~RunBackgroundTaskAndWaitForFailpoint();
    };

    std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
    _makeOperationContext();

    void _commit(const NamespaceString& ns,
                 const std::shared_ptr<WriteBatch>& batch,
                 uint16_t numPreviouslyCommittedMeasurements,
                 size_t expectedBatchSize = 1);

    std::shared_ptr<bucket_catalog::WriteBatch> _insertOneWithoutReopening(
        OperationContext* opCtx,
        BucketCatalog& catalog,
        const mongo::NamespaceString& nss,
        const UUID& uuid,
        const mongo::BSONObj& doc);

    // Check that each group of objects has compatible schema with itself, but that inserting the
    // first object in new group closes the existing bucket and opens a new one
    void _testMeasurementSchema(
        const std::initializer_list<std::initializer_list<BSONObj>>& groups);

    StatusWith<tracking::unique_ptr<Bucket>> _testRehydrateBucket(const CollectionPtr& coll,
                                                                  const BSONObj& bucketDoc);

    Status _reopenBucket(
        const CollectionPtr& coll,
        const BSONObj& bucketDoc,
        const boost::optional<unsigned long>& rehydrateEra = boost::none,
        const boost::optional<unsigned long>& loadBucketIntoCatalogEra = boost::none,
        const boost::optional<BucketKey>& bucketKey = boost::none,
        const boost::optional<internal::BucketDocumentValidator>& documentValidator = boost::none);

    BSONObj _getCompressedBucketDoc(const BSONObj& bucketDoc);

    BSONObj _getMetadata(BucketCatalog& catalog, const BucketId& bucketId);

    RolloverReason _rolloverReason(const std::shared_ptr<WriteBatch>& batch);

    void _testBucketMetadataFieldOrdering(const BSONObj& inputMetadata,
                                          const BSONObj& expectedMetadata);
    boost::optional<bucket_catalog::BucketMetadata> getBucketMetadata(BSONObj measurement) const;

    void _testBuildBatchedInsertContextWithMetaField(
        std::vector<BSONObj>& userMeasurementsBatch,
        stdx::unordered_map<bucket_catalog::BucketMetadata, std::vector<size_t>>&
            metaFieldMetadataToCorrectIndexOrderMap,
        stdx::unordered_set<size_t>& expectedIndicesWithErrors) const;

    void _testBuildBatchedInsertContextOneBatchWithSameMetaFieldType(BSONType type) const;

    template <typename T>
    void _testBuildBatchedInsertContextMultipleBatchesWithSameMetaFieldType(
        BSONType type = BSONType::string,
        std::vector<T> metaValues = {_metaValue, _metaValue2, _metaValue3}) const;

    void _testBuildBatchedInsertContextWithoutMetaField(
        const NamespaceString& ns,
        const std::vector<BSONObj>& userMeasurementsBatch,
        const std::vector<size_t>& correctIndexOrder,
        stdx::unordered_set<size_t>& expectedIndicesWithErrors) const;

    void _testBuildBatchedInsertContextWithoutMetaFieldInCollWithMetaField(
        const NamespaceString& ns,
        const std::vector<BSONObj>& userMeasurementsBatch,
        const std::vector<size_t>& correctIndexOrder,
        stdx::unordered_set<size_t>& expectedIndicesWithErrors) const;

    void _testBuildBatchedInsertContextWithoutMetaFieldInCollWithoutMetaField(
        const NamespaceString& ns,
        const std::vector<BSONObj>& userMeasurementsBatch,
        const std::vector<size_t>& correctIndexOrder,
        stdx::unordered_set<size_t>& expectedIndicesWithErrors) const;

    void _testStageInsertBatch(const NamespaceString& ns,
                               const UUID& collectionUUID,
                               const std::vector<BSONObj>& batchOfMeasurements,
                               const std::vector<size_t>& numWriteBatches) const;

    void _testStageInsertBatchWithMetaField(const NamespaceString& ns,
                                            const UUID& collectionUUID,
                                            const std::vector<BSONObj>& batchOfMeasurements,
                                            const std::vector<size_t>& numWriteBatches) const;

    void _testStageInsertBatchInCollWithoutMetaField(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& batchOfMeasurements,
        const std::vector<size_t>& numWriteBatches) const;

    void _testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(
        BSONType type, std::vector<BSONObj> metaValues) const;

    void _testStageInsertBatchWithoutMetaFieldInCollWithMetaField(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& batchOfMeasurements,
        const std::vector<size_t>& numWriteBatches) const;

    void _testStageInsertBatchIntoEligibleBucket(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& batchOfMeasurements,
        const std::vector<size_t>& curBatchedInsertContextsIndex,
        const std::vector<size_t>& numMeasurementsInWriteBatch,
        size_t numBatchedInsertContexts,
        boost::optional<absl::InlinedVector<Bucket*, 8>> buckets = boost::none) const;

    void _testStageInsertBatchIntoEligibleBucketWithMetaField(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& batchOfMeasurements,
        const std::vector<size_t>& curBatchedInsertContextsIndex,
        const std::vector<size_t>& numMeasurementsInWriteBatch,
        size_t numBatchedInsertContexts,
        boost::optional<absl::InlinedVector<Bucket*, 8>> buckets = boost::none) const;

    void _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& batchOfMeasurements,
        const std::vector<size_t>& curBatchedInsertContextsIndex,
        const std::vector<size_t>& numMeasurementsInWriteBatch,
        size_t numBatchedInsertContexts,
        boost::optional<absl::InlinedVector<Bucket*, 8>> buckets = boost::none) const;

    void _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& batchOfMeasurements,
        const std::vector<size_t>& curBatchedInsertContextsIndex,
        const std::vector<size_t>& numMeasurementsInWriteBatch,
        size_t numBatchedInsertContexts,
        boost::optional<absl::InlinedVector<Bucket*, 8>> buckets = boost::none) const;

    void _testCreateOrderedPotentialBucketsVector(
        PotentialBucketOptions& potentialBucketOptions) const;

    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> _getMeasurementRolloverReasonVec(
        const std::vector<std::vector<BSONObj>>& measurements, const RolloverReason& reason) const;

    // We generate these measurements without a rollover reason to guarantee that the measurements
    // we create will fit into one bucket, but we can mark the bucket generated with a rollover
    // reason.
    const std::vector<BSONObj> measurement1Vec = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone, .numMeasurements = 1});
    const std::vector<BSONObj> measurement2Vec = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone, .numMeasurements = 2});
    const std::vector<BSONObj> measurement3Vec = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone, .numMeasurements = 3});
    const std::vector<BSONObj> measurement4Vec = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone, .numMeasurements = 4});
    const std::vector<BSONObj> measurement5Vec = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone, .numMeasurements = 5});
    const std::vector<BSONObj> measurementTimeseriesBucketMaxCountMinus1Vec =
        _generateMeasurementsWithRolloverReason(
            {.reason = RolloverReason::kNone,
             .numMeasurements = static_cast<size_t>(gTimeseriesBucketMaxCount - 1)});
    const std::vector<BSONObj> measurementTimeseriesBucketMaxCountVec =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kNone});

    const std::vector<std::vector<BSONObj>> unsorted1{measurement1Vec,
                                                      measurement2Vec,
                                                      measurement3Vec,
                                                      measurementTimeseriesBucketMaxCountVec,
                                                      measurement5Vec};
    const std::vector<std::vector<BSONObj>> unsorted2{
        measurementTimeseriesBucketMaxCountVec,
        measurement4Vec,
        measurement2Vec,
        measurement3Vec,
    };
    const std::vector<std::vector<BSONObj>> unsorted3{measurement1Vec,
                                                      measurement2Vec,
                                                      measurement1Vec,
                                                      measurement4Vec,
                                                      measurement3Vec,
                                                      measurement4Vec};
    const std::vector<std::vector<BSONObj>> unsorted4{measurementTimeseriesBucketMaxCountVec,
                                                      measurementTimeseriesBucketMaxCountVec,
                                                      measurementTimeseriesBucketMaxCountVec,
                                                      measurementTimeseriesBucketMaxCountVec,
                                                      measurementTimeseriesBucketMaxCountVec,
                                                      measurement1Vec};
    const std::vector<std::vector<BSONObj>> unsortedLong1 =
        _getFlattenedVector(std::vector<std::vector<std::vector<BSONObj>>>{unsorted1, unsorted2});
    const std::vector<std::vector<BSONObj>> unsortedLong2 =
        _getFlattenedVector(std::vector<std::vector<std::vector<BSONObj>>>{unsorted2, unsorted3});
    const std::vector<std::vector<BSONObj>> unsortedLong3 =
        _getFlattenedVector(std::vector<std::vector<std::vector<BSONObj>>>{unsorted3, unsorted4});
    const std::vector<std::vector<BSONObj>> sorted1{measurement1Vec,
                                                    measurement2Vec,
                                                    measurement3Vec,
                                                    measurement4Vec,
                                                    measurement5Vec,
                                                    measurementTimeseriesBucketMaxCountVec};
    const std::vector<std::vector<BSONObj>> sorted2{measurement2Vec,
                                                    measurement3Vec,
                                                    measurement3Vec,
                                                    measurement3Vec,
                                                    measurementTimeseriesBucketMaxCountVec,
                                                    measurementTimeseriesBucketMaxCountVec,
                                                    measurementTimeseriesBucketMaxCountVec};
    const std::vector<std::vector<BSONObj>> sorted3{
        measurement1Vec, measurement1Vec, measurement2Vec, measurement2Vec, measurement4Vec};
    const std::vector<std::vector<std::vector<BSONObj>>> allMeasurementVecs =
        std::vector<std::vector<std::vector<BSONObj>>>{unsorted1,
                                                       unsorted2,
                                                       unsorted3,
                                                       unsorted4,
                                                       unsortedLong1,
                                                       unsortedLong2,
                                                       unsortedLong3,
                                                       sorted1,
                                                       sorted2,
                                                       sorted3};
};


BucketCatalogTest::RunBackgroundTaskAndWaitForFailpoint::RunBackgroundTaskAndWaitForFailpoint(
    const std::string& failpointName, std::function<void()>&& fn) {
    auto fp = globalFailPointRegistry().find(failpointName);
    auto timesEntered = fp->setMode(FailPoint::alwaysOn, 0);

    // Start background job.
    _taskThread = stdx::thread(std::move(fn));

    // Once we hit the failpoint once, turn it off.
    fp->waitForTimesEntered(timesEntered + 1);
    fp->setMode(FailPoint::off, 0);
}

BucketCatalogTest::RunBackgroundTaskAndWaitForFailpoint::~RunBackgroundTaskAndWaitForFailpoint() {
    _taskThread.join();
}

std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
BucketCatalogTest::_makeOperationContext() {
    auto client = getServiceContext()->getService()->makeClient("BucketCatalogTest");
    auto opCtx = client->makeOperationContext();
    return {std::move(client), std::move(opCtx)};
}

void BucketCatalogTest::_commit(const NamespaceString& ns,
                                const std::shared_ptr<WriteBatch>& batch,
                                uint16_t numPreviouslyCommittedMeasurements,
                                size_t expectedBatchSize) {
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(ns)));
    ASSERT_EQ(batch->measurements.size(), expectedBatchSize) << batch->toBSON();
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, numPreviouslyCommittedMeasurements)
        << batch->toBSON();
    finish(*_bucketCatalog, batch);
}

std::shared_ptr<bucket_catalog::WriteBatch> BucketCatalogTest::_insertOneWithoutReopening(
    OperationContext* opCtx,
    BucketCatalog& catalog,
    const mongo::NamespaceString& nss,
    const UUID& uuid,
    const mongo::BSONObj& doc) {
    auto timeseriesOptions = _getTimeseriesOptions(nss);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts = bucket_catalog::buildBatchedInsertContexts(catalog,
                                                                            uuid,
                                                                            timeseriesOptions,
                                                                            {doc},
                                                                            /*startIndex=*/0,
                                                                            /*numDocsToStage=*/1,
                                                                            /*docsToRetry=*/{},
                                                                            errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    auto batchedInsertCtx = batchedInsertContexts[0];
    auto measurementTimestamp = std::get<Date_t>(batchedInsertCtx.measurementsTimesAndIndices[0]);

    auto& stripe = *catalog.stripes[batchedInsertCtx.stripeNumber];
    auto bucketKey = batchedInsertCtx.key;
    auto options = batchedInsertCtx.options;
    auto& stats = batchedInsertCtx.stats;
    auto collator = _getCollator(nss);
    Bucket& bucket = [&]() -> Bucket& {
        auto allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
        auto bucketOpenedDueToMetadata = true;
        if (auto eligibleBucket = findOpenBucketForMeasurement(catalog,
                                                               stripe,
                                                               WithLock::withoutLock(),
                                                               doc,
                                                               bucketKey,
                                                               measurementTimestamp,
                                                               options,
                                                               collator,
                                                               _storageCacheSizeBytes,
                                                               allowQueryBasedReopening,
                                                               stats,
                                                               bucketOpenedDueToMetadata)) {
            return *eligibleBucket;
        }

        // Roll over buckets to preemptively update the stats.
        findAndRolloverOpenBuckets(catalog,
                                   stripe,
                                   WithLock::withoutLock(),
                                   bucketKey,
                                   measurementTimestamp,
                                   Seconds(*options.getBucketMaxSpanSeconds()),
                                   allowQueryBasedReopening,
                                   bucketOpenedDueToMetadata);

        return internal::allocateBucket(catalog,
                                        stripe,
                                        WithLock::withoutLock(),
                                        bucketKey,
                                        options,
                                        measurementTimestamp,
                                        collator,
                                        stats);
    }();

    auto opId = opCtx->getOpID();
    size_t currentPosition = 0;
    std::shared_ptr<bucket_catalog::WriteBatch> writeBatch =
        activeBatch(catalog.trackingContexts, bucket, opId, batchedInsertCtx.stripeNumber, stats);
    ASSERT_EQ(bucket_catalog::internal::StageInsertBatchResult::Success,
              bucket_catalog::internal::stageInsertBatchIntoEligibleBucket(catalog,
                                                                           opId,
                                                                           collator,
                                                                           batchedInsertCtx,
                                                                           stripe,
                                                                           WithLock::withoutLock(),
                                                                           _storageCacheSizeBytes,
                                                                           bucket,
                                                                           currentPosition,
                                                                           writeBatch));

    return writeBatch;
}

void BucketCatalogTest::_testMeasurementSchema(
    const std::initializer_list<std::initializer_list<BSONObj>>& groups) {
    // Make sure we start and end with a clean slate.
    clear(*_bucketCatalog, _uuid1);
    ScopeGuard guard([this]() { clear(*_bucketCatalog, _uuid1); });

    bool firstGroup = true;
    for (const auto& group : groups) {
        bool firstMember = true;
        for (const auto& doc : group) {
            BSONObjBuilder timestampedDoc;
            timestampedDoc.append(_timeField, Date_t::now());
            timestampedDoc.appendElements(doc);

            auto pre = _getExecutionStat(_uuid1, kNumSchemaChanges);
            auto batch = _insertOneWithoutReopening(
                _opCtx, *_bucketCatalog, _ns1, _uuid1, timestampedDoc.obj());
            ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
            finish(*_bucketCatalog, batch);
            auto post = _getExecutionStat(_uuid1, kNumSchemaChanges);

            if (firstMember) {
                if (firstGroup) {
                    // We don't expect to close a bucket if we are on the first group.
                    ASSERT_EQ(pre, post) << "expected " << doc << " to be compatible";
                    firstGroup = false;
                } else {
                    // Otherwise we expect that we are in fact closing a bucket because we have
                    // an incompatible schema change.
                    ASSERT_EQ(pre + 1, post) << "expected " << doc << " to be incompatible";
                }
                firstMember = false;
            } else {
                // Should have compatible schema, no expected bucket closure.
                ASSERT_EQ(pre, post) << "expected " << doc << " to be compatible";
            }
        }
    }
}

BSONObj BucketCatalogTest::_getCompressedBucketDoc(const BSONObj& bucketDoc) {
    CompressionResult compressionResult = compressBucket(bucketDoc,
                                                         _timeField,
                                                         _ns1,
                                                         /*validateDecompression*/ true);
    return compressionResult.compressedBucket.value();
}

BSONObj BucketCatalogTest::_getMetadata(BucketCatalog& catalog, const BucketId& bucketId) {
    auto const& stripe = *catalog.stripes[internal::getStripeNumber(catalog, bucketId)];
    stdx::lock_guard stripeLock{stripe.mutex};

    const Bucket* bucket =
        internal::findBucket(catalog.bucketStateRegistry, stripe, stripeLock, bucketId);
    if (!bucket) {
        return {};
    }

    return bucket->key.metadata.toBSON();
}

StatusWith<tracking::unique_ptr<Bucket>> BucketCatalogTest::_testRehydrateBucket(
    const CollectionPtr& coll, const BSONObj& bucketDoc) {
    const NamespaceString ns = coll->ns().getTimeseriesViewNamespace();
    const UUID uuid = coll->uuid();
    const boost::optional<TimeseriesOptions> options = coll->getTimeseriesOptions();

    BSONElement metadata;
    auto metaFieldName = options->getMetaField();
    if (metaFieldName) {
        metadata = bucketDoc.getField(kBucketMetaFieldName);
    }
    auto key = BucketKey{uuid,
                         BucketMetadata{getTrackingContext(_bucketCatalog->trackingContexts,
                                                           TrackingScope::kOpenBucketsByKey),
                                        metadata,
                                        metaFieldName}};
    auto stats = internal::getOrInitializeExecutionStats(*_bucketCatalog, uuid);
    auto validator = [opCtx = _opCtx, &coll](const BSONObj& bucketDoc) -> auto {
        return coll->checkValidation(opCtx, bucketDoc);
    };
    auto era = getCurrentEra(_bucketCatalog->bucketStateRegistry);

    return internal::rehydrateBucket(*_bucketCatalog,
                                     bucketDoc,
                                     key,
                                     *options,
                                     era,
                                     coll->getDefaultCollator(),
                                     validator,
                                     stats);
}

Status BucketCatalogTest::_reopenBucket(
    const CollectionPtr& coll,
    const BSONObj& bucketDoc,
    const boost::optional<unsigned long>& rehydrateEra,
    const boost::optional<unsigned long>& loadBucketIntoCatalogEra,
    const boost::optional<BucketKey>& bucketKey,
    const boost::optional<internal::BucketDocumentValidator>& documentValidator) {
    const NamespaceString ns = coll->ns().getTimeseriesViewNamespace();
    const UUID uuid = coll->uuid();
    const boost::optional<TimeseriesOptions> options = coll->getTimeseriesOptions();
    invariant(options,
              str::stream() << "Attempting to reopen a bucket for a non-timeseries collection: "
                            << ns.toStringForErrorMsg());

    BSONElement metadata;
    auto metaFieldName = options->getMetaField();
    if (metaFieldName) {
        metadata = bucketDoc.getField(kBucketMetaFieldName);
    }
    auto key = bucketKey.value_or(
        BucketKey{uuid,
                  BucketMetadata{getTrackingContext(_bucketCatalog->trackingContexts,
                                                    TrackingScope::kOpenBucketsByKey),
                                 metadata,
                                 metaFieldName}});
    auto stats = internal::getOrInitializeExecutionStats(*_bucketCatalog, uuid);
    auto stripeNumber = internal::getStripeNumber(*_bucketCatalog, key);
    // Validate the bucket document against the schema.
    auto validator =
        documentValidator.value_or([opCtx = _opCtx, &coll](const BSONObj& bucketDoc) -> auto {
            return coll->checkValidation(opCtx, bucketDoc);
        });
    auto era = rehydrateEra.value_or(getCurrentEra(_bucketCatalog->bucketStateRegistry));

    auto res = internal::rehydrateBucket(*_bucketCatalog,
                                         bucketDoc,
                                         key,
                                         *options,
                                         era,
                                         coll->getDefaultCollator(),
                                         validator,
                                         stats);
    if (!res.isOK()) {
        return res.getStatus();
    }
    auto bucket = std::move(res.getValue());

    // Register the reopened bucket with the catalog.
    auto& stripe = *_bucketCatalog->stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    era = loadBucketIntoCatalogEra.value_or(getCurrentEra(_bucketCatalog->bucketStateRegistry));
    return internal::loadBucketIntoCatalog(
               *_bucketCatalog, stripe, stripeLock, stats, key, std::move(bucket), era)
        .getStatus();
}

RolloverReason BucketCatalogTest::_rolloverReason(const std::shared_ptr<WriteBatch>& batch) {
    auto& stripe =
        _bucketCatalog->stripes[internal::getStripeNumber(*_bucketCatalog, batch->bucketId)];
    auto& [key, bucket] = *stripe->openBucketsById.find(batch->bucketId);
    return bucket->rolloverReason;
}

void BucketCatalogTest::_testBucketMetadataFieldOrdering(const BSONObj& inputMetadata,
                                                         const BSONObj& expectedMetadata) {
    auto swBucketKeyAndTime = extractBucketingParameters(
        getTrackingContext(_bucketCatalog->trackingContexts, TrackingScope::kOpenBucketsByKey),
        UUID::gen(),
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField << inputMetadata));
    ASSERT_OK(swBucketKeyAndTime);

    auto metadata = swBucketKeyAndTime.getValue().first.metadata.toBSON();
    ASSERT_EQ(metadata.woCompare(BSON(_metaField << expectedMetadata)), 0);
}

boost::optional<bucket_catalog::BucketMetadata> BucketCatalogTest::getBucketMetadata(
    BSONObj measurement) const {
    auto tsOptions = _getTimeseriesOptions(_ns1);
    auto metaValue = measurement.getField(_metaField);
    if (metaValue.eoo())
        return boost::none;
    return bucket_catalog::BucketMetadata{
        getTrackingContext(_bucketCatalog->trackingContexts,
                           bucket_catalog::TrackingScope::kMeasurementBatching),
        metaValue,
        tsOptions.getMetaField().get()};
}

// expectedIndicesWithErrors should have unique index values.
void BucketCatalogTest::_testBuildBatchedInsertContextWithMetaField(
    std::vector<BSONObj>& userMeasurementsBatch,
    stdx::unordered_map<bucket_catalog::BucketMetadata, std::vector<size_t>>&
        metaFieldMetadataToCorrectIndexOrderMap,
    stdx::unordered_set<size_t>& expectedIndicesWithErrors) const {
    AutoGetCollection bucketsColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), LockMode::MODE_IX);
    tracking::Context trackingContext;
    timeseries::bucket_catalog::ExecutionStatsController stats;
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto tsOptions = _getTimeseriesOptions(_ns1);

    auto batchedInsertContextVector = bucket_catalog::buildBatchedInsertContextsWithMetaField(
        *_bucketCatalog,
        bucketsColl->uuid(),
        tsOptions,
        userMeasurementsBatch,
        /*startIndex=*/0,
        /*numDocsToStage=*/userMeasurementsBatch.size(),
        /*docsToRetry=*/{},
        stats,
        trackingContext,
        errorsAndIndices);

    ASSERT_EQ(batchedInsertContextVector.size(), metaFieldMetadataToCorrectIndexOrderMap.size());

    // Check that all of the tuples in each BatchedInsertContext have the correct order and
    // measurement.
    for (size_t i = 0; i < batchedInsertContextVector.size(); i++) {
        auto insertBatchContext = batchedInsertContextVector[i];
        ASSERT_EQ(insertBatchContext.key.metadata.getMetaField().get(), _metaField);
        auto metaFieldMetadata = insertBatchContext.key.metadata;
        ASSERT_EQ(insertBatchContext.measurementsTimesAndIndices.size(),
                  metaFieldMetadataToCorrectIndexOrderMap[metaFieldMetadata].size());

        for (size_t j = 0; j < insertBatchContext.measurementsTimesAndIndices.size(); j++) {
            auto tuple = insertBatchContext.measurementsTimesAndIndices[j];
            auto measurement = std::get<BSONObj>(tuple);
            bucket_catalog::BucketMetadata metadata = bucket_catalog::BucketMetadata{
                trackingContext, measurement[_metaField], tsOptions.getMetaField().get()};
            ASSERT(metadata == metaFieldMetadata);
            auto index = std::get<size_t>(tuple);
            ASSERT_EQ(index, metaFieldMetadataToCorrectIndexOrderMap[metaFieldMetadata][j]);
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
    }
};

void BucketCatalogTest::_testBuildBatchedInsertContextOneBatchWithSameMetaFieldType(
    BSONType type) const {
    std::vector<BSONObj> userMeasurementsBatch{
        _generateMeasurement(type, Date_t::fromMillisSinceEpoch(200)).obj(),
        _generateMeasurement(type, Date_t::fromMillisSinceEpoch(100)).obj(),
        _generateMeasurement(type, Date_t::fromMillisSinceEpoch(101)).obj(),
        _generateMeasurement(type, Date_t::fromMillisSinceEpoch(202)).obj(),
        _generateMeasurement(type, Date_t::fromMillisSinceEpoch(201)).obj(),
    };

    stdx::unordered_map<bucket_catalog::BucketMetadata, std::vector<size_t>>
        metaFieldMetadataToCorrectIndexOrderMap;
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[0])),
        std::initializer_list<size_t>{1, 2, 0, 4, 3});
    stdx::unordered_set<size_t> expectedIndicesWithErrors;
    _testBuildBatchedInsertContextWithMetaField(
        userMeasurementsBatch, metaFieldMetadataToCorrectIndexOrderMap, expectedIndicesWithErrors);
}

template <typename T>
void BucketCatalogTest::_testBuildBatchedInsertContextMultipleBatchesWithSameMetaFieldType(
    BSONType type, std::vector<T> metaValues) const {
    BSONObj meta0 = BSON(_metaField << metaValues[0]);
    BSONObj meta1 = BSON(_metaField << metaValues[1]);
    BSONObj meta2 = BSON(_metaField << metaValues[2]);
    std::vector<BSONObj> userMeasurementsBatch{
        _generateMeasurement(meta1, Date_t::fromMillisSinceEpoch(101)).obj(),
        _generateMeasurement(meta2, Date_t::fromMillisSinceEpoch(104)).obj(),
        _generateMeasurement(meta0, Date_t::fromMillisSinceEpoch(105)).obj(),
        _generateMeasurement(meta0, Date_t::fromMillisSinceEpoch(107)).obj(),
        _generateMeasurement(meta1, Date_t::fromMillisSinceEpoch(103)).obj(),
        _generateMeasurement(meta2, Date_t::fromMillisSinceEpoch(102)).obj(),
        _generateMeasurement(meta0, Date_t::fromMillisSinceEpoch(106)).obj(),
    };
    stdx::unordered_map<bucket_catalog::BucketMetadata, std::vector<size_t>>
        metaFieldMetadataToCorrectIndexOrderMap;
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[2])), /* for _metaValues[0] */
        std::initializer_list<size_t>{2, 6, 3});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[0])), /* for _metaValue[1] */
        std::initializer_list<size_t>{0, 4});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[1])), /* for _metaValue[2] */
        std::initializer_list<size_t>{5, 1});
    stdx::unordered_set<size_t> expectedIndicesWithErrors;
    _testBuildBatchedInsertContextWithMetaField(
        userMeasurementsBatch, metaFieldMetadataToCorrectIndexOrderMap, expectedIndicesWithErrors);
}

// expectedIndicesWithErrors should have unique index values.
void BucketCatalogTest::_testBuildBatchedInsertContextWithoutMetaField(
    const NamespaceString& ns,
    const std::vector<BSONObj>& userMeasurementsBatch,
    const std::vector<size_t>& correctIndexOrder,
    stdx::unordered_set<size_t>& expectedIndicesWithErrors) const {
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();
    tracking::Context trackingContext;
    timeseries::bucket_catalog::ExecutionStatsController stats;
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;

    auto batchedInsertContextVector =
        buildBatchedInsertContextsNoMetaField(*_bucketCatalog,
                                              bucketsColl->uuid(),
                                              bucketsColl->getTimeseriesOptions().get(),
                                              userMeasurementsBatch,
                                              /*startIndex=*/0,
                                              /*numDocsToStage=*/userMeasurementsBatch.size(),
                                              /*docsToRetry=*/{},
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
    }
}

void BucketCatalogTest::_testBuildBatchedInsertContextWithoutMetaFieldInCollWithMetaField(
    const NamespaceString& ns,
    const std::vector<BSONObj>& userMeasurementsBatch,
    const std::vector<size_t>& correctIndexOrder,
    stdx::unordered_set<size_t>& expectedIndicesWithErrors) const {
    _assertNoMetaFieldsInCollWithMetaField(ns, userMeasurementsBatch);
    _testBuildBatchedInsertContextWithoutMetaField(
        ns, userMeasurementsBatch, correctIndexOrder, expectedIndicesWithErrors);
}

void BucketCatalogTest::_testBuildBatchedInsertContextWithoutMetaFieldInCollWithoutMetaField(
    const NamespaceString& ns,
    const std::vector<BSONObj>& userMeasurementsBatch,
    const std::vector<size_t>& correctIndexOrder,
    stdx::unordered_set<size_t>& expectedIndicesWithErrors) const {
    _assertCollWithoutMetaField(ns);
    _testBuildBatchedInsertContextWithoutMetaField(
        ns, userMeasurementsBatch, correctIndexOrder, expectedIndicesWithErrors);
}

// _testBuildBatchedInsertContextWithMalformedMeasurements will accept a BSONType and an optional
// std::vector<T> with only two elements that should store the values of meta values if we have a
// non-constant BSONType.
// We assert if the BSONType is non-constant and we don't have a metaValues vector.
// We assert if metaValues.size() != 2.
void BucketCatalogTest::_testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(
    const BSONType type, const std::vector<BSONObj> metaValues) const {
    auto isConstantBSONType =
        (std::find(_constantBSONTypes.begin(), _constantBSONTypes.end(), type) !=
         _constantBSONTypes.end());
    // We case on how we generate based on if we have the metaValues vector or not.
    // We assert if there is no metaValues vector and a non-constant BSONType.
    ASSERT(isConstantBSONType || metaValues.size() == 2);
    auto measurement1 = (isConstantBSONType)
        ? _generateMeasurement(type, Date_t::fromMillisSinceEpoch(105)).obj()
        : _generateMeasurement(metaValues[0], Date_t::fromMillisSinceEpoch(105)).obj();
    auto measurement2 = (isConstantBSONType)
        ? _generateMeasurement(type, boost::none).obj()
        : _generateMeasurement(metaValues[0], boost::none).obj();
    auto measurement3 = (isConstantBSONType)
        ? _generateMeasurement(type, boost::none).obj()
        : _generateMeasurement(metaValues[0], boost::none).obj();
    auto measurement4 = (isConstantBSONType)
        ? _generateMeasurement(type, Date_t::fromMillisSinceEpoch(103)).obj()
        : _generateMeasurement(metaValues[1], Date_t::fromMillisSinceEpoch(103)).obj();
    auto measurement5 = (isConstantBSONType)
        ? _generateMeasurement(type, Date_t::fromMillisSinceEpoch(101)).obj()
        : _generateMeasurement(metaValues[0], Date_t::fromMillisSinceEpoch(101)).obj();
    std::vector<BSONObj> userMeasurementsBatch{
        measurement1, measurement2, measurement3, measurement4, measurement5};
    stdx::unordered_map<bucket_catalog::BucketMetadata, std::vector<size_t>>
        metaFieldMetadataToCorrectIndexOrderMap;
    if (isConstantBSONType) {
        metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
            *(getBucketMetadata(userMeasurementsBatch[0])), std::initializer_list<size_t>{4, 3, 0});
    } else {
        metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
            *(getBucketMetadata(userMeasurementsBatch[0])), std::initializer_list<size_t>{4, 0});
        metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
            *(getBucketMetadata(userMeasurementsBatch[3])), std::initializer_list<size_t>{3});
    }
    stdx::unordered_set<size_t> expectedIndicesWithErrors{1, 2};
    _testBuildBatchedInsertContextWithMetaField(
        userMeasurementsBatch, metaFieldMetadataToCorrectIndexOrderMap, expectedIndicesWithErrors);
}

// numWriteBatches is the size of the writeBatches that should be generated from the
// batchedInsertContext at the same index.
// numWriteBatches.size() == batchedInsertContexts.size()
// sum(numWriteBatches) == the total number of buckets that should be written to from the input
// batchOfMeasurements.
// Example with RolloverReason::kSchemaChange and two distinct meta fields:
// batchOfMeasurements = [
//                         {_timeField: Date_t::now(), _metaField: "a",  "deathGrips": "isOnline"},
//                         {_timeField: Date_t::now(), _metaField: "a",  "deathGrips": "isOffline"},
//                         {_timeField: Date_t::now() + Seconds(1), _metaField: "a",  "deathGrips":
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
//                              {_timeField: Date_t::now() + Seconds(1), _metaField: "a",
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
void BucketCatalogTest::_testStageInsertBatch(const NamespaceString& ns,
                                              const UUID& collectionUUID,
                                              const std::vector<BSONObj>& batchOfMeasurements,
                                              const std::vector<size_t>& numWriteBatches) const {
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();
    auto timeseriesOptions = _getTimeseriesOptions(ns);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;

    auto batchedInsertContexts =
        bucket_catalog::buildBatchedInsertContexts(*_bucketCatalog,
                                                   collectionUUID,
                                                   timeseriesOptions,
                                                   batchOfMeasurements,
                                                   /*startIndex=*/0,
                                                   /*numDocsToStage=*/batchOfMeasurements.size(),
                                                   /*docsToRetry=*/{},
                                                   errorsAndIndices);
    ASSERT(errorsAndIndices.empty());

    ASSERT_EQ(batchedInsertContexts.size(), numWriteBatches.size());
    size_t numMeasurements = 0;
    for (size_t i = 0; i < batchedInsertContexts.size(); i++) {
        numMeasurements += batchedInsertContexts[i].measurementsTimesAndIndices.size();
    }
    ASSERT_EQ(numMeasurements, batchOfMeasurements.size());

    for (size_t i = 0; i < batchedInsertContexts.size(); i++) {
        auto writeBatches = bucket_catalog::stageInsertBatch(_opCtx,
                                                             *_bucketCatalog,
                                                             bucketsColl.get(),
                                                             _opCtx->getOpID(),
                                                             _stringDataComparatorUnused,
                                                             _storageCacheSizeBytes,
                                                             _compressBucketFuncUnused,
                                                             AllowQueryBasedReopening::kAllow,
                                                             batchedInsertContexts[i]);
        ASSERT_EQ(writeBatches.size(), numWriteBatches[i]);
    }
}

void BucketCatalogTest::_testStageInsertBatchWithMetaField(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& batchOfMeasurements,
    const std::vector<size_t>& numWriteBatches) const {
    _assertCollWithMetaField(ns, batchOfMeasurements);
    _testStageInsertBatch(ns, collectionUUID, batchOfMeasurements, numWriteBatches);
}

void BucketCatalogTest::_testStageInsertBatchInCollWithoutMetaField(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& batchOfMeasurements,
    const std::vector<size_t>& numWriteBatches) const {
    _assertCollWithoutMetaField(ns);
    _testStageInsertBatch(ns, collectionUUID, batchOfMeasurements, numWriteBatches);
}

void BucketCatalogTest::_testStageInsertBatchWithoutMetaFieldInCollWithMetaField(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& batchOfMeasurements,
    const std::vector<size_t>& numWriteBatches) const {
    _assertNoMetaFieldsInCollWithMetaField(ns, batchOfMeasurements);
    _testStageInsertBatch(ns, collectionUUID, batchOfMeasurements, numWriteBatches);
}

// curBatchedInsertContextsIndex is the index in batchedInsertContexts that the current write
// batch should be accessing.
// numMeasurementsInWriteBatch is the number of measurements that should be in the current write
// batch returned by stageInsertBatch.
// The curBatchedInsertContextsIndex.size() == numMeasurementsInWriteBatch.size() == the total
// number of buckets that should be written to from the input batchOfMeasurements.
// We require curBatchedInsertContextsIndex.size() == numMeasurementsInWriteBatch.size() > 0, and
// will assert otherwise.
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
// curBatchedInsertContextsIndex = [0, 0, 1]
// numMeasurementsInWriteBatch = [2, 1, 1]
//
// We are accessing the {_metaField: "a"}/batchedInsertContexts[0] and write the
// two measurements into one bucket (numMeasurementsInWriteBatch[0]).
// We detect a schema change with the "deathGrips" field for the
// last measurement in batchedInsertContexts[0].measurementsTimesAndIndices and write one
// measurement to a second bucket (numMeasurementsInWriteBatch[1]).
//
// We then write one measurement to a third bucket because we have a
// distinct {_metaField: "b"} in batchedInsertContexts[1] (that doesn't hash to the same stripe)
// with only one measurement (numMeasurementsInWriteBatch[2]).
//
// If we are attempting to trigger kCachePressure, we must call this function with
// _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes. Otherwise, _storageCacheSizeBytes
// = kDefaultStorageCacheSizeBytes.
//
// We can also test stageInsertBatchIntoEligibleBucket for an vector of input buckets.
// We require that buckets.size() == curBatchedInsertContextsIndex.size() ==
// numMeasurementsInWriteBatch.size(), and will assert otherwise.
void BucketCatalogTest::_testStageInsertBatchIntoEligibleBucket(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& measurements,
    const std::vector<size_t>& curBatchedInsertContextsIndex,
    const std::vector<size_t>& numMeasurementsInWriteBatch,
    size_t numBatchedInsertContexts,
    boost::optional<absl::InlinedVector<Bucket*, 8>> buckets) const {
    ASSERT(numMeasurementsInWriteBatch.size() > 0 && numBatchedInsertContexts > 0);
    // These size values are equivalent to the total number of buckets that should be written to
    // from the input measurements.
    ASSERT(curBatchedInsertContextsIndex.size() == numMeasurementsInWriteBatch.size());
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    auto timeseriesOptions = _getTimeseriesOptions(ns);
    std::vector<timeseries::bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts =
        bucket_catalog::buildBatchedInsertContexts(*_bucketCatalog,
                                                   collectionUUID,
                                                   timeseriesOptions,
                                                   measurements,
                                                   /*startIndex=*/0,
                                                   /*numDocsToStage=*/measurements.size(),
                                                   /*docsToRetry=*/{},
                                                   errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    ASSERT_EQ(batchedInsertContexts.size(), numBatchedInsertContexts);
    size_t numMeasurements = 0;
    for (size_t i = 0; i < batchedInsertContexts.size(); i++) {
        numMeasurements += batchedInsertContexts[i].measurementsTimesAndIndices.size();
    }
    ASSERT_EQ(numMeasurements, measurements.size());

    size_t curPosition = 0;
    size_t totalMeasurementsInserted = 0;
    size_t curPositionFromNumMeasurementsInBatch = 0;
    for (size_t i = 0; i < numMeasurementsInWriteBatch.size(); i++) {
        auto& curBatch = batchedInsertContexts[curBatchedInsertContextsIndex[i]];
        auto& stripe = *_bucketCatalog->stripes[curBatch.stripeNumber];
        stdx::lock_guard stripeLock{stripe.mutex};

        // We are looking at a new batchedInsertContext, so we should reset curPosition
        if (i != 0 && (curBatchedInsertContextsIndex[i] != curBatchedInsertContextsIndex[i - 1])) {
            curPosition = 0;
        }
        Bucket* bucketToInsertInto = (buckets)
            ? (*buckets)[i]
            : _generateBucketWithBatch(ns, collectionUUID, curBatch, curPosition);
        if (buckets) {
            bucketToInsertInto->rolloverReason = RolloverReason::kNone;
        }
        size_t prevNumMeasurements = bucketToInsertInto->numMeasurements;
        auto newWriteBatch = activeBatch(_bucketCatalog->trackingContexts,
                                         *bucketToInsertInto,
                                         _opCtx->getOpID(),
                                         curBatch.stripeNumber,
                                         curBatch.stats);
        size_t prevCurPosition = curPosition;
        auto successfulInsertion =
            internal::stageInsertBatchIntoEligibleBucket(*_bucketCatalog,
                                                         _opCtx->getOpID(),
                                                         bucketsColl->getDefaultCollator(),
                                                         curBatch,
                                                         stripe,
                                                         stripeLock,
                                                         _storageCacheSizeBytes,
                                                         *bucketToInsertInto,
                                                         curPosition,
                                                         newWriteBatch);
        ASSERT_EQ((bucketToInsertInto->numMeasurements - prevNumMeasurements),
                  numMeasurementsInWriteBatch[i]);
        curPositionFromNumMeasurementsInBatch += numMeasurementsInWriteBatch[i];
        totalMeasurementsInserted += (curPosition - prevCurPosition);
        ASSERT_EQ(totalMeasurementsInserted, curPositionFromNumMeasurementsInBatch);

        // We need to rollover so we don't have multiple open buckets.
        // We rollover with kSchemaChange regardless of the rollover reason. This
        // will make our stats inaccurate, but shouldn't impact testing
        // stageInsertBatchIntoEligibleBucket itself.
        bucketToInsertInto->rolloverReason = RolloverReason::kSchemaChange;
        internal::rollover(*_bucketCatalog,
                           stripe,
                           stripeLock,
                           *bucketToInsertInto,
                           RolloverReason::kSchemaChange);
        if (i == (numMeasurementsInWriteBatch.size() - 1)) {
            ASSERT_EQ(successfulInsertion,
                      bucket_catalog::internal::StageInsertBatchResult::Success);
        } else {
            ASSERT_NE(successfulInsertion,
                      bucket_catalog::internal::StageInsertBatchResult::Success);
        }
    }
}

void BucketCatalogTest::_testStageInsertBatchIntoEligibleBucketWithMetaField(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& measurements,
    const std::vector<size_t>& curBatchedInsertContextsIndex,
    const std::vector<size_t>& numMeasurementsInWriteBatch,
    size_t numBatchedInsertContexts,
    boost::optional<absl::InlinedVector<Bucket*, 8>> buckets) const {
    _assertCollWithMetaField(ns, measurements);
    _testStageInsertBatchIntoEligibleBucket(ns,
                                            collectionUUID,
                                            measurements,
                                            curBatchedInsertContextsIndex,
                                            numMeasurementsInWriteBatch,
                                            numBatchedInsertContexts,
                                            buckets);
}

void BucketCatalogTest::_testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& measurements,
    const std::vector<size_t>& curBatchedInsertContextsIndex,
    const std::vector<size_t>& numMeasurementsInWriteBatch,
    size_t numBatchedInsertContexts,
    boost::optional<absl::InlinedVector<Bucket*, 8>> buckets) const {
    _assertCollWithoutMetaField(ns);
    _testStageInsertBatchIntoEligibleBucket(ns,
                                            collectionUUID,
                                            measurements,
                                            curBatchedInsertContextsIndex,
                                            numMeasurementsInWriteBatch,
                                            numBatchedInsertContexts,
                                            buckets);
}

void BucketCatalogTest::_testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& measurements,
    const std::vector<size_t>& curBatchedInsertContextsIndex,
    const std::vector<size_t>& numMeasurementsInWriteBatch,
    size_t numBatchedInsertContexts,
    boost::optional<absl::InlinedVector<Bucket*, 8>> buckets) const {
    _assertNoMetaFieldsInCollWithMetaField(ns, measurements);
    _testStageInsertBatchIntoEligibleBucket(ns,
                                            collectionUUID,
                                            measurements,
                                            curBatchedInsertContextsIndex,
                                            numMeasurementsInWriteBatch,
                                            numBatchedInsertContexts,
                                            buckets);
}

void BucketCatalogTest::_testCreateOrderedPotentialBucketsVector(
    PotentialBucketOptions& potentialBucketOptions) const {
    auto& kSoftClosedBuckets = potentialBucketOptions.kSoftClosedBuckets;
    auto& kArchivedBuckets = potentialBucketOptions.kArchivedBuckets;
    auto& kNoneBucket = potentialBucketOptions.kNoneBucket;
    bool kNoneBucketExists = (kNoneBucket != nullptr);
    auto potentialBuckets = createOrderedPotentialBucketsVector(potentialBucketOptions);
    ASSERT_EQ(potentialBuckets.size(),
              (kSoftClosedBuckets.size() + kArchivedBuckets.size() + kNoneBucketExists));

    // Check that all the buckets are correctly ordered by their action priority:
    // kSoftClose -> kArchived -> kNone
    // and check that all buckets have numMeasurements in increasing order.
    for (size_t i = 0; i < kSoftClosedBuckets.size(); i++) {
        ASSERT(getRolloverAction(potentialBuckets[i]->rolloverReason) ==
               RolloverAction::kSoftClose);
        if (i != kSoftClosedBuckets.size() - 1) {
            ASSERT(potentialBuckets[i]->numMeasurements <=
                   potentialBuckets[i + 1]->numMeasurements);
        }
    }
    for (size_t i = kSoftClosedBuckets.size();
         i < (kSoftClosedBuckets.size() + kArchivedBuckets.size());
         i++) {
        ASSERT(getRolloverAction(potentialBuckets[i]->rolloverReason) == RolloverAction::kArchive);
        if (i != (kSoftClosedBuckets.size() + kArchivedBuckets.size()) - 1) {
            ASSERT(potentialBuckets[i]->numMeasurements <=
                   potentialBuckets[i + 1]->numMeasurements);
        }
    }
    if (kNoneBucketExists) {
        ASSERT(getRolloverAction(potentialBuckets[potentialBuckets.size() - 1]->rolloverReason) ==
               RolloverAction::kNone);
    }
}

std::vector<std::pair<std::vector<BSONObj>, RolloverReason>>
BucketCatalogTest::_getMeasurementRolloverReasonVec(
    const std::vector<std::vector<BSONObj>>& measurements, const RolloverReason& reason) const {
    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> result;
    result.reserve(measurements.size());
    std::transform(
        measurements.begin(),
        measurements.end(),
        std::back_inserter(result),
        [&reason](const std::vector<BSONObj>& element) { return std::make_pair(element, reason); });
    return result;
}

TEST_F(BucketCatalogTest, InsertIntoSameBucket) {
    auto batch1 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));

    // A subsequent insert into the same bucket should land in the same batch.
    auto batch2 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_EQ(batch1, batch2) << batch1->toBSON() << batch2->toBSON();

    // The batch hasn't actually been committed yet.
    ASSERT(!isWriteBatchFinished(*batch1));

    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));

    // Still not finished.
    ASSERT(!isWriteBatchFinished(*batch1));

    // The batch should contain both documents since they belong in the same bucket and happened
    // in the same commit epoch. Nothing else has been committed in this bucket yet.
    ASSERT_EQ(batch1->measurements.size(), 2) << batch1->toBSON();
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0) << batch1->toBSON();

    // Once the commit has occurred, the waiter should be notified.
    finish(*_bucketCatalog, batch1);
    ASSERT(isWriteBatchFinished(*batch2));
    ASSERT_OK(getWriteBatchStatus(*batch2));
}

TEST_F(BucketCatalogTest, InsertIntoDifferentBuckets) {
    auto batch1 =
        _insertOneWithoutReopening(_opCtx,
                                   *_bucketCatalog,
                                   _ns1,
                                   _uuid1,
                                   BSON(_timeField << Date_t::now() << _metaField << "123"));
    auto batch2 =
        _insertOneWithoutReopening(_opCtx,
                                   *_bucketCatalog,
                                   _ns1,
                                   _uuid1,
                                   BSON(_timeField << Date_t::now() << _metaField << BSONObj()));
    auto batch3 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns2, _uuid2, BSON(_timeField << Date_t::now()));

    // Inserts should all be into three distinct buckets (and therefore batches).
    ASSERT_NE(batch1, batch2);
    ASSERT_NE(batch1, batch3);
    ASSERT_NE(batch2, batch3);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(BSON(_metaField << "123"), _getMetadata(*_bucketCatalog, batch1->bucketId));
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSONObj()),
                      _getMetadata(*_bucketCatalog, batch2->bucketId));
    ASSERT(_getMetadata(*_bucketCatalog, batch3->bucketId).isEmpty());

    // Committing one bucket should only return the one document in that bucket and should not
    // affect the other bucket.
    _commit(_ns1, batch1, 0);
    _commit(_ns1, batch2, 0);
    _commit(_ns2, batch3, 0);
}

TEST_F(BucketCatalogTest, InsertThroughDifferentCatalogsIntoDifferentBuckets) {
    BucketCatalog temporaryBucketCatalog(/*numberOfStripes=*/1,
                                         getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes);
    auto batch1 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));

    auto batch2 = _insertOneWithoutReopening(
        _opCtx, temporaryBucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));

    // Inserts should be into different buckets (and therefore batches) because they went through
    // different bucket catalogs.
    ASSERT_NE(batch1, batch2) << batch1->toBSON() << batch2->toBSON();

    // Committing one bucket should only return the one document in that bucket and should not
    // affect the other bucket.
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));
    ASSERT_EQ(batch1->measurements.size(), 1) << batch1->toBSON();
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0) << batch1->toBSON();
    finish(*_bucketCatalog, batch1);

    ASSERT_OK(prepareCommit(temporaryBucketCatalog, batch2, _getCollator(_ns2)));
    ASSERT_EQ(batch2->measurements.size(), 1) << batch2->toBSON();
    ASSERT_EQ(batch2->numPreviouslyCommittedMeasurements, 0) << batch2->toBSON();
    finish(temporaryBucketCatalog, batch2);
}

TEST_F(BucketCatalogTest, InsertIntoSameBucketArray) {
    auto timeseriesOptions = _getTimeseriesOptions(_ns1);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts1 = bucket_catalog::buildBatchedInsertContexts(
        *_bucketCatalog,
        _uuid1,
        timeseriesOptions,
        {BSON(_timeField << Date_t::now() << _metaField << BSON_ARRAY(BSON("a" << 0 << "b" << 1)))},
        /*startIndex=*/0,
        /*numDocsToStage=*/1,
        /*docsToRetry=*/{},
        errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    auto batchedInsertCtx1 = batchedInsertContexts1[0];

    auto batchedInsertContexts2 = bucket_catalog::buildBatchedInsertContexts(
        *_bucketCatalog,
        _uuid1,
        timeseriesOptions,
        {BSON(_timeField << Date_t::now() << _metaField << BSON_ARRAY(BSON("b" << 1 << "a" << 0)))},
        /*startIndex=*/0,
        /*numDocsToStage=*/1,
        /*docsToRetry=*/{},
        errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    auto batchedInsertCtx2 = batchedInsertContexts2[0];

    // Check metadata in buckets.
    ASSERT_EQ(batchedInsertCtx1.key, batchedInsertCtx2.key);
}

TEST_F(BucketCatalogTest, InsertIntoSameBucketObjArray) {
    auto timeseriesOptions = _getTimeseriesOptions(_ns1);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts1 = bucket_catalog::buildBatchedInsertContexts(
        *_bucketCatalog,
        _uuid1,
        timeseriesOptions,
        {BSON(_timeField << Date_t::now() << _metaField
                         << BSONObj(BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1)
                                                           << BSON("f" << 1 << "g" << 0)))))},
        /*startIndex=*/0,
        /*numDocsToStage=*/1,
        /*docsToRetry=*/{},
        errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    auto batchedInsertCtx1 = batchedInsertContexts1[0];

    auto batchedInsertContexts2 = bucket_catalog::buildBatchedInsertContexts(
        *_bucketCatalog,
        _uuid1,
        timeseriesOptions,
        {BSON(_timeField << Date_t::now() << _metaField
                         << BSONObj(BSON("c" << BSON_ARRAY(BSON("b" << 1 << "a" << 0)
                                                           << BSON("g" << 0 << "f" << 1)))))},
        /*startIndex=*/0,
        /*numDocsToStage=*/1,
        /*docsToRetry=*/{},
        errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    auto batchedInsertCtx2 = batchedInsertContexts2[0];

    // Check metadata in buckets.
    ASSERT_EQ(batchedInsertCtx1.key, batchedInsertCtx2.key);
}


TEST_F(BucketCatalogTest, InsertIntoSameBucketNestedArray) {
    auto timeseriesOptions = _getTimeseriesOptions(_ns1);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts1 = bucket_catalog::buildBatchedInsertContexts(
        *_bucketCatalog,
        _uuid1,
        timeseriesOptions,
        {BSON(_timeField << Date_t::now() << _metaField
                         << BSONObj(BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1)
                                                           << BSON_ARRAY("123" << "456")))))},
        /*startIndex=*/0,
        /*numDocsToStage=*/1,
        /*docsToRetry=*/{},
        errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    auto batchedInsertCtx1 = batchedInsertContexts1[0];

    auto batchedInsertContexts2 = bucket_catalog::buildBatchedInsertContexts(
        *_bucketCatalog,
        _uuid1,
        timeseriesOptions,
        {BSON(_timeField << Date_t::now() << _metaField
                         << BSONObj(BSON("c" << BSON_ARRAY(BSON("b" << 1 << "a" << 0)
                                                           << BSON_ARRAY("123" << "456")))))},
        /*startIndex=*/0,
        /*numDocsToStage=*/1,
        /*docsToRetry=*/{},
        errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    auto batchedInsertCtx2 = batchedInsertContexts2[0];

    // Check metadata in buckets.
    ASSERT_EQ(batchedInsertCtx1.key, batchedInsertCtx2.key);
}

TEST_F(BucketCatalogTest, InsertNullAndMissingMetaFieldIntoDifferentBuckets) {
    auto timeseriesOptions = _getTimeseriesOptions(_ns1);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts1 = bucket_catalog::buildBatchedInsertContexts(
        *_bucketCatalog,
        _uuid1,
        timeseriesOptions,
        {BSON(_timeField << Date_t::now() << _metaField << BSONNULL)},
        /*startIndex=*/0,
        /*numDocsToStage=*/1,
        /*docsToRetry=*/{},
        errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    auto batchedInsertCtx1 = batchedInsertContexts1[0];

    auto batchedInsertContexts2 =
        bucket_catalog::buildBatchedInsertContexts(*_bucketCatalog,
                                                   _uuid1,
                                                   timeseriesOptions,
                                                   {BSON(_timeField << Date_t::now())},
                                                   /*startIndex=*/0,
                                                   /*numDocsToStage=*/1,
                                                   /*docsToRetry=*/{},
                                                   errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    auto batchedInsertCtx2 = batchedInsertContexts2[0];

    // Inserts should all be into two distinct buckets.
    ASSERT_NE(batchedInsertCtx1.key, batchedInsertCtx2.key);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSONNULL), batchedInsertCtx1.key.metadata.toBSON());
    ASSERT(batchedInsertCtx2.key.metadata.toBSON().isEmpty());
}


TEST_F(BucketCatalogTest, InsertBetweenPrepareAndFinish) {
    auto batch1 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));
    ASSERT_EQ(batch1->measurements.size(), 1) << batch1->toBSON();
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0) << batch1->toBSON();

    // Insert before finish so there's a second batch live at the same time.
    auto batch2 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_NE(batch1, batch2) << batch1->toBSON() << batch2->toBSON();

    finish(*_bucketCatalog, batch1);
    ASSERT(isWriteBatchFinished(*batch1));

    // Verify the second batch still commits one doc, and that the first batch only commited one.
    _commit(_ns1, batch2, 1);
}

TEST_F(BucketCatalogTest, GetMetadataReturnsEmptyDoc) {
    auto batch = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _nsNoMeta, _uuidNoMeta, BSON(_timeField << Date_t::now()));

    ASSERT_BSONOBJ_EQ(BSONObj(), _getMetadata(*_bucketCatalog, batch->bucketId));

    _commit(_nsNoMeta, batch, 0);
}

TEST_F(BucketCatalogTest, CommitReturnsNewFields) {
    // Creating a new bucket should return all fields from the initial measurement.
    auto batch = _insertOneWithoutReopening(_opCtx,
                                            *_bucketCatalog,
                                            _nsNoMeta,
                                            _uuidNoMeta,
                                            BSON(_timeField << Date_t::now() << "a" << 0));
    auto oldId = batch->bucketId;
    _commit(_nsNoMeta, batch, 0);
    ASSERT_EQ(2U, batch->newFieldNamesToBeInserted.size()) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted.count(_timeField)) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted.count("a")) << batch->toBSON();

    // Inserting a new measurement with the same fields should return an empty set of new fields.
    batch = _insertOneWithoutReopening(_opCtx,
                                       *_bucketCatalog,
                                       _nsNoMeta,
                                       _uuidNoMeta,
                                       BSON(_timeField << Date_t::now() << "a" << 1));
    _commit(_nsNoMeta, batch, 1);
    ASSERT_EQ(0U, batch->newFieldNamesToBeInserted.size()) << batch->toBSON();

    // Insert a new measurement with the a new field.
    batch = _insertOneWithoutReopening(_opCtx,
                                       *_bucketCatalog,
                                       _nsNoMeta,
                                       _uuidNoMeta,
                                       BSON(_timeField << Date_t::now() << "a" << 2 << "b" << 2));
    _commit(_nsNoMeta, batch, 2);
    ASSERT_EQ(1U, batch->newFieldNamesToBeInserted.size()) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted.count("b")) << batch->toBSON();

    // Fill up the bucket.
    for (auto i = 3; i < gTimeseriesBucketMaxCount; ++i) {
        batch = _insertOneWithoutReopening(_opCtx,
                                           *_bucketCatalog,
                                           _nsNoMeta,
                                           _uuidNoMeta,
                                           BSON(_timeField << Date_t::now() << "a" << i));
        _commit(_nsNoMeta, batch, i);
        ASSERT_EQ(0U, batch->newFieldNamesToBeInserted.size()) << i << ":" << batch->toBSON();
    }

    // When a bucket overflows, committing to the new overflow bucket should return the fields of
    // the first measurement as new fields.
    auto batch2 = _insertOneWithoutReopening(
        _opCtx,
        *_bucketCatalog,
        _nsNoMeta,
        _uuidNoMeta,
        BSON(_timeField << Date_t::now() << "a" << gTimeseriesBucketMaxCount));
    ASSERT_NE(oldId, batch2->bucketId) << batch2->toBSON();
    _commit(_nsNoMeta, batch2, 0);
    ASSERT_EQ(2U, batch2->newFieldNamesToBeInserted.size()) << batch2->toBSON();
    ASSERT(batch2->newFieldNamesToBeInserted.count(_timeField)) << batch2->toBSON();
    ASSERT(batch2->newFieldNamesToBeInserted.count("a")) << batch2->toBSON();
}

TEST_F(BucketCatalogTest, AbortBatchOnBucketWithPreparedCommit) {
    auto batch1 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));
    ASSERT_EQ(batch1->measurements.size(), 1) << batch1->toBSON();
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0) << batch1->toBSON();

    // Insert before finish so there's a second batch live at the same time.
    auto batch2 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_NE(batch1, batch2) << batch1->toBSON() << batch2->toBSON();

    abort(*_bucketCatalog, batch2, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(isWriteBatchFinished(*batch2));
    ASSERT_EQ(getWriteBatchStatus(*batch2), ErrorCodes::TimeseriesBucketCleared);

    finish(*_bucketCatalog, batch1);
    ASSERT(isWriteBatchFinished(*batch1));
    ASSERT_OK(getWriteBatchStatus(*batch1));
}

TEST_F(BucketCatalogTest, ClearNamespaceWithConcurrentWrites) {
    auto batch = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));

    clear(*_bucketCatalog, _uuid1);

    ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchStatus(*batch), ErrorCodes::TimeseriesBucketCleared);

    batch = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT_EQ(batch->measurements.size(), 1) << batch->toBSON();
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 0) << batch->toBSON();

    clear(*_bucketCatalog, _uuid1);

    // Even though bucket has been cleared, finish should still report success. Basically, in this
    // case we know that the write succeeded, so it must have happened before the namespace drop
    // operation got the collection lock. So the write did actually happen, but is has since been
    // removed, and that's fine for our purposes. The finish just records the result to the batch
    // and updates some statistics.
    finish(*_bucketCatalog, batch);
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_OK(getWriteBatchStatus(*batch));
}


TEST_F(BucketCatalogTest, ClearBucketWithPreparedBatchThrowsConflict) {
    auto batch = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT_EQ(batch->measurements.size(), 1) << batch->toBSON();
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 0) << batch->toBSON();

    ASSERT_THROWS(directWriteStart(_bucketCatalog->bucketStateRegistry, batch->bucketId),
                  WriteConflictException);

    abort(*_bucketCatalog, batch, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchStatus(*batch), ErrorCodes::TimeseriesBucketCleared);
}

TEST_F(BucketCatalogTest, PrepareCommitOnClearedBucketWithAlreadyPreparedBatch) {
    auto batch1 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));
    ASSERT_EQ(batch1->measurements.size(), 1) << batch1->toBSON();
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0) << batch1->toBSON();

    // Insert before clear so there's a second batch live at the same time.
    auto batch2 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_NE(batch1, batch2) << batch1->toBSON() << batch2->toBSON();
    ASSERT_EQ(batch1->bucketId, batch2->bucketId) << batch1->toBSON() << batch2->toBSON();

    // Now clear the bucket. Since there's a prepared batch it should conflict.
    clearBucketState(_bucketCatalog->bucketStateRegistry, batch1->bucketId);

    // Now try to prepare the second batch. Ensure it aborts the batch.
    ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, batch2, _getCollator(_ns2)));
    ASSERT(isWriteBatchFinished(*batch2));
    ASSERT_EQ(getWriteBatchStatus(*batch2), ErrorCodes::TimeseriesBucketCleared);

    // Make sure we didn't clear the bucket state when we aborted the second batch.
    clear(*_bucketCatalog, _uuid1);

    // Make sure a subsequent insert, which opens a new bucket, doesn't corrupt the old bucket
    // state and prevent us from finishing the first batch.
    auto batch3 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_NE(batch1, batch3) << batch1->toBSON() << batch3->toBSON();
    ASSERT_NE(batch2, batch3) << batch2->toBSON() << batch3->toBSON();
    ASSERT_NE(batch1->bucketId, batch3->bucketId) << batch1->toBSON() << batch3->toBSON();
    // Clean up this batch
    abort(*_bucketCatalog, batch3, {ErrorCodes::TimeseriesBucketCleared, ""});

    // Make sure we can finish the cleanly prepared batch.
    finish(*_bucketCatalog, batch1);
    ASSERT(isWriteBatchFinished(*batch1));
    ASSERT_OK(getWriteBatchStatus(*batch1));
}

TEST_F(BucketCatalogTest, PrepareCommitOnAlreadyAbortedBatch) {
    auto batch = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));

    abort(*_bucketCatalog, batch, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchStatus(*batch), ErrorCodes::TimeseriesBucketCleared);

    ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchStatus(*batch), ErrorCodes::TimeseriesBucketCleared);
}

TEST_F(BucketCatalogTest, CannotConcurrentlyCommitBatchesForSameBucket) {
    auto batch1 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));

    auto batch2 = _insertOneWithoutReopening(_makeOperationContext().second.get(),
                                             *_bucketCatalog,
                                             _ns1,
                                             _uuid1,
                                             BSON(_timeField << Date_t::now()));

    // Batch 2 will not be able to commit until batch 1 has finished.
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));

    {
        auto task = RunBackgroundTaskAndWaitForFailpoint{
            "hangTimeSeriesBatchPrepareWaitingForConflictingOperation", [&]() {
                ASSERT_OK(prepareCommit(*_bucketCatalog, batch2, _getCollator(_ns1)));
            }};

        // Finish the first batch.
        finish(*_bucketCatalog, batch1);
        ASSERT(isWriteBatchFinished(*batch1));
    }

    finish(*_bucketCatalog, batch2);
    ASSERT(isWriteBatchFinished(*batch2));
}

TEST_F(BucketCatalogTest, AbortingBatchEnsuresBucketIsEventuallyClosed) {
    auto batch1 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));

    auto batch2 = _insertOneWithoutReopening(_makeOperationContext().second.get(),
                                             *_bucketCatalog,
                                             _ns1,
                                             _uuid1,
                                             BSON(_timeField << Date_t::now()));

    auto batch3 = _insertOneWithoutReopening(_makeOperationContext().second.get(),
                                             *_bucketCatalog,
                                             _ns1,
                                             _uuid1,
                                             BSON(_timeField << Date_t::now()));

    ASSERT_EQ(batch1->bucketId, batch2->bucketId) << batch1->toBSON() << batch2->toBSON();
    ASSERT_EQ(batch1->bucketId, batch3->bucketId) << batch1->toBSON() << batch3->toBSON();

    // Batch 2 will not be able to commit until batch 1 has finished.
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));

    {
        auto task = RunBackgroundTaskAndWaitForFailpoint{
            "hangTimeSeriesBatchPrepareWaitingForConflictingOperation", [&]() {
                ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, batch2, _getCollator(_ns2)));
            }};

        // If we abort the third batch, it should abort the second one too, as it isn't prepared.
        // However, since the first batch is prepared, we can't abort it or clean up the bucket. We
        // can then finish the first batch, which will allow the second batch to proceed. It should
        // recognize it has been aborted and clean up the bucket.
        abort(*_bucketCatalog, batch3, Status{ErrorCodes::TimeseriesBucketCleared, "cleared"});
        finish(*_bucketCatalog, batch1);
        ASSERT(isWriteBatchFinished(*batch1));
    }
    // Wait for the batch 2 task to finish preparing commit. Since batch 1 finished, batch 2 should
    // be unblocked. Note that after aborting batch 3, batch 2 was not in a prepared state, so we
    // expect the prepareCommit() call to fail.
    ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, batch2, _getCollator(_ns2)));
    ASSERT(isWriteBatchFinished(*batch2));

    // Make sure a new batch ends up in a new bucket.
    auto batch4 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_NE(batch2->bucketId, batch4->bucketId) << batch2->toBSON() << batch4->toBSON();
}

TEST_F(BucketCatalogTest, AbortingBatchEnsuresNewInsertsGoToNewBucket) {
    auto batch1 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));

    auto batch2 = _insertOneWithoutReopening(_makeOperationContext().second.get(),
                                             *_bucketCatalog,
                                             _ns1,
                                             _uuid1,
                                             BSON(_timeField << Date_t::now()));

    // Batch 1 and 2 use the same bucket.
    ASSERT_EQ(batch1->bucketId, batch2->bucketId) << batch1->toBSON() << batch2->toBSON();
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));

    // Batch 1 will be in a prepared state now. Abort the second batch so that bucket 1 will be
    // closed after batch 1 finishes.
    abort(*_bucketCatalog, batch2, Status{ErrorCodes::TimeseriesBucketCleared, "cleared"});
    finish(*_bucketCatalog, batch1);
    ASSERT(isWriteBatchFinished(*batch1));
    ASSERT(isWriteBatchFinished(*batch2));

    // Ensure a batch started after batch 2 aborts, does not insert future measurements into the
    // aborted batch/bucket.
    auto batch3 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    ASSERT_NE(batch1->bucketId, batch3->bucketId) << batch1->toBSON() << batch3->toBSON();
}

TEST_F(BucketCatalogTest, DuplicateNewFieldNamesAcrossConcurrentBatches) {
    auto batch1 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));

    auto batch2 = _insertOneWithoutReopening(_makeOperationContext().second.get(),
                                             *_bucketCatalog,
                                             _ns1,
                                             _uuid1,
                                             BSON(_timeField << Date_t::now()));

    // Batch 2 is the first batch to commit the time field.
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch2, _getCollator(_ns2)));
    ASSERT_EQ(batch2->newFieldNamesToBeInserted.size(), 1) << batch2->toBSON();
    ASSERT_EQ(batch2->newFieldNamesToBeInserted.begin()->first, _timeField) << batch2->toBSON();
    finish(*_bucketCatalog, batch2);

    // Batch 1 was the first batch to insert the time field, but by commit time it was already
    // committed by batch 2.
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));
    ASSERT(batch1->newFieldNamesToBeInserted.empty()) << batch1->toBSON();
    finish(*_bucketCatalog, batch1);
}

TEST_F(BucketCatalogTest, SchemaChanges) {
    std::vector<BSONObj> docs = {
        ::mongo::fromjson(R"({a: 1})"),                                // 0
        ::mongo::fromjson(R"({a: true})"),                             // 1
        ::mongo::fromjson(R"({a: {}})"),                               // 2
        ::mongo::fromjson(R"({a: {b: 1}})"),                           // 3
        ::mongo::fromjson(R"({a: {b: true}})"),                        // 4
        ::mongo::fromjson(R"({a: {c: true}})"),                        // 5
        ::mongo::fromjson(R"({a: {d: true}})"),                        // 6
        ::mongo::fromjson(R"({a: {e: true}})"),                        // 7
        ::mongo::fromjson(R"({a: {f: true}})"),                        // 8
        ::mongo::fromjson(R"({a: {d: 1.0}})"),                         // 9
        ::mongo::fromjson(R"({b: 1.0})"),                              // 10
        ::mongo::fromjson(R"({c: {}})"),                               // 11
        ::mongo::fromjson(R"({a: 1.0, b: 2.0, c: 3.0})"),              // 12
        ::mongo::fromjson(R"({c: 1.0, b: 3.0, a: 2.0})"),              // 13
        ::mongo::fromjson(R"({b: 1.0, a: 3.0, c: 2.0})"),              // 14
        ::mongo::fromjson(R"({a: {b: [1.0, 2.0]}})"),                  // 15
        ::mongo::fromjson(R"({a: {b: [true, false]}})"),               // 16
        ::mongo::fromjson(R"({a: {b: [false, true, false, true]}})"),  // 17
        ::mongo::fromjson(R"({a: {b: [{a: true}, {b: false}]}})"),     // 18
        ::mongo::fromjson(R"({a: {b: [{b: true}, {a: false}]}})"),     // 19
        ::mongo::fromjson(R"({a: {b: [{a: 1.0}, {b: 2.0}]}})"),        // 20
        ::mongo::fromjson(R"({a: {b: [{}, {}, true, false]}})"),       // 21
    };

    _testMeasurementSchema({{docs[0]}, {docs[1]}, {docs[2], docs[3]}, {docs[4]}});
    _testMeasurementSchema({{docs[0]}, {docs[1]}, {docs[2], docs[4]}, {docs[3]}});
    _testMeasurementSchema({{docs[4], docs[5], docs[6], docs[7], docs[8]}, {docs[9]}});
    _testMeasurementSchema({{docs[4], docs[5], docs[6], docs[7], docs[8], docs[10]}});
    _testMeasurementSchema({{docs[10], docs[11]}, {docs[12]}});
    _testMeasurementSchema({{docs[12], docs[13], docs[14]}, {docs[15]}, {docs[16], docs[17]}});
    _testMeasurementSchema({{docs[18], docs[19]}, {docs[20], docs[21]}});
}

TEST_F(BucketCatalogTest, ReopenMalformedBucket) {
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
    BSONObj compressedBucketDoc = _getCompressedBucketDoc(bucketDoc);
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    ASSERT_OK(_reopenBucket(autoColl.getCollection(), compressedBucketDoc));
    auto stats = internal::getCollectionExecutionStats(*_bucketCatalog, _uuid1);

    auto& registry = _bucketCatalog->bucketStateRegistry;
    ASSERT_EQ(1, registry.bucketStates.size());
    const auto bucketId = registry.bucketStates.begin()->first;

    // Buckets are frozen when rejected by the validator.
    auto unfreezeBucket = [&]() -> void {
        registry.bucketStates[bucketId] = {BucketState::kNormal};
        ASSERT_EQ(registry.bucketStates.bucket_count(), 1);
        ASSERT_EQ(*std::get_if<BucketState>(&registry.bucketStates.find(bucketId)->second),
                  BucketState::kNormal);
    };

    {
        // Missing _id field.
        BSONObj missingIdObj = compressedBucketDoc.removeField("_id");
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), missingIdObj));
        ASSERT_EQ(1, stats->numBucketReopeningsFailedDueToMalformedIdField.load());

        // Bad _id type.
        BSONObj badIdObj = compressedBucketDoc.addFields(BSON("_id" << 123));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badIdObj));
        ASSERT_EQ(2, stats->numBucketReopeningsFailedDueToMalformedIdField.load());
    }

    {
        // Missing control field.
        BSONObj missingControlObj = compressedBucketDoc.removeField("control");
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), missingControlObj));
        ASSERT_EQ(1, stats->numBucketReopeningsFailedDueToValidator.load());
        unfreezeBucket();

        // Bad control type.
        BSONObj badControlObj = compressedBucketDoc.addFields(BSON("control" << BSONArray()));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badControlObj));
        ASSERT_EQ(2, stats->numBucketReopeningsFailedDueToValidator.load());
        unfreezeBucket();

        // Bad control.version type.
        BSONObj badVersionObj = compressedBucketDoc.addFields(BSON(
            "control" << BSON(
                "version" << BSONArray() << "min"
                          << BSON("time" << BSON("$date" << "2022-06-06T15:34:00.000Z")) << "max"
                          << BSON("time" << BSON("$date" << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badVersionObj));
        ASSERT_EQ(3, stats->numBucketReopeningsFailedDueToValidator.load());
        unfreezeBucket();

        // Bad control.min type.
        BSONObj badMinObj = compressedBucketDoc.addFields(BSON(
            "control" << BSON("version"
                              << 1 << "min" << 123 << "max"
                              << BSON("time" << BSON("$date" << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badMinObj));
        ASSERT_EQ(4, stats->numBucketReopeningsFailedDueToValidator.load());
        unfreezeBucket();

        // Bad control.max type.
        BSONObj badMaxObj = compressedBucketDoc.addFields(
            BSON("control" << BSON("version"
                                   << 1 << "min"
                                   << BSON("time" << BSON("$date" << "2022-06-06T15:34:00.000Z"))
                                   << "max" << 123)));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badMaxObj));
        ASSERT_EQ(5, stats->numBucketReopeningsFailedDueToValidator.load());
        unfreezeBucket();

        // Missing control.min.time.
        BSONObj missingMinTimeObj = compressedBucketDoc.addFields(BSON(
            "control" << BSON("version"
                              << 1 << "min" << BSON("abc" << 1) << "max"
                              << BSON("time" << BSON("$date" << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), missingMinTimeObj));
        ASSERT_EQ(6, stats->numBucketReopeningsFailedDueToValidator.load());
        unfreezeBucket();

        // Missing control.max.time.
        BSONObj missingMaxTimeObj = compressedBucketDoc.addFields(
            BSON("control" << BSON("version"
                                   << 1 << "min"
                                   << BSON("time" << BSON("$date" << "2022-06-06T15:34:00.000Z"))
                                   << "max" << BSON("abc" << 1))));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), missingMaxTimeObj));
        ASSERT_EQ(7, stats->numBucketReopeningsFailedDueToValidator.load());
        unfreezeBucket();
    }

    {
        // Missing data field.
        BSONObj missingDataObj = compressedBucketDoc.removeField("data");
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), missingDataObj));
        ASSERT_EQ(8, stats->numBucketReopeningsFailedDueToValidator.load());
        unfreezeBucket();

        // Bad time field in the data field.
        BSONObj badTimeFieldInDataFieldObj =
            compressedBucketDoc.addFields(BSON("data" << BSON("time" << BSON("0" << 123))));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badTimeFieldInDataFieldObj));
        ASSERT_EQ(1, stats->numBucketReopeningsFailedDueToUncompressedTimeColumn.load());
        unfreezeBucket();

        // Bad data type.
        BSONObj badDataObj = compressedBucketDoc.addFields(BSON("data" << 123));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badDataObj));
        ASSERT_EQ(9, stats->numBucketReopeningsFailedDueToValidator.load());
        unfreezeBucket();
    }
}

TEST_F(BucketCatalogTest, ReopenMixedSchemaDataBucket) {
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"02091c2c050b7495eaef4581"},
            "control":{"version":1,
                       "min":{"_id":{"$oid":"63091c30138e9261fd70a903"},
                              "time":{"$date":"2022-08-26T19:19:00Z"},
                              "x":1},
                       "max":{"_id":{"$oid":"63091c30138e9261fd70a905"},
                       "time":{"$date":"2022-08-26T19:19:30Z"},
                       "x":{"y":"z"}}},
            "data":{"_id":{"0":{"$oid":"63091c30138e9261fd70a903"},
                           "1":{"$oid":"63091c30138e9261fd70a904"},
                           "2":{"$oid":"63091c30138e9261fd70a905"}},
                    "time":{"0":{"$date":"2022-08-26T19:19:30Z"},
                            "1":{"$date":"2022-08-26T19:19:30Z"},
                            "2":{"$date":"2022-08-26T19:19:30Z"}},
                    "x":{"0":1,"1":{"y":"z"},"2":"abc"}}})");

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    BSONObj compressedBucketDoc = _getCompressedBucketDoc(bucketDoc);
    ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), compressedBucketDoc));

    auto stats = internal::getCollectionExecutionStats(*_bucketCatalog, _uuid1);
    ASSERT_EQ(1, stats->numBucketReopeningsFailedDueToSchemaGeneration.load());
}

TEST_F(BucketCatalogTest, ReopenClosedBuckets) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    {
        // control.closed: true
        BSONObj closedBucket = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3},
                                   "closed": true},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
        BSONObj compressedClosedBucketDoc = _getCompressedBucketDoc(closedBucket);
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), compressedClosedBucketDoc));
        auto stats = internal::getCollectionExecutionStats(*_bucketCatalog, _uuid1);
        ASSERT_EQ(1, stats->numBucketReopeningsFailedDueToMarkedClosed.load());
        auto bucketStates = _bucketCatalog->bucketStateRegistry.bucketStates;
        ASSERT_EQ(1, bucketStates.size());
        ASSERT(isBucketStateFrozen(bucketStates.begin()->second));
    }
}

TEST_F(BucketCatalogTest, ReopenNotClosedBuckets) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    {
        // control.closed: false
        BSONObj openBucket = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a518"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3},
                                   "closed": false},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
        BSONObj compressedOpenBucketDoc = _getCompressedBucketDoc(openBucket);
        ASSERT_OK(_reopenBucket(autoColl.getCollection(), compressedOpenBucketDoc));
        auto bucketStates = _bucketCatalog->bucketStateRegistry.bucketStates;
        ASSERT_EQ(1, bucketStates.size());
        ASSERT(std::holds_alternative<BucketState>(bucketStates.begin()->second));
        ASSERT_EQ(BucketState::kNormal, std::get<BucketState>(bucketStates.begin()->second));
    }

    {
        // No control.closed
        BSONObj openBucket = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
        BSONObj compressedOpenBucketDoc = _getCompressedBucketDoc(openBucket);
        ASSERT_OK(_reopenBucket(autoColl.getCollection(), compressedOpenBucketDoc));
        auto bucketStates = _bucketCatalog->bucketStateRegistry.bucketStates;
        ASSERT_EQ(1, bucketStates.size());
        ASSERT(std::holds_alternative<BucketState>(bucketStates.begin()->second));
        ASSERT_EQ(BucketState::kNormal, std::get<BucketState>(bucketStates.begin()->second));
    }
}

TEST_F(BucketCatalogTest, ReopenCompressedBucketAndInsertCompatibleMeasurement) {
    // Bucket document to reopen.
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
    BSONObj compressedBucketDoc = _getCompressedBucketDoc(bucketDoc);
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    auto memUsageBefore = getMemoryUsage(*_bucketCatalog);
    Status status = _reopenBucket(autoColl.getCollection(), compressedBucketDoc);
    auto memUsageAfter = getMemoryUsage(*_bucketCatalog);
    ASSERT_OK(status);
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumBucketsReopened));
    ASSERT_GT(memUsageAfter, memUsageBefore);

    // Insert a measurement that is compatible with the reopened bucket.
    auto batch = _insertOneWithoutReopening(
        _opCtx,
        *_bucketCatalog,
        _ns1,
        _uuid1,
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":-100,"b":100})"));

    ASSERT_EQ(0, _getExecutionStat(_uuid1, kNumSchemaChanges));

    ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT_EQ(batch->measurements.size(), 1) << batch->toBSON();

    // The reopened bucket already contains three committed measurements.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 3) << batch->toBSON();

    // Verify that the min and max is updated correctly when inserting new measurements.
    ASSERT_BSONOBJ_BINARY_EQ(batch->min, BSON("u" << BSON("a" << -100)));
    ASSERT_BSONOBJ_BINARY_EQ(
        batch->max,
        BSON("u" << BSON("time" << Date_t::fromMillisSinceEpoch(1654529680000) << "b" << 100)));

    finish(*_bucketCatalog, batch);
}

TEST_F(BucketCatalogTest, ReopenCompressedBucketAndInsertIncompatibleMeasurement) {
    // Bucket document to reopen.
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
    BSONObj compressedBucketDoc = _getCompressedBucketDoc(bucketDoc);
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    auto memUsageBefore = getMemoryUsage(*_bucketCatalog);
    Status status = _reopenBucket(autoColl.getCollection(), compressedBucketDoc);
    auto memUsageAfter = getMemoryUsage(*_bucketCatalog);
    ASSERT_OK(status);
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumBucketsReopened));
    ASSERT_GT(memUsageAfter, memUsageBefore);

    // Insert a measurement that is incompatible with the reopened bucket.
    auto batch = _insertOneWithoutReopening(
        _opCtx,
        *_bucketCatalog,
        _ns1,
        _uuid1,
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":{},"b":{}})"));

    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumSchemaChanges));

    ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT_EQ(batch->measurements.size(), 1) << batch->toBSON();

    // Since the reopened bucket was incompatible, we opened a new one.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 0) << batch->toBSON();

    finish(*_bucketCatalog, batch);
}

TEST_F(BucketCatalogTest, RehydrateMalformedBucket) {
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
    BSONObj compressedBucketDoc = _getCompressedBucketDoc(bucketDoc);
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    ASSERT_OK(_testRehydrateBucket(autoColl.getCollection(), compressedBucketDoc));

    {
        // Missing _id field.
        BSONObj missingIdObj = compressedBucketDoc.removeField("_id");
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), missingIdObj));

        // Bad _id type.
        BSONObj badIdObj = compressedBucketDoc.addFields(BSON("_id" << 123));
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), badIdObj));
    }

    {
        // Missing control field.
        BSONObj missingControlObj = compressedBucketDoc.removeField("control");
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), missingControlObj));

        // Bad control type.
        BSONObj badControlObj = compressedBucketDoc.addFields(BSON("control" << BSONArray()));
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), badControlObj));

        // Bad control.version type.
        BSONObj badVersionObj = compressedBucketDoc.addFields(BSON(
            "control" << BSON(
                "version" << BSONArray() << "min"
                          << BSON("time" << BSON("$date" << "2022-06-06T15:34:00.000Z")) << "max"
                          << BSON("time" << BSON("$date" << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), badVersionObj));

        // Bad control.min type.
        BSONObj badMinObj = compressedBucketDoc.addFields(BSON(
            "control" << BSON("version"
                              << 1 << "min" << 123 << "max"
                              << BSON("time" << BSON("$date" << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), badMinObj));

        // Bad control.max type.
        BSONObj badMaxObj = compressedBucketDoc.addFields(
            BSON("control" << BSON("version"
                                   << 1 << "min"
                                   << BSON("time" << BSON("$date" << "2022-06-06T15:34:00.000Z"))
                                   << "max" << 123)));
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), badMaxObj));

        // Missing control.min.time.
        BSONObj missingMinTimeObj = compressedBucketDoc.addFields(BSON(
            "control" << BSON("version"
                              << 1 << "min" << BSON("abc" << 1) << "max"
                              << BSON("time" << BSON("$date" << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), missingMinTimeObj));

        // Missing control.max.time.
        BSONObj missingMaxTimeObj = compressedBucketDoc.addFields(
            BSON("control" << BSON("version"
                                   << 1 << "min"
                                   << BSON("time" << BSON("$date" << "2022-06-06T15:34:00.000Z"))
                                   << "max" << BSON("abc" << 1))));
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), missingMaxTimeObj));
    }

    {
        // Missing data field.
        BSONObj missingDataObj = compressedBucketDoc.removeField("data");
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), missingDataObj));

        // Bad time field in the data field.
        BSONObj badTimeFieldInDataFieldObj =
            compressedBucketDoc.addFields(BSON("data" << BSON("time" << BSON("0" << 123))));
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), badTimeFieldInDataFieldObj));

        // Bad data type.
        BSONObj badDataObj = compressedBucketDoc.addFields(BSON("data" << 123));
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), badDataObj));
    }
}

TEST_F(BucketCatalogTest, RehydrateMixedSchemaDataBucket) {
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"02091c2c050b7495eaef4581"},
            "control":{"version":1,
                       "min":{"_id":{"$oid":"63091c30138e9261fd70a903"},
                              "time":{"$date":"2022-08-26T19:19:00Z"},
                              "x":1},
                       "max":{"_id":{"$oid":"63091c30138e9261fd70a905"},
                       "time":{"$date":"2022-08-26T19:19:30Z"},
                       "x":{"y":"z"}}},
            "data":{"_id":{"0":{"$oid":"63091c30138e9261fd70a903"},
                           "1":{"$oid":"63091c30138e9261fd70a904"},
                           "2":{"$oid":"63091c30138e9261fd70a905"}},
                    "time":{"0":{"$date":"2022-08-26T19:19:30Z"},
                            "1":{"$date":"2022-08-26T19:19:30Z"},
                            "2":{"$date":"2022-08-26T19:19:30Z"}},
                    "x":{"0":1,"1":{"y":"z"},"2":"abc"}}})");

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    BSONObj compressedBucketDoc = _getCompressedBucketDoc(bucketDoc);
    ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), compressedBucketDoc));

    auto stats = internal::getCollectionExecutionStats(*_bucketCatalog, _uuid1);
    ASSERT_EQ(1, stats->numBucketReopeningsFailedDueToSchemaGeneration.load());
}

TEST_F(BucketCatalogTest, RehydrateClosedBuckets) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    {
        // control.closed: true
        BSONObj closedBucket = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3},
                                   "closed": true},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
        BSONObj compressedClosedBucketDoc = _getCompressedBucketDoc(closedBucket);
        ASSERT_NOT_OK(_testRehydrateBucket(autoColl.getCollection(), compressedClosedBucketDoc));
        auto bucketStates = _bucketCatalog->bucketStateRegistry.bucketStates;
        ASSERT_EQ(1, bucketStates.size());
        ASSERT(isBucketStateFrozen(bucketStates.begin()->second));
    }
}

TEST_F(BucketCatalogTest, RehydrateNotClosedBuckets) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    {
        // control.closed: false
        BSONObj openBucket = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a518"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3},
                                   "closed": false},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
        BSONObj compressedOpenBucketDoc = _getCompressedBucketDoc(openBucket);
        ASSERT_OK(_testRehydrateBucket(autoColl.getCollection(), compressedOpenBucketDoc));
    }

    {
        // No control.closed
        BSONObj openBucket = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
        BSONObj compressedOpenBucketDoc = _getCompressedBucketDoc(openBucket);
        ASSERT_OK(_testRehydrateBucket(autoColl.getCollection(), compressedOpenBucketDoc));
    }
}

TEST_F(BucketCatalogTest, ReopenBucketWithIncorrectEra) {
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    BSONObj compressedBucketDoc = _getCompressedBucketDoc(bucketDoc);

    _bucketCatalog->bucketStateRegistry.currentEra = 1ul;
    ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), compressedBucketDoc, 0ul));
    auto stats = internal::getCollectionExecutionStats(*_bucketCatalog, _uuid1);
    ASSERT_EQ(1, stats->numBucketReopeningsFailedDueToEraMismatch.load());

    ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), compressedBucketDoc, boost::none, 0ul));
    ASSERT_EQ(1, stats->numBucketReopeningsFailedDueToWriteConflict.load());
}

TEST_F(BucketCatalogTest, ReopeningFailedDueToHashCollision) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
    BSONObj compressedBucketDoc = _getCompressedBucketDoc(bucketDoc);

    auto dummyUUID = UUID::parse("12345678-1234-4000-8000-000000000000");
    tracking::Context dummyTrackingContext{};
    auto incorrectMetadata = BSON(kBucketMetaFieldName << 1);
    auto invalidKey = BucketKey{dummyUUID.getValue(),
                                BucketMetadata{dummyTrackingContext,
                                               incorrectMetadata.firstElement(),
                                               StringData("incorrect field")}};

    ASSERT_NOT_OK(_reopenBucket(
        autoColl.getCollection(), compressedBucketDoc, boost::none, boost::none, invalidKey));
    auto stats = internal::getCollectionExecutionStats(*_bucketCatalog, _uuid1);
    ASSERT_EQ(1, stats->numBucketReopeningsFailedDueToHashCollision.load());
}

TEST_F(BucketCatalogTest, ReopeningFailedDueToMarkedFrozen) {
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
    BSONObj compressedBucketDoc = _getCompressedBucketDoc(bucketDoc);
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    ASSERT_OK(_reopenBucket(autoColl.getCollection(), compressedBucketDoc));
    auto stats = internal::getCollectionExecutionStats(*_bucketCatalog, _uuid1);

    auto& registry = _bucketCatalog->bucketStateRegistry;
    ASSERT_EQ(1, registry.bucketStates.size());
    const auto bucketId = registry.bucketStates.begin()->first;
    registry.bucketStates[bucketId] = {BucketState::kFrozen};

    ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), compressedBucketDoc.copy()));
    ASSERT_EQ(1, stats->numBucketReopeningsFailedDueToMarkedFrozen.load());
}

TEST_F(BucketCatalogTest, ReopeningFailedDueToMinMaxCalculation) {
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"}},
                                   "max":{}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    BSONObj compressedBucketDoc = _getCompressedBucketDoc(bucketDoc);

    const auto alwaysPassValidator =
        [](const BSONObj& document) -> std::pair<Collection::SchemaValidationResult, Status> {
        return {Collection::SchemaValidationResult::kPass, Status::OK()};
    };
    ASSERT_NOT_OK(
        _reopenBucket(autoColl.getCollection(),
                      compressedBucketDoc,
                      boost::none,
                      boost::none,
                      boost::none,
                      boost::optional<internal::BucketDocumentValidator>{alwaysPassValidator}));
    auto stats = internal::getCollectionExecutionStats(*_bucketCatalog, _uuid1);
    ASSERT_EQ(1, stats->numBucketReopeningsFailedDueToMinMaxCalculation.load());
}

DEATH_TEST_F(BucketCatalogTest, ReopeningFailedDueToCompression, "invariant") {
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":2,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3},
                                   "count":3},
            "data":{"time":{"$binary":"CQBwO6c5gQEAAIANAAAAAAAAAAA=","$type":"07"},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"$binary":"AQAAAAAAAADwP5AtAAAACAAAAAA=","$type":"07"}}})");
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    BSONObj compressedBucketDoc = _getCompressedBucketDoc(bucketDoc);

    std::ignore = _reopenBucket(autoColl.getCollection(), bucketDoc);
}

TEST_F(BucketCatalogTest, ArchivingAndClosingUnderSideBucketCatalogMemoryPressure) {
    // Initialize the side bucket catalog.
    auto sideBucketCatalog =
        std::make_unique<BucketCatalog>(1, getTimeseriesSideBucketCatalogMemoryUsageThresholdBytes);

    // Create dummy bucket and populate bucket state registry.
    auto dummyUUID = UUID::gen();
    auto dummyBucketId = BucketId(dummyUUID, OID(), 0);
    auto dummyBucketKey =
        BucketKey(dummyUUID,
                  BucketMetadata(getTrackingContext(sideBucketCatalog->trackingContexts,
                                                    TrackingScope::kOpenBucketsById),
                                 BSONElement{},
                                 boost::none));
    sideBucketCatalog->bucketStateRegistry.bucketStates.emplace(dummyBucketId,
                                                                BucketState::kNormal);
    auto dummyBucket = std::make_unique<Bucket>(sideBucketCatalog->trackingContexts,
                                                dummyBucketId,
                                                dummyBucketKey,
                                                "time",
                                                Date_t(),
                                                sideBucketCatalog->bucketStateRegistry);

    // Create and populate stripe.
    auto& stripe = *sideBucketCatalog->stripes[0];
    stripe.openBucketsById.try_emplace(
        dummyBucketId,
        tracking::make_unique<Bucket>(getTrackingContext(sideBucketCatalog->trackingContexts,
                                                         TrackingScope::kOpenBucketsById),
                                      sideBucketCatalog->trackingContexts,
                                      dummyBucketId,
                                      dummyBucketKey,
                                      "time",
                                      Date_t(),
                                      sideBucketCatalog->bucketStateRegistry));
    stripe.openBucketsByKey[dummyBucketKey].emplace(dummyBucket.get());
    stripe.idleBuckets.push_front(dummyBucket.get());
    dummyBucket->idleListEntry = stripe.idleBuckets.begin();
    stdx::lock_guard stripeLock{stripe.mutex};

    // Create execution stats controller.
    auto collectionStats = std::make_shared<ExecutionStats>();
    auto statsController =
        ExecutionStatsController(collectionStats, sideBucketCatalog->globalExecutionStats);

    // Ensure we start out with no buckets archived or closed due to memory pressure.
    ASSERT_EQ(0, collectionStats->numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(0, collectionStats->numBucketsClosedDueToMemoryThreshold.load());
    ASSERT_EQ(
        0, sideBucketCatalog->globalExecutionStats.numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(0,
              sideBucketCatalog->globalExecutionStats.numBucketsClosedDueToMemoryThreshold.load());

    // Set the catalog memory usage to be above the memory usage threshold.
    auto& sideContext =
        getTrackingContext(sideBucketCatalog->trackingContexts, TrackingScope::kOpenBucketsById);
    sideContext.stats().bytesAllocated(getTimeseriesSideBucketCatalogMemoryUsageThresholdBytes() -
                                       getMemoryUsage(*sideBucketCatalog) + 1);

    // When we exceed the memory usage threshold we will first try to archive idle buckets to try
    // to get below the threshold. If this does not get us beneath the threshold, we will then try
    // to close archived buckets, until we hit the global expiry max count limit (or if we run out
    // of idle buckets in this stripe). Then, we try to close any archived buckets. In this
    // particular execution we should expect not to close any buckets, but we should archive one.
    internal::expireIdleBuckets(*sideBucketCatalog, stripe, stripeLock, dummyUUID, statsController);

    ASSERT_EQ(1, collectionStats->numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(
        1, sideBucketCatalog->globalExecutionStats.numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(0, collectionStats->numBucketsClosedDueToMemoryThreshold.load());
    ASSERT_EQ(0,
              sideBucketCatalog->globalExecutionStats.numBucketsClosedDueToMemoryThreshold.load());

    // Clears the list of idle buckets - usually this is done within the expire idle buckets
    // function, but that requires setting up more state with the bucket's idleListEntry. This gets
    // around that for testing purposes.
    stripe.idleBuckets.clear();

    // Set the memory usage to be back at the threshold. Now, when we run expire idle buckets again,
    // because there are no idle buckets left to archive, we will close the bucket that we
    // previously archived.
    sideContext.stats().bytesAllocated(getTimeseriesSideBucketCatalogMemoryUsageThresholdBytes() -
                                       getMemoryUsage(*sideBucketCatalog) + +1);
    internal::expireIdleBuckets(*sideBucketCatalog, stripe, stripeLock, dummyUUID, statsController);

    ASSERT_EQ(1, collectionStats->numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(
        1, sideBucketCatalog->globalExecutionStats.numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(1, collectionStats->numBucketsClosedDueToMemoryThreshold.load());
    ASSERT_EQ(1,
              sideBucketCatalog->globalExecutionStats.numBucketsClosedDueToMemoryThreshold.load());
}

TEST_F(BucketCatalogTest, GetCacheDerivedBucketMaxSize) {
    auto [effectiveMaxSize, cacheDerivedBucketMaxSize] = internal::getCacheDerivedBucketMaxSize(
        /*storageCacheSizeBytes=*/128 * 1000 * 1000, /*workloadCardinality=*/1000);
    ASSERT_EQ(effectiveMaxSize, 64 * 1000);
    ASSERT_EQ(cacheDerivedBucketMaxSize, 64 * 1000);

    std::tie(effectiveMaxSize, cacheDerivedBucketMaxSize) = internal::getCacheDerivedBucketMaxSize(
        /*storageCacheSizeBytes=*/0, /*workloadCardinality=*/1000);
    ASSERT_EQ(effectiveMaxSize, gTimeseriesBucketMinSize.load());
    ASSERT_EQ(cacheDerivedBucketMaxSize, gTimeseriesBucketMinSize.load());

    std::tie(effectiveMaxSize, cacheDerivedBucketMaxSize) = internal::getCacheDerivedBucketMaxSize(
        /*storageCacheSizeBytes=*/128 * 1000 * 1000, /*workloadCardinality=*/0);
    ASSERT_EQ(effectiveMaxSize, gTimeseriesBucketMaxSize);
    ASSERT_EQ(cacheDerivedBucketMaxSize, INT_MAX);
}

TEST_F(BucketCatalogTest, GetCacheDerivedBucketMaxSizeRespectsAbsoluteMax) {
    auto [effectiveMaxSize, cacheDerivedBucketMaxSize] = internal::getCacheDerivedBucketMaxSize(
        /*storageCacheSizeBytes=*/gTimeseriesBucketMaxSize * 10, /*workloadCardinality=*/1);
    ASSERT_EQ(effectiveMaxSize, gTimeseriesBucketMaxSize);
    ASSERT_EQ(cacheDerivedBucketMaxSize, gTimeseriesBucketMaxSize * 5);
}

TEST_F(BucketCatalogTest, GetCacheDerivedBucketMaxSizeRespectsAbsoluteMin) {
    auto [effectiveMaxSize, cacheDerivedBucketMaxSize] = internal::getCacheDerivedBucketMaxSize(
        /*storageCacheSizeBytes=*/1, /*workloadCardinality=*/1);
    ASSERT_EQ(effectiveMaxSize, gTimeseriesBucketMinSize.load());
    ASSERT_EQ(cacheDerivedBucketMaxSize, gTimeseriesBucketMinSize.load());
}

TEST_F(BucketCatalogTest, OIDCollisionIsHandledForFrozenBucket) {
    // Simplify this test by only using one stripe.
    FailPointEnableBlock failPoint("alwaysUseSameBucketCatalogStripe");
    auto time = Date_t::now();
    // Insert a measurement to create a bucket.
    auto batch = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << time << _metaField << "A"));
    ASSERT(batch);

    auto doc = BSON(_timeField << time << _metaField << "B");
    // Get the batchedInsertContexts so that we can get the key signature for the BucketID - again,
    // to trigger a collision of our frozen bucketID with the bucketID of the bucket we will
    // allocate.
    auto timeseriesOptions = _getTimeseriesOptions(_ns1);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts = bucket_catalog::buildBatchedInsertContexts(*_bucketCatalog,
                                                                            _uuid1,
                                                                            timeseriesOptions,
                                                                            {doc},
                                                                            /*startIndex=*/0,
                                                                            /*numDocsToStage=*/1,
                                                                            /*docsToRetry=*/{},
                                                                            errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    auto batchedInsertCtx = batchedInsertContexts[0];

    // Get the next sequential OID so that we can trigger an ID collision down the line.
    auto OIDAndRoundedTime = internal::generateBucketOID(time, batchedInsertCtx.options);
    OID nextBucketOID = std::get<OID>(OIDAndRoundedTime);

    BucketId nextBucketId{_uuid1, nextBucketOID, batchedInsertCtx.key.signature()};

    // Mark the next bucketID as being frozen. We could arrive at this state if there was a bucket
    // that we tried reopening that was not compressed, and was also corrupted; when we try to
    // compress it and fail to do so successfully because it is corrupted, we freeze it. When it is
    // in this state, it wouldn't in memory in the openBucketsByKey/openBucketsById structures of
    // any stripe, but it would have an entry in the bucketStateRegistry for its id. This is an edge
    // case since currently there should be no way to end up with the same bucketID across
    // stripes/within the same stripe.
    freezeBucket(_bucketCatalog->bucketStateRegistry, nextBucketId);

    // We should see bucket collision that gets retried, leading to the insert eventually
    // succeeding.
    auto batch2 = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << time << _metaField << "B"));
    ASSERT_NE(nextBucketId, batch2->bucketId) << batch2->toBSON();
    // We should check that the bucketID that we failed to create is not stored in the stripe.
    ASSERT(!_bucketCatalog->stripes[0]->openBucketsById.contains(nextBucketId));
}

TEST_F(BucketCatalogTest, WriteConflictIfPrepareCommitOnClearedBucket) {
    auto time = Date_t::now();
    // Set up an insert to create a bucket.
    auto batch = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << time << _metaField << "A"));
    ASSERT(batch);

    directWriteStart(_bucketCatalog->bucketStateRegistry, batch->bucketId);
    directWriteFinish(_bucketCatalog->bucketStateRegistry, batch->bucketId);

    // Preparing fails on a cleared bucket and aborts the batch.
    auto status = prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1));
    ASSERT_EQ(status.code(), ErrorCodes::TimeseriesBucketCleared);
}

TEST_F(BucketCatalogTest, WriteConflictIfDirectWriteOnPreparedBucket) {
    auto time = Date_t::now();

    auto batch = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << time << _metaField << "A"));
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));

    // A direct write on a prepared bucket will throw a write conflict.
    ASSERT_THROWS_CODE(directWriteStart(_bucketCatalog->bucketStateRegistry, batch->bucketId),
                       DBException,
                       ErrorCodes::WriteConflict);
}

TEST_F(BucketCatalogTest, DirectWritesCanStack) {
    auto time = Date_t::now();
    // The batch can be used for both, as it is only used to obtain the bucketId.
    auto batch = _insertOneWithoutReopening(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << time << _metaField << "A"));

    directWriteStart(_bucketCatalog->bucketStateRegistry, batch->bucketId);
    directWriteStart(_bucketCatalog->bucketStateRegistry, batch->bucketId);

    // The status of the bucket will reflect both writes.
    auto bucketState = getBucketState(_bucketCatalog->bucketStateRegistry, batch->bucketId);
    ASSERT(bucketState);
    ASSERT(std::holds_alternative<int>(*bucketState));
    ASSERT_EQ(std::get<int>(*bucketState), 2);

    directWriteFinish(_bucketCatalog->bucketStateRegistry, batch->bucketId);
    directWriteFinish(_bucketCatalog->bucketStateRegistry, batch->bucketId);

    // Once both direct writes are finished, the bucket will transition to kCleared normally.
    bucketState = getBucketState(_bucketCatalog->bucketStateRegistry, batch->bucketId);
    ASSERT(bucketState);
    ASSERT(std::holds_alternative<BucketState>(*bucketState));
    ASSERT_EQ(std::get<BucketState>(*bucketState), BucketState::kCleared);
}

TEST_F(BucketCatalogTest, FindOpenBucketsEmptyCatalog) {
    auto dummyBucketKey =
        BucketKey(_uuid1,
                  BucketMetadata(getTrackingContext(_bucketCatalog->trackingContexts,
                                                    TrackingScope::kOpenBucketsByKey),
                                 BSONElement{},
                                 boost::none));

    ASSERT(_bucketCatalog->stripes[0]->openBucketsByKey.empty());
    ASSERT(internal::findOpenBuckets(
               *_bucketCatalog->stripes[0], WithLock::withoutLock(), dummyBucketKey)
               .empty());
}

TEST_F(BucketCatalogTest, FindOpenBucketsReturnsBucket) {
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
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);
    auto openBuckets =
        internal::findOpenBuckets(*_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                  WithLock::withoutLock(),
                                  batchedInsertCtx.key);
    ASSERT_EQ(1, openBuckets.size());
    ASSERT_EQ(&bucket, openBuckets[0]);
}

TEST_F(BucketCatalogTest, CheckBucketStateAndCleanupWithBucketWithDirectWrite) {
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
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);

    directWriteStart(_bucketCatalog->bucketStateRegistry, bucket.bucketId);

    // Ineligible for inserts. Remove from 'openBucketsByKey'.
    ASSERT(internal::isBucketStateEligibleForInsertsAndCleanup(
               *_bucketCatalog,
               *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
               WithLock::withoutLock(),
               &bucket) == internal::BucketStateForInsertAndCleanup::kInsertionConflict);
    ASSERT(_bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsByKey.empty());
}

TEST_F(BucketCatalogTest, CheckBucketStateAndCleanupWithClearedBucket) {
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
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);

    clear(*_bucketCatalog, bucket.bucketId.collectionUUID);

    // Ineligible for inserts. Remove from 'openBucketsByKey'.
    ASSERT(internal::isBucketStateEligibleForInsertsAndCleanup(
               *_bucketCatalog,
               *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
               WithLock::withoutLock(),
               &bucket) == internal::BucketStateForInsertAndCleanup::kInsertionConflict);
    ASSERT(_bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsByKey.empty());
}

TEST_F(BucketCatalogTest, CheckBucketStateAndCleanupWithFrozenBucket) {
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
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);

    freezeBucket(_bucketCatalog->bucketStateRegistry, bucket.bucketId);

    // Ineligible for inserts. Remove from 'openBucketsByKey'.
    ASSERT(internal::isBucketStateEligibleForInsertsAndCleanup(
               *_bucketCatalog,
               *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
               WithLock::withoutLock(),
               &bucket) == internal::BucketStateForInsertAndCleanup::kInsertionConflict);
    ASSERT(_bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsByKey.empty());
}

TEST_F(BucketCatalogTest, determineBucketRolloverForMeasurementNone) {
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
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kNone);

    auto rolloverReason = determineBucketRolloverForMeasurement(*_bucketCatalog,
                                                                _measurement,
                                                                measurementTimestamp,
                                                                timeseriesOptions,
                                                                _stringDataComparatorUnused,
                                                                _storageCacheSizeBytes,
                                                                bucket,
                                                                batchedInsertCtx.stats);

    ASSERT_EQ(rolloverReason, RolloverReason::kNone);
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kNone);
}

TEST_F(BucketCatalogTest, determineBucketRolloverForMeasurementTimeForward) {
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
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kNone);

    auto forwardTimestamp = bucket.minTime + Seconds(*timeseriesOptions.getBucketMaxSpanSeconds());
    auto timeForwardMeasurement = BSON(_timeField << forwardTimestamp << _metaField << _metaValue);
    auto rolloverReason = determineBucketRolloverForMeasurement(*_bucketCatalog,
                                                                timeForwardMeasurement,
                                                                forwardTimestamp,
                                                                timeseriesOptions,
                                                                _stringDataComparatorUnused,
                                                                _storageCacheSizeBytes,
                                                                bucket,
                                                                batchedInsertCtx.stats);

    ASSERT_EQ(rolloverReason, RolloverReason::kTimeForward);
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kTimeForward);
}

TEST_F(BucketCatalogTest, determineBucketRolloverForMeasurementTimeBackward) {
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
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kNone);

    auto backwardTimestamp = bucket.minTime - Seconds(1);
    auto timeBackwardMeasurement =
        BSON(_timeField << backwardTimestamp << _metaField << _metaValue);
    auto rolloverReason = determineBucketRolloverForMeasurement(*_bucketCatalog,
                                                                timeBackwardMeasurement,
                                                                backwardTimestamp,
                                                                timeseriesOptions,
                                                                _stringDataComparatorUnused,
                                                                _storageCacheSizeBytes,
                                                                bucket,
                                                                batchedInsertCtx.stats);

    ASSERT_EQ(rolloverReason, RolloverReason::kTimeBackward);
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kTimeBackward);
}

TEST_F(BucketCatalogTest, determineBucketRolloverForMeasurementCount) {
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
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);
    bucket.numMeasurements = gTimeseriesBucketMaxCount;
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kNone);

    auto rolloverReason = determineBucketRolloverForMeasurement(*_bucketCatalog,
                                                                _measurement,
                                                                measurementTimestamp,
                                                                timeseriesOptions,
                                                                _stringDataComparatorUnused,
                                                                _storageCacheSizeBytes,
                                                                bucket,
                                                                batchedInsertCtx.stats);

    ASSERT_EQ(rolloverReason, RolloverReason::kCount);
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kCount);
}

TEST_F(BucketCatalogTest, determineBucketRolloverForMeasurementSize) {
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
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);
    bucket.size = gTimeseriesBucketMaxSize;
    bucket.numMeasurements = gTimeseriesBucketMinCount;
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kNone);

    auto rolloverReason = determineBucketRolloverForMeasurement(*_bucketCatalog,
                                                                _measurement,
                                                                measurementTimestamp,
                                                                timeseriesOptions,
                                                                _stringDataComparatorUnused,
                                                                _storageCacheSizeBytes,
                                                                bucket,
                                                                batchedInsertCtx.stats);

    ASSERT_EQ(rolloverReason, RolloverReason::kSize);
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kSize);
}

TEST_F(BucketCatalogTest, determineBucketRolloverForMeasurementCachePressure) {
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
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);
    bucket.size = gTimeseriesBucketMaxSize;
    bucket.numMeasurements = gTimeseriesBucketMinCount;
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kNone);

    auto rolloverReason = determineBucketRolloverForMeasurement(*_bucketCatalog,
                                                                _measurement,
                                                                measurementTimestamp,
                                                                timeseriesOptions,
                                                                _stringDataComparatorUnused,
                                                                kLimitedStorageCacheSizeBytes,
                                                                bucket,
                                                                batchedInsertCtx.stats);

    ASSERT_EQ(rolloverReason, RolloverReason::kCachePressure);
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kCachePressure);
}

TEST_F(BucketCatalogTest, determineBucketRolloverForMeasurementSchemaChange) {
    auto timeseriesOptions = _getTimeseriesOptions(_ns1);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto measurementSchema1 =
        BSON(_timeField << Date_t::now() << _metaField << _metaValue << "metricField"
                        << "a");
    auto measurementSchema2 =
        BSON(_timeField << Date_t::now() << _metaField << _metaValue << "metricField" << 1);
    auto batchedInsertContexts = bucket_catalog::buildBatchedInsertContexts(*_bucketCatalog,
                                                                            _uuid1,
                                                                            timeseriesOptions,
                                                                            {measurementSchema1},
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
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);
    auto updateStatus = bucket.schema.update(
        measurementSchema1, timeseriesOptions.getMetaField(), _stringDataComparatorUnused);
    invariant(updateStatus == Schema::UpdateStatus::Updated);
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kNone);

    auto rolloverReason = determineBucketRolloverForMeasurement(*_bucketCatalog,
                                                                measurementSchema2,
                                                                measurementTimestamp,
                                                                timeseriesOptions,
                                                                _stringDataComparatorUnused,
                                                                _storageCacheSizeBytes,
                                                                bucket,
                                                                batchedInsertCtx.stats);

    ASSERT_EQ(rolloverReason, RolloverReason::kSchemaChange);
    ASSERT_EQ(bucket.rolloverReason, RolloverReason::kSchemaChange);
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsOpen) {
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
    auto bucketOpenedDueToMetadata = true;

    Bucket& bucket =
        internal::allocateBucket(*_bucketCatalog,
                                 *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                 WithLock::withoutLock(),
                                 batchedInsertCtx.key,
                                 batchedInsertCtx.options,
                                 measurementTimestamp,
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);

    AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
    auto potentialBuckets =
        findAndRolloverOpenBuckets(*_bucketCatalog,
                                   *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                   WithLock::withoutLock(),
                                   batchedInsertCtx.key,
                                   measurementTimestamp,
                                   Seconds(*batchedInsertCtx.options.getBucketMaxSpanSeconds()),
                                   allowQueryBasedReopening,
                                   bucketOpenedDueToMetadata);
    ASSERT(!bucketOpenedDueToMetadata);
    ASSERT_EQ(1, potentialBuckets.size());
    ASSERT_EQ(&bucket, potentialBuckets[0]);
    ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kAllow);
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsSoftClose) {
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
    auto bucketOpenedDueToMetadata = true;

    Bucket& bucket =
        internal::allocateBucket(*_bucketCatalog,
                                 *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                 WithLock::withoutLock(),
                                 batchedInsertCtx.key,
                                 batchedInsertCtx.options,
                                 measurementTimestamp,
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);

    bucket.rolloverReason = RolloverReason::kTimeForward;
    AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
    auto potentialBuckets =
        findAndRolloverOpenBuckets(*_bucketCatalog,
                                   *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                   WithLock::withoutLock(),
                                   batchedInsertCtx.key,
                                   measurementTimestamp,
                                   Seconds(*batchedInsertCtx.options.getBucketMaxSpanSeconds()),
                                   allowQueryBasedReopening,
                                   bucketOpenedDueToMetadata);
    ASSERT(!bucketOpenedDueToMetadata);
    ASSERT_EQ(1, potentialBuckets.size());
    ASSERT_EQ(&bucket, potentialBuckets[0]);
    ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kAllow);
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsSoftCloseNotSelected) {
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
    auto bucketOpenedDueToMetadata = true;

    Bucket& bucket =
        internal::allocateBucket(*_bucketCatalog,
                                 *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                 WithLock::withoutLock(),
                                 batchedInsertCtx.key,
                                 batchedInsertCtx.options,
                                 measurementTimestamp,
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);

    bucket.rolloverReason = RolloverReason::kTimeForward;
    AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
    auto potentialBuckets =
        findAndRolloverOpenBuckets(*_bucketCatalog,
                                   *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                   WithLock::withoutLock(),
                                   batchedInsertCtx.key,
                                   measurementTimestamp + Hours(2),
                                   Seconds(*batchedInsertCtx.options.getBucketMaxSpanSeconds()),
                                   allowQueryBasedReopening,
                                   bucketOpenedDueToMetadata);
    ASSERT(!bucketOpenedDueToMetadata);
    ASSERT_EQ(0, potentialBuckets.size());
    ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kDisallow);
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsArchive) {
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
    auto bucketOpenedDueToMetadata = true;

    Bucket& bucket =
        internal::allocateBucket(*_bucketCatalog,
                                 *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                 WithLock::withoutLock(),
                                 batchedInsertCtx.key,
                                 batchedInsertCtx.options,
                                 measurementTimestamp,
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);

    bucket.rolloverReason = RolloverReason::kTimeBackward;
    AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
    auto potentialBuckets =
        findAndRolloverOpenBuckets(*_bucketCatalog,
                                   *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                   WithLock::withoutLock(),
                                   batchedInsertCtx.key,
                                   measurementTimestamp,
                                   Seconds(*batchedInsertCtx.options.getBucketMaxSpanSeconds()),
                                   allowQueryBasedReopening,
                                   bucketOpenedDueToMetadata);
    ASSERT(!bucketOpenedDueToMetadata);
    ASSERT_EQ(1, potentialBuckets.size());
    ASSERT_EQ(&bucket, potentialBuckets[0]);
    ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kAllow);
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsHardClose) {
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
    std::vector<RolloverReason> allHardClosedRolloverReasons = {RolloverReason::kCount,
                                                                RolloverReason::kSchemaChange,
                                                                RolloverReason::kCachePressure,
                                                                RolloverReason::kSize};

    for (size_t i = 0; i < allHardClosedRolloverReasons.size(); i++) {
        auto bucketOpenedDueToMetadata = true;
        Bucket& bucket =
            internal::allocateBucket(*_bucketCatalog,
                                     *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                     WithLock::withoutLock(),
                                     batchedInsertCtx.key,
                                     batchedInsertCtx.options,
                                     measurementTimestamp,
                                     _stringDataComparatorUnused,
                                     batchedInsertCtx.stats);

        bucket.rolloverReason = allHardClosedRolloverReasons[i];
        AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
        auto potentialBuckets =
            findAndRolloverOpenBuckets(*_bucketCatalog,
                                       *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                       WithLock::withoutLock(),
                                       batchedInsertCtx.key,
                                       measurementTimestamp,
                                       Seconds(*batchedInsertCtx.options.getBucketMaxSpanSeconds()),
                                       allowQueryBasedReopening,
                                       bucketOpenedDueToMetadata);
        ASSERT(!bucketOpenedDueToMetadata);
        ASSERT_EQ(0, potentialBuckets.size());
        ASSERT(_bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsByKey.empty());
        ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kDisallow);
    }
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsUncommitted) {
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
    std::vector<RolloverReason> allHardClosedRolloverReasons = {RolloverReason::kCount,
                                                                RolloverReason::kSchemaChange,
                                                                RolloverReason::kCachePressure,
                                                                RolloverReason::kSize};

    for (size_t i = 0; i < allHardClosedRolloverReasons.size(); i++) {
        auto bucketOpenedDueToMetadata = true;
        Bucket& bucket =
            internal::allocateBucket(*_bucketCatalog,
                                     *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                     WithLock::withoutLock(),
                                     batchedInsertCtx.key,
                                     batchedInsertCtx.options,
                                     measurementTimestamp,
                                     _stringDataComparatorUnused,
                                     batchedInsertCtx.stats);

        bucket.rolloverReason = allHardClosedRolloverReasons[i];
        std::shared_ptr<WriteBatch> batch;
        auto opId = 0;
        bucket.batches.emplace(opId, batch);
        AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
        auto potentialBuckets =
            findAndRolloverOpenBuckets(*_bucketCatalog,
                                       *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                       WithLock::withoutLock(),
                                       batchedInsertCtx.key,
                                       measurementTimestamp,
                                       Seconds(*batchedInsertCtx.options.getBucketMaxSpanSeconds()),
                                       allowQueryBasedReopening,
                                       bucketOpenedDueToMetadata);

        // No results returned. Do not close the bucket because of uncommitted batches.
        ASSERT(!bucketOpenedDueToMetadata);
        ASSERT_EQ(0, potentialBuckets.size());
        ASSERT(!_bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsByKey.empty());
        ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kDisallow);
    }
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsOrder) {
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
    auto bucketOpenedDueToMetadata = true;

    Bucket& bucket1 =
        internal::allocateBucket(*_bucketCatalog,
                                 *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                 WithLock::withoutLock(),
                                 batchedInsertCtx.key,
                                 batchedInsertCtx.options,
                                 measurementTimestamp,
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);
    bucket1.rolloverReason = RolloverReason::kTimeBackward;

    Bucket& bucket2 =
        internal::allocateBucket(*_bucketCatalog,
                                 *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                 WithLock::withoutLock(),
                                 batchedInsertCtx.key,
                                 batchedInsertCtx.options,
                                 measurementTimestamp,
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);

    AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
    auto potentialBuckets =
        findAndRolloverOpenBuckets(*_bucketCatalog,
                                   *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                   WithLock::withoutLock(),
                                   batchedInsertCtx.key,
                                   measurementTimestamp,
                                   Seconds(*batchedInsertCtx.options.getBucketMaxSpanSeconds()),
                                   allowQueryBasedReopening,
                                   bucketOpenedDueToMetadata);
    ASSERT(!bucketOpenedDueToMetadata);
    ASSERT_EQ(2, potentialBuckets.size());
    ASSERT_EQ(&bucket1, potentialBuckets[0]);
    ASSERT_EQ(&bucket2, potentialBuckets[1]);
    ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kAllow);
}

TEST_F(BucketCatalogTest, GetEligibleBucketAllocateBucket) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();
    auto measurement = BSON(_timeField << Date_t::now() << _metaField << _metaValue);
    auto timeseriesOptions = _getTimeseriesOptions(_ns1);
    std::vector<timeseries::bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts = bucket_catalog::buildBatchedInsertContexts(*_bucketCatalog,
                                                                            _uuid1,
                                                                            timeseriesOptions,
                                                                            {measurement},
                                                                            /*startIndex=*/0,
                                                                            /*numDocsToStage=*/1,
                                                                            /*docsToRetry=*/{},
                                                                            errorsAndIndices);
    ASSERT(errorsAndIndices.empty());

    auto batchedInsertCtx = batchedInsertContexts[0];
    auto measurementTimestamp = std::get<1>(batchedInsertCtx.measurementsTimesAndIndices[0]);

    ASSERT(_bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsByKey.empty());
    ASSERT(_bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsById.empty());

    {
        stdx::unique_lock<stdx::mutex> stripeLock(
            _bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->mutex);
        bool bucketOpenedDueToMetadata = true;
        auto& bucket = getEligibleBucket(_opCtx,
                                         *_bucketCatalog,
                                         *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                         stripeLock,
                                         bucketsColl.get(),
                                         measurement,
                                         batchedInsertCtx.key,
                                         measurementTimestamp,
                                         batchedInsertCtx.options,
                                         bucketsColl->getDefaultCollator(),
                                         _storageCacheSizeBytes,
                                         _compressBucketFuncUnused,
                                         AllowQueryBasedReopening::kAllow,
                                         batchedInsertCtx.stats,
                                         bucketOpenedDueToMetadata);
        ASSERT_EQ(0, bucket.size);
        ASSERT_EQ(RolloverReason::kNone, bucket.rolloverReason);
        ASSERT_EQ(1,
                  _bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsByKey.size());
        ASSERT_EQ(1,
                  _bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsById.size());
        ASSERT(bucketOpenedDueToMetadata);
    }
}

TEST_F(BucketCatalogTest, GetEligibleBucketOpenBucket) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();
    auto measurement = BSON(_timeField << Date_t::now() << _metaField << _metaValue);
    auto timeseriesOptions = _getTimeseriesOptions(_ns1);
    std::vector<timeseries::bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts = bucket_catalog::buildBatchedInsertContexts(*_bucketCatalog,
                                                                            _uuid1,
                                                                            timeseriesOptions,
                                                                            {measurement},
                                                                            /*startIndex=*/0,
                                                                            /*numDocsToStage=*/1,
                                                                            /*docsToRetry=*/{},
                                                                            errorsAndIndices);
    ASSERT(errorsAndIndices.empty());

    auto batchedInsertCtx = batchedInsertContexts[0];
    auto measurementTimestamp = std::get<1>(batchedInsertCtx.measurementsTimesAndIndices[0]);

    Bucket& bucketAllocated =
        internal::allocateBucket(*_bucketCatalog,
                                 *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                 WithLock::withoutLock(),
                                 batchedInsertCtx.key,
                                 batchedInsertCtx.options,
                                 measurementTimestamp,
                                 _stringDataComparatorUnused,
                                 batchedInsertCtx.stats);

    ASSERT_EQ(1, _bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsByKey.size());
    ASSERT_EQ(1, _bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsById.size());

    {
        stdx::unique_lock<stdx::mutex> stripeLock(
            _bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->mutex);
        bool bucketOpenedDueToMetadata = true;
        auto& bucketFound =
            getEligibleBucket(_opCtx,
                              *_bucketCatalog,
                              *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                              stripeLock,
                              bucketsColl.get(),
                              measurement,
                              batchedInsertCtx.key,
                              measurementTimestamp,
                              batchedInsertCtx.options,
                              bucketsColl->getDefaultCollator(),
                              _storageCacheSizeBytes,
                              _compressBucketFuncUnused,
                              AllowQueryBasedReopening::kAllow,
                              batchedInsertCtx.stats,
                              bucketOpenedDueToMetadata);
        ASSERT_EQ(&bucketAllocated, &bucketFound);
        ASSERT_EQ(RolloverReason::kNone, bucketFound.rolloverReason);
        ASSERT_EQ(1,
                  _bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsByKey.size());
        ASSERT_EQ(1,
                  _bucketCatalog->stripes[batchedInsertCtx.stripeNumber]->openBucketsById.size());
        ASSERT(!bucketOpenedDueToMetadata);
    }
}


TEST_F(BucketCatalogTest, BuildBatchedInsertContextsOneBatchNoMetafield) {
    std::vector<BSONObj> userMeasurementsBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(5) << "x" << 1),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4) << "x" << 2),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2) << "x" << 3),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3) << "x" << 4),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1) << "x" << 5)};

    std::vector<size_t> correctIndexOrder{4, 2, 3, 1, 0};
    stdx::unordered_set<size_t> expectedIndicesWithErrors;

    // Test in a collection with a meta field.
    _testBuildBatchedInsertContextWithoutMetaFieldInCollWithMetaField(
        _ns1, userMeasurementsBatch, correctIndexOrder, expectedIndicesWithErrors);

    // Test in a collection without a meta field.
    _testBuildBatchedInsertContextWithoutMetaFieldInCollWithoutMetaField(
        _nsNoMeta, userMeasurementsBatch, correctIndexOrder, expectedIndicesWithErrors);
}

TEST_F(BucketCatalogTest, BuildBatchedInsertContextsOneBatchWithMetafield) {
    // Test with all possible BSONTypes.
    std::vector<BSONType> allBSONTypes = _getFlattenedVector(std::vector<std::vector<BSONType>>{
        _stringComponentBSONTypes, _nonStringComponentVariableBSONTypes, _constantBSONTypes});
    for (BSONType type : allBSONTypes) {
        _testBuildBatchedInsertContextOneBatchWithSameMetaFieldType(type);
    }
}

TEST_F(BucketCatalogTest, BuildBatchedInsertContextsMultipleBatchesWithMetafield) {
    // Test with BSONTypes that aren't constant (so we create distinct batches).
    // BSONTypes that have a StringData component.
    for (BSONType type : _stringComponentBSONTypes) {
        _testBuildBatchedInsertContextMultipleBatchesWithSameMetaFieldType(
            type, std::vector<StringData>{_metaValue, _metaValue2, _metaValue3});
    }

    // Test with BSONTypes that don't have a StringData type metaValue.
    _testBuildBatchedInsertContextMultipleBatchesWithSameMetaFieldType(
        BSONType::timestamp,
        std::vector<Timestamp>{Timestamp(1, 2), Timestamp(2, 3), Timestamp(3, 4)});
    StatusWith<Date_t> date1 = dateFromISOString("2022-06-06T15:34:00.000Z");
    StatusWith<Date_t> date2 = dateFromISOString("2022-06-06T16:34:00.000Z");
    StatusWith<Date_t> date3 = dateFromISOString("2022-06-06T17:34:00.000Z");
    ASSERT(date1.isOK() && date2.isOK() && date3.isOK());
    _testBuildBatchedInsertContextMultipleBatchesWithSameMetaFieldType(
        BSONType::date, std::vector<Date_t>{date1.getValue(), date2.getValue(), date3.getValue()});
    _testBuildBatchedInsertContextMultipleBatchesWithSameMetaFieldType(
        BSONType::numberInt, std::vector<int>{365, 10, 4});
    _testBuildBatchedInsertContextMultipleBatchesWithSameMetaFieldType(
        BSONType::numberLong,
        std::vector<long long>{0x0123456789aacdeff, 0x0fedcba987654321, 0x0123456789abcdefll});
    _testBuildBatchedInsertContextMultipleBatchesWithSameMetaFieldType(
        BSONType::numberDecimal,
        std::vector<Decimal128>{Decimal128("0.490"), Decimal128("0.30"), Decimal128("1.50")});
    _testBuildBatchedInsertContextMultipleBatchesWithSameMetaFieldType(
        BSONType::numberDouble, std::vector<double>{1.5, 1.4, 1.3});
    _testBuildBatchedInsertContextMultipleBatchesWithSameMetaFieldType(
        BSONType::oid,
        std::vector<OID>{OID("00000000ff00000000000002"),
                         OID("000000000000000000000002"),
                         OID("0000000000fff00000000002")});
    // We don't include bool because it only has two distinct meta values when this test requires 3.
    // We don't include BinDataType because we don't have a constructor for
    // std::vector<BinDataType>.
    // We make up for this missing coverage for both BinDataType and Bool in the
    // BuildBatchedInsertContextsMultipleBatchesWithDifferentMetafieldTypes1/2 unit tests.
}

TEST_F(BucketCatalogTest, BuildBatchedInsertContextsMultipleBatchesWithDifferentMetafieldTypes1) {
    std::vector<BSONObj> userMeasurementsBatch{
        _generateMeasurement(BSONType::oid, Date_t::fromMillisSinceEpoch(113)).obj(),
        _generateMeasurement(BSONType::code, Date_t::fromMillisSinceEpoch(105)).obj(),
        _generateMeasurement(BSONType::code, Date_t::fromMillisSinceEpoch(107)).obj(),
        _generateMeasurement(
            BSON(_metaField << BSONDBRef(_metaValue2, OID("dbdbdbdbdbdbdbdbdbdbdbdb"))),
            Date_t::fromMillisSinceEpoch(103))
            .obj(),
        _generateMeasurement(BSONType::oid, Date_t::fromMillisSinceEpoch(104)).obj(),
        _generateMeasurement(BSON(_metaField << _metaValue3), Date_t::fromMillisSinceEpoch(102))
            .obj(),
        _generateMeasurement(BSONType::eoo, Date_t::fromMillisSinceEpoch(109)).obj(),
        _generateMeasurement(BSONType::string, Date_t::fromMillisSinceEpoch(108)).obj(),
        _generateMeasurement(BSONType::string, Date_t::fromMillisSinceEpoch(111)).obj(),
        _generateMeasurement(BSONType::binData, Date_t::fromMillisSinceEpoch(204)).obj(),
        _generateMeasurement(BSON(_metaField << BSONCode(_metaValue2)),
                             Date_t::fromMillisSinceEpoch(200))
            .obj(),
        _generateMeasurement(BSON(_metaField << _metaValue3), Date_t::fromMillisSinceEpoch(101))
            .obj(),
        _generateMeasurement(BSON(_metaField << _metaValue3), Date_t::fromMillisSinceEpoch(121))
            .obj(),
        _generateMeasurement(BSONType::string, Date_t::fromMillisSinceEpoch(65)).obj(),
        _generateMeasurement(
            BSON(_metaField << BSONDBRef(_metaValue2, OID("dbdbdbdbdbdbdbdbdbdbdbdb"))),
            Date_t::fromMillisSinceEpoch(400))
            .obj(),
        _generateMeasurement(BSONType::minKey, Date_t::fromMillisSinceEpoch(250)).obj(),
        _generateMeasurement(BSONType::eoo, Date_t::fromMillisSinceEpoch(108)).obj(),
        _generateMeasurement(BSONType::maxKey, Date_t::fromMillisSinceEpoch(231)).obj(),
        _generateMeasurement(BSONType::eoo, Date_t::fromMillisSinceEpoch(107)).obj(),
        _generateMeasurement(BSON(_metaField << BSONBinData("", 1, BinDataGeneral)),
                             Date_t::fromMillisSinceEpoch(204))
            .obj(),
    };
    stdx::unordered_map<bucket_catalog::BucketMetadata, std::vector<size_t>>
        metaFieldMetadataToCorrectIndexOrderMap;
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[0])), /* for jstOID */
        std::initializer_list<size_t>{4, 0});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[1])), /* for Code (_metaValue) */
        std::initializer_list<size_t>{1, 2});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[10])), /* for Code (_metaValue2) */
        std::initializer_list<size_t>{10});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[6])), /* for EOO */
        std::initializer_list<size_t>{18, 16, 6});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[3])), /* for DBRef (_metaValue2) */
        std::initializer_list<size_t>{3, 14});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[7])), /* for String (_metaValue) */
        std::initializer_list<size_t>{13, 7, 8});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[5])), /* for String (_metaValue3) */
        std::initializer_list<size_t>{11, 5, 12});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[9])), /* for BinData (0) */
        std::initializer_list<size_t>{9});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[19])), /* for BinData (1) */
        std::initializer_list<size_t>{19});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[15])), /* for MinKey */
        std::initializer_list<size_t>{15});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[17])), /* for MaxKey */
        std::initializer_list<size_t>{17});
    stdx::unordered_set<size_t> expectedIndicesWithErrors;
    _testBuildBatchedInsertContextWithMetaField(
        userMeasurementsBatch, metaFieldMetadataToCorrectIndexOrderMap, expectedIndicesWithErrors);
}

TEST_F(BucketCatalogTest, BuildBatchedInsertContextsMultipleBatchesWithDifferentMetafieldTypes2) {
    std::vector<BSONObj> userMeasurementsBatch{
        _generateMeasurement(BSON(_metaField << BSONSymbol(_metaValue2)),
                             Date_t::fromMillisSinceEpoch(382))
            .obj(),
        _generateMeasurement(BSON(_metaField << BSONCodeWScope(_metaValue2, BSON("x" << 1))),
                             Date_t::fromMillisSinceEpoch(493))
            .obj(),
        _generateMeasurement(BSONType::object, Date_t::fromMillisSinceEpoch(212)).obj(),
        _generateMeasurement(BSON(_metaField << BSONSymbol(_metaValue2)),
                             Date_t::fromMillisSinceEpoch(284))
            .obj(),
        _generateMeasurement(BSON(_metaField << BSONCodeWScope(_metaValue2, BSON("x" << 1))),
                             Date_t::fromMillisSinceEpoch(958))
            .obj(),
        _generateMeasurement(BSONType::object, Date_t::fromMillisSinceEpoch(103)).obj(),
        _generateMeasurement(BSON(_metaField << BSON("0" << _metaValue2)),
                             Date_t::fromMillisSinceEpoch(492))
            .obj(),
        _generateMeasurement(BSONType::object, Date_t::fromMillisSinceEpoch(365)).obj(),
        _generateMeasurement(BSONType::null, Date_t::fromMillisSinceEpoch(590)).obj(),
        _generateMeasurement(BSONType::array, Date_t::fromMillisSinceEpoch(204)).obj(),
        _generateMeasurement(BSONType::null, Date_t::fromMillisSinceEpoch(58)).obj(),
        _generateMeasurement(BSON(_metaField << BSONCodeWScope(_metaValue3, BSON("x" << 1))),
                             Date_t::fromMillisSinceEpoch(93))
            .obj(),
        _generateMeasurement(BSON(_metaField << BSONCodeWScope(_metaValue3, BSON("x" << 1))),
                             Date_t::fromMillisSinceEpoch(304))
            .obj(),
        _generateMeasurement(BSONType::null, Date_t::fromMillisSinceEpoch(384)).obj(),
        _generateMeasurement(BSONType::codeWScope, Date_t::fromMillisSinceEpoch(888)).obj(),
        _generateMeasurement(BSONType::array, Date_t::fromMillisSinceEpoch(764)).obj(),
        _generateMeasurement(BSONType::array, Date_t::fromMillisSinceEpoch(593)).obj(),
    };

    stdx::unordered_map<bucket_catalog::BucketMetadata, std::vector<size_t>>
        metaFieldMetadataToCorrectIndexOrderMap;
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[0])), /* for Symbol */
        std::initializer_list<size_t>{3, 0});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[1])), /* for CodeWScope (_metaValue2) */
        std::initializer_list<size_t>{1, 4});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[14])), /* for CodeWScope (_metaValue) */
        std::initializer_list<size_t>{14});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[11])), /* for CodeWScope (_metaValue3) */
        std::initializer_list<size_t>{11, 12});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[2])), /* for Object (_metaValue) */
        std::initializer_list<size_t>{5, 2, 7});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[6])), /* for Object (_metaValue2) */
        std::initializer_list<size_t>{6});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[8])), /* for jstNULL */
        std::initializer_list<size_t>{10, 13, 8});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[9])), /* for Array */
        std::initializer_list<size_t>{9, 16, 15});
    stdx::unordered_set<size_t> expectedIndicesWithErrors;
    _testBuildBatchedInsertContextWithMetaField(
        userMeasurementsBatch, metaFieldMetadataToCorrectIndexOrderMap, expectedIndicesWithErrors);
}

TEST_F(BucketCatalogTest, BuildBatchedInsertContextsMultipleBatchesWithDifferentMetafieldTypes3) {
    std::vector<BSONObj> userMeasurementsBatch{
        _generateMeasurement(BSONType::date, Date_t::fromMillisSinceEpoch(113)).obj(),
        _generateMeasurement(BSONType::regEx, Date_t::fromMillisSinceEpoch(105)).obj(),
        _generateMeasurement(BSONType::regEx, Date_t::fromMillisSinceEpoch(107)).obj(),
        _generateMeasurement(BSONType::undefined, Date_t::fromMillisSinceEpoch(103)).obj(),
        _generateMeasurement(BSON(_metaField << true), Date_t::fromMillisSinceEpoch(104)).obj(),
        _generateMeasurement(BSON(_metaField << true), Date_t::fromMillisSinceEpoch(102)).obj(),
        _generateMeasurement(BSON(_metaField << false), Date_t::fromMillisSinceEpoch(104)).obj(),
        _generateMeasurement(BSON(_metaField << false), Date_t::fromMillisSinceEpoch(102)).obj(),
        _generateMeasurement(BSONType::undefined, Date_t::fromMillisSinceEpoch(102)).obj(),
        _generateMeasurement(BSONType::eoo, Date_t::fromMillisSinceEpoch(109)).obj(),
        _generateMeasurement(BSONType::numberInt, Date_t::fromMillisSinceEpoch(108)).obj(),
        _generateMeasurement(BSONType::numberInt, Date_t::fromMillisSinceEpoch(111)).obj(),
        _generateMeasurement(BSON(_metaField << 2.3), Date_t::fromMillisSinceEpoch(204)).obj(),
        _generateMeasurement(BSONType::numberLong, Date_t::fromMillisSinceEpoch(200)).obj(),
        _generateMeasurement(BSON(_metaField << 2.1), Date_t::fromMillisSinceEpoch(101)).obj(),
        _generateMeasurement(BSON(_metaField << 2.3), Date_t::fromMillisSinceEpoch(121)).obj(),
        _generateMeasurement(BSONType::date, Date_t::fromMillisSinceEpoch(65)).obj(),
        _generateMeasurement(BSON(_metaField << Decimal128("0.4")),
                             Date_t::fromMillisSinceEpoch(400))
            .obj(),
        _generateMeasurement(BSON(_metaField << Decimal128("0.3")),
                             Date_t::fromMillisSinceEpoch(400))
            .obj(),
        _generateMeasurement(BSONType::regEx, Date_t::fromMillisSinceEpoch(108)).obj(),
        _generateMeasurement(BSONType::timestamp, Date_t::fromMillisSinceEpoch(231)).obj(),
        _generateMeasurement(BSONType::eoo, Date_t::fromMillisSinceEpoch(107)).obj(),
    };
    stdx::unordered_map<bucket_catalog::BucketMetadata, std::vector<size_t>>
        metaFieldMetadataToCorrectIndexOrderMap;
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[0])), /* for Date */
        std::initializer_list<size_t>{16, 0});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[1])), /* for RegEx */
        std::initializer_list<size_t>{1, 2, 19});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[3])), /* for Undefined  */
        std::initializer_list<size_t>{8, 3});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[4])), /* for Bool (true) */
        std::initializer_list<size_t>{5, 4});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[6])), /* for Bool (false) */
        std::initializer_list<size_t>{7, 6});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[9])), /* for EOO  */
        std::initializer_list<size_t>{21, 9});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[10])), /* for NumberInt */
        std::initializer_list<size_t>{10, 11});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[12])), /* for NumberDouble (2.3) */
        std::initializer_list<size_t>{15, 12});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[13])), /* for NumberLong */
        std::initializer_list<size_t>{13});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[14])), /* for NumberDouble (2.1) */
        std::initializer_list<size_t>{14});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[17])), /* for NumberDecimal (0.4) */
        std::initializer_list<size_t>{17});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[18])), /* for NumberDecimal (0.3) */
        std::initializer_list<size_t>{18});
    metaFieldMetadataToCorrectIndexOrderMap.try_emplace(
        *(getBucketMetadata(userMeasurementsBatch[20])), /* for bsonTimestamp */
        std::initializer_list<size_t>{20});
    stdx::unordered_set<size_t> expectedIndicesWithErrors;
    _testBuildBatchedInsertContextWithMetaField(
        userMeasurementsBatch, metaFieldMetadataToCorrectIndexOrderMap, expectedIndicesWithErrors);
}

TEST_F(BucketCatalogTest, BuildBatchedInsertContextsNoMetaReportsMalformedMeasurements) {
    std::vector<BSONObj> userMeasurementsBatch{
        BSON("x" << 1),  // Malformed measurement, missing time field
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4) << "x" << 2),
        BSON("x" << 3),  // Malformed measurement, missing time field
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3) << "x" << 4),
        BSON("x" << 5),  // Malformed measurement, missing time field
    };
    std::vector<size_t> correctIndexOrder{3, 1};
    stdx::unordered_set<size_t> expectedIndicesWithErrors{0, 2, 4};

    // Test in a collection with a meta field.
    _testBuildBatchedInsertContextWithoutMetaFieldInCollWithMetaField(
        _ns1, userMeasurementsBatch, correctIndexOrder, expectedIndicesWithErrors);

    // Test in a collection without a meta field.
    _testBuildBatchedInsertContextWithoutMetaFieldInCollWithoutMetaField(
        _nsNoMeta, userMeasurementsBatch, correctIndexOrder, expectedIndicesWithErrors);
}

TEST_F(BucketCatalogTest,
       BuildBatchedInsertContextsWithConstantBSONTypeMetaReportsMalformedMeasurements) {
    for (BSONType type : _constantBSONTypes) {
        std::vector<BSONObj> emptyMetaValues{};
        _testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(type, emptyMetaValues);
    }
}

TEST_F(BucketCatalogTest,
       BuildBatchedInsertContextsWithNonConstantBSONTypeMetaReportsMalformedMeasurements) {
    // For non-string component meta field types, we directly have to declare two meta values.
    _testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(
        BSONType::timestamp,
        std::vector<BSONObj>{BSON(_metaField << Timestamp(1, 2)),
                             BSON(_metaField << Timestamp(2, 3))});
    _testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(
        BSONType::numberInt, std::vector<BSONObj>{BSON(_metaField << 1), BSON(_metaField << 2)});
    _testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(
        BSONType::numberLong,
        std::vector<BSONObj>{BSON(_metaField << 0x0123456789abcdefll),
                             BSON(_metaField << 0x0123456789abcdeell)});
    _testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(
        BSONType::numberDecimal,
        std::vector<BSONObj>{BSON(_metaField << Decimal128("1.45")),
                             BSON(_metaField << Decimal128("0.987"))});
    _testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(
        BSONType::numberDouble,
        std::vector<BSONObj>{BSON(_metaField << 1.4), BSON(_metaField << 8.6)});
    _testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(
        BSONType::oid,
        std::vector<BSONObj>{BSON(_metaField << OID("649f0704230f18da067519c4")),
                             BSON(_metaField << OID("64a33d9cdf56a62781061048"))});
    _testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(
        BSONType::boolean,
        std::vector<BSONObj>{BSON(_metaField << true), BSON(_metaField << false)});
    _testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(
        BSONType::binData,
        std::vector<BSONObj>{BSON(_metaField << BSONBinData("", 0, BinDataGeneral)),
                             BSON(_metaField << BSONBinData("\x69\xb7", 2, BinDataGeneral))});

    // For string component meta field types, we can don't need to directly declare the values; we
    // can just use two different strings.
    for (BSONType type : _stringComponentBSONTypes) {
        std::vector<BSONObj> stringMetaValues{BSON(_metaField << _metaValue),
                                              BSON(_metaField << _metaValue2)};
        _testBuildBatchedInsertContextWithMalformedMeasurementsWithMetaField(type,
                                                                             stringMetaValues);
    }
}

TEST_F(BucketCatalogTest, BuildBatchedInsertContextsAllMeasurementsErrorNoMeta) {
    std::vector<BSONObj> userMeasurementsBatch{
        BSON("x" << 2),  // Malformed measurement, missing time field
        BSON("x" << 3),  // Malformed measurement, missing time field
    };
    std::vector<size_t> correctIndexOrder;
    stdx::unordered_set<size_t> expectedIndicesWithErrors{0, 1};

    // Test in a collection with a meta field.
    _testBuildBatchedInsertContextWithoutMetaFieldInCollWithMetaField(
        _ns1, userMeasurementsBatch, correctIndexOrder, expectedIndicesWithErrors);

    // Test in a collection without a meta field.
    _testBuildBatchedInsertContextWithoutMetaFieldInCollWithoutMetaField(
        _nsNoMeta, userMeasurementsBatch, correctIndexOrder, expectedIndicesWithErrors);
};

TEST_F(BucketCatalogTest, BuildBatchedInsertContextsAllMeasurementsErrorWithMeta) {
    std::vector<BSONObj> userMeasurementsBatch{
        BSON(_metaField << _metaValue << "x" << 2),  // Malformed measurement, missing time field
        BSON(_metaField << _metaValue << "x" << 3),  // Malformed measurement, missing time field
    };
    stdx::unordered_map<bucket_catalog::BucketMetadata, std::vector<size_t>>
        metaFieldMetadataToCorrectIndexOrderMap;
    stdx::unordered_set<size_t> expectedIndicesWithErrors{0, 1};
    _testBuildBatchedInsertContextWithMetaField(
        userMeasurementsBatch, metaFieldMetadataToCorrectIndexOrderMap, expectedIndicesWithErrors);
};

TEST_F(BucketCatalogTest, StageInsertBatchFillsUpSingleBucketWithMetaField) {
    std::vector<BSONObj> batchOfMeasurementsTimeseriesBucketMaxCount =
        _generateMeasurementsWithRolloverReason({.reason = bucket_catalog::RolloverReason::kNone});
    std::vector<size_t> numWriteBatches{1};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchWithMetaField(
        _ns1, _uuid1, batchOfMeasurementsTimeseriesBucketMaxCount, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchFillsUpSingleBucketWithoutMetaField) {
    std::vector<BSONObj> batchOfMeasurementsTimeseriesBucketMaxCountNoMetaField =
        _generateMeasurementsWithRolloverReason(
            {.reason = bucket_catalog::RolloverReason::kNone, .metaValueType = boost::none});
    std::vector<size_t> numWriteBatches{1};

    // Inserting a batch of measurements without meta field values into a collection without a
    // meta field.
    _testStageInsertBatchInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsTimeseriesBucketMaxCountNoMetaField,
        numWriteBatches);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchWithoutMetaFieldInCollWithMetaField(
        _ns1, _uuid1, batchOfMeasurementsTimeseriesBucketMaxCountNoMetaField, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverReasonCountWithMetaField) {
    auto batchOfMeasurementsWithCount = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kCount,
         .numMeasurements = static_cast<size_t>(2 * gTimeseriesBucketMaxCount)});

    // batchOfMeasurements will be 2 * gTimeseriesBucketMaxCount measurements with all the
    // measurements having the same meta field and time, which means we should have two buckets.
    std::vector<size_t> numWriteBatches{2};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchWithMetaField(_ns1, _uuid1, batchOfMeasurementsWithCount, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverReasonCountWithoutMetaField) {
    auto batchOfMeasurementsWithCountNoMetaField = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kCount,
         .numMeasurements = static_cast<size_t>(2 * gTimeseriesBucketMaxCount),
         .metaValueType = boost::none});

    // batchOfMeasurements will be 2 * gTimeseriesBucketMaxCount measurements with all the
    // measurements having the same meta field and time, which means we should have two buckets.
    std::vector<size_t> numWriteBatches{2};

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchInCollWithoutMetaField(
        _nsNoMeta, _uuidNoMeta, batchOfMeasurementsWithCountNoMetaField, numWriteBatches);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchWithoutMetaFieldInCollWithMetaField(
        _ns1, _uuid1, batchOfMeasurementsWithCountNoMetaField, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverReasonTimeForwardWithMetaField) {
    // Max bucket size with only the last measurement having kTimeForward.
    auto batchOfMeasurementsWithTimeForwardAtEnd = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kTimeForward});

    // The last measurement in batchOfMeasurementsWithTimeForwardAtEnd will have a timestamp outside
    // of the bucket range encompassing the previous measurements, which means that this
    // measurement will be in a different bucket.
    std::vector<size_t> numWriteBatches{2};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchWithMetaField(
        _ns1, _uuid1, batchOfMeasurementsWithTimeForwardAtEnd, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverReasonTimeForwardWithoutMetaField) {
    // Max bucket size with measurements[1:gTimeseriesBucketMaxCount] having kTimeForward.
    auto batchOfMeasurementsWithTimeForwardAfterFirstMeasurementNoMetaField =
        _generateMeasurementsWithRolloverReason(
            {.reason = bucket_catalog::RolloverReason::kTimeForward,
             .idxWithDiffMeasurement = 1,
             .metaValueType = boost::none});

    // The last measurement in batchOfMeasurementsWithTimeForwardAtEnd will have a timestamp outside
    // of the bucket range encompassing the previous measurements, which means that this
    // measurement will be in a different bucket.
    std::vector<size_t> numWriteBatches{2};

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithTimeForwardAfterFirstMeasurementNoMetaField,
        numWriteBatches);

    // 50 measurements with measurements[25:50] having kTimeForward.
    auto batchOfMeasurementsWithTimeForwardInMiddle = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kTimeForward,
         .numMeasurements = 50,
         .idxWithDiffMeasurement = 25,
         .metaValueType = boost::none});

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchWithoutMetaFieldInCollWithMetaField(
        _ns1, _uuid1, batchOfMeasurementsWithTimeForwardInMiddle, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverReasonSchemaChangeWithMetaField) {
    // Testing fix of mixed schema detection when using large measurements
    auto batchOfMeasurementsWithSchemaChangeAtEnd = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kSchemaChange,
         .numMeasurements = 3,      // staying below bucket mincount to keep bucket open
         .extraPayload = 130000});  // exceeding bucket size to exercise large path

    // The last measurement in batchOfMeasurementsWithSchemaChangeAtEnd will have a int rather than
    // a string for field "deathGrips", which means this measurement will be in a different
    // bucket.
    std::vector<size_t> numWriteBatches{2};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchWithMetaField(
        _ns1, _uuid1, batchOfMeasurementsWithSchemaChangeAtEnd, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchChecksSchemaChangeWithLargeMeasurements) {
    // Max bucket size with only the last measurement having kSchemaChange.
    auto batchOfMeasurementsWithSchemaChangeAtEnd = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kSchemaChange});

    // The last measurement in batchOfMeasurementsWithSchemaChangeAtEnd will have a int rather than
    // a string for field "deathGrips", which means this measurement will be in a different
    // bucket.
    std::vector<size_t> numWriteBatches{2};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchWithMetaField(
        _ns1, _uuid1, batchOfMeasurementsWithSchemaChangeAtEnd, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverReasonSchemaChangeWithoutMetaField) {
    // Max bucket size with measurements[1:gTimeseriesBucketMaxCount] having kSchemaChange.
    auto batchOfMeasurementsWithSchemaChangeAfterFirstMeasurementNoMetaField =
        _generateMeasurementsWithRolloverReason(
            {.reason = bucket_catalog::RolloverReason::kSchemaChange,
             .idxWithDiffMeasurement = 1,
             .metaValueType = boost::none});

    // The last measurement in batchOfMeasurementsWithSchemaChangeAtEnd will have a int rather than
    // a string for field "deathGrips", which means this measurement will be in a different
    // bucket.
    std::vector<size_t> numWriteBatches{2};

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithSchemaChangeAfterFirstMeasurementNoMetaField,
        numWriteBatches);

    // 50 measurements with measurements[25:50] having kSchemaChange.
    auto batchOfMeasurementsWithTimeForwardInMiddle = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kSchemaChange,
         .numMeasurements = 50,
         .idxWithDiffMeasurement = 25,
         .metaValueType = boost::none});

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchWithoutMetaFieldInCollWithMetaField(
        _ns1, _uuid1, batchOfMeasurementsWithTimeForwardInMiddle, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverReasonSizeWithMetaField) {
    auto batchOfMeasurementsWithSize =
        _generateMeasurementsWithRolloverReason({.reason = bucket_catalog::RolloverReason::kSize});

    // The last measurement will exceed the size that the bucket can store, which will mean it
    // should be in a different bucket.
    std::vector<size_t> numWriteBatches{2};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchWithMetaField(_ns1, _uuid1, batchOfMeasurementsWithSize, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverReasonSizeWithoutMetaField) {
    auto batchOfMeasurementsWithSizeNoMetaField = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kSize, .metaValueType = boost::none});

    // The last measurement will exceed the size that the bucket can store, which will mean it
    // should be in a different bucket.
    std::vector<size_t> numWriteBatches{2};

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchInCollWithoutMetaField(
        _nsNoMeta, _uuidNoMeta, batchOfMeasurementsWithSizeNoMetaField, numWriteBatches);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchWithoutMetaFieldInCollWithMetaField(
        _ns1, _uuid1, batchOfMeasurementsWithSizeNoMetaField, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverReasonCachePressureWithMetaField) {
    // Artificially lower _storageCacheSizeBytes so we can simulate kCachePressure.
    _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes;

    std::vector<BSONObj> batchOfMeasurementsWithCachePressure =
        _generateMeasurementsWithRolloverReason(
            {.reason = bucket_catalog::RolloverReason::kCachePressure});

    // The last measurement will exceed the size that the bucket can store. Coupled with the lowered
    // cache size, we will trigger kCachePressure, so the measurement will be in a different bucket.
    std::vector<size_t> numWriteBatches{2};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchWithMetaField(
        _ns1, _uuid1, batchOfMeasurementsWithCachePressure, numWriteBatches);

    // Reset _storageCacheSizeBytes back to a representative value.
    _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverReasonCachePressureWithoutMetaField) {
    // Artificially lower _storageCacheSizeBytes so we can simulate kCachePressure.
    _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes;
    std::vector<BSONObj> batchOfMeasurementsWithCachePressureNoMetaField =
        _generateMeasurementsWithRolloverReason(
            {.reason = bucket_catalog::RolloverReason::kCachePressure,
             .metaValueType = boost::none});

    // The last measurement will exceed the size that the bucket can store. Coupled with the lowered
    // cache size, we will trigger kCachePressure, so the measurement will be in a different bucket.
    std::vector<size_t> numWriteBatches{2};

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchInCollWithoutMetaField(
        _nsNoMeta, _uuidNoMeta, batchOfMeasurementsWithCachePressureNoMetaField, numWriteBatches);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchWithoutMetaFieldInCollWithMetaField(
        _ns1, _uuid1, batchOfMeasurementsWithCachePressureNoMetaField, numWriteBatches);

    // Reset _storageCacheSizeBytes back to a representative value.
    _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverMixed) {
    std::vector<BSONObj> batchOfMeasurements =
        _generateMeasurementsWithRolloverReason({.reason = bucket_catalog::RolloverReason::kNone,
                                                 .numMeasurements = 50,
                                                 .timeValue = Date_t::now()});

    auto batchOfMeasurementsWithCount = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kCount,
         .numMeasurements = static_cast<size_t>(2 * gTimeseriesBucketMaxCount),
         .timeValue = Date_t::now() + Seconds(1)});

    auto batchOfMeasurementsWithTimeForward = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kTimeForward,
         .numMeasurements = 50,
         .idxWithDiffMeasurement = 25,
         .timeValue = Date_t::now() + Seconds(2)});

    std::vector<BSONObj> mixedRolloverReasonsMeasurements = _getFlattenedVector(
        std::vector<std::vector<BSONObj>>({batchOfMeasurements,
                                           batchOfMeasurementsWithCount,
                                           batchOfMeasurementsWithTimeForward}));
    ASSERT_EQ(mixedRolloverReasonsMeasurements.size(),
              batchOfMeasurements.size() + batchOfMeasurementsWithCount.size() +
                  batchOfMeasurementsWithTimeForward.size());

    // The first bucket will consist of all 50 measurements from batchOfMeasurements.
    // We will then insert gTimeseriesBucketMaxCount - 50 measurements from
    // batchOfMeasurementsWithCountA before rolling over due to kCount.
    // We will create a second bucket that will have the gTimeseriesBucketMaxCount measurements
    // from batchOfMeasurementsWithCount, and then roll over due to kCount.
    // We will then create a third bucket that will have the last 50 measurements from
    // batchOfMeasurementsWithCount.
    // We will then insert 25 measurements from batchOfMeasurementsWithTimeForward into the third
    // bucket before encountering kTimeForward and add the remaining 25 measurements into a fourth
    // bucket.
    std::vector<size_t> numWriteBatches{4};

    // Test in a collection without a meta field.
    _testStageInsertBatchInCollWithoutMetaField(
        _nsNoMeta, _uuidNoMeta, mixedRolloverReasonsMeasurements, numWriteBatches);

    // Test in a collection with a meta field.
    _testStageInsertBatchWithMetaField(
        _ns1, _uuid1, mixedRolloverReasonsMeasurements, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverMixedWithNoMeta) {
    auto batchOfMeasurementsWithSize =
        _generateMeasurementsWithRolloverReason({.reason = bucket_catalog::RolloverReason::kSize,
                                                 .timeValue = Date_t::now(),
                                                 .metaValueType = boost::none});

    std::vector<BSONObj> batchOfMeasurements =
        _generateMeasurementsWithRolloverReason({.reason = bucket_catalog::RolloverReason::kNone,
                                                 .numMeasurements = 50,
                                                 .timeValue = Date_t::now() + Seconds(1),
                                                 .metaValueType = boost::none});

    auto batchOfMeasurementsWithSchemaChange = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kSchemaChange,
         .timeValue = Date_t::now() + Seconds(2),
         .metaValueType = boost::none});

    std::vector<BSONObj> mixedRolloverReasonsMeasurements =
        _getFlattenedVector(std::vector<std::vector<BSONObj>>{
            batchOfMeasurementsWithSize, batchOfMeasurements, batchOfMeasurementsWithSchemaChange});
    ASSERT_EQ(mixedRolloverReasonsMeasurements.size(),
              batchOfMeasurementsWithSize.size() + batchOfMeasurements.size() +
                  batchOfMeasurementsWithSchemaChange.size());

    // The first bucket will consist of 124 measurements before we rollover due to kSize from
    // batchOfMeasurementsWithSize.
    // The second bucket will consist of one measurement from batchOfMeasurementsWithSize with
    // the 50 measurements from batchOfMeasurements. We will then insert
    // gTimeseriesBucketMaxCount - 50 - 1 measurements from batchOfMeasurementsWithSchemaChange
    // into this second bucket, and rollover due to kCount. The third bucket will be created and
    // we will insert the measurements
    // batchOfMeasurementsWithSchemaChange[gTimeseriesBucketMaxCount - 50 -
    // 1:gTimeseriesBucketMaxCount-1].
    // Finally, the fourth bucket created to insert the last measurement
    // batchOfMeasurementsWithSchemaChange due to kSchemaChange.
    std::vector<size_t> numWriteBatches{4};

    // Test in a collection without a meta field.
    _testStageInsertBatchInCollWithoutMetaField(
        _nsNoMeta, _uuidNoMeta, mixedRolloverReasonsMeasurements, numWriteBatches);

    // Test in collection with a meta field.
    _testStageInsertBatchWithoutMetaFieldInCollWithMetaField(
        _ns1, _uuid1, mixedRolloverReasonsMeasurements, numWriteBatches);
}

TEST_F(BucketCatalogTest, StageInsertBatchHandlesRolloverReasonMixedWithCachePressure) {
    // Artificially lower _storageCacheSizeBytes so we can simulate kCachePressure.
    _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes;

    auto batchOfMeasurements =
        _generateMeasurementsWithRolloverReason({.reason = bucket_catalog::RolloverReason::kNone,
                                                 .numMeasurements = 10,
                                                 .timeValue = Date_t::now()});

    std::vector<BSONObj> batchOfMeasurementsWithTimeForward =
        _generateMeasurementsWithRolloverReason(
            {.reason = bucket_catalog::RolloverReason::kTimeForward,
             .numMeasurements = 10,
             .idxWithDiffMeasurement = 5,
             .timeValue = Date_t::now() + Seconds(1)});

    auto batchOfMeasurementsWithCachePressure = _generateMeasurementsWithRolloverReason(
        {.reason = bucket_catalog::RolloverReason::kCachePressure,
         .timeValue = Date_t::now() + Seconds(2)});

    std::vector<BSONObj> mixedRolloverReasonsMeasurements = _getFlattenedVector(
        std::vector<std::vector<BSONObj>>{batchOfMeasurements,
                                          batchOfMeasurementsWithTimeForward,
                                          batchOfMeasurementsWithCachePressure});
    ASSERT_EQ(mixedRolloverReasonsMeasurements.size(),
              batchOfMeasurements.size() + batchOfMeasurementsWithTimeForward.size() +
                  batchOfMeasurementsWithCachePressure.size());

    // The first bucket will consist of 10 measurements from batchOfMeasurements. We will then
    // insert 5 measurements from batchOfMeasurementsWithTimeForward until we rollover due to
    // kTimeForward.
    // The second bucket will consist of the remaining 5 measurements from
    // batchOfMeasurementsWithTimeForward. We will then insert 4 measurements from
    // batchOfMeasurementsWithCachePressure into the second bucket and will rollover due to
    // kCachePressure.
    // We will insert the last measurement from batchOfMeasurementsWithCachePressure into a
    // third bucket.
    std::vector<size_t> numWriteBatches{3};

    // Test in a collection without a meta field.
    _testStageInsertBatchInCollWithoutMetaField(
        _nsNoMeta, _uuidNoMeta, mixedRolloverReasonsMeasurements, numWriteBatches);

    // Test in collection with a meta field.
    _testStageInsertBatchWithMetaField(
        _ns1, _uuid1, mixedRolloverReasonsMeasurements, numWriteBatches);

    // Reset _storageCacheSizeBytes back to a representative value.
    _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketFillsUpSingleBucketWithMetaField) {
    std::vector<BSONObj> batchOfMeasurementsTimeseriesBucketMaxCount =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kNone});
    std::vector<size_t> currBatchedInsertContextsIndex{0};
    std::vector<size_t> numMeasurementsInWriteBatch{static_cast<size_t>(gTimeseriesBucketMaxCount)};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsTimeseriesBucketMaxCount,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketFillsUpSingleBucketWithoutMetaField) {
    std::vector<BSONObj> batchOfMeasurementsTimeseriesBucketMaxCount =
        _generateMeasurementsWithRolloverReason(
            {.reason = RolloverReason::kNone, .metaValueType = boost::none});
    std::vector<size_t> curBatchedInsertContextsIndex{0};
    std::vector<size_t> numMeasurementsInWriteBatch{static_cast<size_t>(gTimeseriesBucketMaxCount)};

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsTimeseriesBucketMaxCount,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsTimeseriesBucketMaxCount,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverCountWithMetaField) {
    // batchOfMeasurements will be 2 * gTimeseriesBucketMaxCount measurements with all the
    // measurements having the same meta field and time, which means we should have two buckets.
    std::vector<BSONObj> batchOfMeasurementsWithCount = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kCount,
         .numMeasurements = static_cast<size_t>(2 * gTimeseriesBucketMaxCount)});
    std::vector<size_t> numMeasurementsInWriteBatch{static_cast<size_t>(gTimeseriesBucketMaxCount),
                                                    static_cast<size_t>(gTimeseriesBucketMaxCount)};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithCount,
                                                         curBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverCountWithoutMetaField) {
    // batchOfMeasurements will be 2 * gTimeseriesBucketMaxCount measurements with all the
    // measurements having the same meta field and time, which means we should have two buckets.
    std::vector<BSONObj> batchOfMeasurementsWithCount = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kCount,
         .numMeasurements = static_cast<size_t>(2 * gTimeseriesBucketMaxCount),
         .metaValueType = boost::none});
    std::vector<size_t> numMeasurementsInWriteBatch{static_cast<size_t>(gTimeseriesBucketMaxCount),
                                                    static_cast<size_t>(gTimeseriesBucketMaxCount)};
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(_nsNoMeta,
                                                                  _uuidNoMeta,
                                                                  batchOfMeasurementsWithCount,
                                                                  currBatchedInsertContextsIndex,
                                                                  numMeasurementsInWriteBatch,
                                                                  /*numBatchedInsertContexts=*/1);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithCount,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest,
       StageInsertBatchIntoEligibleBucketHandlesRolloverTimeForwardWithMetaField) {
    // The last measurement in batchOfMeasurementsWithTimeForwardAtEnd will have a timestamp outside
    // of the bucket range encompassing the previous measurements, which means that this
    // measurement will be in a different bucket.
    // Max bucket count with only the last measurement having kTimeForward.
    std::vector<BSONObj> batchOfMeasurementsWithTimeForwardAtEnd =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kTimeForward});

    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};
    std::vector<size_t> numMeasurementsInWriteBatchAtEnd{
        static_cast<size_t>(gTimeseriesBucketMaxCount - 1), 1};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithTimeForwardAtEnd,
                                                         currBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatchAtEnd,
                                                         /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest,
       StageInsertBatchIntoEligibleBucketHandlesRolloverTimeForwardWithoutMetaField) {
    // Max bucket size with measurements[1:gTimeseriesBucketMaxCount] having kTimeForward.
    auto batchOfMeasurementsWithTimeForwardAfterFirstMeasurement =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kTimeForward,
                                                 .idxWithDiffMeasurement = 1,
                                                 .metaValueType = boost::none});
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};
    std::vector<size_t> numMeasurementsInWriteBatchAfterFirstMeasurement{
        1, static_cast<size_t>(gTimeseriesBucketMaxCount - 1)};

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithTimeForwardAfterFirstMeasurement,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchAfterFirstMeasurement,
        /*numBatchedInsertContexts=*/1);

    // 50 measurements with measurements[25:50] having kTimeForward.
    std::vector<size_t> numMeasurementsInWriteBatchInMiddle{25, 25};
    auto batchOfMeasurementsWithTimeForwardInMiddle =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kTimeForward,
                                                 .numMeasurements = 50,
                                                 .idxWithDiffMeasurement = 25,
                                                 .metaValueType = boost::none});

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithTimeForwardInMiddle,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchInMiddle,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest,
       StageInsertBatchIntoEligibleBucketHandlesRolloverSchemaChangeWithMetaField) {
    // The last measurement in batchOfMeasurementsWithSchemaChangeAtEnd will have a int rather
    // than a string for field "deathGrips", which means this measurement will be in a different
    // bucket.
    // Max bucket count with only the last measurement having kSchemaChange.
    auto batchOfMeasurementsWithSchemaChangeAtEnd =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSchemaChange});

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};
    std::vector<size_t> numMeasurementsInWriteBatchAtEnd{
        static_cast<size_t>(gTimeseriesBucketMaxCount - 1), 1};
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithSchemaChangeAtEnd,
                                                         currBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatchAtEnd,
                                                         /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest,
       StageInsertBatchIntoEligibleBucketHandlesRolloverSchemaChangeWithoutMetaField) {
    // Max bucket count with measurements[1:gTimeseriesBucketMaxCount] having kSchemaChange.
    auto batchOfMeasurementsWithSchemaChangeAfterFirstMeasurement =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSchemaChange,
                                                 .idxWithDiffMeasurement = 1,
                                                 .metaValueType = boost::none});
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};
    std::vector<size_t> numMeasurementsInWriteBatchAfterFirstMeasurement{
        1, static_cast<size_t>(gTimeseriesBucketMaxCount - 1)};

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithSchemaChangeAfterFirstMeasurement,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchAfterFirstMeasurement,
        /*numBatchedInsertContexts=*/1);

    // 50 measurements with measurements[25:50] having kSchemaChange.
    auto batchOfMeasurementsWithTimeForwardInMiddle =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSchemaChange,
                                                 .numMeasurements = 50,
                                                 .idxWithDiffMeasurement = 25,
                                                 .metaValueType = boost::none});
    std::vector<size_t> numMeasurementsInWriteBatchInMiddle{25, 25};

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithTimeForwardInMiddle,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchInMiddle,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverSizeWithMetaField) {
    // The last measurement will exceed the size that the bucket can store. We will trigger kSize,
    // so the measurement will be in a different bucket.
    std::vector<BSONObj> batchOfMeasurementsWithSize =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSize});
    std::vector<size_t> numMeasurementsInWriteBatch{124, 1};
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithSize,
                                                         currBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverSizeWithoutMetaField) {
    // The last measurement will exceed the size that the bucket can store. We will trigger kSize,
    // so the measurement will be in a different bucket.
    std::vector<BSONObj> batchOfMeasurementsWithSize = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kSize, .metaValueType = boost::none});
    std::vector<size_t> numMeasurementsInWriteBatch{124, 1};
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(_nsNoMeta,
                                                                  _uuidNoMeta,
                                                                  batchOfMeasurementsWithSize,
                                                                  currBatchedInsertContextsIndex,
                                                                  numMeasurementsInWriteBatch,
                                                                  /*numBatchedInsertContexts=*/1);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithSize,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest,
       StageInsertBatchIntoEligibleBucketHandlesRolloverCachePressureWithMetaField) {
    // Artificially lower _storageCacheSizeBytes so we can simulate kCachePressure.
    _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes;

    // The last measurement will exceed the size that the bucket can store. Coupled with the
    // lowered cache size, we will trigger kCachePressure, so the measurement will be in a
    // different bucket.
    std::vector<BSONObj> batchOfMeasurementsWithCachePressure =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kCachePressure});
    std::vector<size_t> numMeasurementsInWriteBatch{2, 2};
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithCachePressure,
                                                         currBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1);

    // Reset _storageCacheSizeBytes back to a representative value.
    _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;
}

TEST_F(BucketCatalogTest,
       StageInsertBatchIntoEligibleBucketHandlesRolloverCachePressureWithoutMetaField) {
    // Artificially lower _storageCacheSizeBytes so we can simulate kCachePressure.
    _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes;

    // The last measurement will exceed the size that the bucket can store. Coupled with the
    // lowered cache size, we will trigger kCachePressure, so the measurement will be in a
    // different bucket.
    std::vector<size_t> numMeasurementsInWriteBatch{3, 1};
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};
    std::vector<BSONObj> batchOfMeasurementsWithCachePressure =
        _generateMeasurementsWithRolloverReason(
            {.reason = RolloverReason::kCachePressure, .metaValueType = boost::none});

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithCachePressure,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithCachePressure,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);

    // Reset _storageCacheSizeBytes back to a representative value.
    _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverMixed) {
    auto batchOfMeasurementsWithCount = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kCount,
         .numMeasurements = static_cast<size_t>(2 * gTimeseriesBucketMaxCount),
         .timeValue = Date_t::now()});

    auto batchOfMeasurementsWithSchemaChange =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSchemaChange,
                                                 .numMeasurements = 50,
                                                 .idxWithDiffMeasurement = 25,
                                                 .timeValue = Date_t::now() + Seconds(1)});

    // Generating measurements with kSchema change will require us to add an additional
    // Seconds(1) to all subsequent measurement vectors because we ensure the measurements with
    // schema change are at the end of the measurements vector by adding Seconds(1) to the time
    // field of those measurements.
    std::vector<BSONObj> batchOfMeasurements = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone, .timeValue = Date_t::now() + Seconds(3)});

    std::vector<BSONObj> mixedRolloverReasonsMeasurements =
        _getFlattenedVector(std::vector<std::vector<BSONObj>>({batchOfMeasurementsWithCount,
                                                               batchOfMeasurementsWithSchemaChange,
                                                               batchOfMeasurements}));
    ASSERT_EQ(mixedRolloverReasonsMeasurements.size(),
              batchOfMeasurementsWithCount.size() + batchOfMeasurementsWithSchemaChange.size() +
                  batchOfMeasurements.size());

    std::vector<size_t> numWriteBatches{5};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0, 0, 0, 0};

    // We first fill up two buckets with gTimeseriesBucketMaxCount measurements due to the
    // batchOfMeasurementsWithCount.
    // We then fill up a third bucket with 25 measurements before encountering a
    // kSchemaChange in batchOfMeasurementsWithSchemaChange.
    // We then fill up a fourth bucket with the rest of the 25 measurements from
    // batchOfMeasurementsWithSchemaChange and the measurements from batchOfMeasurements.
    // Because batchOfMeasurements has gTimeseriesBucketMaxCount measurements, we write
    // gTimeseriesBucketMaxCount - 25 measurements before rolling over due to count and opening
    // a fifth bucket to insert the last 25 measurements.
    //
    // Note that the testing behavior is a bit different from stageInsertBatch (in
    // _testStageInsertBatchIntoEligibleBucket, as soon as we encounter a RolloverReason, we
    // rollover) and we would expect the third bucket to be reopened and written to with the
    // last 25 measurements in batchOfMeasurements in stageInsertBatch.
    std::vector<size_t> numMeasurementsInWriteBatch{static_cast<size_t>(gTimeseriesBucketMaxCount),
                                                    static_cast<size_t>(gTimeseriesBucketMaxCount),
                                                    25,
                                                    static_cast<size_t>(gTimeseriesBucketMaxCount),
                                                    25};

    // Test in a collection without a meta field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(_nsNoMeta,
                                                                  _uuidNoMeta,
                                                                  mixedRolloverReasonsMeasurements,
                                                                  curBatchedInsertContextsIndex,
                                                                  numMeasurementsInWriteBatch,
                                                                  /*numBatchedInsertContexts=*/1);

    // Test in a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         mixedRolloverReasonsMeasurements,
                                                         curBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverMixedWithNoMeta) {
    auto batchOfMeasurementsWithSize =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSize,
                                                 .timeValue = Date_t::now(),
                                                 .metaValueType = boost::none});

    std::vector<BSONObj> batchOfMeasurements =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kNone,
                                                 .numMeasurements = 50,
                                                 .timeValue = Date_t::now() + Seconds(1),
                                                 .metaValueType = boost::none});

    auto batchOfMeasurementsWithTimeForward =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kTimeForward,
                                                 .idxWithDiffMeasurement = 1,
                                                 .timeValue = Date_t::now() + Seconds(2),
                                                 .metaValueType = boost::none});

    std::vector<BSONObj> mixedRolloverReasonsMeasurements =
        _getFlattenedVector(std::vector<std::vector<BSONObj>>{
            batchOfMeasurementsWithSize, batchOfMeasurements, batchOfMeasurementsWithTimeForward});
    ASSERT_EQ(mixedRolloverReasonsMeasurements.size(),
              batchOfMeasurementsWithSize.size() + batchOfMeasurements.size() +
                  batchOfMeasurementsWithTimeForward.size());

    std::vector<size_t> numWriteBatches{3};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0, 0};

    // We will insert the first 124 measurements from batchOfMeasurementsWithSize into our first
    // bucket until we encounter the last measurement making us rollover due to size.
    // We write this last measurement from batchOfMeasurementsWithSize into the second bucket,
    // and then write all of the measurements from batchOfMeasurements into the second bucket.
    // We then insert the first measurement from batchOfMeasurementsWithTimeForward into the
    // second bucket. We then encounter kTimeForward and create a third bucket to write the
    // remaining gTimeseriesBucketMaxCount - 1 measurements from
    // batchOfMeasurementsWithTimeForward.
    std::vector<size_t> numMeasurementsInWriteBatch{
        124, 52, static_cast<size_t>(gTimeseriesBucketMaxCount - 1)};

    // Test in collection without a meta field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(_nsNoMeta,
                                                                  _uuidNoMeta,
                                                                  mixedRolloverReasonsMeasurements,
                                                                  curBatchedInsertContextsIndex,
                                                                  numMeasurementsInWriteBatch,
                                                                  /*numBatchedInsertContexts=*/1);

    // Test in collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        mixedRolloverReasonsMeasurements,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverMixedWithCachePressure) {
    // Artificially lower _storageCacheSizeBytes so we can simulate kCachePressure.
    _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes;

    auto batchOfMeasurementsWithSchemaChange =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSchemaChange,
                                                 .numMeasurements = 10,
                                                 .idxWithDiffMeasurement = 1,
                                                 .timeValue = Date_t::now()});

    // Generating measurements with kSchema change will require us to add an additional
    // Seconds(1) to all subsequent measurement vectors because we ensure the measurements with
    // schema change are at the end of the measurements vector by adding Seconds(1) to the time
    // field of those measurements.
    auto batchOfMeasurementsWithCachePressure = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kCachePressure, .timeValue = Date_t::now() + Seconds(2)});

    auto batchOfMeasurements =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kNone,
                                                 .numMeasurements = 10,
                                                 .timeValue = Date_t::now() + Seconds(3)});

    std::vector<BSONObj> mixedRolloverReasonsMeasurements =
        _getFlattenedVector(std::vector<std::vector<BSONObj>>{batchOfMeasurementsWithSchemaChange,
                                                              batchOfMeasurementsWithCachePressure,
                                                              batchOfMeasurements});
    ASSERT_EQ(mixedRolloverReasonsMeasurements.size(),
              batchOfMeasurementsWithSchemaChange.size() +
                  batchOfMeasurementsWithCachePressure.size() + batchOfMeasurements.size());

    std::vector<size_t> numWriteBatches{3};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0, 0};

    // The first bucket will consist of one  measurement from
    // batchOfMeasurementsWithSchemaChange. We will then rollover due to kSchemaChange. The
    // second bucket will consist of the remaining 9 measurements from
    // batchOfMeasurementsWithSchemaChange. We will then insert two measurements from
    // batchOfMeasurementsWithCachePressure into the second bucket and will rollover due to
    // kCachePressure.
    // We will insert the last two measurements from batchOfMeasurementsWithCachePressure into a
    // third bucket. Finally, we insert all 10 measurements from batchOfMeasurements into the
    // third bucket.
    std::vector<size_t> numMeasurementsInWriteBatch{1, 11, 12};

    // Test in collection without a meta field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(_nsNoMeta,
                                                                  _uuidNoMeta,
                                                                  mixedRolloverReasonsMeasurements,
                                                                  curBatchedInsertContextsIndex,
                                                                  numMeasurementsInWriteBatch,
                                                                  /*numBatchedInsertContexts=*/1);

    // Test in collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         mixedRolloverReasonsMeasurements,
                                                         curBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1);


    // Reset _storageCacheSizeBytes back to a representative value.
    _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;
}

TEST_F(BucketCatalogTest, BucketMetadataNormalization) {
    _testBucketMetadataFieldOrdering(BSON("a" << 1 << "b" << 1), BSON("a" << 1 << "b" << 1));
    _testBucketMetadataFieldOrdering(BSON("b" << 1 << "a" << 1), BSON("a" << 1 << "b" << 1));
    _testBucketMetadataFieldOrdering(BSON("nested" << BSON("a" << 1 << "b" << 1)),
                                     BSON("nested" << BSON("a" << 1 << "b" << 1)));
    _testBucketMetadataFieldOrdering(BSON("nested" << BSON("b" << 1 << "a" << 1)),
                                     BSON("nested" << BSON("a" << 1 << "b" << 1)));
}


TEST_F(
    BucketCatalogTest,
    StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketFillsUpSingleBucketWithMetaField) {
    std::vector<BSONObj> batchOfMeasurementsTimeseriesBucketMaxCount =
        _generateMeasurementsWithRolloverReason(
            {.reason = RolloverReason::kNone,
             .numMeasurements = static_cast<size_t>(gTimeseriesBucketMaxCount - 1)});
    std::vector<size_t> curBatchedInsertContextsIndex{0};
    std::vector<size_t> numMeasurementsInWriteBatch{
        static_cast<size_t>(gTimeseriesBucketMaxCount - 1)};

    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;
    measurementsAndRolloverReason.push_back(std::make_pair(measurement1Vec, RolloverReason::kNone));
    absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);

    // Inserting a batch of measurements with meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsTimeseriesBucketMaxCount,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1,
        buckets);
}

TEST_F(BucketCatalogTest,
       StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketFillsUpSingleBucketWithoutMeta) {
    std::vector<BSONObj> batchOfMeasurementsOne = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone, .numMeasurements = 1, .metaValueType = boost::none});
    std::vector<size_t> curBatchedInsertContextsIndex{0};
    std::vector<size_t> numMeasurementsInWriteBatch{1};

    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;
    measurementsAndRolloverReason.push_back(
        std::make_pair(measurementTimeseriesBucketMaxCountMinus1Vec, RolloverReason::kNone));
    absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_nsNoMeta, _uuidNoMeta, measurementsAndRolloverReason);

    // Inserting a batch of measurements without meta field values into a collection without a
    // meta field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(_nsNoMeta,
                                                                  _uuidNoMeta,
                                                                  batchOfMeasurementsOne,
                                                                  curBatchedInsertContextsIndex,
                                                                  numMeasurementsInWriteBatch,
                                                                  /*numBatchedInsertContexts=*/1,
                                                                  buckets);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    buckets = _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsOne,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1,
        buckets);
}

TEST_F(BucketCatalogTest,
       StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketHandlesCountWithMetaField) {
    std::vector<BSONObj> batchOfMeasurementsWithCount = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kCount,
         .numMeasurements = static_cast<size_t>(2 * gTimeseriesBucketMaxCount)});
    std::vector<size_t> numMeasurementsInWriteBatch{
        static_cast<size_t>(gTimeseriesBucketMaxCount - 1),
        static_cast<size_t>(gTimeseriesBucketMaxCount - 2),
        3};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0, 0};

    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;
    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {std::make_pair(measurement1Vec, RolloverReason::kSchemaChange),
         std::make_pair(measurement2Vec, RolloverReason::kSchemaChange),
         std::make_pair(measurement5Vec, RolloverReason::kSchemaChange)});
    absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);

    // Inserting a batch of measurements with meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithCount,
                                                         curBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1,
                                                         buckets);
}

TEST_F(BucketCatalogTest,
       StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketHandlesCountWithoutMetaField) {
    std::vector<BSONObj> batchOfMeasurementsWithCount = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kCount,
         .numMeasurements = static_cast<size_t>(gTimeseriesBucketMaxCount + 1),
         .metaValueType = boost::none});
    std::vector<size_t> numMeasurementsInWriteBatch{
        0, static_cast<size_t>(gTimeseriesBucketMaxCount - 5), 6};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0, 0};

    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;
    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {std::make_pair(measurementTimeseriesBucketMaxCountVec, RolloverReason::kCount),
         std::make_pair(measurement5Vec, RolloverReason::kSchemaChange),
         std::make_pair(measurement1Vec, RolloverReason::kSchemaChange)});
    absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_nsNoMeta, _uuidNoMeta, measurementsAndRolloverReason);

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(_nsNoMeta,
                                                                  _uuidNoMeta,
                                                                  batchOfMeasurementsWithCount,
                                                                  curBatchedInsertContextsIndex,
                                                                  numMeasurementsInWriteBatch,
                                                                  /*numBatchedInsertContexts=*/1,
                                                                  buckets);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    buckets = _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithCount,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1,
        buckets);
}

TEST_F(BucketCatalogTest,
       StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketHandlesTimeForwardWithMetaField) {
    // The last measurement in batchOfMeasurementsWithTimeForwardAtEnd will have a timestamp outside
    // of the bucket range encompassing the previous measurements, which means that this
    // measurement will be in a different bucket.
    std::vector<BSONObj> batchOfMeasurementsWithTimeForwardAtEnd =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kTimeForward,
                                                 .numMeasurements = 12,
                                                 .idxWithDiffMeasurement = 11,
                                                 .timeValue = Date_t::fromMillisSinceEpoch(100)});
    std::vector<size_t> numMeasurementsInWriteBatch{1, 10, 1};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0, 0};

    auto nonTimeForwardBucketVec1 = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone,
         .numMeasurements = static_cast<size_t>(gTimeseriesBucketMaxCount - 1),
         .timeValue = (Date_t::fromMillisSinceEpoch(100))});
    auto nonTimeForwardBucketVec2 =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kNone,
                                                 .numMeasurements = 5,
                                                 .timeValue = (Date_t::fromMillisSinceEpoch(100))});
    auto timeForwardBucketVec = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone,
         .numMeasurements = 1,
         .timeValue = (Date_t::fromMillisSinceEpoch(100) + Hours(2))});

    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;
    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {std::make_pair(nonTimeForwardBucketVec1, RolloverReason::kSchemaChange),
         std::make_pair(nonTimeForwardBucketVec2, RolloverReason::kSchemaChange),
         std::make_pair(timeForwardBucketVec, RolloverReason::kSchemaChange)});
    absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);

    // Inserting a batch of measurements with meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithTimeForwardAtEnd,
                                                         curBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1,
                                                         buckets);
}

TEST_F(
    BucketCatalogTest,
    StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketHandlesTimeForwardWithoutMetaField) {
    // Max bucket size with measurements[1:gTimeseriesBucketMaxCount] having kTimeForward.
    auto batchOfMeasurementsWithTimeForwardAfterFirstMeasurement =
        _generateMeasurementsWithRolloverReason(
            {.reason = RolloverReason::kTimeForward,
             .numMeasurements = static_cast<size_t>(gTimeseriesBucketMaxCount),
             .idxWithDiffMeasurement = 1,
             .timeValue = Date_t::fromMillisSinceEpoch(100),
             .metaValueType = boost::none});
    std::vector<size_t> numMeasurementsInWriteBatchAfterFirstMeasurement{
        1, static_cast<size_t>(gTimeseriesBucketMaxCount - 1)};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0};

    auto nonTimeForwardBucketVec = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone,
         .numMeasurements = static_cast<size_t>(gTimeseriesBucketMaxCount - 1),
         .timeValue = (Date_t::fromMillisSinceEpoch(100))});
    auto timeForwardBucketVec = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone,
         .numMeasurements = 1,
         .timeValue = (Date_t::fromMillisSinceEpoch(100) + Hours(2))});
    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;
    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {std::make_pair(nonTimeForwardBucketVec, RolloverReason::kSchemaChange),
         std::make_pair(timeForwardBucketVec, RolloverReason::kSchemaChange)});
    absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_nsNoMeta, _uuidNoMeta, measurementsAndRolloverReason);

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithTimeForwardAfterFirstMeasurement,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchAfterFirstMeasurement,
        /*numBatchedInsertContexts=*/1,
        buckets);

    // 50 measurements with measurements[25:50] having kTimeForward.
    std::vector<size_t> numMeasurementsInWriteBatchInMiddle{25, 25};
    auto batchOfMeasurementsWithTimeForwardInMiddle =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kTimeForward,
                                                 .numMeasurements = 50,
                                                 .idxWithDiffMeasurement = 25,
                                                 .timeValue = Date_t::fromMillisSinceEpoch(100),
                                                 .metaValueType = boost::none});

    nonTimeForwardBucketVec =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kNone,
                                                 .numMeasurements = 50,
                                                 .timeValue = (Date_t::fromMillisSinceEpoch(100))});
    timeForwardBucketVec = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone,
         .numMeasurements = static_cast<size_t>(gTimeseriesBucketMaxCount - 50),
         .timeValue = (Date_t::fromMillisSinceEpoch(100) + Hours(2))});
    measurementsAndRolloverReason.clear();
    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {std::make_pair(nonTimeForwardBucketVec, RolloverReason::kSchemaChange),
         std::make_pair(timeForwardBucketVec, RolloverReason::kSchemaChange)});
    buckets = _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithTimeForwardInMiddle,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchInMiddle,
        /*numBatchedInsertContexts=*/1,
        buckets);
}

TEST_F(
    BucketCatalogTest,
    StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketHandlesRolloverSchemaChangeWithMetaField) {
    Date_t timeValue = Date_t::now();
    // Max bucket size with only the last measurement having kSchemaChange.
    auto batchOfMeasurementsWithSchemaChangeAtEnd = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kSchemaChange, .timeValue = timeValue});

    // The last measurement in batchOfMeasurementsWithSchemaChangeAtEnd will have a int rather
    // than a string for field "deathGrips", which means this measurement will be in a different
    // bucket.
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0};
    std::vector<size_t> numMeasurementsInWriteBatchAtEnd{
        static_cast<size_t>(gTimeseriesBucketMaxCount - 1), 1};

    auto nonSchemaChangeBucketVec = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kNone, .numMeasurements = 1, .timeValue = timeValue});
    std::vector<BSONObj> schemaChangeVec;
    for (size_t i = 0; i < static_cast<size_t>(gTimeseriesBucketMaxCount - 2); i++) {
        schemaChangeVec.emplace_back(
            BSON(_timeField << timeValue << _metaField << _metaValue << "deathGrips" << 100));
    }
    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;
    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {std::make_pair(nonSchemaChangeBucketVec, RolloverReason::kSchemaChange),
         std::make_pair(schemaChangeVec, RolloverReason::kSchemaChange)});
    [[maybe_unused]] absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithSchemaChangeAtEnd,
                                                         curBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatchAtEnd,
                                                         /*numBatchedInsertContexts=*/1,
                                                         buckets);
}

TEST_F(
    BucketCatalogTest,
    StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketHandlesRolloverSchemaChangeWithoutMetaField) {
    Date_t timeValue = Date_t::now();
    // The first measurement in batchOfMeasurementsWithSchemaChangeAtEnd will have a int rather
    // than a string for field "deathGrips", which means this measurement will be in a different
    // bucket.
    // Max bucket size with measurements[1:gTimeseriesBucketMaxCount] having kSchemaChange.
    auto batchOfMeasurementsWithSchemaChangeAfterFirstMeasurement =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSchemaChange,
                                                 .idxWithDiffMeasurement = 1,
                                                 .timeValue = timeValue,
                                                 .metaValueType = boost::none});


    std::vector<size_t> curBatchedInsertContextsIndex{0, 0};
    std::vector<size_t> numMeasurementsInWriteBatchAfterFirstMeasurement{
        1, static_cast<size_t>(gTimeseriesBucketMaxCount - 1)};

    auto nonSchemaChangeBucketVec =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kNone,
                                                 .numMeasurements = 50,
                                                 .timeValue = timeValue,
                                                 .metaValueType = boost::none});
    std::vector<BSONObj> schemaChangeVec{BSON(_timeField << timeValue << "deathGrips" << 100)};
    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;

    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {std::make_pair(nonSchemaChangeBucketVec, RolloverReason::kSchemaChange),
         std::make_pair(schemaChangeVec, RolloverReason::kSchemaChange)});
    absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_nsNoMeta, _uuidNoMeta, measurementsAndRolloverReason);

    // Inserting a batch of measurements without meta field values into a collection without a
    // meta field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithSchemaChangeAfterFirstMeasurement,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchAfterFirstMeasurement,
        /*numBatchedInsertContexts=*/1);
    // 50 measurements with measurements [25:50] having kSchemaChange.
    auto batchOfMeasurementsWithTimeForwardInMiddle =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSchemaChange,
                                                 .numMeasurements = 50,
                                                 .idxWithDiffMeasurement = 25,
                                                 .metaValueType = boost::none});

    schemaChangeVec.clear();
    for (size_t i = 0; i < static_cast<size_t>(gTimeseriesBucketMaxCount - 25); i++) {
        schemaChangeVec.emplace_back(BSON(_timeField << timeValue << "deathGrips" << 100));
    }
    measurementsAndRolloverReason.clear();
    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {std::make_pair(nonSchemaChangeBucketVec, RolloverReason::kSchemaChange),
         std::make_pair(schemaChangeVec, RolloverReason::kSchemaChange)});
    buckets = _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);
    std::vector<size_t> numMeasurementsInWriteBatchInMiddle{25, 25};

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithTimeForwardInMiddle,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchInMiddle,
        /*numBatchedInsertContexts=*/1,
        buckets);
}

TEST_F(
    BucketCatalogTest,
    StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketHandlesRolloverSizeWithMetaField) {
    // The last measurement will exceed the size that the bucket can store. We will trigger kSize,
    // so the measurement will be in a different bucket.
    std::vector<BSONObj> batchOfMeasurementsWithSize =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSize});
    std::vector<size_t> numMeasurementsInWriteBatch{124, 1};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0};

    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;
    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {std::make_pair(measurement4Vec, RolloverReason::kSchemaChange),
         std::make_pair(measurement1Vec, RolloverReason::kSchemaChange)});
    absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithSize,
                                                         curBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1,
                                                         buckets);
}

TEST_F(
    BucketCatalogTest,
    StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketHandlesRolloverSizeWithoutMetaField) {
    // The last measurement will exceed the size that the bucket can store. We will trigger kSize,
    // so the measurement will be in a different bucket.
    std::vector<BSONObj> batchOfMeasurementsWithSize = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kSize, .metaValueType = boost::none});
    std::vector<size_t> numMeasurementsInWriteBatch{124, 1};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0};

    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;
    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {std::make_pair(measurement2Vec, RolloverReason::kSchemaChange),
         std::make_pair(measurement5Vec, RolloverReason::kSchemaChange)});
    absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_nsNoMeta, _uuidNoMeta, measurementsAndRolloverReason);

    // Inserting a batch of measurements without meta field values into a collection without a
    // meta field
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(_nsNoMeta,
                                                                  _uuidNoMeta,
                                                                  batchOfMeasurementsWithSize,
                                                                  curBatchedInsertContextsIndex,
                                                                  numMeasurementsInWriteBatch,
                                                                  /*numBatchedInsertContexts=*/1,
                                                                  buckets);

    buckets = _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);
    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithSize,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1,
        buckets);
}

TEST_F(
    BucketCatalogTest,
    StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketHandlesRolloverCachePressureWithMetaField) {
    // Artificially lower _storageCacheSizeBytes so we can simulate kCachePressure.
    _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes;

    //  We will trigger kCachePressure due to the size of the measurements stored and the lowered
    //  storageCacheSizeBytes.
    std::vector<BSONObj> batchOfMeasurementsWithCachePressure =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kCachePressure});
    std::vector<size_t> numMeasurementsInWriteBatch{2, 2};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0};

    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;
    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {
            std::make_pair(measurement1Vec, RolloverReason::kSchemaChange),
            std::make_pair(measurement1Vec, RolloverReason::kSchemaChange),
        });
    absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);

    // Inserting a batch of measurements with meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithCachePressure,
                                                         curBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1,
                                                         buckets);

    // Reset _storageCacheSizeBytes back to a representative value.
    _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;
}

TEST_F(
    BucketCatalogTest,
    StageInsertBatchIntoEligibleBucketWithPartiallyFilledBucketHandlesRolloverCachePressureWithoutMetaField) {
    // Artificially lower _storageCacheSizeBytes so we can simulate kCachePressure.
    _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes;

    // We will trigger kCachePressure due to the size of the measurements stored and the lowered
    // storageCacheSizeBytes.
    std::vector<BSONObj> batchOfMeasurementsWithCachePressure =
        _generateMeasurementsWithRolloverReason(
            {.reason = RolloverReason::kCachePressure, .metaValueType = boost::none});
    std::vector<size_t> numMeasurementsInWriteBatch{2, 2};
    std::vector<size_t> curBatchedInsertContextsIndex{0, 0};

    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> measurementsAndRolloverReason;
    measurementsAndRolloverReason.insert(
        measurementsAndRolloverReason.end(),
        {
            std::make_pair(measurement1Vec, RolloverReason::kSchemaChange),
            std::make_pair(measurement1Vec, RolloverReason::kSchemaChange),
        });
    [[maybe_unused]] absl::InlinedVector<Bucket*, 8> buckets =
        _generateBucketsWithMeasurements(_nsNoMeta, _uuidNoMeta, measurementsAndRolloverReason);

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithCachePressure,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1,
        buckets);

    buckets = _generateBucketsWithMeasurements(_ns1, _uuid1, measurementsAndRolloverReason);
    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithCachePressure,
        curBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1,
        buckets);

    // Reset _storageCacheSizeBytes back to a representative value.
    _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;
}

TEST_F(BucketCatalogTest, PrepareInsertsToBucketsSimpleOneFullBucket) {
    auto tsOptions = _getTimeseriesOptions(_ns1);
    std::vector<BSONObj> userBatch;
    for (auto i = 0; i < gTimeseriesBucketMaxCount; i++) {
        userBatch.emplace_back(BSON(_timeField << Date_t::now() << _metaField << _metaValue));
    }

    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns1),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/true,
                                                  _compressBucketFuncUnused,
                                                  userBatch,
                                                  /*startIndex=*/0,
                                                  /*numDocsToStage=*/userBatch.size(),
                                                  /*docsToRetry=*/{},
                                                  AllowQueryBasedReopening::kAllow,
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

TEST_F(BucketCatalogTest, PrepareInsertsToBucketsMultipleBucketsOneMeta) {
    auto tsOptions = _getTimeseriesOptions(_ns1);
    std::vector<BSONObj> userBatch;
    for (auto i = 0; i < 2 * gTimeseriesBucketMaxCount; i++) {
        userBatch.emplace_back(BSON(_timeField << Date_t::now() << _metaField << _metaValue));
    }
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns1),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/true,
                                                  _compressBucketFuncUnused,
                                                  userBatch,
                                                  /*startIndex=*/0,
                                                  /*numDocsToStage=*/userBatch.size(),
                                                  /*docsToRetry=*/{},
                                                  AllowQueryBasedReopening::kAllow,
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

TEST_F(BucketCatalogTest, PrepareInsertsToBucketsMultipleBucketsMultipleMetas) {
    auto tsOptions = _getTimeseriesOptions(_ns1);
    std::vector<BSONObj> userBatch;
    for (auto i = 0; i < gTimeseriesBucketMaxCount; i++) {
        userBatch.emplace_back(BSON(_timeField << Date_t::now() << _metaField << _metaValue));
    }
    for (auto i = 0; i < gTimeseriesBucketMaxCount; i++) {
        userBatch.emplace_back(BSON(_timeField << Date_t::now() << _metaField << "m"));
    }
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns1),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/true,
                                                  _compressBucketFuncUnused,
                                                  userBatch,
                                                  /*startIndex=*/0,
                                                  /*numDocsToStage=*/userBatch.size(),
                                                  /*docsToRetry=*/{},
                                                  AllowQueryBasedReopening::kAllow,
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

TEST_F(BucketCatalogTest, PrepareInsertsToBucketsMultipleBucketsMultipleMetasInterleaved) {
    auto tsOptions = _getTimeseriesOptions(_ns1);
    std::vector<BSONObj> userBatch;
    for (auto i = 0; i < 2 * gTimeseriesBucketMaxCount; i++) {
        userBatch.emplace_back(BSON(_timeField << Date_t::now() << _metaField
                                               << (i % 2 == 0 ? _metaValue : _metaValue2)));
    }
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns1),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/true,
                                                  _compressBucketFuncUnused,
                                                  userBatch,
                                                  /*startIndex=*/0,
                                                  /*numDocsToStage=*/userBatch.size(),
                                                  /*docsToRetry=*/{},
                                                  AllowQueryBasedReopening::kAllow,
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

TEST_F(BucketCatalogTest, PrepareInsertsBadMeasurementsAll) {
    auto tsOptions = _getTimeseriesOptions(_ns1);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> userMeasurementsBatch{
        BSON(_metaField << _metaValue << "x" << 2),  // Malformed measurement, missing time field
        BSON(_metaField << _metaValue << "x" << 3),  // Malformed measurement, missing time field
    };

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns1),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/true,
                                                  _compressBucketFuncUnused,
                                                  userMeasurementsBatch,
                                                  /*startIndex=*/0,
                                                  /*numDocsToStage=*/userMeasurementsBatch.size(),
                                                  /*docsToRetry=*/{},
                                                  AllowQueryBasedReopening::kAllow,
                                                  errorsAndIndices);

    ASSERT_FALSE(swWriteBatches.isOK());
    ASSERT_EQ(errorsAndIndices.size(), 2);
}

TEST_F(BucketCatalogTest, PrepareInsertsBadMeasurementsSome) {
    auto tsOptions = _getTimeseriesOptions(_ns1);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> userMeasurementsBatch{
        BSON(_metaField << _metaValue << "x" << 2),  // Malformed measurement, missing time field
        BSON(_timeField << Date_t::fromMillisSinceEpoch(5) << _metaField << _metaValue << "x" << 3),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2) << _metaField << _metaValue << "x" << 3),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3) << _metaField << _metaValue2 << "x"
                        << 3),
        BSON(_metaField << _metaValue2 << "x" << 3),  // Malformed measurement, missing time field
        BSON(_timeField << Date_t::fromMillisSinceEpoch(9) << _metaField << _metaValue2 << "x"
                        << 3),
    };

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_ns1),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/true,
                                                  _compressBucketFuncUnused,
                                                  userMeasurementsBatch,
                                                  /*startIndex=*/0,
                                                  /*numDocsToStage=*/userMeasurementsBatch.size(),
                                                  /*docsToRetry=*/{},
                                                  AllowQueryBasedReopening::kAllow,
                                                  errorsAndIndices);

    ASSERT_FALSE(swWriteBatches.isOK());
    ASSERT_EQ(errorsAndIndices.size(), 2);

    ASSERT_EQ(errorsAndIndices[0].index, 0);
    ASSERT_EQ(errorsAndIndices[1].index, 4);
}

TEST_F(BucketCatalogTest, PrepareInsertsToBucketsRespectsStartIndexNoMeta) {
    auto tsOptions = _getTimeseriesOptions(_nsNoMeta);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> originalUserBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(5)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(6)),
    };

    stdx::unordered_set<size_t> expectedIndices = {2, 3, 4, 5, 6};
    size_t startIndex = 2;
    size_t numDocsToStage = 5;

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_nsNoMeta),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/false,
                                                  _compressBucketFuncUnused,
                                                  originalUserBatch,
                                                  /*startIndex=*/startIndex,
                                                  /*numDocsToStage=*/numDocsToStage,
                                                  /*docsToRetry=*/{},
                                                  AllowQueryBasedReopening::kAllow,
                                                  errorsAndIndices);
    ASSERT_OK(swWriteBatches);
    auto writeBatches = swWriteBatches.getValue();
    ASSERT_EQ(writeBatches.size(), 1);

    auto writeBatch = writeBatches.front();
    ASSERT_EQ(writeBatch->measurements.size(), numDocsToStage);
    ASSERT_EQ(stdx::unordered_set<size_t>(writeBatch->userBatchIndices.begin(),
                                          writeBatch->userBatchIndices.end()),
              expectedIndices);
}

TEST_F(BucketCatalogTest, PrepareInsertsToBucketsRespectsStartIndexWithMeta) {
    auto tsOptions = _getTimeseriesOptions(_ns1);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> originalUserBatch{
        BSON(_metaField << _metaValue << _timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_metaField << _metaValue2 << _timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON(_metaField << _metaValue2 << _timeField << Date_t::fromMillisSinceEpoch(1)),
        BSON(_metaField << _metaValue << _timeField << Date_t::fromMillisSinceEpoch(3)),
        BSON(_metaField << _metaValue2 << _timeField << Date_t::fromMillisSinceEpoch(5)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)),
        BSON(_metaField << _metaValue << _timeField << Date_t::fromMillisSinceEpoch(6)),
    };

    stdx::unordered_set<size_t> expectedIndices = {2, 3, 4, 5, 6};
    size_t startIndex = 2;
    size_t numDocsToStage = 5;

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_nsNoMeta),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/false,
                                                  _compressBucketFuncUnused,
                                                  originalUserBatch,
                                                  /*startIndex=*/startIndex,
                                                  /*numDocsToStage=*/numDocsToStage,
                                                  /*docsToRetry=*/{},
                                                  AllowQueryBasedReopening::kAllow,
                                                  errorsAndIndices);
    ASSERT_OK(swWriteBatches);
    auto writeBatches = swWriteBatches.getValue();
    ASSERT_EQ(writeBatches.size(), 3);

    stdx::unordered_set<size_t> actualIndices;
    for (auto& batch : writeBatches) {
        for (auto index : batch->userBatchIndices) {
            actualIndices.insert(index);
        }
    }

    ASSERT_EQ(actualIndices, expectedIndices);
}

TEST_F(BucketCatalogTest, PrepareInsertsToBucketsRespectsDocsToRetryNoMeta) {
    auto tsOptions = _getTimeseriesOptions(_nsNoMeta);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> originalUserBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(5)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(6)),
    };

    std::vector<size_t> docsToRetry = {0, 2, 4};

    // These should be ignored when docsToRetry is specified.
    size_t startIndex = 2;
    size_t numDocsToStage = 5;

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_nsNoMeta),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/false,
                                                  _compressBucketFuncUnused,
                                                  originalUserBatch,
                                                  /*startIndex=*/startIndex,
                                                  /*numDocsToStage=*/numDocsToStage,
                                                  /*docsToRetry=*/docsToRetry,
                                                  AllowQueryBasedReopening::kAllow,
                                                  errorsAndIndices);

    ASSERT_OK(swWriteBatches);
    auto writeBatches = swWriteBatches.getValue();
    ASSERT_EQ(writeBatches.size(), 1);

    auto writeBatch = writeBatches.front();
    ASSERT_EQ(writeBatch->measurements.size(), docsToRetry.size());
    ASSERT_EQ(stdx::unordered_set<size_t>(writeBatch->userBatchIndices.begin(),
                                          writeBatch->userBatchIndices.end()),
              stdx::unordered_set<size_t>(docsToRetry.begin(), docsToRetry.end()));
}

TEST_F(BucketCatalogTest, PrepareInsertsToBucketsRespectsDocsToRetryWithMeta) {
    auto tsOptions = _getTimeseriesOptions(_ns1);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> originalUserBatch{
        BSON(_metaField << _metaValue << _timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_metaField << _metaValue2 << _timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON(_metaField << _metaValue2 << _timeField << Date_t::fromMillisSinceEpoch(1)),
        BSON(_metaField << _metaValue << _timeField << Date_t::fromMillisSinceEpoch(3)),
        BSON(_metaField << _metaValue2 << _timeField << Date_t::fromMillisSinceEpoch(5)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)),
        BSON(_metaField << _metaValue << _timeField << Date_t::fromMillisSinceEpoch(6)),
    };

    std::vector<size_t> docsToRetry = {0, 2, 4, 5};

    // These should be ignored when docsToRetry is specified.
    size_t startIndex = 2;
    size_t numDocsToStage = 5;

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_nsNoMeta),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/false,
                                                  _compressBucketFuncUnused,
                                                  originalUserBatch,
                                                  /*startIndex=*/startIndex,
                                                  /*numDocsToStage=*/numDocsToStage,
                                                  /*docsToRetry=*/docsToRetry,
                                                  AllowQueryBasedReopening::kAllow,
                                                  errorsAndIndices);

    ASSERT_OK(swWriteBatches);
    auto writeBatches = swWriteBatches.getValue();
    ASSERT_EQ(writeBatches.size(), 3);

    stdx::unordered_set<size_t> actualIndices;
    for (auto& batch : writeBatches) {
        for (auto index : batch->userBatchIndices) {
            actualIndices.insert(index);
        }
    }

    ASSERT_EQ(actualIndices, stdx::unordered_set<size_t>(docsToRetry.begin(), docsToRetry.end()));
}

TEST_F(BucketCatalogTest, CreateOrderedPotentialBucketsVectorWithoutAnyBuckets) {
    PotentialBucketOptions bucketOptions;
    _testCreateOrderedPotentialBucketsVector(bucketOptions);
}

TEST_F(BucketCatalogTest, CreateOrderedPotentialBucketsVectorWithOneSoftClosedBucketSimple) {
    PotentialBucketOptions bucketOptions;
    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> softClosedSimpleVec;
    softClosedSimpleVec.push_back(std::make_pair(measurement1Vec, RolloverReason::kTimeForward));
    absl::InlinedVector<Bucket*, 8> softClosedSimpleBucket =
        _generateBucketsWithMeasurements(_ns1, _uuid1, softClosedSimpleVec);
    bucketOptions.kSoftClosedBuckets = softClosedSimpleBucket;
    _testCreateOrderedPotentialBucketsVector(bucketOptions);
}

TEST_F(BucketCatalogTest, CreateOrderedPotentialBucketsVectorWithOneArchivedBucketSimple) {
    PotentialBucketOptions bucketOptions;
    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> archiveSimpleVec;
    archiveSimpleVec.push_back(std::make_pair(measurement1Vec, RolloverReason::kTimeBackward));
    absl::InlinedVector<Bucket*, 8> archiveSimpleBucket =
        _generateBucketsWithMeasurements(_ns1, _uuid1, archiveSimpleVec);
    bucketOptions.kArchivedBuckets = archiveSimpleBucket;
    _testCreateOrderedPotentialBucketsVector(bucketOptions);
}

TEST_F(BucketCatalogTest, CreateOrderedPotentialBucketsVectorWithoutNoneBucketSimple) {
    PotentialBucketOptions bucketOptions;
    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> softClosedSimpleVec;
    softClosedSimpleVec.push_back(std::make_pair(measurement1Vec, RolloverReason::kTimeForward));
    absl::InlinedVector<Bucket*, 8> softClosedSimpleBucket =
        _generateBucketsWithMeasurements(_ns1, _uuid1, softClosedSimpleVec);
    bucketOptions.kSoftClosedBuckets = softClosedSimpleBucket;

    std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> archiveSimpleVec;
    archiveSimpleVec.push_back(std::make_pair(measurement1Vec, RolloverReason::kTimeBackward));
    absl::InlinedVector<Bucket*, 8> archiveSimpleBucket =
        _generateBucketsWithMeasurements(_ns1, _uuid1, archiveSimpleVec);
    bucketOptions.kSoftClosedBuckets = softClosedSimpleBucket;
    bucketOptions.kArchivedBuckets = archiveSimpleBucket;
    _testCreateOrderedPotentialBucketsVector(bucketOptions);
}

TEST_F(BucketCatalogTest, CreateOrderedPotentialBucketsVectorWithOnlySoftClosedBuckets) {
    // Test with only setting kSoftClosedBucket.
    for (size_t i = 0; i < allMeasurementVecs.size(); i++) {
        PotentialBucketOptions bucketOptionsWithEmptyArchive;
        std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> softClosedVec =
            _getMeasurementRolloverReasonVec(allMeasurementVecs[i], RolloverReason::kTimeForward);
        absl::InlinedVector<Bucket*, 8> softClosedBucket =
            _generateBucketsWithMeasurements(_ns1, _uuid1, softClosedVec);
        bucketOptionsWithEmptyArchive.kSoftClosedBuckets = softClosedBucket;
        _testCreateOrderedPotentialBucketsVector(bucketOptionsWithEmptyArchive);
    }
}

TEST_F(BucketCatalogTest, CreateOrderedPotentialBucketsVectorWithOnlyArchivedBuckets) {
    // Test with only setting kArchivedBuckets.
    for (size_t i = 0; i < allMeasurementVecs.size(); i++) {
        PotentialBucketOptions bucketOptionsWithEmptySoftClosed;
        std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> archivedVec =
            _getMeasurementRolloverReasonVec(allMeasurementVecs[i], RolloverReason::kTimeBackward);
        absl::InlinedVector<Bucket*, 8> archivedBucket =
            _generateBucketsWithMeasurements(_ns1, _uuid1, archivedVec);
        bucketOptionsWithEmptySoftClosed.kArchivedBuckets = archivedBucket;
        _testCreateOrderedPotentialBucketsVector(bucketOptionsWithEmptySoftClosed);
    }
}

TEST_F(BucketCatalogTest, CreateOrderedPotentialBucketsVectorWithoutNoneBucket) {
    std::vector<absl::InlinedVector<Bucket*, 8>> softClosedBuckets;
    std::vector<absl::InlinedVector<Bucket*, 8>> archiveClosedBuckets;

    // Initialize buckets.
    for (size_t i = 0; i < allMeasurementVecs.size(); i++) {
        softClosedBuckets.push_back(_generateBucketsWithMeasurements(
            _ns1,
            _uuid1,
            _getMeasurementRolloverReasonVec(allMeasurementVecs[i], RolloverReason::kTimeForward)));
        archiveClosedBuckets.push_back(_generateBucketsWithMeasurements(
            _ns1,
            _uuid1,
            _getMeasurementRolloverReasonVec(allMeasurementVecs[i],
                                             RolloverReason::kTimeBackward)));
    }

    for (size_t i = 0; i < allMeasurementVecs.size(); i++) {
        PotentialBucketOptions bucketOptions;
        bucketOptions.kSoftClosedBuckets = softClosedBuckets[i];

        for (size_t j = 0; j < allMeasurementVecs.size(); j++) {
            bucketOptions.kArchivedBuckets = archiveClosedBuckets[j];
            _testCreateOrderedPotentialBucketsVector(bucketOptions);
        }
    }
}

TEST_F(BucketCatalogTest, CreateOrderedPotentialBucketsVectorWithOnlyNoneBucket) {
    // The sorted1 vec contains all the potential sizes of measurement vectors [1, 2, 3, 4, 5,
    // kBucketMaxCount] we created in the fixture.
    for (size_t i = 0; i < sorted1.size(); i++) {
        PotentialBucketOptions bucketOptions;
        std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> kNoneVec;
        kNoneVec.push_back(std::make_pair(sorted1[i], RolloverReason::kNone));
        absl::InlinedVector<Bucket*, 8> kNoneBucket =
            _generateBucketsWithMeasurements(_ns1, _uuid1, kNoneVec);
        bucketOptions.kNoneBucket = kNoneBucket[0];
        _testCreateOrderedPotentialBucketsVector(bucketOptions);

        // We will invariant if we have > 1 uncleared, kNone bucket. So we have to set the
        // RolloverReason for the next kNone open bucket we create.
        kNoneBucket[0]->rolloverReason = RolloverReason::kTimeForward;
    }
}

TEST_F(BucketCatalogTest, CreateOrderedPotentialBucketsVectorWithNoneBucket) {
    std::vector<absl::InlinedVector<Bucket*, 8>> softClosedBuckets;
    std::vector<absl::InlinedVector<Bucket*, 8>> archiveClosedBuckets;
    std::vector<Bucket*> kNoneBuckets;

    // Initialize buckets.
    for (size_t i = 0; i < allMeasurementVecs.size(); i++) {
        softClosedBuckets.push_back(_generateBucketsWithMeasurements(
            _ns1,
            _uuid1,
            _getMeasurementRolloverReasonVec(allMeasurementVecs[i], RolloverReason::kTimeForward)));
        archiveClosedBuckets.push_back(_generateBucketsWithMeasurements(
            _ns1,
            _uuid1,
            _getMeasurementRolloverReasonVec(allMeasurementVecs[i],
                                             RolloverReason::kTimeBackward)));
    }
    for (size_t i = 0; i < sorted1.size(); i++) {
        std::vector<std::pair<std::vector<BSONObj>, RolloverReason>> kNoneVec;
        kNoneVec.push_back(std::make_pair(sorted1[i], RolloverReason::kTimeForward));
        kNoneBuckets.push_back(_generateBucketsWithMeasurements(_ns1, _uuid1, kNoneVec)[0]);
    }

    for (size_t i = 0; i < allMeasurementVecs.size(); i++) {
        PotentialBucketOptions bucketOptions;
        bucketOptions.kSoftClosedBuckets = softClosedBuckets[i];
        for (size_t j = 0; j < allMeasurementVecs.size(); j++) {
            bucketOptions.kArchivedBuckets = archiveClosedBuckets[j];
            for (size_t k = 0; k < sorted1.size(); k++) {
                kNoneBuckets[k]->rolloverReason = RolloverReason::kNone;
                bucketOptions.kNoneBucket = kNoneBuckets[k];
                _testCreateOrderedPotentialBucketsVector(bucketOptions);

                // We will invariant if we have > 1 uncleared, kNone bucket. So we have to set
                // the RolloverReason for the next kNone open bucket we create.
                kNoneBuckets[k]->rolloverReason = RolloverReason::kTimeForward;
            }
        }
    }
}

TEST_F(BucketCatalogTest, ExecutionStatsNumActiveBucketsSentinel) {
    auto stats = internal::getOrInitializeExecutionStats(*_bucketCatalog, _uuid1);
    auto collStatsVec = internal::releaseExecutionStatsFromBucketCatalog(
        *_bucketCatalog, std::span<const UUID>(&_uuid1, 1));
    ASSERT_EQ(collStatsVec.size(), 1);

    auto& collStats = *collStatsVec[0];
    auto& globalStats = _bucketCatalog->globalExecutionStats;

    ASSERT_EQ(collStats.numActiveBuckets.loadRelaxed(), 0);
    ASSERT_EQ(globalStats.numActiveBuckets.loadRelaxed(), 0);

    // Set an initial value for the stats.
    stats.incNumActiveBuckets(5);
    ASSERT_EQ(collStats.numActiveBuckets.loadRelaxed(), 5);
    ASSERT_EQ(globalStats.numActiveBuckets.loadRelaxed(), 5);

    // Removing collection's 'numActiveBuckets' should also set it to the sentinel value.
    constexpr long long kNumActiveBucketsSentinel = std::numeric_limits<long long>::min();
    removeCollectionExecutionGauges(globalStats, collStats);
    ASSERT_EQ(collStats.numActiveBuckets.loadRelaxed(), kNumActiveBucketsSentinel);
    ASSERT_EQ(globalStats.numActiveBuckets.loadRelaxed(), 0);

    // Check no increment can be done anymore.
    stats.incNumActiveBuckets();
    ASSERT_EQ(collStats.numActiveBuckets.loadRelaxed(), kNumActiveBucketsSentinel);
    ASSERT_EQ(globalStats.numActiveBuckets.loadRelaxed(), 0);

    // Check no decrement can be done anymore.
    stats.decNumActiveBuckets();
    ASSERT_EQ(collStats.numActiveBuckets.loadRelaxed(), kNumActiveBucketsSentinel);
    ASSERT_EQ(globalStats.numActiveBuckets.loadRelaxed(), 0);

    // Reset all stats.
    collStats.numActiveBuckets.swap(0);
    globalStats.numActiveBuckets.swap(0);
}

TEST_F(BucketCatalogTest, ExecutionStatsNumActiveBucketsNonNegative) {
    auto stats = internal::getOrInitializeExecutionStats(*_bucketCatalog, _uuid1);
    auto collStatsVec = internal::releaseExecutionStatsFromBucketCatalog(
        *_bucketCatalog, std::span<const UUID>(&_uuid1, 1));
    ASSERT_EQ(collStatsVec.size(), 1);

    auto& collStats = *collStatsVec[0];
    auto& globalStats = _bucketCatalog->globalExecutionStats;

    ASSERT_EQ(collStats.numActiveBuckets.loadRelaxed(), 0);
    ASSERT_EQ(globalStats.numActiveBuckets.loadRelaxed(), 0);

    // Cannot decrement 'numActiveBuckets' below 0.
    stats.decNumActiveBuckets();
    ASSERT_EQ(collStats.numActiveBuckets.loadRelaxed(), 0);
    ASSERT_EQ(globalStats.numActiveBuckets.loadRelaxed(), 0);

    // Global and collection stats should decrement by the same value.
    globalStats.numActiveBuckets.fetchAndAddRelaxed(1);
    ASSERT_EQ(globalStats.numActiveBuckets.loadRelaxed(), 1);
    stats.decNumActiveBuckets();
    ASSERT_EQ(collStats.numActiveBuckets.loadRelaxed(), 0);
    ASSERT_EQ(globalStats.numActiveBuckets.loadRelaxed(), 1);

    // Reset all stats.
    collStats.numActiveBuckets.swap(0);
    globalStats.numActiveBuckets.swap(0);
}
}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
