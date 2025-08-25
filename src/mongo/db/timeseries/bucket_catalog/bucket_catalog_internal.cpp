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

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_compression_failure.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/tracking/unordered_map.h"

#include <algorithm>
#include <climits>
#include <limits>
#include <list>
#include <string>
#include <tuple>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/utility/in_place_factory.hpp>  // IWYU pragma: keep

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries::bucket_catalog::internal {
namespace {
MONGO_FAIL_POINT_DEFINE(alwaysUseSameBucketCatalogStripe);
MONGO_FAIL_POINT_DEFINE(hangTimeSeriesBatchPrepareWaitingForConflictingOperation);

stdx::mutex _bucketIdGenLock;
PseudoRandom _bucketIdGenPRNG(SecureRandom().nextInt64());
AtomicWord<uint64_t> _bucketIdGenCounter{static_cast<uint64_t>(_bucketIdGenPRNG.nextInt64())};

std::string rolloverReasonToString(const RolloverReason reason) {
    switch (reason) {
        case RolloverReason::kCount:
            return "kCount";
        case RolloverReason::kSchemaChange:
            return "kSchemaChange";
        case RolloverReason::kCachePressure:
            return "kCachePressure";
        case RolloverReason::kSize:
            return "kSize";
        case RolloverReason::kTimeForward:
            return "kTimeForward";
        case RolloverReason::kTimeBackward:
            return "kTimeBackward";
        case RolloverReason::kNone:
            return "kNone";
    }
    MONGO_UNREACHABLE;
}
void assertNoOpenUnclearedBucketsForKey(Stripe& stripe,
                                        BucketStateRegistry& registry,
                                        const BucketKey& key) {
    auto it = stripe.openBucketsByKey.find(key);
    if (it != stripe.openBucketsByKey.end()) {
        auto& openSet = it->second;
        for (Bucket* bucket : openSet) {
            if (bucket->rolloverReason == RolloverReason::kNone) {
                if (auto state = materializeAndGetBucketState(registry, bucket);
                    state && !conflictsWithInsertions(state.value())) {
                    for (Bucket* b : openSet) {
                        LOGV2_INFO(8999000,
                                   "Dumping buckets for key",
                                   "key"_attr = key.metadata,
                                   "bucketId"_attr = b->bucketId.oid,
                                   "bucketState"_attr = bucketStateToString(
                                       materializeAndGetBucketState(registry, b).value()),
                                   "rolloverReason"_attr =
                                       rolloverReasonToString(b->rolloverReason));
                    }
                    invariant(bucket->rolloverReason != RolloverReason::kNone || !state ||
                              conflictsWithInsertions(state.value()));
                }
            }
        }
    }
}

/**
 * Abandons the write batch and notifies any waiters that the bucket has been cleared.
 */
void abortWriteBatch(WriteBatch& batch, const Status& status) {
    if (batch.promise.getFuture().isReady()) {
        return;
    }

    batch.promise.setError(status);
}

tracking::shared_ptr<ExecutionStats> getEmptyStats() {
    static const auto kEmptyStats{std::make_shared<ExecutionStats>()};
    return kEmptyStats;
}

/**
 * Quick and incomplete check of bucket fill by size, when timeseriesBucketMaxSize size is in
 * effect. It safely excludes checking any further optimizations like dynamic bucketing and large
 * measurements which can change the effective max size.
 */
bool isBucketDefinitelyFullDueToSize(const Bucket& bucket) {
    return (bucket.size >= gTimeseriesBucketMaxSize &&
            bucket.numMeasurements >= static_cast<std::uint64_t>(gTimeseriesBucketMinCount));
}
}  // namespace

StripeNumber getStripeNumber(const BucketCatalog& catalog, const BucketKey& key) {
    if (MONGO_unlikely(alwaysUseSameBucketCatalogStripe.shouldFail())) {
        return 0;
    }
    return key.hash % catalog.stripes.size();
}

StripeNumber getStripeNumber(const BucketCatalog& catalog, const BucketId& bucketId) {
    if (MONGO_unlikely(alwaysUseSameBucketCatalogStripe.shouldFail())) {
        return 0;
    }
    return bucketId.keySignature % catalog.stripes.size();
}

const Bucket* findBucket(BucketStateRegistry& registry,
                         const Stripe& stripe,
                         WithLock,
                         const BucketId& bucketId,
                         const IgnoreBucketState mode) {
    auto it = stripe.openBucketsById.find(bucketId);
    if (it != stripe.openBucketsById.end()) {
        if (mode == IgnoreBucketState::kYes) {
            return it->second.get();
        }

        if (auto state = materializeAndGetBucketState(registry, it->second.get());
            state && !conflictsWithInsertions(state.value())) {
            return it->second.get();
        }
    }
    return nullptr;
}

Bucket* useBucket(BucketStateRegistry& registry,
                  Stripe& stripe,
                  WithLock stripeLock,
                  const BucketId& bucketId,
                  const IgnoreBucketState mode) {
    return const_cast<Bucket*>(findBucket(registry, stripe, stripeLock, bucketId, mode));
}

Bucket* useBucketAndChangePreparedState(BucketStateRegistry& registry,
                                        Stripe& stripe,
                                        WithLock stripeLock,
                                        const BucketId& bucketId,
                                        const BucketPrepareAction prepare) {
    auto it = stripe.openBucketsById.find(bucketId);
    if (it != stripe.openBucketsById.end()) {
        StateChangeSuccessful stateChangeResult = (prepare == BucketPrepareAction::kPrepare)
            ? prepareBucketState(registry, it->second.get()->bucketId, it->second.get())
            : unprepareBucketState(registry, it->second.get()->bucketId, it->second.get());
        if (stateChangeResult == StateChangeSuccessful::kYes) {
            return it->second.get();
        }
    }
    return nullptr;
}

std::vector<Bucket*> findOpenBuckets(Stripe& stripe,
                                     WithLock stripeLock,
                                     const BucketKey& bucketKey) {
    std::vector<Bucket*> openBuckets;
    if (const auto& it = stripe.openBucketsByKey.find(bucketKey);
        it != stripe.openBucketsByKey.end()) {
        const auto& openBucketSet = it->second;
        for (Bucket* openBucket : openBucketSet) {
            openBuckets.push_back(openBucket);
        }
    }
    return openBuckets;
}

BucketStateForInsertAndCleanup isBucketStateEligibleForInsertsAndCleanup(BucketCatalog& catalog,
                                                                         Stripe& stripe,
                                                                         WithLock stripeLock,
                                                                         Bucket* bucket) {
    auto state = materializeAndGetBucketState(catalog.bucketStateRegistry, bucket);
    if (!state) {
        invariant(bucket->preparedBatch);
        return BucketStateForInsertAndCleanup::kNoState;
    }

    if (conflictsWithInsertions(state.value())) {
        // Clean up buckets ineligible for inserts.
        ExecutionStatsController statsStorage =
            getExecutionStats(catalog, bucket->bucketId.collectionUUID);
        abort(catalog,
              stripe,
              stripeLock,
              *bucket,
              statsStorage,
              /*batch=*/nullptr,
              getTimeseriesBucketClearedError(bucket->bucketId.oid));
        return BucketStateForInsertAndCleanup::kInsertionConflict;
    }

    return BucketStateForInsertAndCleanup::kEligibleForInsert;
}

Status compressAndWriteBucket(OperationContext* opCtx,
                              BucketCatalog& catalog,
                              const Collection* bucketsColl,
                              const BucketId& uncompressedBucketId,
                              StringData timeField,
                              const CompressAndWriteBucketFunc& compressAndWriteBucketFunc) {
    try {
        LOGV2(8654200,
              "Compressing uncompressed bucket upon bucket reopen",
              "bucketId"_attr = uncompressedBucketId.oid);
        // Compress the uncompressed bucket and write to disk.
        invariant(compressAndWriteBucketFunc);
        compressAndWriteBucketFunc(opCtx, uncompressedBucketId, bucketsColl->ns(), timeField);
    } catch (...) {
        bucket_catalog::freeze(catalog, uncompressedBucketId);
        LOGV2_WARNING(8654201,
                      "Failed to compress bucket for time-series insert upon reopening, will retry "
                      "insert on a new bucket",
                      "bucketId"_attr = uncompressedBucketId.oid);
        return Status{timeseries::BucketCompressionFailure(bucketsColl->uuid(),
                                                           uncompressedBucketId.oid,
                                                           uncompressedBucketId.keySignature),
                      "Failed to compress bucket for time-series insert upon reopening"};
    }
    return Status::OK();
}

BSONObj reopenFetchedBucket(OperationContext* opCtx,
                            const Collection* bucketsColl,
                            const OID& bucketId,
                            ExecutionStatsController& stats) {

    const auto reopenedBucketDoc = [&] {
        FindCommandRequest findReq{bucketsColl->ns()};
        findReq.setFilter(BSON("_id" << bucketId));
        findReq.setRawData(true);
        return DBDirectClient{opCtx}.findOne(std::move(findReq));
    }();

    if (!reopenedBucketDoc.isEmpty()) {
        stats.incNumBucketsFetched();
    } else {
        stats.incNumBucketFetchesFailed();
    }
    return reopenedBucketDoc;
}

BSONObj reopenQueriedBucket(OperationContext* opCtx,
                            const Collection* bucketsColl,
                            const TimeseriesOptions& options,
                            const std::vector<BSONObj>& pipeline,
                            ExecutionStatsController& stats) {
    // Skips running the query if we don't have an index on meta and time for the time-series
    // collection. Without the index we will perform a full collection scan which could cause us to
    // take a performance hit.
    if (const auto& index =
            getIndexSupportingReopeningQuery(opCtx, bucketsColl->getIndexCatalog(), options)) {
        DBDirectClient client{opCtx};

        // Run an aggregation to find a suitable bucket to reopen.
        AggregateCommandRequest aggRequest(bucketsColl->ns(), pipeline);
        aggRequest.setHint(index);
        aggRequest.setRawData(true);

        // TODO SERVER-86094: remove after fixing perf regression.
        query_settings::QuerySettings querySettings;
        querySettings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);
        aggRequest.setQuerySettings(querySettings);

        auto swCursor = DBClientCursor::fromAggregationRequest(
            &client, aggRequest, false /* secondaryOk */, false /* useExhaust */);
        if (swCursor.isOK() && swCursor.getValue()->more()) {
            stats.incNumBucketsQueried();
            return swCursor.getValue()->next();
        }
    }

    stats.incNumBucketQueriesFailed();
    return BSONObj{};
}

StatusWith<tracking::unique_ptr<Bucket>> rehydrateBucket(BucketCatalog& catalog,
                                                         const BSONObj& bucketDoc,
                                                         const BucketKey& bucketKey,
                                                         const TimeseriesOptions& options,
                                                         const uint64_t catalogEra,
                                                         const StringDataComparator* comparator,
                                                         const BucketDocumentValidator& validator,
                                                         ExecutionStatsController& stats) {

    if (catalogEra < getCurrentEra(catalog.bucketStateRegistry)) {
        stats.incNumBucketReopeningsFailedDueToEraMismatch();
        return {ErrorCodes::WriteConflict, "Bucket is from an earlier era, may be outdated"};
    }

    BSONElement bucketIdElem = bucketDoc.getField(kBucketIdFieldName);
    if (bucketIdElem.eoo() || bucketIdElem.type() != BSONType::oid) {
        stats.incNumBucketReopeningsFailedDueToMalformedIdField();
        return {ErrorCodes::BadValue,
                str::stream() << kBucketIdFieldName << " is missing or not an ObjectId"};
    }

    BSONElement metadata;
    auto metaFieldName = options.getMetaField();
    if (metaFieldName) {
        metadata = bucketDoc.getField(kBucketMetaFieldName);
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key = BucketKey{bucketKey.collectionUUID,
                         BucketMetadata{getTrackingContext(catalog.trackingContexts,
                                                           TrackingScope::kOpenBucketsById),
                                        metadata,
                                        options.getMetaField()}};
    if (key != bucketKey) {
        stats.incNumBucketReopeningsFailedDueToHashCollision();
        return {ErrorCodes::BadValue, "Bucket metadata does not match (hash collision)"};
    }

    BucketId bucketId{key.collectionUUID, bucketIdElem.OID(), key.signature()};
    {
        auto& bucketStateRegistry = catalog.bucketStateRegistry;
        stdx::lock_guard catalogLock{bucketStateRegistry.mutex};

        auto it = bucketStateRegistry.bucketStates.find(bucketId);
        if (it != bucketStateRegistry.bucketStates.end() && isBucketStateFrozen(it->second)) {
            stats.incNumBucketReopeningsFailedDueToMarkedFrozen();
            return {ErrorCodes::BadValue,
                    "Bucket has been marked frozen and is not eligible for reopening"};
        }
    }

    ScopeGuard freezeBucketOnError(
        [&catalog, &bucketId] { timeseries::bucket_catalog::freeze(catalog, bucketId); });

    // Validate the bucket document against the schema.
    auto result = validator(bucketDoc);
    if (result.first != Collection::SchemaValidationResult::kPass) {
        stats.incNumBucketReopeningsFailedDueToValidator();
        return result.second;
    }

    auto controlField = bucketDoc.getObjectField(kBucketControlFieldName);
    auto closedElem = controlField.getField(kBucketControlClosedFieldName);
    if (closedElem.booleanSafe()) {
        stats.incNumBucketReopeningsFailedDueToMarkedClosed();
        return {ErrorCodes::BadValue,
                "Bucket has been marked closed and is not eligible for reopening"};
    }

    auto minTime = controlField.getObjectField(kBucketControlMinFieldName)
                       .getField(options.getTimeField())
                       .Date();

    tracking::unique_ptr<Bucket> bucket = tracking::make_unique<Bucket>(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kOpenBucketsById),
        catalog.trackingContexts,
        bucketId,
        std::move(key),
        options.getTimeField(),
        minTime,
        catalog.bucketStateRegistry);

    bucket->isReopened = true;

    // Initialize the remaining member variables from the bucket document.
    bucket->size = bucketDoc.objsize();

    // Populate the top-level data field names.
    const BSONObj& dataObj = bucketDoc.getObjectField(kBucketDataFieldName);
    for (const BSONElement& dataElem : dataObj) {
        bucket->fieldNames.emplace(tracking::make_string(
            getTrackingContext(catalog.trackingContexts, TrackingScope::kOpenBucketsById),
            dataElem.fieldName(),
            dataElem.fieldNameSize() - 1));
    }

    auto swMinMax = generateMinMaxFromBucketDoc(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kSummaries),
        bucketDoc,
        comparator);
    if (!swMinMax.isOK()) {
        stats.incNumBucketReopeningsFailedDueToMinMaxCalculation();
        return swMinMax.getStatus();
    }
    bucket->minmax = std::move(swMinMax.getValue());

    auto swSchema = generateSchemaFromBucketDoc(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kSummaries),
        bucketDoc,
        comparator);
    if (!swSchema.isOK()) {
        stats.incNumBucketReopeningsFailedDueToSchemaGeneration();
        return swSchema.getStatus();
    }
    bucket->schema = std::move(swSchema.getValue());

    uint32_t numMeasurements = 0;
    const BSONElement timeColumnElem = dataObj.getField(options.getTimeField());

    // This check accounts for if a user performs a direct bucket write on the timeColumnElem and we
    // attempt to rehydrate the bucket as part of query-based reopening.
    if (timeColumnElem.type() != BSONType::binData) {
        stats.incNumBucketReopeningsFailedDueToUncompressedTimeColumn();
        return {ErrorCodes::BadValue,
                "Bucket data field is malformed (time column is not compressed)"};
    }
    BSONColumn storage{timeColumnElem};
    numMeasurements = storage.size();

    bucket->bucketIsSortedByTime = controlField.getField(kBucketControlVersionFieldName).Number() ==
            kTimeseriesControlCompressedSortedVersion
        ? true
        : false;
    bucket->numMeasurements = numMeasurements;
    bucket->numCommittedMeasurements = numMeasurements;

    // Initialize BSONColumnBuilders from the compressed bucket data fields.
    try {
        bucket->measurementMap.initBuilders(dataObj, bucket->numCommittedMeasurements);
    } catch (const DBException& ex) {
        LOGV2_WARNING(
            8830601,
            "Failed to decompress bucket for time-series insert upon reopening, will retry "
            "insert on a new bucket",
            "error"_attr = ex,
            "bucketId"_attr = bucket->bucketId.oid,
            "collectionUUID"_attr = bucket->bucketId.collectionUUID);

        LOGV2_WARNING_OPTIONS(8852900,
                              logv2::LogTruncation::Disabled,
                              "Failed to decompress bucket for time-series insert upon "
                              "reopening, will retry insert on a new bucket",
                              "bucket"_attr =
                                  base64::encode(bucketDoc.objdata(), bucketDoc.objsize()));

        invariant(!TestingProctor::instance().isEnabled());
        stats.incNumBucketReopeningsFailedDueToCompressionFailure();
        return Status(
            BucketCompressionFailure(bucketKey.collectionUUID, bucketId.oid, bucketId.keySignature),
            ex.reason());
    }

    freezeBucketOnError.dismiss();
    return {std::move(bucket)};
}

StatusWith<std::reference_wrapper<Bucket>> loadBucketIntoCatalog(
    BucketCatalog& catalog,
    Stripe& stripe,
    WithLock stripeLock,
    ExecutionStatsController& stats,
    const BucketKey& key,
    tracking::unique_ptr<Bucket>&& bucket,
    const std::uint64_t targetEra) {
    invariant(bucket.get());

    expireIdleBuckets(catalog, stripe, stripeLock, key.collectionUUID, stats);

    auto status = initializeBucketState(
        catalog.bucketStateRegistry, bucket->bucketId, targetEra, bucket.get());

    // Forward the WriteConflict if the bucket has been cleared or has a pending direct write.
    if (!status.isOK()) {
        if (status.code() == ErrorCodes::WriteConflict) {
            stats.incNumBucketReopeningsFailedDueToWriteConflict();
        } else if (status.code() == ErrorCodes::TimeseriesBucketFrozen) {
            stats.incNumBucketReopeningsFailedDueToMarkedFrozen();
        }
        return status;
    }

    // If this bucket was archived, we need to remove it from the set of archived buckets.
    auto archivedKey = std::make_tuple(key.collectionUUID, key.hash, bucket->minTime);
    if (auto it = stripe.archivedBuckets.find(archivedKey); it != stripe.archivedBuckets.end()) {
        stripe.archivedBuckets.erase(it);
        stats.decNumActiveBuckets();
    }

    // Pass ownership of the reopened bucket to the bucket catalog.
    auto [insertedIt, newlyInserted] =
        stripe.openBucketsById.try_emplace(bucket->bucketId, std::move(bucket));
    invariant(newlyInserted);
    Bucket* unownedBucket = insertedIt->second.get();

    // If we already have an open bucket for this key, we need to close it.
    if (auto it = stripe.openBucketsByKey.find(key); it != stripe.openBucketsByKey.end()) {
        auto& openSet = it->second;
        for (Bucket* existingBucket : openSet) {
            if (existingBucket->rolloverReason == RolloverReason::kNone) {
                auto state =
                    materializeAndGetBucketState(catalog.bucketStateRegistry, existingBucket);
                if (state && !conflictsWithInsertions(state.value())) {
                    stats.incNumBucketsClosedDueToReopening();
                    if (allCommitted(*existingBucket)) {
                        closeOpenBucket(catalog, stripe, stripeLock, *existingBucket, stats);
                    } else {
                        existingBucket->rolloverReason = RolloverReason::kTimeForward;
                    }
                    // We should only have one open, uncleared bucket at a time.
                    break;
                }
            }
        }
    }

    // Now actually mark this bucket as open.
    if constexpr (kDebugBuild) {
        assertNoOpenUnclearedBucketsForKey(stripe, catalog.bucketStateRegistry, key);
    }
    stripe.openBucketsByKey[key].emplace(unownedBucket);
    stats.incNumBucketsReopened();

    stats.incNumActiveBuckets();

    return *unownedBucket;
}

bool tryToInsertIntoBucketWithoutRollover(BucketCatalog& catalog,
                                          Stripe& stripe,
                                          WithLock stripeLock,
                                          const BatchedInsertTuple& batchedInsertTuple,
                                          const OperationId opId,
                                          const TimeseriesOptions& timeseriesOptions,
                                          const StripeNumber& stripeNumber,
                                          const uint64_t storageCacheSizeBytes,
                                          const StringDataComparator* comparator,
                                          Bucket& bucket,
                                          ExecutionStatsController& stats,
                                          std::shared_ptr<WriteBatch>& writeBatch) {
    Bucket::NewFieldNames newFieldNamesToBeInserted;
    Sizes sizesToBeAdded;

    auto [measurement, date, index] = batchedInsertTuple;

    bool isNewlyOpenedBucket = (bucket.size == 0);
    calculateBucketFieldsAndSizeChange(catalog.trackingContexts,
                                       bucket,
                                       measurement,
                                       timeseriesOptions.getMetaField(),
                                       newFieldNamesToBeInserted,
                                       sizesToBeAdded);

    if (!isNewlyOpenedBucket) {
        auto reason =
            determineRolloverReason(measurement,
                                    timeseriesOptions,
                                    catalog.globalExecutionStats.numActiveBuckets.loadRelaxed(),
                                    sizesToBeAdded,
                                    date,
                                    storageCacheSizeBytes,
                                    comparator,
                                    bucket,
                                    stats);
        if (reason != RolloverReason::kNone) {
            // We cannot insert this measurement without rolling over the bucket.
            // Mark the bucket's RolloverAction so this bucket won't be eligible for staging the
            // next measurement.
            bucket.rolloverReason = reason;
            return false;
        }
    }
    addMeasurementToBatchAndBucket(catalog,
                                   measurement,
                                   index,
                                   opId,
                                   timeseriesOptions,
                                   stripeNumber,
                                   comparator,
                                   sizesToBeAdded,
                                   isNewlyOpenedBucket,
                                   newFieldNamesToBeInserted,
                                   bucket,
                                   writeBatch);
    return true;
}

void waitToCommitBatch(BucketStateRegistry& registry,
                       Stripe& stripe,
                       const std::shared_ptr<WriteBatch>& batch) {
    while (true) {
        boost::optional<InsertWaiter> waiter;
        {
            stdx::lock_guard stripeLock{stripe.mutex};
            Bucket* bucket =
                useBucket(registry, stripe, stripeLock, batch->bucketId, IgnoreBucketState::kNo);
            if (!bucket || isWriteBatchFinished(*batch)) {
                return;
            }

            if (bucket->preparedBatch) {
                // Conflict with other prepared batch on this bucket.
                waiter = bucket->preparedBatch;
            } else if (auto it = stripe.outstandingReopeningRequests.find(batch->bucketKey);
                       it != stripe.outstandingReopeningRequests.end()) {
                // Conflict with any query-based reopening request on this series (metaField value)
                // or an archive-based request on this bucket.
                auto& list = it->second;
                for (auto&& request : list) {
                    if (!request->oid.has_value() || request->oid.value() == batch->bucketId.oid) {
                        waiter = request;
                        break;
                    }
                }
            }

            if (!waiter.has_value()) {
                // No other batches for this bucket are currently committing, so we can proceed.
                bucket->preparedBatch = batch;
                bucket->batches.erase(batch->opId);
                return;
            }
        }
        invariant(waiter.has_value());

        // We only hit this failpoint when there are conflicting operations on the same series.
        hangTimeSeriesBatchPrepareWaitingForConflictingOperation.pauseWhileSet();

        // We have to wait for someone else to finish.
        waitToInsert(&waiter.value());  // We don't care about the result.
        waiter = boost::none;
    }
}

void removeBucket(BucketCatalog& catalog,
                  Stripe& stripe,
                  WithLock stripeLock,
                  Bucket& bucket,
                  ExecutionStatsController& stats) {
    removeBucketWithoutStats(catalog, stripe, stripeLock, bucket);
    stats.decNumActiveBuckets();
}

void removeBucketWithoutStats(BucketCatalog& catalog,
                              Stripe& stripe,
                              WithLock stripeLock,
                              Bucket& bucket) {
    invariant(bucket.batches.empty());
    invariant(!bucket.preparedBatch);

    // Clear out all open bucket state in the Stripe & Registry.
    auto allIt = stripe.openBucketsById.find(bucket.bucketId);
    invariant(allIt != stripe.openBucketsById.end());

    markBucketNotIdle(stripe, stripeLock, bucket);

    // Removes the state associated with this open bucket, leaving behind the BucketKey & set if
    // there is an uncleared bucket besides the current one still tracked by the Stripe.
    auto openIt =
        stripe.openBucketsByKey.find({bucket.bucketId.collectionUUID, bucket.key.metadata});
    if (openIt != stripe.openBucketsByKey.end()) {
        auto& openSet = openIt->second;
        auto bucketIt = openSet.find(&bucket);
        if (bucketIt != openSet.end()) {
            if (openSet.size() == 1) {
                stripe.openBucketsByKey.erase(openIt);
            } else {
                // Can't delete the set, only remove 'bucket'.
                openSet.erase(bucketIt);
            }
        }
    }

    // Remove the bucket from the bucket state registry.
    stopTrackingBucketState(catalog.bucketStateRegistry, bucket.bucketId);

    stripe.openBucketsById.erase(allIt);
}

void archiveBucket(BucketCatalog& catalog,
                   Stripe& stripe,
                   WithLock stripeLock,
                   Bucket& bucket,
                   ExecutionStatsController& stats) {
    if (bucket.numMeasurements >= static_cast<std::uint32_t>(gTimeseriesBucketMaxCount) ||
        isBucketDefinitelyFullDueToSize(bucket)) {
        // Do not archive a bucket that is known to be full.
        // It is impractical to conclusively determine all closure reasons at this point.
        if (bucket.numMeasurements >= static_cast<std::uint32_t>(gTimeseriesBucketMaxCount)) {
            stats.incNumBucketsClosedDueToCount();
        } else {
            stats.incNumBucketsClosedDueToSize();
        }
        closeOpenBucket(catalog, stripe, stripeLock, bucket, stats);
        return;
    }

    bool archived =
        stripe.archivedBuckets
            .emplace(
                std::make_tuple(bucket.bucketId.collectionUUID, bucket.key.hash, bucket.minTime),
                ArchivedBucket{bucket.bucketId.oid})
            .second;

    if (archived) {
        removeBucketWithoutStats(catalog, stripe, stripeLock, bucket);
    } else {
        // We had a meta hash collision, and already have a bucket archived with the same meta hash
        // and timestamp as this bucket. Since it's somewhat arbitrary which bucket we keep, we'll
        // keep the one that's already archived and just plain close this one.
        closeOpenBucket(catalog, stripe, stripeLock, bucket, stats);
    }
}

boost::optional<OID> findArchivedCandidate(BucketCatalog& catalog,
                                           Stripe& stripe,
                                           WithLock stripeLock,
                                           const BucketKey& bucketKey,
                                           const TimeseriesOptions& options,
                                           const Date_t& time) {

    // We want to find the largest time that is not greater than info.time. Generally
    // lower_bound will return the smallest element not less than the search value, but we are
    // using std::greater instead of std::less for the map's comparisons. This means the order
    // of keys will be reversed, and lower_bound will return what we want.
    auto it = stripe.archivedBuckets.lower_bound(
        std::make_tuple(bucketKey.collectionUUID, bucketKey.hash, time));
    if (it == stripe.archivedBuckets.end()) {
        return boost::none;
    }

    // Ensure we have an exact match on UUID and BucketKey::Hash
    const auto& uuid = std::get<UUID>(it->first);
    const auto& hash = std::get<BucketKey::Hash>(it->first);
    if (uuid != bucketKey.collectionUUID || hash != bucketKey.hash) {
        return boost::none;
    }

    const auto& candidateTime = std::get<Date_t>(it->first);
    invariant(candidateTime <= time);
    // We need to make sure our measurement can fit without violating max span. If not, we
    // can't use this bucket.
    if (time - candidateTime >= Seconds(*options.getBucketMaxSpanSeconds())) {
        return boost::none;
    }

    return it->second.oid;
}

std::pair<int32_t, int32_t> getCacheDerivedBucketMaxSize(const uint64_t storageCacheSizeBytes,
                                                         const int64_t workloadCardinality) {
    invariant(workloadCardinality >= 0, std::to_string(workloadCardinality));
    if (workloadCardinality == 0) {
        return {gTimeseriesBucketMaxSize, INT_MAX};
    }

    uint64_t intMax = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    uint64_t dynamicMaxSize =
        storageCacheSizeBytes / static_cast<uint64_t>(2 * workloadCardinality);
    int32_t derivedMaxSize = std::max(static_cast<int32_t>(std::min(dynamicMaxSize, intMax)),
                                      gTimeseriesBucketMinSize.load());
    return {std::min(gTimeseriesBucketMaxSize, derivedMaxSize), derivedMaxSize};
}

boost::optional<OID> getArchiveReopeningCandidate(BucketCatalog& catalog,
                                                  Stripe& stripe,
                                                  WithLock stripeLock,
                                                  const BucketKey& bucketKey,
                                                  const TimeseriesOptions& options,
                                                  const Date_t& time) {
    return findArchivedCandidate(catalog, stripe, stripeLock, bucketKey, options, time);
}

std::vector<BSONObj> getQueryReopeningCandidate(BucketCatalog& catalog,
                                                Stripe& stripe,
                                                WithLock stripeLock,
                                                const BucketKey& bucketKey,
                                                const TimeseriesOptions& options,
                                                const uint64_t storageCacheSizeBytes,
                                                const Date_t& time) {
    boost::optional<BSONElement> metaElement;
    if (options.getMetaField().has_value()) {
        metaElement = bucketKey.metadata.element();
    }

    auto controlMinTimePath = std::string{kControlMinFieldNamePrefix} + options.getTimeField();
    auto maxDataTimeFieldPath = std::string{kDataFieldNamePrefix} + options.getTimeField() + "." +
        std::to_string(gTimeseriesBucketMaxCount - 1);

    // Derive the maximum bucket size.
    auto [bucketMaxSize, _] = getCacheDerivedBucketMaxSize(
        storageCacheSizeBytes, catalog.globalExecutionStats.numActiveBuckets.loadRelaxed());

    return generateReopeningPipeline(time,
                                     metaElement,
                                     controlMinTimePath,
                                     maxDataTimeFieldPath,
                                     *options.getBucketMaxSpanSeconds(),
                                     bucketMaxSize);
}

std::shared_ptr<WriteBatch> findPreparedBatch(const Stripe& stripe,
                                              WithLock stripeLock,
                                              const BucketKey& key,
                                              const boost::optional<OID>& oid) {
    auto it = stripe.openBucketsByKey.find(key);
    if (it == stripe.openBucketsByKey.end()) {
        // No open bucket for this metadata.
        return {};
    }

    auto& openSet = it->second;
    for (Bucket* potentialBucket : openSet) {
        if (potentialBucket->preparedBatch &&
            (!oid.has_value() || oid.value() == potentialBucket->bucketId.oid)) {
            return potentialBucket->preparedBatch;
        }
    }

    return {};
}

void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           std::shared_ptr<WriteBatch> batch,
           const Status& status) {
    // Before we access the bucket, make sure it's still there.
    Bucket* bucket = useBucket(
        catalog.bucketStateRegistry, stripe, stripeLock, batch->bucketId, IgnoreBucketState::kYes);
    if (!bucket) {
        // Special case, bucket has already been cleared, and we need only abort this batch.
        abortWriteBatch(*batch, status);
        return;
    }

    // Proceed to abort any unprepared batches and remove the bucket if possible
    abort(catalog, stripe, stripeLock, *bucket, batch->stats, batch, status);
}

void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           Bucket& bucket,
           ExecutionStatsController& stats,
           std::shared_ptr<WriteBatch> batch,
           const Status& status) {
    // Abort any unprepared batches. This should be safe since we have a lock on the stripe,
    // preventing anyone else from using these.
    for (const auto& [_, current] : bucket.batches) {
        abortWriteBatch(*current, status);
    }
    bucket.batches.clear();

    bool doRemove = true;  // We shouldn't remove the bucket if there's a prepared batch outstanding
                           // and it's not the one we manage. In that case, we don't know what the
                           // user is doing with it, but we need to keep the bucket around until
                           // that batch is finished.
    if (auto& prepared = bucket.preparedBatch) {
        if (batch && prepared == batch) {
            // We own the prepared batch, so we can go ahead and abort it and remove the bucket.
            abortWriteBatch(*prepared, status);
            prepared.reset();
        } else {
            doRemove = false;
        }
    }

    if (doRemove) {
        removeBucket(catalog, stripe, stripeLock, bucket, stats);
    } else {
        clearBucketState(catalog.bucketStateRegistry, bucket.bucketId);
    }
}

void markBucketIdle(Stripe& stripe, WithLock stripeLock, Bucket& bucket) {
    invariant(!bucket.idleListEntry.has_value());
    invariant(allCommitted(bucket));
    stripe.idleBuckets.push_front(&bucket);
    bucket.idleListEntry = stripe.idleBuckets.begin();
}

void markBucketNotIdle(Stripe& stripe, WithLock stripeLock, Bucket& bucket) {
    if (bucket.idleListEntry.has_value()) {
        stripe.idleBuckets.erase(bucket.idleListEntry.value());
        bucket.idleListEntry = boost::none;
    }
}

void expireIdleBuckets(BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       const UUID& collectionUUID,
                       ExecutionStatsController& collectionStats) {
    // As long as we still need space and have entries and remaining attempts, close idle buckets.
    int32_t numExpired = 0;

    ExecutionStatsController storage;
    auto statsForBucket = [&](const BucketId& bucketId) -> ExecutionStatsController& {
        if (bucketId.collectionUUID == collectionUUID)
            return collectionStats;

        storage = getExecutionStats(catalog, bucketId.collectionUUID);
        return storage;
    };

    while (!stripe.idleBuckets.empty() &&
           getMemoryUsage(catalog) > catalog.memoryUsageThreshold() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {
        Bucket* bucket = stripe.idleBuckets.back();
        ExecutionStatsController& stats = statsForBucket(bucket->bucketId);

        auto state = materializeAndGetBucketState(catalog.bucketStateRegistry, bucket);
        if (state && !conflictsWithInsertions(state.value())) {
            // Can archive a bucket if it's still eligible for insertions.
            archiveBucket(catalog, stripe, stripeLock, *bucket, stats);
            stats.incNumBucketsArchivedDueToMemoryThreshold();
        } else if (state &&
                   (isBucketStateCleared(state.value()) || isBucketStateFrozen(state.value()))) {
            // Bucket was cleared and just needs to be removed from catalog.
            removeBucket(catalog, stripe, stripeLock, *bucket, stats);
        } else {
            closeOpenBucket(catalog, stripe, stripeLock, *bucket, stats);
            stats.incNumBucketsClosedDueToMemoryThreshold();
        }

        ++numExpired;
    }

    while (!stripe.archivedBuckets.empty() &&
           getMemoryUsage(catalog) > catalog.memoryUsageThreshold() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {

        auto it = stripe.archivedBuckets.begin();
        const auto& [key, archived] = *it;
        const auto& uuid = std::get<UUID>(key);
        const auto& hash = std::get<BucketKey::Hash>(key);

        BucketId bucketId(uuid, archived.oid, BucketKey::signature(hash));
        ExecutionStatsController& stats = statsForBucket(bucketId);

        closeArchivedBucket(catalog, bucketId, stats);

        stripe.archivedBuckets.erase(it);
        stats.incNumBucketsClosedDueToMemoryThreshold();
        ++numExpired;
    }
}

std::pair<OID, Date_t> generateBucketOID(const Date_t& time, const TimeseriesOptions& options) {
    OID oid;

    // We round the measurement timestamp down to the nearest minute, hour, or day depending on the
    // granularity. We do this for two reasons. The first is so that if measurements come in
    // slightly out of order, we don't have to close the current bucket due to going backwards in
    // time. The second, and more important reason, is so that we reliably group measurements
    // together into predictable chunks for sharding. This way we know from a measurement timestamp
    // what the bucket timestamp will be, so we can route measurements to the right shard chunk.
    auto roundedTime = roundTimestampToGranularity(time, options);
    int64_t const roundedSeconds = durationCount<Seconds>(roundedTime.toDurationSinceEpoch());
    oid.setTimestamp(roundedSeconds);

    // Now, if we used the standard OID generation method for the remaining bytes we could end up
    // with lots of bucket OID collisions. Consider the case where we have the granularity set to
    // 'Hours'. This means we will round down to the nearest day, so any bucket generated on the
    // same machine on the same day will have the same timestamp portion and unique instance portion
    // of the OID. Only the increment would differ. Since we only use 3 bytes for the increment
    // portion, we run a serious risk of overflow if we are generating lots of buckets.
    //
    // To address this, we'll instead use a PRNG to generate the rest of the bytes. With 8 bytes of
    // randomness, we should have a pretty low chance of collisions. The limit of the birthday
    // paradox converges to roughly the square root of the size of the space, so we would need a few
    // billion buckets with the same timestamp to expect collisions. In the rare case that we do get
    // a collision, we can (and do) simply regenerate the bucket _id at a higher level.
    uint64_t bits = BigEndian<uint64_t>::store(_bucketIdGenCounter.addAndFetch(1));

    OID::InstanceUnique instance;
    const auto instanceBuf = static_cast<uint8_t*>(instance.bytes);
    std::memcpy(instanceBuf, &bits, OID::kInstanceUniqueSize);

    OID::Increment increment;
    const auto incrementBuf = static_cast<uint8_t*>(increment.bytes);
    uint8_t* bitsBuf = (uint8_t*)&bits;
    std::memcpy(incrementBuf, &(bitsBuf)[OID::kInstanceUniqueSize], OID::kIncrementSize);

    oid.setInstanceUnique(instance);
    oid.setIncrement(increment);

    return {oid, roundedTime};
}

void resetBucketOIDCounter() {
    stdx::lock_guard lk{_bucketIdGenLock};
    _bucketIdGenCounter.store(static_cast<uint64_t>(_bucketIdGenPRNG.nextInt64()));
}

Bucket& allocateBucket(BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       const BucketKey& key,
                       const TimeseriesOptions& timeseriesOptions,
                       const Date_t& time,
                       const StringDataComparator* comparator,
                       ExecutionStatsController& stats) {
    expireIdleBuckets(catalog, stripe, stripeLock, key.collectionUUID, stats);

    // In rare cases duplicate bucket _id fields can be generated in the same stripe and fail to be
    // inserted. We will perform a limited number of retries to minimize the probability of
    // collision.
    auto maxRetries = gTimeseriesInsertMaxRetriesOnDuplicates.load();
    OID oid;
    Date_t roundedTime;
    tracking::unordered_map<BucketId, tracking::unique_ptr<Bucket>, BucketHasher>::iterator it;
    bool successfullyCreatedId = false;
    for (int retryAttempts = 0; !successfullyCreatedId && retryAttempts < maxRetries;
         ++retryAttempts) {
        std::tie(oid, roundedTime) = generateBucketOID(time, timeseriesOptions);
        auto bucketId = BucketId{key.collectionUUID, oid, key.signature()};
        std::tie(it, successfullyCreatedId) = stripe.openBucketsById.try_emplace(
            bucketId,
            tracking::make_unique<Bucket>(
                getTrackingContext(catalog.trackingContexts, TrackingScope::kOpenBucketsById),
                catalog.trackingContexts,
                bucketId,
                key,
                timeseriesOptions.getTimeField(),
                roundedTime,
                catalog.bucketStateRegistry));
        if (successfullyCreatedId) {
            Bucket* bucket = it->second.get();
            auto status = initializeBucketState(catalog.bucketStateRegistry, bucket->bucketId);
            if (!status.isOK()) {
                successfullyCreatedId = false;
            }
        }
        if (!successfullyCreatedId) {
            stripe.openBucketsById.erase(bucketId);
            resetBucketOIDCounter();
        }
    }
    uassert(6130900,
            "Unable to insert documents due to internal OID generation collision. Increase the "
            "value of server parameter 'timeseriesInsertMaxRetriesOnDuplicates' and try again",
            successfullyCreatedId);

    Bucket* bucket = it->second.get();
    if constexpr (kDebugBuild) {
        assertNoOpenUnclearedBucketsForKey(stripe, catalog.bucketStateRegistry, key);
    }
    stripe.openBucketsByKey[key].emplace(bucket);

    stats.incNumActiveBuckets();
    // Make sure we set the control.min time field to match the rounded _id timestamp.
    auto controlDoc = buildControlMinTimestampDoc(timeseriesOptions.getTimeField(), roundedTime);
    bucket->minmax.update(controlDoc, /*metaField=*/boost::none, comparator);
    return *bucket;
}

void rollover(BucketCatalog& catalog,
              Stripe& stripe,
              WithLock stripeLock,
              Bucket& bucket,
              const RolloverReason reason) {
    invariant(reason != RolloverReason::kNone);
    auto action = getRolloverAction(reason);
    if (allCommitted(bucket)) {
        ExecutionStatsController stats = getExecutionStats(catalog, bucket.bucketId.collectionUUID);
        updateRolloverStats(stats, reason);
        // The bucket does not contain any measurements that are yet to be committed, so we can
        // take action now.
        if (action == RolloverAction::kArchive) {
            archiveBucket(catalog, stripe, stripeLock, bucket, stats);
        } else {
            closeOpenBucket(catalog, stripe, stripeLock, bucket, stats);
        }
    } else {
        // We must keep the bucket around until all measurements are committed, just mark
        // the reason we chose now so we know what to do when the last batch finishes.
        bucket.rolloverReason = reason;
    }
}

void updateRolloverStats(ExecutionStatsController& stats, const RolloverReason reason) {
    switch (reason) {
        case RolloverReason::kTimeForward:
            stats.incNumBucketsClosedDueToTimeForward();
            break;
        case RolloverReason::kTimeBackward:
            stats.incNumBucketsArchivedDueToTimeBackward();
            break;
        case RolloverReason::kCount:
            stats.incNumBucketsClosedDueToCount();
            break;
        case RolloverReason::kCachePressure:
            stats.incNumBucketsClosedDueToCachePressure();
            break;
        case RolloverReason::kSize:
            stats.incNumBucketsClosedDueToSize();
            break;
        case RolloverReason::kSchemaChange:
            stats.incNumBucketsClosedDueToSchemaChange();
            break;
        default:
            break;
    }
}

RolloverReason determineRolloverReason(const BSONObj& doc,
                                       const TimeseriesOptions& timeseriesOptions,
                                       const int64_t numberOfActiveBuckets,
                                       const Sizes& sizesToBeAdded,
                                       const Date_t& time,
                                       const uint64_t storageCacheSizeBytes,
                                       const StringDataComparator* comparator,
                                       Bucket& bucket,
                                       ExecutionStatsController& stats) {
    auto bucketTime = bucket.minTime;
    if (time - bucketTime >= Seconds(*timeseriesOptions.getBucketMaxSpanSeconds())) {
        return RolloverReason::kTimeForward;
    }
    if (time < bucketTime) {
        return RolloverReason::kTimeBackward;
    }
    if (bucket.numMeasurements == static_cast<std::uint64_t>(gTimeseriesBucketMaxCount)) {
        return RolloverReason::kCount;
    }

    // In scenarios where we have a high cardinality workload and face increased cache pressure
    // we will decrease the size of buckets before we close them.
    auto [effectiveMaxSize, cacheDerivedBucketMaxSize] =
        getCacheDerivedBucketMaxSize(storageCacheSizeBytes, numberOfActiveBuckets);

    // We restrict the ceiling of the bucket max size under cache pressure.
    int32_t absoluteMaxSize =
        std::min(Bucket::kLargeMeasurementsMaxBucketSize, cacheDerivedBucketMaxSize);
    if (bucket.size + sizesToBeAdded.total() > effectiveMaxSize) {
        bool keepBucketOpenForLargeMeasurements =
            bucket.numMeasurements < static_cast<std::uint64_t>(gTimeseriesBucketMinCount);
        if (keepBucketOpenForLargeMeasurements) {
            if (bucket.size + sizesToBeAdded.total() > absoluteMaxSize) {
                if (absoluteMaxSize != Bucket::kLargeMeasurementsMaxBucketSize) {
                    return RolloverReason::kCachePressure;
                }
                return RolloverReason::kSize;
            }

            // There's enough space to add this measurement and we're still below the large
            // measurement threshold.
            if (!bucket.keptOpenDueToLargeMeasurements) {
                // Only increment this metric once per bucket.
                bucket.keptOpenDueToLargeMeasurements = true;
                stats.incNumBucketsKeptOpenDueToLargeMeasurements();
            }

            // Fall through to remaining checks
        } else {
            if (effectiveMaxSize == gTimeseriesBucketMaxSize) {
                return RolloverReason::kSize;
            }
            return RolloverReason::kCachePressure;
        }
    }
    if (schemaIncompatible(bucket, doc, timeseriesOptions.getMetaField(), comparator)) {
        return RolloverReason::kSchemaChange;
    }
    return RolloverReason::kNone;
}

ExecutionStatsController getOrInitializeExecutionStats(BucketCatalog& catalog,
                                                       const UUID& collectionUUID) {
    stdx::lock_guard catalogLock{catalog.mutex};
    auto it = catalog.executionStats.find(collectionUUID);
    if (it != catalog.executionStats.end()) {
        return {it->second, catalog.globalExecutionStats};
    }

    auto res =
        catalog.executionStats.emplace(collectionUUID,
                                       tracking::make_shared<ExecutionStats>(getTrackingContext(
                                           catalog.trackingContexts, TrackingScope::kStats)));
    return {res.first->second, catalog.globalExecutionStats};
}

ExecutionStatsController getExecutionStats(BucketCatalog& catalog, const UUID& collectionUUID) {
    stdx::lock_guard catalogLock{catalog.mutex};

    auto it = catalog.executionStats.find(collectionUUID);
    if (it != catalog.executionStats.end()) {
        return {it->second, catalog.globalExecutionStats};
    }

    // If the collection doesn't exist, return a set we can send stats into the void for.
    auto emptyStats = getEmptyStats();
    return {emptyStats, *emptyStats};
}

tracking::shared_ptr<ExecutionStats> getCollectionExecutionStats(const BucketCatalog& catalog,
                                                                 const UUID& collectionUUID) {
    stdx::lock_guard catalogLock{catalog.mutex};

    auto it = catalog.executionStats.find(collectionUUID);
    if (it != catalog.executionStats.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<tracking::shared_ptr<ExecutionStats>> releaseExecutionStatsFromBucketCatalog(
    BucketCatalog& catalog, std::span<const UUID> collectionUUIDs) {
    std::vector<tracking::shared_ptr<ExecutionStats>> out;
    out.reserve(collectionUUIDs.size());

    stdx::lock_guard catalogLock{catalog.mutex};
    for (auto&& uuid : collectionUUIDs) {
        auto it = catalog.executionStats.find(uuid);
        if (it != catalog.executionStats.end()) {
            out.push_back(std::move(it->second));
            catalog.executionStats.erase(it);
        }
    }

    return out;
}

Status getTimeseriesBucketClearedError(const OID& oid) {
    return {ErrorCodes::TimeseriesBucketCleared,
            str::stream() << "Time-series bucket " << oid << " was cleared"};
}

void closeOpenBucket(BucketCatalog& catalog,
                     Stripe& stripe,
                     WithLock stripeLock,
                     Bucket& bucket,
                     ExecutionStatsController& stats) {
    // Make a copy since bucket will be destroyed in removeBucket.
    BucketId bucketId = bucket.bucketId;

    removeBucket(catalog, stripe, stripeLock, bucket, stats);

    // Perform these safety checks after stopTrackingBucketState.
    auto state = getBucketState(catalog.bucketStateRegistry, bucketId);
    // When removing a closed bucket, the BucketStateRegistry may contain state for this
    // bucket due to an untracked ongoing direct write (such as TTL delete).
    if (state.has_value()) {
        std::visit(OverloadedVisitor{[&](const DirectWriteCounter& value) {
                                         invariant(value < 0, bucketStateToString(*state));
                                     },
                                     [&](const BucketState& value) {
                                         invariant(value == BucketState::kFrozen,
                                                   bucketStateToString(*state));
                                     }},
                   state.value());
    }
}

void closeArchivedBucket(BucketCatalog& catalog,
                         const BucketId& bucketId,
                         ExecutionStatsController& stats) {
    // Remove the bucket from the bucket state registry.
    stopTrackingBucketState(catalog.bucketStateRegistry, bucketId);

    stats.decNumActiveBuckets();
}

StageInsertBatchResult stageInsertBatchIntoEligibleBucket(BucketCatalog& catalog,
                                                          const OperationId opId,
                                                          const StringDataComparator* comparator,
                                                          BatchedInsertContext& batch,
                                                          Stripe& stripe,
                                                          WithLock stripeLock,
                                                          const uint64_t storageCacheSizeBytes,
                                                          Bucket& eligibleBucket,
                                                          size_t& currentPosition,
                                                          std::shared_ptr<WriteBatch>& writeBatch) {
    invariant(currentPosition < batch.measurementsTimesAndIndices.size());
    bool anySuccessfulInserts = false;
    while (currentPosition < batch.measurementsTimesAndIndices.size()) {
        if (!tryToInsertIntoBucketWithoutRollover(
                catalog,
                stripe,
                stripeLock,
                batch.measurementsTimesAndIndices[currentPosition],
                opId,
                batch.options,
                batch.stripeNumber,
                storageCacheSizeBytes,
                comparator,
                eligibleBucket,
                batch.stats,
                writeBatch)) {
            return anySuccessfulInserts ? StageInsertBatchResult::RolloverNeeded
                                        : StageInsertBatchResult::NoMeasurementsStaged;
        }
        ++currentPosition;
        anySuccessfulInserts = true;
    }

    return StageInsertBatchResult::Success;
}

void addMeasurementToBatchAndBucket(BucketCatalog& catalog,
                                    const BSONObj& measurement,
                                    const UserBatchIndex& index,
                                    const OperationId opId,
                                    const TimeseriesOptions& timeseriesOptions,
                                    const StripeNumber& stripeNumber,
                                    const StringDataComparator* comparator,
                                    const Sizes& sizesToBeAdded,
                                    const bool isNewlyOpenedBucket,
                                    const Bucket::NewFieldNames& newFieldNamesToBeInserted,
                                    Bucket& bucket,
                                    std::shared_ptr<WriteBatch>& writeBatch) {
    writeBatch->measurements.push_back(measurement);
    writeBatch->userBatchIndices.push_back(index);
    for (auto&& field : newFieldNamesToBeInserted) {
        writeBatch->newFieldNamesToBeInserted[field] = field.hash();
        bucket.uncommittedFieldNames.emplace(tracking::StringMapHashedKey{
            getTrackingContext(catalog.trackingContexts, TrackingScope::kOpenBucketsById),
            field.key(),
            field.hash()});
    }

    bucket.numMeasurements++;
    writeBatch->sizes.uncommittedMeasurementEstimate +=
        sizesToBeAdded.uncommittedMeasurementEstimate;
    bucket.size +=
        sizesToBeAdded.uncommittedVerifiedSize + sizesToBeAdded.uncommittedMeasurementEstimate;
    if (isNewlyOpenedBucket) {
        auto updateStatus =
            bucket.schema.update(measurement, timeseriesOptions.getMetaField(), comparator);
        invariant(updateStatus == Schema::UpdateStatus::Updated);
    }
}

}  // namespace mongo::timeseries::bucket_catalog::internal
