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

#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/timeseries/write_ops/internal/timeseries_write_ops_internal.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils_internal.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

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
    void _insertOneAndCommit(const NamespaceString& ns,
                             const UUID& uuid,
                             uint16_t numPreviouslyCommittedMeasurements);

    InsertResult _insertOneHelper(OperationContext* opCtx,
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
        const boost::optional<BucketDocumentValidator>& documentValidator = boost::none);

    BSONObj _getCompressedBucketDoc(const BSONObj& bucketDoc);

    RolloverReason _rolloverReason(const std::shared_ptr<WriteBatch>& batch);

    void _testUseBucketSkipsConflictingBucket(std::function<void(BucketCatalog&, Bucket&)>);

    void _testBucketMetadataFieldOrdering(const BSONObj& inputMetadata,
                                          const BSONObj& expectedMetadata);

    void _testStageInsertBatchIntoEligibleBucket(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& batchOfMeasurements,
        const std::vector<size_t>& currBatchedInsertContextsIndex,
        const std::vector<size_t>& numMeasurementsInWriteBatch,
        size_t numBatchedInsertContexts) const;

    void _testStageInsertBatchIntoEligibleBucketWithMetaField(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& batchOfMeasurements,
        const std::vector<size_t>& currBatchedInsertContextsIndex,
        const std::vector<size_t>& numMeasurementsInWriteBatch,
        size_t numBatchedInsertContexts) const;

    void _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& batchOfMeasurements,
        const std::vector<size_t>& currBatchedInsertContextsIndex,
        const std::vector<size_t>& numMeasurementsInWriteBatch,
        size_t numBatchedInsertContexts) const;

    void _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        const NamespaceString& ns,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& batchOfMeasurements,
        const std::vector<size_t>& currBatchedInsertContextsIndex,
        const std::vector<size_t>& numMeasurementsInWriteBatch,
        size_t numBatchedInsertContexts) const;
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
    ASSERT_EQ(batch->measurements.size(), expectedBatchSize);
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, numPreviouslyCommittedMeasurements);
    TimeseriesStmtIds stmtIds;
    std::vector<mongo::write_ops::InsertCommandRequest> insertOps;
    std::vector<mongo::write_ops::UpdateCommandRequest> updateOps;
    write_ops_utils::makeWriteRequest(_opCtx,
                                      batch,
                                      getMetadata(*_bucketCatalog, batch->bucketId),
                                      stmtIds,
                                      ns.makeTimeseriesBucketsNamespace(),
                                      &insertOps,
                                      &updateOps);
    finish(*_bucketCatalog, batch);
}

void BucketCatalogTest::_insertOneAndCommit(const NamespaceString& ns,
                                            const UUID& uuid,
                                            uint16_t numPreviouslyCommittedMeasurements) {
    auto time = Date_t::now();
    auto insertContextAndTime = uassertStatusOK(
        prepareInsert(*_bucketCatalog, uuid, _getTimeseriesOptions(ns), BSON(_timeField << time)));

    auto result = insert(*_bucketCatalog,
                         _getCollator(ns),
                         BSON(_timeField << time),
                         _opCtx->getOpID(),
                         std::get<InsertContext>(insertContextAndTime),
                         std::get<Date_t>(insertContextAndTime),
                         _storageCacheSizeBytes);
    auto& batch = get<SuccessfulInsertion>(result).batch;
    _commit(ns, batch, numPreviouslyCommittedMeasurements);
}

InsertResult BucketCatalogTest::_insertOneHelper(OperationContext* opCtx,
                                                 BucketCatalog& catalog,
                                                 const mongo::NamespaceString& nss,
                                                 const UUID& uuid,
                                                 const mongo::BSONObj& doc) {

    auto insertContextAndTime =
        uassertStatusOK(prepareInsert(catalog, uuid, _getTimeseriesOptions(nss), doc));

    return insert(catalog,
                  _getCollator(nss),
                  doc,
                  opCtx->getOpID(),
                  std::get<InsertContext>(insertContextAndTime),
                  std::get<Date_t>(insertContextAndTime),
                  _storageCacheSizeBytes);
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
            auto result =
                _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, timestampedDoc.obj());
            auto& batch = get<SuccessfulInsertion>(result).batch;
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
    auto stripeNumber = internal::getStripeNumber(*_bucketCatalog, key);
    InsertContext insertContext{key, stripeNumber, *options, stats};
    auto validator = [opCtx = _opCtx, &coll](const BSONObj& bucketDoc) -> auto {
        return coll->checkValidation(opCtx, bucketDoc);
    };
    auto era = getCurrentEra(_bucketCatalog->bucketStateRegistry);

    return internal::rehydrateBucket(*_bucketCatalog,
                                     bucketDoc,
                                     insertContext.key,
                                     insertContext.options,
                                     era,
                                     coll->getDefaultCollator(),
                                     validator,
                                     insertContext.stats);
}

Status BucketCatalogTest::_reopenBucket(
    const CollectionPtr& coll,
    const BSONObj& bucketDoc,
    const boost::optional<unsigned long>& rehydrateEra,
    const boost::optional<unsigned long>& loadBucketIntoCatalogEra,
    const boost::optional<BucketKey>& bucketKey,
    const boost::optional<BucketDocumentValidator>& documentValidator) {
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
    InsertContext insertContext{key, stripeNumber, *options, stats};

    // Validate the bucket document against the schema.
    auto validator =
        documentValidator.value_or([opCtx = _opCtx, &coll](const BSONObj& bucketDoc) -> auto {
            return coll->checkValidation(opCtx, bucketDoc);
        });
    auto era = rehydrateEra.value_or(getCurrentEra(_bucketCatalog->bucketStateRegistry));

    auto res = internal::rehydrateBucket(*_bucketCatalog,
                                         bucketDoc,
                                         insertContext.key,
                                         insertContext.options,
                                         era,
                                         coll->getDefaultCollator(),
                                         validator,
                                         insertContext.stats);
    if (!res.isOK()) {
        return res.getStatus();
    }
    auto bucket = std::move(res.getValue());

    // Register the reopened bucket with the catalog.
    auto& stripe = *_bucketCatalog->stripes[insertContext.stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    era = loadBucketIntoCatalogEra.value_or(getCurrentEra(_bucketCatalog->bucketStateRegistry));
    return internal::loadBucketIntoCatalog(*_bucketCatalog,
                                           stripe,
                                           stripeLock,
                                           insertContext.stats,
                                           insertContext.key,
                                           std::move(bucket),
                                           era)
        .getStatus();
}

RolloverReason BucketCatalogTest::_rolloverReason(const std::shared_ptr<WriteBatch>& batch) {
    auto& stripe =
        _bucketCatalog->stripes[internal::getStripeNumber(*_bucketCatalog, batch->bucketId)];
    auto& [key, bucket] = *stripe->openBucketsById.find(batch->bucketId);
    return bucket->rolloverReason;
}

void BucketCatalogTest::_testUseBucketSkipsConflictingBucket(
    std::function<void(BucketCatalog&, Bucket&)> makeBucketConflict) {
    BSONObj measurement = BSON(_timeField << Date_t::now() << _metaField << 1);
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();

    Bucket& bucket1 = internal::allocateBucket(*_bucketCatalog,
                                               *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                               WithLock::withoutLock(),
                                               insertCtx.key,
                                               insertCtx.options,
                                               time,
                                               nullptr,
                                               insertCtx.stats);
    makeBucketConflict(*_bucketCatalog, bucket1);

    Bucket& bucket2 = internal::allocateBucket(*_bucketCatalog,
                                               *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                               WithLock::withoutLock(),
                                               insertCtx.key,
                                               insertCtx.options,
                                               time,
                                               nullptr,
                                               insertCtx.stats);

    ASSERT_EQ(&bucket2,
              internal::useBucket(*_bucketCatalog,
                                  *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                  WithLock::withoutLock(),
                                  insertCtx,
                                  time,
                                  nullptr));
}

void BucketCatalogTest::_testBucketMetadataFieldOrdering(const BSONObj& inputMetadata,
                                                         const BSONObj& expectedMetadata) {
    auto swBucketKeyAndTime = internal::extractBucketingParameters(
        getTrackingContext(_bucketCatalog->trackingContexts, TrackingScope::kOpenBucketsByKey),
        UUID::gen(),
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField << inputMetadata));
    ASSERT_OK(swBucketKeyAndTime);

    auto metadata = swBucketKeyAndTime.getValue().first.metadata.toBSON();
    ASSERT_EQ(metadata.woCompare(BSON(_metaField << expectedMetadata)), 0);
}

// currBatchedInsertContextsIndex is the index in batchedInsertContexts that the current write
// batch should be accessing.
// numMeasurementsInWriteBatch is the number of measurements that should be in the current write
// batch returned by stageInsertBatch.
// The currBatchedInsertContextsIndex.size() == numMeasurementsInWriteBatch.size() == the total
// number of buckets that should be written to from the input batchOfMeasurements.
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
// currBatchedInsertContextsIndex = [0, 0, 1]
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
void BucketCatalogTest::_testStageInsertBatchIntoEligibleBucket(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& batchOfMeasurements,
    const std::vector<size_t>& currBatchedInsertContextsIndex,
    const std::vector<size_t>& numMeasurementsInWriteBatch,
    size_t numBatchedInsertContexts) const {
    ASSERT(numMeasurementsInWriteBatch.size() > 0 && numBatchedInsertContexts > 0);
    // These size values are equivalent to the total number of buckets that should be written to
    // from the input batchOfMeasurements.
    ASSERT(currBatchedInsertContextsIndex.size() == numMeasurementsInWriteBatch.size());

    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();
    auto timeseriesOptions = _getTimeseriesOptions(ns);
    std::vector<timeseries::write_ops::internal::WriteStageErrorAndIndex> errorsAndIndices;

    auto batchedInsertContexts = write_ops::internal::buildBatchedInsertContexts(
        *_bucketCatalog,
        collectionUUID,
        timeseriesOptions,
        batchOfMeasurements,
        /*startIndex=*/0,
        /*numDocsToStage=*/batchOfMeasurements.size(),
        /*docsToRetry=*/{},
        errorsAndIndices);
    ASSERT(errorsAndIndices.empty());
    ASSERT_EQ(batchedInsertContexts.size(), numBatchedInsertContexts);
    size_t numMeasurements = 0;
    for (size_t i = 0; i < batchedInsertContexts.size(); i++) {
        numMeasurements += batchedInsertContexts[i].measurementsTimesAndIndices.size();
    }
    ASSERT_EQ(numMeasurements, batchOfMeasurements.size());

    auto currentBatch = batchedInsertContexts[0];
    auto measurementTimestamp = std::get<Date_t>(currentBatch.measurementsTimesAndIndices[0]);

    Bucket& bucketToInsertInto =
        internal::allocateBucket(*_bucketCatalog,
                                 *_bucketCatalog->stripes[currentBatch.stripeNumber],
                                 WithLock::withoutLock(),
                                 currentBatch.key,
                                 currentBatch.options,
                                 measurementTimestamp,
                                 nullptr,
                                 currentBatch.stats);

    size_t currentPosition = 0;
    size_t currentPositionFromNumMeasurementsInBatch = 0;
    auto& stripe = *_bucketCatalog->stripes[currentBatch.stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};
    auto writeBatch = activeBatch(_bucketCatalog->trackingContexts,
                                  bucketToInsertInto,
                                  _opCtx->getOpID(),
                                  currentBatch.stripeNumber,
                                  currentBatch.stats);
    auto successfulInsertion =
        internal::stageInsertBatchIntoEligibleBucket(*_bucketCatalog,
                                                     _opCtx->getOpID(),
                                                     bucketsColl->getDefaultCollator(),
                                                     currentBatch,
                                                     stripe,
                                                     stripeLock,
                                                     _storageCacheSizeBytes,
                                                     bucketToInsertInto,
                                                     currentPosition,
                                                     writeBatch);
    currentPositionFromNumMeasurementsInBatch += numMeasurementsInWriteBatch[0];
    ASSERT_EQ(currentPosition, currentPositionFromNumMeasurementsInBatch);

    if (numMeasurementsInWriteBatch.size() == 1) {
        ASSERT(successfulInsertion);
        ASSERT_EQ(currentPosition, batchOfMeasurements.size());
        ASSERT_EQ(bucketToInsertInto.numMeasurements, batchOfMeasurements.size());
        ASSERT_EQ(currentPositionFromNumMeasurementsInBatch, batchOfMeasurements.size());
        return;
    }

    // We do the first insertion outside of the loop so we don't rollover/allocate an additional
    // bucket if we don't need to.
    for (size_t i = 1; i < numMeasurementsInWriteBatch.size(); i++) {
        auto prevBatch = batchedInsertContexts[currBatchedInsertContextsIndex[i - 1]];
        // Let's rollover our first bucket and finish inserting the batch with another call into
        // the helper.
        auto currentMeasurementTime =
            std::get<Date_t>(currentBatch.measurementsTimesAndIndices[currentPosition]);

        // We rollover with kSchemaChange regardless of the rollover reason. This will make our
        // stats inaccurate, but shouldn't impact testing stageInsertBatchIntoEligibleBucket itself.
        auto newBucketToInsertInto =
            &internal::rolloverAndAllocateBucket(*_bucketCatalog,
                                                 stripe,
                                                 stripeLock,
                                                 bucketToInsertInto,
                                                 prevBatch.key,
                                                 prevBatch.options,
                                                 RolloverReason::kSchemaChange,
                                                 currentMeasurementTime,
                                                 bucketsColl->getDefaultCollator(),
                                                 prevBatch.stats);
        auto newWriteBatch = activeBatch(_bucketCatalog->trackingContexts,
                                         *newBucketToInsertInto,
                                         _opCtx->getOpID(),
                                         currentBatch.stripeNumber,
                                         currentBatch.stats);
        auto currBatch = batchedInsertContexts[currBatchedInsertContextsIndex[i]];
        successfulInsertion =
            internal::stageInsertBatchIntoEligibleBucket(*_bucketCatalog,
                                                         _opCtx->getOpID(),
                                                         bucketsColl->getDefaultCollator(),
                                                         currBatch,
                                                         stripe,
                                                         stripeLock,
                                                         _storageCacheSizeBytes,
                                                         *newBucketToInsertInto,
                                                         currentPosition,
                                                         newWriteBatch);
        ASSERT_EQ(newBucketToInsertInto->numMeasurements, numMeasurementsInWriteBatch[i]);
        currentPositionFromNumMeasurementsInBatch += numMeasurementsInWriteBatch[i];
        ASSERT_EQ(currentPosition, currentPositionFromNumMeasurementsInBatch);

        if (i == (numMeasurementsInWriteBatch.size() - 1)) {
            ASSERT(successfulInsertion);
        } else {
            ASSERT(!successfulInsertion);
        }
    }
}

void BucketCatalogTest::_testStageInsertBatchIntoEligibleBucketWithMetaField(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& batchOfMeasurements,
    const std::vector<size_t>& currBatchedInsertContextsIndex,
    const std::vector<size_t>& numMeasurementsInWriteBatch,
    size_t numBatchedInsertContexts) const {
    _assertCollWithMetaField(ns, batchOfMeasurements);
    _testStageInsertBatchIntoEligibleBucket(ns,
                                            collectionUUID,
                                            batchOfMeasurements,
                                            currBatchedInsertContextsIndex,
                                            numMeasurementsInWriteBatch,
                                            numBatchedInsertContexts);
}

void BucketCatalogTest::_testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& batchOfMeasurements,
    const std::vector<size_t>& currBatchedInsertContextsIndex,
    const std::vector<size_t>& numMeasurementsInWriteBatch,
    size_t numBatchedInsertContexts) const {
    _assertCollWithoutMetaField(ns, batchOfMeasurements);
    _testStageInsertBatchIntoEligibleBucket(ns,
                                            collectionUUID,
                                            batchOfMeasurements,
                                            currBatchedInsertContextsIndex,
                                            numMeasurementsInWriteBatch,
                                            numBatchedInsertContexts);
}

void BucketCatalogTest::_testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
    const NamespaceString& ns,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& batchOfMeasurements,
    const std::vector<size_t>& currBatchedInsertContextsIndex,
    const std::vector<size_t>& numMeasurementsInWriteBatch,
    size_t numBatchedInsertContexts) const {
    _assertNoMetaFieldsInCollWithMetaField(ns, batchOfMeasurements);
    _testStageInsertBatchIntoEligibleBucket(ns,
                                            collectionUUID,
                                            batchOfMeasurements,
                                            currBatchedInsertContextsIndex,
                                            numMeasurementsInWriteBatch,
                                            numBatchedInsertContexts);
}

TEST_F(BucketCatalogTest, InsertIntoSameBucket) {
    auto result1 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch1 = get<SuccessfulInsertion>(result1).batch;

    // A subsequent insert into the same bucket should land in the same batch.
    auto result2 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch2 = get<SuccessfulInsertion>(result2).batch;
    ASSERT_EQ(batch1, batch2);

    // The batch hasn't actually been committed yet.
    ASSERT(!isWriteBatchFinished(*batch1));

    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));

    // Still not finished.
    ASSERT(!isWriteBatchFinished(*batch1));

    // The batch should contain both documents since they belong in the same bucket and happened
    // in the same commit epoch. Nothing else has been committed in this bucket yet.
    ASSERT_EQ(batch1->measurements.size(), 2);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0);

    // Once the commit has occurred, the waiter should be notified.
    finish(*_bucketCatalog, batch1);
    ASSERT(isWriteBatchFinished(*batch2));
    ASSERT_OK(getWriteBatchStatus(*batch2));
}

TEST_F(BucketCatalogTest, GetMetadataReturnsEmptyDocOnMissingBucket) {
    auto result =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch = get<SuccessfulInsertion>(result).batch;
    auto bucketId = batch->bucketId;
    abort(*_bucketCatalog, batch, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT_BSONOBJ_EQ(BSONObj(), getMetadata(*_bucketCatalog, bucketId));
}

TEST_F(BucketCatalogTest, InsertIntoDifferentBuckets) {
    auto result1 = _insertOneHelper(_opCtx,
                                    *_bucketCatalog,
                                    _ns1,
                                    _uuid1,
                                    BSON(_timeField << Date_t::now() << _metaField << "123"));
    auto result2 = _insertOneHelper(_opCtx,
                                    *_bucketCatalog,
                                    _ns1,
                                    _uuid1,
                                    BSON(_timeField << Date_t::now() << _metaField << BSONObj()));
    auto result3 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns2, _uuid2, BSON(_timeField << Date_t::now()));

    // Inserts should all be into three distinct buckets (and therefore batches).
    ASSERT_NE(get<SuccessfulInsertion>(result1).batch, get<SuccessfulInsertion>(result2).batch);
    ASSERT_NE(get<SuccessfulInsertion>(result1).batch, get<SuccessfulInsertion>(result3).batch);
    ASSERT_NE(get<SuccessfulInsertion>(result2).batch, get<SuccessfulInsertion>(result3).batch);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << "123"),
        getMetadata(*_bucketCatalog, get<SuccessfulInsertion>(result1).batch->bucketId));
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << BSONObj()),
        getMetadata(*_bucketCatalog, get<SuccessfulInsertion>(result2).batch->bucketId));
    ASSERT(
        getMetadata(*_bucketCatalog, get<SuccessfulInsertion>(result3).batch->bucketId).isEmpty());

    // Committing one bucket should only return the one document in that bucket and should not
    // affect the other bucket.
    _commit(_ns1, get<SuccessfulInsertion>(result1).batch, 0);
    _commit(_ns1, get<SuccessfulInsertion>(result2).batch, 0);
    _commit(_ns2, get<SuccessfulInsertion>(result3).batch, 0);
}

TEST_F(BucketCatalogTest, InsertThroughDifferentCatalogsIntoDifferentBuckets) {
    BucketCatalog temporaryBucketCatalog(/*numberOfStripes=*/1,
                                         getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes);
    auto result1 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch1 = get<SuccessfulInsertion>(result1).batch;

    auto result2 = _insertOneHelper(
        _opCtx, temporaryBucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch2 = get<SuccessfulInsertion>(result2).batch;

    // Inserts should be into different buckets (and therefore batches) because they went through
    // different bucket catalogs.
    ASSERT_NE(batch1, batch2);

    // Committing one bucket should only return the one document in that bucket and should not
    // affect the other bucket.
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));
    ASSERT_EQ(batch1->measurements.size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0);
    finish(*_bucketCatalog, batch1);

    ASSERT_OK(prepareCommit(temporaryBucketCatalog, batch2, _getCollator(_ns2)));
    ASSERT_EQ(batch2->measurements.size(), 1);
    ASSERT_EQ(batch2->numPreviouslyCommittedMeasurements, 0);
    finish(temporaryBucketCatalog, batch2);
}

TEST_F(BucketCatalogTest, InsertIntoSameBucketArray) {
    auto insertContextAndTime1 = uassertStatusOK(prepareInsert(
        *_bucketCatalog,
        _uuid1,
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField << BSON_ARRAY(BSON("a" << 0 << "b" << 1)))));

    auto insertContextAndTime2 = uassertStatusOK(prepareInsert(
        *_bucketCatalog,
        _uuid1,
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField << BSON_ARRAY(BSON("b" << 1 << "a" << 0)))));

    // Check metadata in buckets.
    ASSERT_EQ(std::get<InsertContext>(insertContextAndTime1),
              std::get<InsertContext>(insertContextAndTime2));
}

TEST_F(BucketCatalogTest, InsertIntoSameBucketObjArray) {
    auto insertContextAndTime1 = uassertStatusOK(prepareInsert(
        *_bucketCatalog,
        _uuid1,
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField
                        << BSONObj(BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1)
                                                          << BSON("f" << 1 << "g" << 0)))))));

    auto insertContextAndTime2 = uassertStatusOK(prepareInsert(
        *_bucketCatalog,
        _uuid1,
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField
                        << BSONObj(BSON("c" << BSON_ARRAY(BSON("b" << 1 << "a" << 0)
                                                          << BSON("g" << 0 << "f" << 1)))))));

    // Check metadata in buckets.
    ASSERT_EQ(std::get<InsertContext>(insertContextAndTime1),
              std::get<InsertContext>(insertContextAndTime2));
}


TEST_F(BucketCatalogTest, InsertIntoSameBucketNestedArray) {
    auto insertContextAndTime1 = uassertStatusOK(prepareInsert(
        *_bucketCatalog,
        _uuid1,
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField
                        << BSONObj(BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1)
                                                          << BSON_ARRAY("123" << "456")))))));

    auto insertContextAndTime2 = uassertStatusOK(prepareInsert(
        *_bucketCatalog,
        _uuid1,
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField
                        << BSONObj(BSON("c" << BSON_ARRAY(BSON("b" << 1 << "a" << 0)
                                                          << BSON_ARRAY("123" << "456")))))));

    // Check metadata in buckets.
    ASSERT_EQ(std::get<InsertContext>(insertContextAndTime1),
              std::get<InsertContext>(insertContextAndTime2));
}

TEST_F(BucketCatalogTest, InsertNullAndMissingMetaFieldIntoDifferentBuckets) {
    auto insertContextAndTime1 =
        uassertStatusOK(prepareInsert(*_bucketCatalog,
                                      _uuid1,
                                      _getTimeseriesOptions(_ns1),
                                      BSON(_timeField << Date_t::now() << _metaField << BSONNULL)));

    auto insertContextAndTime2 = uassertStatusOK(prepareInsert(
        *_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), BSON(_timeField << Date_t::now())));


    // Inserts should all be into two distinct buckets.
    ASSERT_NE(std::get<InsertContext>(insertContextAndTime1),
              std::get<InsertContext>(insertContextAndTime2));

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSONNULL),
                      std::get<InsertContext>(insertContextAndTime1).key.metadata.toBSON());
    ASSERT(std::get<InsertContext>(insertContextAndTime2).key.metadata.toBSON().isEmpty());
}

TEST_F(BucketCatalogTest, NumCommittedMeasurementsAccumulates) {
    // The numCommittedMeasurements returned when committing should accumulate as more entries in
    // the bucket are committed.
    _insertOneAndCommit(_ns1, _uuid1, 0);
    _insertOneAndCommit(_ns1, _uuid1, 1);

    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumActiveBuckets));
}

TEST_F(BucketCatalogTest, ClearNamespaceBuckets) {
    _insertOneAndCommit(_ns1, _uuid1, 0);
    _insertOneAndCommit(_ns2, _uuid2, 0);
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumActiveBuckets));
    ASSERT_EQ(1, _getExecutionStat(_uuid2, kNumActiveBuckets));

    // Clearing the UUID does not immediately remove tracking from the bucket catalog as this is
    // async.
    clear(*_bucketCatalog, _uuid1);
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumActiveBuckets));
    ASSERT_EQ(1, _getExecutionStat(_uuid2, kNumActiveBuckets));

    _insertOneAndCommit(_ns1, _uuid1, 0);
    _insertOneAndCommit(_ns2, _uuid2, 1);
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumActiveBuckets));
    ASSERT_EQ(1, _getExecutionStat(_uuid2, kNumActiveBuckets));
}

TEST_F(BucketCatalogTest, DropNamespaceBuckets) {
    _insertOneAndCommit(_ns1, _uuid1, 0);
    _insertOneAndCommit(_ns2, _uuid2, 0);
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumActiveBuckets));
    ASSERT_EQ(1, _getExecutionStat(_uuid2, kNumActiveBuckets));

    // Dropping the UUID immediately remove tracking from the bucket catalog.
    drop(*_bucketCatalog, _uuid1);
    ASSERT_EQ(0, _getExecutionStat(_uuid1, kNumActiveBuckets));
    ASSERT_EQ(1, _getExecutionStat(_uuid2, kNumActiveBuckets));
}

TEST_F(BucketCatalogTest, InsertBetweenPrepareAndFinish) {
    auto result1 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch1 = get<SuccessfulInsertion>(result1).batch;
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));
    ASSERT_EQ(batch1->measurements.size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0);

    // Insert before finish so there's a second batch live at the same time.
    auto result2 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch2 = get<SuccessfulInsertion>(result2).batch;
    ASSERT_NE(batch1, batch2);

    (void)write_ops_utils::makeTimeseriesInsertOp(batch1,
                                                  _ns1.makeTimeseriesBucketsNamespace(),
                                                  getMetadata(*_bucketCatalog, batch1->bucketId));
    finish(*_bucketCatalog, batch1);
    ASSERT(isWriteBatchFinished(*batch1));

    // Verify the second batch still commits one doc, and that the first batch only commited one.
    _commit(_ns1, batch2, 1);
}

TEST_F(BucketCatalogTest, GetMetadataReturnsEmptyDoc) {
    auto result = _insertOneHelper(
        _opCtx, *_bucketCatalog, _nsNoMeta, _uuidNoMeta, BSON(_timeField << Date_t::now()));
    auto batch = get<SuccessfulInsertion>(result).batch;

    ASSERT_BSONOBJ_EQ(BSONObj(), getMetadata(*_bucketCatalog, batch->bucketId));

    _commit(_nsNoMeta, batch, 0);
}

TEST_F(BucketCatalogTest, CommitReturnsNewFields) {
    // Creating a new bucket should return all fields from the initial measurement.
    auto result = _insertOneHelper(_opCtx,
                                   *_bucketCatalog,
                                   _nsNoMeta,
                                   _uuidNoMeta,
                                   BSON(_timeField << Date_t::now() << "a" << 0));
    auto batch = get<SuccessfulInsertion>(result).batch;
    auto oldId = batch->bucketId;
    _commit(_nsNoMeta, batch, 0);
    ASSERT_EQ(2U, batch->newFieldNamesToBeInserted.size()) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted.count(_timeField)) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted.count("a")) << batch->toBSON();

    // Inserting a new measurement with the same fields should return an empty set of new fields.
    result = _insertOneHelper(_opCtx,
                              *_bucketCatalog,
                              _nsNoMeta,
                              _uuidNoMeta,
                              BSON(_timeField << Date_t::now() << "a" << 1));
    batch = get<SuccessfulInsertion>(result).batch;
    _commit(_nsNoMeta, batch, 1);
    ASSERT_EQ(0U, batch->newFieldNamesToBeInserted.size()) << batch->toBSON();

    // Insert a new measurement with the a new field.
    result = _insertOneHelper(_opCtx,
                              *_bucketCatalog,
                              _nsNoMeta,
                              _uuidNoMeta,
                              BSON(_timeField << Date_t::now() << "a" << 2 << "b" << 2));
    batch = get<SuccessfulInsertion>(result).batch;
    _commit(_nsNoMeta, batch, 2);
    ASSERT_EQ(1U, batch->newFieldNamesToBeInserted.size()) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted.count("b")) << batch->toBSON();

    // Fill up the bucket.
    for (auto i = 3; i < gTimeseriesBucketMaxCount; ++i) {
        result = _insertOneHelper(_opCtx,
                                  *_bucketCatalog,
                                  _nsNoMeta,
                                  _uuidNoMeta,
                                  BSON(_timeField << Date_t::now() << "a" << i));
        batch = get<SuccessfulInsertion>(result).batch;
        _commit(_nsNoMeta, batch, i);
        ASSERT_EQ(0U, batch->newFieldNamesToBeInserted.size()) << i << ":" << batch->toBSON();
    }

    // When a bucket overflows, committing to the new overflow bucket should return the fields of
    // the first measurement as new fields.
    auto result2 =
        _insertOneHelper(_opCtx,
                         *_bucketCatalog,
                         _nsNoMeta,
                         _uuidNoMeta,
                         BSON(_timeField << Date_t::now() << "a" << gTimeseriesBucketMaxCount));
    auto& batch2 = get<SuccessfulInsertion>(result2).batch;
    ASSERT_NE(oldId, batch2->bucketId);
    _commit(_nsNoMeta, batch2, 0);
    ASSERT_EQ(2U, batch2->newFieldNamesToBeInserted.size()) << batch2->toBSON();
    ASSERT(batch2->newFieldNamesToBeInserted.count(_timeField)) << batch2->toBSON();
    ASSERT(batch2->newFieldNamesToBeInserted.count("a")) << batch2->toBSON();
}

TEST_F(BucketCatalogTest, AbortBatchOnBucketWithPreparedCommit) {
    auto result1 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch1 = get<SuccessfulInsertion>(result1).batch;
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));
    ASSERT_EQ(batch1->measurements.size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0);

    // Insert before finish so there's a second batch live at the same time.
    auto result2 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch2 = get<SuccessfulInsertion>(result2).batch;
    ASSERT_NE(batch1, batch2);

    abort(*_bucketCatalog, batch2, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(isWriteBatchFinished(*batch2));
    ASSERT_EQ(getWriteBatchStatus(*batch2), ErrorCodes::TimeseriesBucketCleared);

    finish(*_bucketCatalog, batch1);
    ASSERT(isWriteBatchFinished(*batch1));
    ASSERT_OK(getWriteBatchStatus(*batch1));
}

TEST_F(BucketCatalogTest, ClearNamespaceWithConcurrentWrites) {
    auto result =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch = get<SuccessfulInsertion>(result).batch;

    clear(*_bucketCatalog, _uuid1);

    ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchStatus(*batch), ErrorCodes::TimeseriesBucketCleared);

    result =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    batch = get<SuccessfulInsertion>(result).batch;
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT_EQ(batch->measurements.size(), 1);
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 0);

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
    auto result =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch = get<SuccessfulInsertion>(result).batch;
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT_EQ(batch->measurements.size(), 1);
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 0);

    ASSERT_THROWS(directWriteStart(_bucketCatalog->bucketStateRegistry, batch->bucketId),
                  WriteConflictException);

    abort(*_bucketCatalog, batch, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchStatus(*batch), ErrorCodes::TimeseriesBucketCleared);
}

TEST_F(BucketCatalogTest, PrepareCommitOnClearedBatchWithAlreadyPreparedBatch) {
    auto result1 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch1 = get<SuccessfulInsertion>(result1).batch;
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));
    ASSERT_EQ(batch1->measurements.size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0);

    // Insert before clear so there's a second batch live at the same time.
    auto result2 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch2 = get<SuccessfulInsertion>(result2).batch;
    ASSERT_NE(batch1, batch2);
    ASSERT_EQ(batch1->bucketId, batch2->bucketId);

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
    auto result3 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch3 = get<SuccessfulInsertion>(result3).batch;
    ASSERT_NE(batch1, batch3);
    ASSERT_NE(batch2, batch3);
    ASSERT_NE(batch1->bucketId, batch3->bucketId);
    // Clean up this batch
    abort(*_bucketCatalog, batch3, {ErrorCodes::TimeseriesBucketCleared, ""});

    // Make sure we can finish the cleanly prepared batch.
    finish(*_bucketCatalog, batch1);
    ASSERT(isWriteBatchFinished(*batch1));
    ASSERT_OK(getWriteBatchStatus(*batch1));
}

TEST_F(BucketCatalogTest, PrepareCommitOnAlreadyAbortedBatch) {
    auto result =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch = get<SuccessfulInsertion>(result).batch;

    abort(*_bucketCatalog, batch, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchStatus(*batch), ErrorCodes::TimeseriesBucketCleared);

    ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchStatus(*batch), ErrorCodes::TimeseriesBucketCleared);
}

TEST_F(BucketCatalogTest, CannotConcurrentlyCommitBatchesForSameBucket) {
    auto result1 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch1 = get<SuccessfulInsertion>(result1).batch;

    auto result2 = _insertOneHelper(_makeOperationContext().second.get(),
                                    *_bucketCatalog,
                                    _ns1,
                                    _uuid1,
                                    BSON(_timeField << Date_t::now()));
    auto batch2 = get<SuccessfulInsertion>(result2).batch;

    // Batch 2 will not be able to commit until batch 1 has finished.
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));

    {
        auto task = RunBackgroundTaskAndWaitForFailpoint{
            "hangTimeSeriesBatchPrepareWaitingForConflictingOperation", [&]() {
                ASSERT_OK(prepareCommit(*_bucketCatalog, batch2, _getCollator(_ns2)));
            }};

        // Finish the first batch.
        finish(*_bucketCatalog, batch1);
        ASSERT(isWriteBatchFinished(*batch1));
    }

    finish(*_bucketCatalog, batch2);
    ASSERT(isWriteBatchFinished(*batch2));
}

TEST_F(BucketCatalogTest, AbortingBatchEnsuresBucketIsEventuallyClosed) {
    auto result1 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch1 = get<SuccessfulInsertion>(result1).batch;

    auto result2 = _insertOneHelper(_makeOperationContext().second.get(),
                                    *_bucketCatalog,
                                    _ns1,
                                    _uuid1,
                                    BSON(_timeField << Date_t::now()));
    auto batch2 = get<SuccessfulInsertion>(result2).batch;

    auto result3 = _insertOneHelper(_makeOperationContext().second.get(),
                                    *_bucketCatalog,
                                    _ns1,
                                    _uuid1,
                                    BSON(_timeField << Date_t::now()));
    auto batch3 = get<SuccessfulInsertion>(result3).batch;

    ASSERT_EQ(batch1->bucketId, batch2->bucketId);
    ASSERT_EQ(batch1->bucketId, batch3->bucketId);

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
    auto result4 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch4 = get<SuccessfulInsertion>(result4).batch;
    ASSERT_NE(batch2->bucketId, batch4->bucketId);
}

TEST_F(BucketCatalogTest, AbortingBatchEnsuresNewInsertsGoToNewBucket) {
    auto result1 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch1 = get<SuccessfulInsertion>(result1).batch;

    auto result2 = _insertOneHelper(_makeOperationContext().second.get(),
                                    *_bucketCatalog,
                                    _ns1,
                                    _uuid1,
                                    BSON(_timeField << Date_t::now()));
    auto batch2 = get<SuccessfulInsertion>(result2).batch;

    // Batch 1 and 2 use the same bucket.
    ASSERT_EQ(batch1->bucketId, batch2->bucketId);
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));

    // Batch 1 will be in a prepared state now. Abort the second batch so that bucket 1 will be
    // closed after batch 1 finishes.
    abort(*_bucketCatalog, batch2, Status{ErrorCodes::TimeseriesBucketCleared, "cleared"});
    finish(*_bucketCatalog, batch1);
    ASSERT(isWriteBatchFinished(*batch1));
    ASSERT(isWriteBatchFinished(*batch2));

    // Ensure a batch started after batch 2 aborts, does not insert future measurements into the
    // aborted batch/bucket.
    auto result3 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch3 = get<SuccessfulInsertion>(result3).batch;
    ASSERT_NE(batch1->bucketId, batch3->bucketId);
}

TEST_F(BucketCatalogTest, DuplicateNewFieldNamesAcrossConcurrentBatches) {
    auto result1 =
        _insertOneHelper(_opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << Date_t::now()));
    auto batch1 = get<SuccessfulInsertion>(result1).batch;

    auto result2 = _insertOneHelper(_makeOperationContext().second.get(),
                                    *_bucketCatalog,
                                    _ns1,
                                    _uuid1,
                                    BSON(_timeField << Date_t::now()));
    auto batch2 = get<SuccessfulInsertion>(result2).batch;

    // Batch 2 is the first batch to commit the time field.
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch2, _getCollator(_ns2)));
    ASSERT_EQ(batch2->newFieldNamesToBeInserted.size(), 1);
    ASSERT_EQ(batch2->newFieldNamesToBeInserted.begin()->first, _timeField);
    (void)write_ops_utils::makeTimeseriesInsertOp(batch2,
                                                  _ns1.makeTimeseriesBucketsNamespace(),
                                                  getMetadata(*_bucketCatalog, batch2->bucketId));
    finish(*_bucketCatalog, batch2);

    // Batch 1 was the first batch to insert the time field, but by commit time it was already
    // committed by batch 2.
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch1, _getCollator(_ns1)));
    ASSERT(batch1->newFieldNamesToBeInserted.empty());
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
    ASSERT_EQ(1, stats->numBucketReopeningsFailed.load());
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
    auto result =
        _insertOneHelper(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":-100,"b":100})"));

    ASSERT_EQ(0, _getExecutionStat(_uuid1, kNumSchemaChanges));

    auto batch = get<SuccessfulInsertion>(result).batch;
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT_EQ(batch->measurements.size(), 1);

    // The reopened bucket already contains three committed measurements.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 3);

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
    auto result =
        _insertOneHelper(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":{},"b":{}})"));

    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumSchemaChanges));

    auto batch = get<SuccessfulInsertion>(result).batch;
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));
    ASSERT_EQ(batch->measurements.size(), 1);

    // Since the reopened bucket was incompatible, we opened a new one.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 0);

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
    ASSERT_EQ(1, stats->numBucketReopeningsFailed.load());
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
    ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(),
                                compressedBucketDoc,
                                boost::none,
                                boost::none,
                                boost::none,
                                boost::optional<BucketDocumentValidator>{alwaysPassValidator}));
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
    auto result = _insertOneHelper(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << time << _metaField << "A"));
    auto batch = get<SuccessfulInsertion>(result).batch;
    ASSERT(batch);

    auto doc = BSON(_timeField << time << _metaField << "B");
    // Get the insertContext so that we can get the key signature for the BucketID — again, to
    // trigger a collision of our frozen bucketID with the bucketID of the bucket we will allocate.
    auto insertContext = std::get<InsertContext>(
        uassertStatusOK(prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), doc)));

    // Get the next sequential OID so that we can trigger an ID collision down the line.
    auto OIDAndRoundedTime = internal::generateBucketOID(time, insertContext.options);
    OID nextBucketOID = std::get<OID>(OIDAndRoundedTime);

    BucketId nextBucketId{_uuid1, nextBucketOID, insertContext.key.signature()};

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
    result = _insertOneHelper(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << time << _metaField << "B"));
    auto batch2 = get<SuccessfulInsertion>(result).batch;
    ASSERT_NE(nextBucketId, batch2->bucketId);
    // We should check that the bucketID that we failed to create is not stored in the stripe.
    ASSERT(!_bucketCatalog->stripes[0]->openBucketsById.contains(nextBucketId));
}

TEST_F(BucketCatalogTest, WriteConflictIfPrepareCommitOnClearedBucket) {
    auto time = Date_t::now();
    // Set up an insert to create a bucket.
    auto result = _insertOneHelper(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << time << _metaField << "A"));
    auto batch = get<SuccessfulInsertion>(result).batch;
    ASSERT(batch);

    directWriteStart(_bucketCatalog->bucketStateRegistry, batch->bucketId);
    directWriteFinish(_bucketCatalog->bucketStateRegistry, batch->bucketId);

    // Preparing fails on a cleared bucket and aborts the batch.
    auto status = prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1));
    ASSERT_EQ(status.code(), ErrorCodes::TimeseriesBucketCleared);
}

TEST_F(BucketCatalogTest, WriteConflictIfDirectWriteOnPreparedBucket) {
    auto time = Date_t::now();

    auto result = _insertOneHelper(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << time << _metaField << "A"));
    auto batch = get<SuccessfulInsertion>(result).batch;
    ASSERT_OK(prepareCommit(*_bucketCatalog, batch, _getCollator(_ns1)));

    // A direct write on a prepared bucket will throw a write conflict.
    ASSERT_THROWS_CODE(directWriteStart(_bucketCatalog->bucketStateRegistry, batch->bucketId),
                       DBException,
                       ErrorCodes::WriteConflict);
}

TEST_F(BucketCatalogTest, DirectWritesCanStack) {
    auto time = Date_t::now();
    auto result = _insertOneHelper(
        _opCtx, *_bucketCatalog, _ns1, _uuid1, BSON(_timeField << time << _metaField << "A"));

    // The batch can be used for both, as it is only used to obtain the bucketId.
    auto batch = get<SuccessfulInsertion>(result).batch;
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

TEST_F(BucketCatalogTest, UseBucketSkipsBucketWithDirectWrite) {
    _testUseBucketSkipsConflictingBucket([](BucketCatalog& catalog, Bucket& bucket) {
        directWriteStart(catalog.bucketStateRegistry, bucket.bucketId);
    });
}

TEST_F(BucketCatalogTest, UseBucketSkipsClearedBucket) {
    _testUseBucketSkipsConflictingBucket([](BucketCatalog& catalog, Bucket& bucket) {
        clear(catalog, bucket.bucketId.collectionUUID);
    });
}

TEST_F(BucketCatalogTest, UseBucketSkipsFrozenBucket) {
    _testUseBucketSkipsConflictingBucket([](BucketCatalog& catalog, Bucket& bucket) {
        freezeBucket(catalog.bucketStateRegistry, bucket.bucketId);
    });
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
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), _measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();

    Bucket& bucket = internal::allocateBucket(*_bucketCatalog,
                                              *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                              WithLock::withoutLock(),
                                              insertCtx.key,
                                              insertCtx.options,
                                              time,
                                              nullptr,
                                              insertCtx.stats);
    auto openBuckets = internal::findOpenBuckets(
        *_bucketCatalog->stripes[insertCtx.stripeNumber], WithLock::withoutLock(), insertCtx.key);
    ASSERT_EQ(1, openBuckets.size());
    ASSERT_EQ(&bucket, openBuckets[0]);
}

TEST_F(BucketCatalogTest, CheckBucketStateAndCleanupWithBucketWithDirectWrite) {
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), _measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();

    Bucket& bucket = internal::allocateBucket(*_bucketCatalog,
                                              *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                              WithLock::withoutLock(),
                                              insertCtx.key,
                                              insertCtx.options,
                                              time,
                                              nullptr,
                                              insertCtx.stats);

    directWriteStart(_bucketCatalog->bucketStateRegistry, bucket.bucketId);

    // Ineligible for inserts. Remove from 'openBucketsByKey'.
    ASSERT(internal::isBucketStateEligibleForInsertsAndCleanup(
               *_bucketCatalog,
               *_bucketCatalog->stripes[insertCtx.stripeNumber],
               WithLock::withoutLock(),
               &bucket) == internal::BucketStateForInsertAndCleanup::kInsertionConflict);
    ASSERT(_bucketCatalog->stripes[insertCtx.stripeNumber]->openBucketsByKey.empty());
}

TEST_F(BucketCatalogTest, CheckBucketStateAndCleanupWithClearedBucket) {
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), _measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();

    Bucket& bucket = internal::allocateBucket(*_bucketCatalog,
                                              *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                              WithLock::withoutLock(),
                                              insertCtx.key,
                                              insertCtx.options,
                                              time,
                                              nullptr,
                                              insertCtx.stats);

    clear(*_bucketCatalog, bucket.bucketId.collectionUUID);

    // Ineligible for inserts. Remove from 'openBucketsByKey'.
    ASSERT(internal::isBucketStateEligibleForInsertsAndCleanup(
               *_bucketCatalog,
               *_bucketCatalog->stripes[insertCtx.stripeNumber],
               WithLock::withoutLock(),
               &bucket) == internal::BucketStateForInsertAndCleanup::kInsertionConflict);
    ASSERT(_bucketCatalog->stripes[insertCtx.stripeNumber]->openBucketsByKey.empty());
}

TEST_F(BucketCatalogTest, CheckBucketStateAndCleanupWithFrozenBucket) {
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), _measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();

    Bucket& bucket = internal::allocateBucket(*_bucketCatalog,
                                              *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                              WithLock::withoutLock(),
                                              insertCtx.key,
                                              insertCtx.options,
                                              time,
                                              nullptr,
                                              insertCtx.stats);

    freezeBucket(_bucketCatalog->bucketStateRegistry, bucket.bucketId);

    // Ineligible for inserts. Remove from 'openBucketsByKey'.
    ASSERT(internal::isBucketStateEligibleForInsertsAndCleanup(
               *_bucketCatalog,
               *_bucketCatalog->stripes[insertCtx.stripeNumber],
               WithLock::withoutLock(),
               &bucket) == internal::BucketStateForInsertAndCleanup::kInsertionConflict);
    ASSERT(_bucketCatalog->stripes[insertCtx.stripeNumber]->openBucketsByKey.empty());
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsOpen) {
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), _measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();
    auto bucketOpenedDueToMetadata = true;

    Bucket& bucket = internal::allocateBucket(*_bucketCatalog,
                                              *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                              WithLock::withoutLock(),
                                              insertCtx.key,
                                              insertCtx.options,
                                              time,
                                              nullptr,
                                              insertCtx.stats);

    AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
    auto potentialBuckets =
        findAndRolloverOpenBuckets(*_bucketCatalog,
                                   *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                   WithLock::withoutLock(),
                                   insertCtx.key,
                                   time,
                                   Seconds(*insertCtx.options.getBucketMaxSpanSeconds()),
                                   allowQueryBasedReopening,
                                   bucketOpenedDueToMetadata);
    ASSERT(!bucketOpenedDueToMetadata);
    ASSERT_EQ(1, potentialBuckets.size());
    ASSERT_EQ(&bucket, potentialBuckets[0]);
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsSoftClose) {
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), _measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();
    auto bucketOpenedDueToMetadata = true;

    Bucket& bucket = internal::allocateBucket(*_bucketCatalog,
                                              *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                              WithLock::withoutLock(),
                                              insertCtx.key,
                                              insertCtx.options,
                                              time,
                                              nullptr,
                                              insertCtx.stats);

    bucket.rolloverReason = RolloverReason::kTimeForward;
    AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
    auto potentialBuckets =
        findAndRolloverOpenBuckets(*_bucketCatalog,
                                   *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                   WithLock::withoutLock(),
                                   insertCtx.key,
                                   time,
                                   Seconds(*insertCtx.options.getBucketMaxSpanSeconds()),
                                   allowQueryBasedReopening,
                                   bucketOpenedDueToMetadata);
    ASSERT(!bucketOpenedDueToMetadata);
    ASSERT_EQ(1, potentialBuckets.size());
    ASSERT_EQ(&bucket, potentialBuckets[0]);
    ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kAllow);
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsSoftCloseNotSelected) {
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), _measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();
    auto bucketOpenedDueToMetadata = true;

    Bucket& bucket = internal::allocateBucket(*_bucketCatalog,
                                              *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                              WithLock::withoutLock(),
                                              insertCtx.key,
                                              insertCtx.options,
                                              time,
                                              nullptr,
                                              insertCtx.stats);

    bucket.rolloverReason = RolloverReason::kTimeForward;
    AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
    auto potentialBuckets =
        findAndRolloverOpenBuckets(*_bucketCatalog,
                                   *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                   WithLock::withoutLock(),
                                   insertCtx.key,
                                   time + Hours(2),
                                   Seconds(*insertCtx.options.getBucketMaxSpanSeconds()),
                                   allowQueryBasedReopening,
                                   bucketOpenedDueToMetadata);
    ASSERT(!bucketOpenedDueToMetadata);
    ASSERT_EQ(0, potentialBuckets.size());
    ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kDisallow);
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsArchive) {
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), _measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();
    auto bucketOpenedDueToMetadata = true;

    Bucket& bucket = internal::allocateBucket(*_bucketCatalog,
                                              *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                              WithLock::withoutLock(),
                                              insertCtx.key,
                                              insertCtx.options,
                                              time,
                                              nullptr,
                                              insertCtx.stats);

    bucket.rolloverReason = RolloverReason::kTimeBackward;
    AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
    auto potentialBuckets =
        findAndRolloverOpenBuckets(*_bucketCatalog,
                                   *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                   WithLock::withoutLock(),
                                   insertCtx.key,
                                   time,
                                   Seconds(*insertCtx.options.getBucketMaxSpanSeconds()),
                                   allowQueryBasedReopening,
                                   bucketOpenedDueToMetadata);
    ASSERT(!bucketOpenedDueToMetadata);
    ASSERT_EQ(1, potentialBuckets.size());
    ASSERT_EQ(&bucket, potentialBuckets[0]);
    ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kAllow);
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsHardClose) {
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), _measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();
    std::vector<RolloverReason> allHardClosedRolloverReasons = {RolloverReason::kCount,
                                                                RolloverReason::kSchemaChange,
                                                                RolloverReason::kCachePressure,
                                                                RolloverReason::kSize};

    for (size_t i = 0; i < allHardClosedRolloverReasons.size(); i++) {
        auto bucketOpenedDueToMetadata = true;
        Bucket& bucket = internal::allocateBucket(*_bucketCatalog,
                                                  *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                                  WithLock::withoutLock(),
                                                  insertCtx.key,
                                                  insertCtx.options,
                                                  time,
                                                  nullptr,
                                                  insertCtx.stats);

        bucket.rolloverReason = allHardClosedRolloverReasons[i];
        AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
        auto potentialBuckets =
            findAndRolloverOpenBuckets(*_bucketCatalog,
                                       *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                       WithLock::withoutLock(),
                                       insertCtx.key,
                                       time,
                                       Seconds(*insertCtx.options.getBucketMaxSpanSeconds()),
                                       allowQueryBasedReopening,
                                       bucketOpenedDueToMetadata);
        ASSERT(!bucketOpenedDueToMetadata);
        ASSERT_EQ(0, potentialBuckets.size());
        ASSERT(_bucketCatalog->stripes[insertCtx.stripeNumber]->openBucketsByKey.empty());
        ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kAllow);
    }
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsUncommitted) {
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), _measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();
    std::vector<RolloverReason> allHardClosedRolloverReasons = {RolloverReason::kCount,
                                                                RolloverReason::kSchemaChange,
                                                                RolloverReason::kCachePressure,
                                                                RolloverReason::kSize};

    for (size_t i = 0; i < allHardClosedRolloverReasons.size(); i++) {
        auto bucketOpenedDueToMetadata = true;
        Bucket& bucket = internal::allocateBucket(*_bucketCatalog,
                                                  *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                                  WithLock::withoutLock(),
                                                  insertCtx.key,
                                                  insertCtx.options,
                                                  time,
                                                  nullptr,
                                                  insertCtx.stats);

        bucket.rolloverReason = allHardClosedRolloverReasons[i];
        std::shared_ptr<WriteBatch> batch;
        auto opId = 0;
        bucket.batches.emplace(opId, batch);
        AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
        auto potentialBuckets =
            findAndRolloverOpenBuckets(*_bucketCatalog,
                                       *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                       WithLock::withoutLock(),
                                       insertCtx.key,
                                       time,
                                       Seconds(*insertCtx.options.getBucketMaxSpanSeconds()),
                                       allowQueryBasedReopening,
                                       bucketOpenedDueToMetadata);

        // No results returned. Do not close the bucket because of uncommitted batches.
        ASSERT(!bucketOpenedDueToMetadata);
        ASSERT_EQ(0, potentialBuckets.size());
        ASSERT(!_bucketCatalog->stripes[insertCtx.stripeNumber]->openBucketsByKey.empty());
        ASSERT_EQ(allowQueryBasedReopening, AllowQueryBasedReopening::kAllow);
    }
}

TEST_F(BucketCatalogTest, FindAndRolloverOpenBucketsOrder) {
    auto swResult =
        prepareInsert(*_bucketCatalog, _uuid1, _getTimeseriesOptions(_ns1), _measurement);
    ASSERT_OK(swResult);
    auto& [insertCtx, time] = swResult.getValue();
    auto bucketOpenedDueToMetadata = true;

    Bucket& bucket1 = internal::allocateBucket(*_bucketCatalog,
                                               *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                               WithLock::withoutLock(),
                                               insertCtx.key,
                                               insertCtx.options,
                                               time,
                                               nullptr,
                                               insertCtx.stats);
    bucket1.rolloverReason = RolloverReason::kTimeBackward;

    Bucket& bucket2 = internal::allocateBucket(*_bucketCatalog,
                                               *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                               WithLock::withoutLock(),
                                               insertCtx.key,
                                               insertCtx.options,
                                               time,
                                               nullptr,
                                               insertCtx.stats);

    AllowQueryBasedReopening allowQueryBasedReopening = AllowQueryBasedReopening::kAllow;
    auto potentialBuckets =
        findAndRolloverOpenBuckets(*_bucketCatalog,
                                   *_bucketCatalog->stripes[insertCtx.stripeNumber],
                                   WithLock::withoutLock(),
                                   insertCtx.key,
                                   time,
                                   Seconds(*insertCtx.options.getBucketMaxSpanSeconds()),
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
    std::vector<timeseries::write_ops::internal::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts =
        write_ops::internal::buildBatchedInsertContexts(*_bucketCatalog,
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
    auto era = getCurrentEra(_bucketCatalog->bucketStateRegistry);

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
                                         era,
                                         _storageCacheSizeBytes,
                                         /*compressAndWriteBucketFunc=*/nullptr,
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
    std::vector<timeseries::write_ops::internal::WriteStageErrorAndIndex> errorsAndIndices;
    auto batchedInsertContexts =
        write_ops::internal::buildBatchedInsertContexts(*_bucketCatalog,
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
    auto era = getCurrentEra(_bucketCatalog->bucketStateRegistry);

    Bucket& bucketAllocated =
        internal::allocateBucket(*_bucketCatalog,
                                 *_bucketCatalog->stripes[batchedInsertCtx.stripeNumber],
                                 WithLock::withoutLock(),
                                 batchedInsertCtx.key,
                                 batchedInsertCtx.options,
                                 measurementTimestamp,
                                 /*comparator=*/nullptr,
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
                              era,
                              _storageCacheSizeBytes,
                              /*compressAndWriteBucketFunc=*/nullptr,
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

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketFillsUpSingleBucket) {
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

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    std::vector<BSONObj> batchOfMeasurementsTimeseriesBucketMaxCountNoMetaField =
        _generateMeasurementsWithRolloverReason(
            {.reason = RolloverReason::kNone, .metaValue = boost::none});
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsTimeseriesBucketMaxCountNoMetaField,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsTimeseriesBucketMaxCountNoMetaField,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverkCount) {
    std::vector<BSONObj> batchOfMeasurementsWithCount =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kCount});

    // batchOfMeasurements will be 2 * gTimeseriesBucketMaxCount measurements with all the
    // measurements having the same meta field and time, which means we should have two buckets.
    std::vector<size_t> numMeasurementsInWriteBatch{static_cast<size_t>(gTimeseriesBucketMaxCount),
                                                    static_cast<size_t>(gTimeseriesBucketMaxCount)};
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithCount,
                                                         currBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1);

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    std::vector<BSONObj> batchOfMeasurementsWithCountNoMetaField =
        _generateMeasurementsWithRolloverReason(
            {.reason = RolloverReason::kCount, .metaValue = boost::none});
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithCountNoMetaField,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithCountNoMetaField,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverkTimeForward) {
    // Max bucket size with only the last measurement having kTimeForward.
    std::vector<BSONObj> batchOfMeasurementsWithTimeForwardAtEnd =
        _generateMeasurementsWithRolloverReason(
            {.reason = RolloverReason::kTimeForward, .metaValue = _metaValue});

    // The last measurement in batchOfMeasurementsWithTimeForwardAtEnd will have a timestamp outside
    // of the bucket range encompassing the previous measurements, which means that this
    // measurement will be in a different bucket.
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

    // Max bucket size with measurements[1:gTimeseriesBucketMaxCount] having kTimeForward.
    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    auto batchOfMeasurementsWithTimeForwardAfterFirstMeasurementNoMetaField =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kTimeForward,
                                                 .idxWithDiffMeasurement = 1,
                                                 .metaValue = boost::none});
    std::vector<size_t> numMeasurementsInWriteBatchAfterFirstMeasurement{
        1, static_cast<size_t>(gTimeseriesBucketMaxCount - 1)};
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithTimeForwardAfterFirstMeasurementNoMetaField,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchAfterFirstMeasurement,
        /*numBatchedInsertContexts=*/1);

    // 50 measurements with measurements[25:50] having kTimeForward.
    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    std::vector<size_t> numMeasurementsInWriteBatchInMiddle{25, 25};
    auto batchOfMeasurementsWithTimeForwardInMiddle =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kTimeForward,
                                                 .numMeasurements = 50,
                                                 .idxWithDiffMeasurement = 25,
                                                 .metaValue = boost::none});
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithTimeForwardInMiddle,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchInMiddle,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverkSchemaChange) {
    // Max bucket size with only the last measurement having kSchemaChange.
    auto batchOfMeasurementsWithSchemaChangeAtEnd = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kSchemaChange, .metaValue = _metaValue});

    // The last measurement in batchOfMeasurementsWithSchemaChangeAtEnd will have a int rather
    // than a string for field "deathGrips", which means this measurement will be in a different
    // bucket.
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

    // Max bucket size with measurements[1:gTimeseriesBucketMaxCount] having kSchemaChange.
    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    auto batchOfMeasurementsWithSchemaChangeAfterFirstMeasurementNoMetaField =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSchemaChange,
                                                 .idxWithDiffMeasurement = 1,
                                                 .metaValue = boost::none});
    std::vector<size_t> numMeasurementsInWriteBatchAfterFirstMeasurement{
        1, static_cast<size_t>(gTimeseriesBucketMaxCount - 1)};
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithSchemaChangeAfterFirstMeasurementNoMetaField,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchAfterFirstMeasurement,
        /*numBatchedInsertContexts=*/1);

    // 50 measurements with measurements[25:50] having kSchemaChange.
    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    auto batchOfMeasurementsWithTimeForwardInMiddle =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSchemaChange,
                                                 .numMeasurements = 50,
                                                 .idxWithDiffMeasurement = 25,
                                                 .metaValue = boost::none});
    std::vector<size_t> numMeasurementsInWriteBatchInMiddle{25, 25};
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithTimeForwardInMiddle,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatchInMiddle,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverkSize) {
    std::vector<BSONObj> batchOfMeasurementsWithSize =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kSize});

    // The last measurement will exceed the size that the bucket can store. We will trigger kSize,
    // so the measurement will be in a different bucket.
    std::vector<size_t> numMeasurementsInWriteBatch{124, 1};
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithSize,
                                                         currBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1);

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    std::vector<BSONObj> batchOfMeasurementsWithSizeNoMetaField =
        _generateMeasurementsWithRolloverReason(
            {.reason = RolloverReason::kSize, .metaValue = boost::none});
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithSizeNoMetaField,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithSizeNoMetaField,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverkCachePressure) {
    // Artificially lower _storageCacheSizeBytes so we can simulate kCachePressure.
    _storageCacheSizeBytes = kLimitedStorageCacheSizeBytes;

    std::vector<BSONObj> batchOfMeasurementsWithCachePressure =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kCachePressure});

    // The last measurement will exceed the size that the bucket can store. Coupled with the
    // lowered cache size, we will trigger kCachePressure, so the measurement will be in a
    // different bucket.
    std::vector<size_t> numMeasurementsInWriteBatch{3, 1};
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0};

    // Inserting a batch of measurements with meta field values into a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         batchOfMeasurementsWithCachePressure,
                                                         currBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1);

    std::vector<BSONObj> batchOfMeasurementsWithCachePressureNoMetaField =
        _generateMeasurementsWithRolloverReason(
            {.reason = RolloverReason::kCachePressure, .metaValue = boost::none});

    // Inserting a batch of measurements without meta field values into a collection without a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketInCollWithoutMetaField(
        _nsNoMeta,
        _uuidNoMeta,
        batchOfMeasurementsWithCachePressureNoMetaField,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);

    // Inserting a batch of measurements without meta field values into a collection with a meta
    // field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        batchOfMeasurementsWithCachePressureNoMetaField,
        currBatchedInsertContextsIndex,
        numMeasurementsInWriteBatch,
        /*numBatchedInsertContexts=*/1);

    // Reset _storageCacheSizeBytes back to a representative value.
    _storageCacheSizeBytes = kDefaultStorageCacheSizeBytes;
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverMixed) {
    auto batchOfMeasurementsWithCount = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kCount, .timeValue = Date_t::now()});

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
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0, 0, 0, 0};

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
                                                                  currBatchedInsertContextsIndex,
                                                                  numMeasurementsInWriteBatch,
                                                                  /*numBatchedInsertContexts=*/1);

    // Test in a collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         mixedRolloverReasonsMeasurements,
                                                         currBatchedInsertContextsIndex,
                                                         numMeasurementsInWriteBatch,
                                                         /*numBatchedInsertContexts=*/1);
}

TEST_F(BucketCatalogTest, StageInsertBatchIntoEligibleBucketHandlesRolloverMixedWithNoMeta) {
    auto batchOfMeasurementsWithSize = _generateMeasurementsWithRolloverReason(
        {.reason = RolloverReason::kSize, .metaValue = boost::none, .timeValue = Date_t::now()});

    std::vector<BSONObj> batchOfMeasurements =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kNone,
                                                 .numMeasurements = 50,
                                                 .metaValue = boost::none,
                                                 .timeValue = Date_t::now() + Seconds(1)});

    auto batchOfMeasurementsWithTimeForward =
        _generateMeasurementsWithRolloverReason({.reason = RolloverReason::kTimeForward,
                                                 .idxWithDiffMeasurement = 1,
                                                 .metaValue = boost::none,
                                                 .timeValue = Date_t::now() + Seconds(2)});

    std::vector<BSONObj> mixedRolloverReasonsMeasurements =
        _getFlattenedVector(std::vector<std::vector<BSONObj>>{
            batchOfMeasurementsWithSize, batchOfMeasurements, batchOfMeasurementsWithTimeForward});
    ASSERT_EQ(mixedRolloverReasonsMeasurements.size(),
              batchOfMeasurementsWithSize.size() + batchOfMeasurements.size() +
                  batchOfMeasurementsWithTimeForward.size());

    std::vector<size_t> numWriteBatches{3};
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0, 0};

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
                                                                  currBatchedInsertContextsIndex,
                                                                  numMeasurementsInWriteBatch,
                                                                  /*numBatchedInsertContexts=*/1);

    // Test in collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithoutMetaFieldInCollWithMetaField(
        _ns1,
        _uuid1,
        mixedRolloverReasonsMeasurements,
        currBatchedInsertContextsIndex,
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
    std::vector<size_t> currBatchedInsertContextsIndex{0, 0, 0};

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
                                                                  currBatchedInsertContextsIndex,
                                                                  numMeasurementsInWriteBatch,
                                                                  /*numBatchedInsertContexts=*/1);

    // Test in collection with a meta field.
    _testStageInsertBatchIntoEligibleBucketWithMetaField(_ns1,
                                                         _uuid1,
                                                         mixedRolloverReasonsMeasurements,
                                                         currBatchedInsertContextsIndex,
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
}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
