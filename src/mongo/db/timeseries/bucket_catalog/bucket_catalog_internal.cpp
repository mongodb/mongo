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

#include <algorithm>
#include <climits>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/utility/in_place_factory.hpp>  // IWYU pragma: keep

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_global_options.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

namespace mongo::timeseries::bucket_catalog::internal {
namespace {
MONGO_FAIL_POINT_DEFINE(alwaysUseSameBucketCatalogStripe);
MONGO_FAIL_POINT_DEFINE(hangTimeSeriesBatchPrepareWaitingForConflictingOperation);

Mutex _bucketIdGenLock =
    MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "bucket_catalog_internal::_bucketIdGenLock");
PseudoRandom _bucketIdGenPRNG(SecureRandom().nextInt64());
AtomicWord<uint64_t> _bucketIdGenCounter{static_cast<uint64_t>(_bucketIdGenPRNG.nextInt64())};

OperationId getOpId(OperationContext* opCtx, CombineWithInsertsFromOtherClients combine) {
    switch (combine) {
        case CombineWithInsertsFromOtherClients::kAllow:
            return 0;
        case CombineWithInsertsFromOtherClients::kDisallow:
            invariant(opCtx->getOpID());
            return opCtx->getOpID();
    }
    MONGO_UNREACHABLE;
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

void updateCompressionStatistics(BucketCatalog& catalog, const Bucket& bucket) {
    ExecutionStatsController stats =
        getOrInitializeExecutionStats(catalog, bucket.key.collectionUUID);
    stats.incNumBytesUncompressed(bucket.uncompressedBucketDoc.objsize());
    stats.incNumBytesCompressed(bucket.compressedBucketDoc->objsize());
}

/**
 * Returns a prepared write batch matching the specified 'key' if one exists, by searching the set
 * of open buckets associated with 'key'.
 */
std::shared_ptr<WriteBatch> findPreparedBatch(const Stripe& stripe,
                                              WithLock stripeLock,
                                              const BucketKey& key,
                                              boost::optional<OID> oid) {
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

/**
 * Finds an outstanding disk access operation that could conflict with reopening a bucket for the
 * series defined by 'key'. If one exists, this function returns an associated awaitable object.
 */
boost::optional<InsertWaiter> checkForWait(const Stripe& stripe,
                                           WithLock stripeLock,
                                           const BucketKey& key,
                                           boost::optional<OID> oid) {
    if (auto batch = findPreparedBatch(stripe, stripeLock, key, oid)) {
        return InsertWaiter{batch};
    }

    if (auto it = stripe.outstandingReopeningRequests.find(key);
        it != stripe.outstandingReopeningRequests.end()) {
        auto& requests = it->second;
        invariant(!requests.empty());

        if (!oid.has_value()) {
            // We are trying to perform a query-based reopening. This conflicts with any reopening
            // for the key.
            return InsertWaiter{requests.front()};
        }

        // We are about to attempt an archive-based reopening. This conflicts with any query-based
        // reopening for the key, or another archive-based reopening for this bucket.
        for (auto&& request : requests) {
            if (!request->oid.has_value() ||
                (request->oid.has_value() && request->oid.value() == oid.value())) {
                return InsertWaiter{request};
            }
        }
    }

    return boost::none;
}
}  // namespace

StripeNumber getStripeNumber(const BucketKey& key, size_t numberOfStripes) {
    if (MONGO_unlikely(alwaysUseSameBucketCatalogStripe.shouldFail())) {
        return 0;
    }
    return key.hash % numberOfStripes;
}

StatusWith<std::pair<BucketKey, Date_t>> extractBucketingParameters(
    const UUID& collectionUUID,
    const StringDataComparator* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc) {
    Date_t time;
    BSONElement metadata;

    if (!options.getMetaField().has_value()) {
        auto swTime = extractTime(doc, options.getTimeField());
        if (!swTime.isOK()) {
            return swTime.getStatus();
        }
        time = swTime.getValue();
    } else {
        auto swDocTimeAndMeta =
            extractTimeAndMeta(doc, options.getTimeField(), options.getMetaField().value());
        if (!swDocTimeAndMeta.isOK()) {
            return swDocTimeAndMeta.getStatus();
        }
        time = swDocTimeAndMeta.getValue().first;
        metadata = swDocTimeAndMeta.getValue().second;
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key =
        BucketKey{collectionUUID, BucketMetadata{metadata, comparator, options.getMetaField()}};

    return {std::make_pair(std::move(key), time)};
}

const Bucket* findBucket(BucketStateRegistry& registry,
                         const Stripe& stripe,
                         WithLock,
                         const BucketId& bucketId,
                         IgnoreBucketState mode) {
    auto it = stripe.openBucketsById.find(bucketId);
    if (it != stripe.openBucketsById.end()) {
        if (mode == IgnoreBucketState::kYes) {
            return it->second.get();
        }

        if (auto state = getBucketState(registry, it->second.get());
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
                  IgnoreBucketState mode) {
    return const_cast<Bucket*>(findBucket(registry, stripe, stripeLock, bucketId, mode));
}

Bucket* useBucketAndChangePreparedState(BucketStateRegistry& registry,
                                        Stripe& stripe,
                                        WithLock stripeLock,
                                        const BucketId& bucketId,
                                        BucketPrepareAction prepare) {
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

Bucket* useBucket(OperationContext* opCtx,
                  BucketCatalog& catalog,
                  Stripe& stripe,
                  WithLock stripeLock,
                  const NamespaceString& nss,
                  const CreationInfo& info,
                  AllowBucketCreation mode) {
    auto it = stripe.openBucketsByKey.find(info.key);
    if (it == stripe.openBucketsByKey.end()) {
        // No open bucket for this metadata.
        return mode == AllowBucketCreation::kYes
            ? &allocateBucket(opCtx, catalog, stripe, stripeLock, info)
            : nullptr;
    }

    auto& openSet = it->second;
    Bucket* bucket = nullptr;
    for (Bucket* potentialBucket : openSet) {
        if (potentialBucket->rolloverAction == RolloverAction::kNone) {
            bucket = potentialBucket;
            break;
        }
    }
    if (!bucket) {
        return mode == AllowBucketCreation::kYes
            ? &allocateBucket(opCtx, catalog, stripe, stripeLock, info)
            : nullptr;
    }

    if (auto state = getBucketState(catalog.bucketStateRegistry, bucket);
        state && !conflictsWithInsertions(state.value())) {
        markBucketNotIdle(stripe, stripeLock, *bucket);
        return bucket;
    }

    abort(catalog,
          stripe,
          stripeLock,
          *bucket,
          nullptr,
          getTimeseriesBucketClearedError(nss, bucket->bucketId.oid));

    return mode == AllowBucketCreation::kYes
        ? &allocateBucket(opCtx, catalog, stripe, stripeLock, info)
        : nullptr;
}

Bucket* useAlternateBucket(BucketCatalog& catalog,
                           Stripe& stripe,
                           WithLock stripeLock,
                           const NamespaceString& nss,
                           const CreationInfo& info) {
    auto it = stripe.openBucketsByKey.find(info.key);
    if (it == stripe.openBucketsByKey.end()) {
        // No open bucket for this metadata.
        return nullptr;
    }

    auto& openSet = it->second;
    // In order to potentially erase elements of the set while we iterate it (via _abort), we need
    // to advance the iterator before we call erase. This means we can't use the more
    // straightforward range iteration, and use the somewhat awkward pattern below.
    for (auto it = openSet.begin(); it != openSet.end();) {
        Bucket* potentialBucket = *it++;

        if (potentialBucket->rolloverAction == RolloverAction::kNone ||
            potentialBucket->rolloverAction == RolloverAction::kHardClose) {
            continue;
        }

        auto bucketTime = potentialBucket->minTime;
        if (info.time - bucketTime >= Seconds(*info.options.getBucketMaxSpanSeconds()) ||
            info.time < bucketTime) {
            continue;
        }

        auto state = getBucketState(catalog.bucketStateRegistry, potentialBucket);
        invariant(state);
        if (!conflictsWithInsertions(state.value())) {
            invariant(!potentialBucket->idleListEntry.has_value());
            return potentialBucket;
        }

        // Clean up the bucket if it has been cleared.
        if (state && (isBucketStateCleared(state.value()) || isBucketStateFrozen(state.value()))) {
            abort(catalog,
                  stripe,
                  stripeLock,
                  *potentialBucket,
                  nullptr,
                  getTimeseriesBucketClearedError(nss, potentialBucket->bucketId.oid));
        }
    }

    return nullptr;
}

StatusWith<unique_tracked_ptr<Bucket>> rehydrateBucket(OperationContext* opCtx,
                                                       BucketCatalog& catalog,
                                                       ExecutionStatsController& stats,
                                                       const UUID& collectionUUID,
                                                       const StringDataComparator* comparator,
                                                       const TimeseriesOptions& options,
                                                       const BucketToReopen& bucketToReopen,
                                                       const uint64_t catalogEra,
                                                       const BucketKey* expectedKey) {
    ScopeGuard updateStatsOnError([&stats] { stats.incNumBucketReopeningsFailed(); });

    const auto& [bucketDoc, validator] = bucketToReopen;
    if (catalogEra < getCurrentEra(catalog.bucketStateRegistry)) {
        return {ErrorCodes::WriteConflict, "Bucket is from an earlier era, may be outdated"};
    }

    BSONElement bucketIdElem = bucketDoc.getField(kBucketIdFieldName);
    if (bucketIdElem.eoo() || bucketIdElem.type() != BSONType::jstOID) {
        return {ErrorCodes::BadValue,
                str::stream() << kBucketIdFieldName << " is missing or not an ObjectId"};
    }

    // Validate the bucket document against the schema.
    auto result = validator(opCtx, bucketDoc);
    if (result.first != Collection::SchemaValidationResult::kPass) {
        return result.second;
    }

    auto controlField = bucketDoc.getObjectField(kBucketControlFieldName);
    auto closedElem = controlField.getField(kBucketControlClosedFieldName);
    if (closedElem.booleanSafe()) {
        return {ErrorCodes::BadValue,
                "Bucket has been marked closed and is not eligible for reopening"};
    }

    BSONElement metadata;
    auto metaFieldName = options.getMetaField();
    if (metaFieldName) {
        metadata = bucketDoc.getField(kBucketMetaFieldName);
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key = BucketKey{
        collectionUUID,
        BucketMetadata{catalog.trackingContext, metadata, comparator, options.getMetaField()}};
    if (expectedKey && key != *expectedKey) {
        return {ErrorCodes::BadValue, "Bucket metadata does not match (hash collision)"};
    }

    auto minTime = controlField.getObjectField(kBucketControlMinFieldName)
                       .getField(options.getTimeField())
                       .Date();
    BucketId bucketId{key.collectionUUID, bucketIdElem.OID()};
    unique_tracked_ptr<Bucket> bucket = make_unique_tracked<Bucket>(catalog.trackingContext,
                                                                    catalog.trackingContext,
                                                                    bucketId,
                                                                    std::move(key),
                                                                    options.getTimeField(),
                                                                    minTime,
                                                                    catalog.bucketStateRegistry);

    const bool isCompressed = isCompressedBucket(bucketDoc);

    // Initialize the remaining member variables from the bucket document.
    if (isCompressed) {
        if (!feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            // Re-opening compressed buckets is only supported when the always use compressed
            // buckets feature flag is enabled.
            return {ErrorCodes::BadValue, "Bucket is compressed and cannot be reopened"};
        }

        auto decompressedBucketDoc = decompressBucket(bucketDoc);
        if (!decompressedBucketDoc.has_value()) {
            return Status{ErrorCodes::BadValue, "Bucket could not be decompressed"};
        }
        bucket->size = decompressedBucketDoc.value().objsize();
        bucket->compressedBucketDoc = bucketDoc;
        bucket->memoryUsage += bucketDoc.objsize();
    } else {
        bucket->size = bucketDoc.objsize();
        if (feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            bucket->uncompressedBucketDoc = bucketDoc;
            bucket->memoryUsage += bucketDoc.objsize();
        }
    }

    // Populate the top-level data field names.
    const BSONObj& dataObj = bucketDoc.getObjectField(kBucketDataFieldName);
    for (const BSONElement& dataElem : dataObj) {
        auto hashedKey = StringSet::hasher().hashed_key(dataElem.fieldName());
        bucket->fieldNames.emplace(hashedKey);
    }

    auto swMinMax = generateMinMaxFromBucketDoc(catalog.trackingContext, bucketDoc, comparator);
    if (!swMinMax.isOK()) {
        return swMinMax.getStatus();
    }
    bucket->minmax = std::move(swMinMax.getValue());

    auto swSchema = generateSchemaFromBucketDoc(catalog.trackingContext, bucketDoc, comparator);
    if (!swSchema.isOK()) {
        return swSchema.getStatus();
    }
    bucket->schema = std::move(swSchema.getValue());

    uint32_t numMeasurements = 0;
    const BSONElement timeColumnElem = dataObj.getField(options.getTimeField());

    if (isCompressed && timeColumnElem.type() == BSONType::BinData) {
        BSONColumn storage{timeColumnElem};
        numMeasurements = storage.size();
    } else if (timeColumnElem.isABSONObj()) {
        numMeasurements = timeColumnElem.Obj().nFields();
    } else {
        return {ErrorCodes::BadValue,
                "Bucket data field is malformed (missing a valid time column)"};
    }

    bucket->bucketIsSortedByTime = controlField.getField(kBucketControlVersionFieldName).Number() ==
            kTimeseriesControlCompressedSortedVersion
        ? true
        : false;
    bucket->numMeasurements = numMeasurements;
    bucket->numCommittedMeasurements = numMeasurements;

    if (isCompressed &&
        feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // Initialize BSONColumnBuilders from the compressed bucket data fields.
        bucket->intermediateBuilders.initBuilders(dataObj, bucket->numCommittedMeasurements);
    }

    updateStatsOnError.dismiss();
    return {std::move(bucket)};
}

StatusWith<std::reference_wrapper<Bucket>> reopenBucket(OperationContext* opCtx,
                                                        BucketCatalog& catalog,
                                                        Stripe& stripe,
                                                        WithLock stripeLock,
                                                        ExecutionStatsController& stats,
                                                        const BucketKey& key,
                                                        unique_tracked_ptr<Bucket>&& bucket,
                                                        std::uint64_t targetEra,
                                                        ClosedBuckets& closedBuckets) {
    invariant(bucket.get());

    expireIdleBuckets(opCtx, catalog, stripe, stripeLock, stats, closedBuckets);

    auto status = initializeBucketState(
        catalog.bucketStateRegistry, bucket->bucketId, bucket.get(), targetEra);

    // Forward the WriteConflict if the bucket has been cleared or has a pending direct write.
    if (!status.isOK()) {
        return status;
    }

    // If this bucket was archived, we need to remove it from the set of archived buckets.
    if (auto setIt = stripe.archivedBuckets.find(key.hash); setIt != stripe.archivedBuckets.end()) {
        auto& archivedSet = setIt->second;
        if (auto bucketIt = archivedSet.find(bucket->minTime);
            bucketIt != archivedSet.end() && bucket->bucketId == bucketIt->second.bucketId) {
            if (archivedSet.size() == 1) {
                stripe.archivedBuckets.erase(setIt);
            } else {
                archivedSet.erase(bucketIt);
            }
            catalog.numberOfActiveBuckets.fetchAndSubtract(1);
        }
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
            if (existingBucket->rolloverAction == RolloverAction::kNone) {
                stats.incNumBucketsClosedDueToReopening();
                if (allCommitted(*existingBucket)) {
                    closeOpenBucket(
                        opCtx, catalog, stripe, stripeLock, *existingBucket, closedBuckets);
                } else {
                    existingBucket->rolloverAction = RolloverAction::kSoftClose;
                }
                // We should only have one open bucket at a time.
                break;
            }
        }
    }

    // Now actually mark this bucket as open.
    stripe.openBucketsByKey[key.cloneAsUntracked()].emplace(unownedBucket);
    stats.incNumBucketsReopened();

    catalog.memoryUsage.addAndFetch(unownedBucket->memoryUsage);
    catalog.numberOfActiveBuckets.fetchAndAdd(1);

    return *unownedBucket;
}

StatusWith<std::reference_wrapper<Bucket>> reuseExistingBucket(BucketCatalog& catalog,
                                                               Stripe& stripe,
                                                               WithLock stripeLock,
                                                               const NamespaceString& nss,
                                                               ExecutionStatsController& stats,
                                                               const BucketKey& key,
                                                               Bucket& existingBucket,
                                                               std::uint64_t targetEra) {
    // If we have an existing bucket, passing the Bucket* will let us check if the bucket was
    // cleared as part of a set since the last time it was used. If we were to just check by OID, we
    // may miss if e.g. there was a move chunk operation.
    auto state = getBucketState(catalog.bucketStateRegistry, &existingBucket);
    invariant(state);
    if (isBucketStateCleared(state.value()) || isBucketStateFrozen(state.value())) {
        abort(catalog,
              stripe,
              stripeLock,
              existingBucket,
              nullptr,
              getTimeseriesBucketClearedError(nss, existingBucket.bucketId.oid));
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    } else if (conflictsWithReopening(state.value())) {
        // Avoid reusing the bucket if it conflicts with reopening.
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    }

    // It's possible to have two buckets with the same ID in different collections, so let's make
    // extra sure the existing bucket is the right one.
    if (existingBucket.bucketId.collectionUUID != key.collectionUUID) {
        return {ErrorCodes::BadValue, "Cannot re-use bucket: same ID but different namespace"};
    }

    // If the bucket was already open, wasn't cleared, the state didn't conflict with reopening, and
    // the namespace matches, then we can simply return it.
    stats.incNumDuplicateBucketsReopened();
    markBucketNotIdle(stripe, stripeLock, existingBucket);

    return existingBucket;
}

std::variant<std::shared_ptr<WriteBatch>, RolloverReason> insertIntoBucket(
    OperationContext* opCtx,
    BucketCatalog& catalog,
    Stripe& stripe,
    WithLock stripeLock,
    StripeNumber stripeNumber,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine,
    AllowBucketCreation mode,
    CreationInfo& info,
    Bucket& existingBucket) {
    Bucket::NewFieldNames newFieldNamesToBeInserted;
    int32_t sizeToBeAdded = 0;

    bool isNewlyOpenedBucket = (existingBucket.size == 0);
    std::reference_wrapper<Bucket> bucketToUse{existingBucket};
    if (!isNewlyOpenedBucket) {
        auto [action, reason] = determineRolloverAction(opCtx,
                                                        doc,
                                                        info,
                                                        existingBucket,
                                                        catalog.numberOfActiveBuckets.load(),
                                                        newFieldNamesToBeInserted,
                                                        sizeToBeAdded,
                                                        mode);
        if ((action == RolloverAction::kSoftClose || action == RolloverAction::kArchive) &&
            mode == AllowBucketCreation::kNo) {
            // We don't actually want to roll this bucket over yet, bail out.
            return reason;
        } else if (action != RolloverAction::kNone) {
            info.openedDuetoMetadata = false;
            bucketToUse =
                rollover(opCtx, catalog, stripe, stripeLock, existingBucket, info, action);
            isNewlyOpenedBucket = true;
        }
    }
    Bucket& bucket = bucketToUse.get();
    const auto previousMemoryUsage = bucket.memoryUsage;

    if (isNewlyOpenedBucket) {
        calculateBucketFieldsAndSizeChange(
            bucket, doc, info.options.getMetaField(), newFieldNamesToBeInserted, sizeToBeAdded);
    }

    auto batch = activeBatch(
        catalog.trackingContext, bucket, getOpId(opCtx, combine), stripeNumber, info.stats);
    batch->measurements.push_back(doc);
    for (auto&& field : newFieldNamesToBeInserted) {
        batch->newFieldNamesToBeInserted[field] = field.hash();
        bucket.uncommittedFieldNames.emplace(field);
    }

    bucket.numMeasurements++;
    bucket.size += sizeToBeAdded;
    if (isNewlyOpenedBucket) {
        if (info.openedDuetoMetadata) {
            batch->openedDueToMetadata = true;
        }

        auto updateStatus = bucket.schema.update(
            doc, info.options.getMetaField(), info.key.metadata.getComparator());
        invariant(updateStatus == Schema::UpdateStatus::Updated);
    } else {
        catalog.memoryUsage.fetchAndSubtract(previousMemoryUsage);
    }
    catalog.memoryUsage.fetchAndAdd(bucket.memoryUsage);

    return batch;
}

void waitToCommitBatch(BucketStateRegistry& registry,
                       Stripe& stripe,
                       const std::shared_ptr<WriteBatch>& batch) {
    while (true) {
        boost::optional<InsertWaiter> waiter;
        {
            stdx::lock_guard stripeLock{stripe.mutex};
            Bucket* bucket = useBucket(
                registry, stripe, stripeLock, batch->bucketHandle.bucketId, IgnoreBucketState::kNo);
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
                    if (!request->oid.has_value() ||
                        request->oid.value() == batch->bucketHandle.bucketId.oid) {
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

void removeBucket(
    BucketCatalog& catalog, Stripe& stripe, WithLock stripeLock, Bucket& bucket, RemovalMode mode) {
    invariant(bucket.batches.empty());
    invariant(!bucket.preparedBatch);

    auto allIt = stripe.openBucketsById.find(bucket.bucketId);
    invariant(allIt != stripe.openBucketsById.end());

    catalog.memoryUsage.fetchAndSubtract(bucket.memoryUsage);
    markBucketNotIdle(stripe, stripeLock, bucket);

    // If the bucket was rolled over, then there may be a different open bucket for this metadata.
    auto openIt = stripe.openBucketsByKey.find(
        {bucket.bucketId.collectionUUID, bucket.key.metadata.cloneAsUntracked()});
    if (openIt != stripe.openBucketsByKey.end()) {
        auto& openSet = openIt->second;
        auto bucketIt = openSet.find(&bucket);
        if (bucketIt != openSet.end()) {
            if (openSet.size() == 1) {
                stripe.openBucketsByKey.erase(openIt);
            } else {
                openSet.erase(bucketIt);
            }
        }
    }

    // If we are cleaning up while archiving a bucket, then we want to preserve its state. Otherwise
    // we can remove the state from the catalog altogether.
    switch (mode) {
        case RemovalMode::kClose: {
            auto state = getBucketState(catalog.bucketStateRegistry, bucket.bucketId);
            if (bucket.usingAlwaysCompressedBuckets) {
                // When removing a closed bucket, the BucketStateRegistry may contain state for this
                // bucket due to an untracked ongoing direct write (such as TTL delete).
                if (state.has_value()) {
                    invariant(holds_alternative<DirectWriteCounter>(state.value()),
                              bucketStateToString(*state));
                    invariant(get<DirectWriteCounter>(state.value()) < 0,
                              bucketStateToString(*state));
                }
            } else {
                // Ensure that we are in a state of pending compression (represented by a negative
                // direct write counter).
                invariant(state.has_value());
                invariant(holds_alternative<DirectWriteCounter>(state.value()));
                invariant(get<DirectWriteCounter>(state.value()) < 0);
            }
            break;
        }
        case RemovalMode::kAbort:
            stopTrackingBucketState(catalog.bucketStateRegistry, bucket.bucketId);
            break;
        case RemovalMode::kArchive:
            // No state change
            break;
    }

    catalog.numberOfActiveBuckets.fetchAndSubtract(1);
    stripe.openBucketsById.erase(allIt);
}

void archiveBucket(OperationContext* opCtx,
                   BucketCatalog& catalog,
                   Stripe& stripe,
                   WithLock stripeLock,
                   Bucket& bucket,
                   ClosedBuckets& closedBuckets) {
    bool archived = false;
    auto& archivedSet = stripe.archivedBuckets[bucket.key.hash];
    auto it = archivedSet.find(bucket.minTime);
    if (it == archivedSet.end()) {
        // TODO SERVER-85293: remove conversion to tracked_string.
        archivedSet.emplace(
            bucket.minTime,
            ArchivedBucket{bucket.bucketId,
                           make_tracked_string(catalog.trackingContext, bucket.timeField)});
        archived = true;
    }

    if (archived) {
        // If we have an archived bucket, we still want to account for it in numberOfActiveBuckets
        // so we will increase it here since removeBucket decrements the count.
        catalog.numberOfActiveBuckets.fetchAndAdd(1);
        removeBucket(catalog, stripe, stripeLock, bucket, RemovalMode::kArchive);
    } else {
        // We had a meta hash collision, and already have a bucket archived with the same meta hash
        // and timestamp as this bucket. Since it's somewhat arbitrary which bucket we keep, we'll
        // keep the one that's already archived and just plain close this one.
        closeOpenBucket(opCtx, catalog, stripe, stripeLock, bucket, closedBuckets);
    }
}

boost::optional<OID> findArchivedCandidate(BucketCatalog& catalog,
                                           Stripe& stripe,
                                           WithLock stripeLock,
                                           const CreationInfo& info) {
    auto setIt = stripe.archivedBuckets.find(info.key.hash);
    if (setIt == stripe.archivedBuckets.end()) {
        return boost::none;
    }

    auto& archivedSet = setIt->second;

    // We want to find the largest time that is not greater than info.time. Generally lower_bound
    // will return the smallest element not less than the search value, but we are using
    // std::greater instead of std::less for the map's comparisons. This means the order of keys
    // will be reversed, and lower_bound will return what we want.
    auto it = archivedSet.lower_bound(info.time);
    if (it == archivedSet.end()) {
        return boost::none;
    }

    const auto& [candidateTime, candidateBucket] = *it;
    invariant(candidateTime <= info.time);
    // We need to make sure our measurement can fit without violating max span. If not, we
    // can't use this bucket.
    if (info.time - candidateTime < Seconds(*info.options.getBucketMaxSpanSeconds())) {
        auto bucketState = getBucketState(catalog.bucketStateRegistry, candidateBucket.bucketId);
        if (bucketState && !conflictsWithReopening(bucketState.value())) {
            return candidateBucket.bucketId.oid;
        } else {
            if (bucketState) {
                // If the bucket is represented by a state in the registry, it conflicts with
                // reopening so we can mark it as untracked to drop the state once the directWrite
                // finishes.
                stopTrackingBucketState(catalog.bucketStateRegistry, candidateBucket.bucketId);
            }
            if (archivedSet.size() == 1) {
                stripe.archivedBuckets.erase(setIt);
            } else {
                archivedSet.erase(it);
            }
            catalog.numberOfActiveBuckets.fetchAndSubtract(1);
        }
    }

    return boost::none;
}

std::pair<int32_t, int32_t> getCacheDerivedBucketMaxSize(uint64_t storageCacheSize,
                                                         uint32_t workloadCardinality) {
    if (workloadCardinality == 0) {
        return {gTimeseriesBucketMaxSize, INT_MAX};
    }

    uint64_t intMax = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    int32_t derivedMaxSize = std::max(
        static_cast<int32_t>(std::min(storageCacheSize / (2 * workloadCardinality), intMax)),
        gTimeseriesBucketMinSize.load());
    return {std::min(gTimeseriesBucketMaxSize, derivedMaxSize), derivedMaxSize};
}

InsertResult getReopeningContext(OperationContext* opCtx,
                                 BucketCatalog& catalog,
                                 Stripe& stripe,
                                 WithLock stripeLock,
                                 const CreationInfo& info,
                                 uint64_t catalogEra,
                                 AllowQueryBasedReopening allowQueryBasedReopening) {
    if (auto archived = findArchivedCandidate(catalog, stripe, stripeLock, info)) {
        // Synchronize concurrent disk accesses. An outstanding query-based reopening request for
        // this series or an outstanding archived-based reopening request or prepared batch for this
        // bucket would potentially conflict with our choice to reopen here, so we must wait for any
        // such operation to finish and retry.
        if (auto waiter = checkForWait(stripe, stripeLock, info.key, archived.value())) {
            return waiter.value();
        }

        return ReopeningContext{
            catalog, stripe, stripeLock, info.key.cloneAsUntracked(), catalogEra, archived.value()};
    }

    if (allowQueryBasedReopening == AllowQueryBasedReopening::kDisallow) {
        return ReopeningContext{
            catalog, stripe, stripeLock, info.key.cloneAsUntracked(), catalogEra, {}};
    }

    // Synchronize concurrent disk accesses. An outstanding reopening request or prepared batch for
    // this series would potentially conflict with our choice to reopen here, so we must wait for
    // any such operation to finish and retry.
    if (auto waiter = checkForWait(stripe, stripeLock, info.key, boost::none)) {
        return waiter.value();
    }

    boost::optional<BSONElement> metaElement;
    if (info.options.getMetaField().has_value()) {
        metaElement = info.key.metadata.element();
    }

    auto controlMinTimePath = kControlMinFieldNamePrefix.toString() + info.options.getTimeField();
    auto maxDataTimeFieldPath = kDataFieldNamePrefix.toString() + info.options.getTimeField() +
        "." + std::to_string(gTimeseriesBucketMaxCount - 1);

    // Derive the maximum bucket size.
    auto storageCacheSize = static_cast<uint64_t>(
        opCtx->getServiceContext()->getStorageEngine()->getEngine()->getCacheSizeMB() * 1024 *
        1024);
    auto [bucketMaxSize, _] =
        getCacheDerivedBucketMaxSize(storageCacheSize, catalog.numberOfActiveBuckets.load());

    return ReopeningContext{catalog,
                            stripe,
                            stripeLock,
                            info.key.cloneAsUntracked(),
                            catalogEra,
                            generateReopeningPipeline(opCtx,
                                                      info.time,
                                                      metaElement,
                                                      controlMinTimePath,
                                                      maxDataTimeFieldPath,
                                                      *info.options.getBucketMaxSpanSeconds(),
                                                      bucketMaxSize)};
}

void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           std::shared_ptr<WriteBatch> batch,
           const Status& status) {
    // Before we access the bucket, make sure it's still there.
    Bucket* bucket = useBucket(catalog.bucketStateRegistry,
                               stripe,
                               stripeLock,
                               batch->bucketHandle.bucketId,
                               IgnoreBucketState::kYes);
    if (!bucket) {
        // Special case, bucket has already been cleared, and we need only abort this batch.
        abortWriteBatch(*batch, status);
        return;
    }

    // Proceed to abort any unprepared batches and remove the bucket if possible
    abort(catalog, stripe, stripeLock, *bucket, batch, status);
}

void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           Bucket& bucket,
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
        removeBucket(catalog, stripe, stripeLock, bucket, RemovalMode::kAbort);
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

void expireIdleBuckets(OperationContext* opCtx,
                       BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       ExecutionStatsController& stats,
                       ClosedBuckets& closedBuckets) {
    // As long as we still need space and have entries and remaining attempts, close idle buckets.
    int32_t numExpired = 0;

    while (!stripe.idleBuckets.empty() &&
           getMemoryUsage(catalog) > catalog.memoryUsageThreshold() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {
        Bucket* bucket = stripe.idleBuckets.back();

        auto state = getBucketState(catalog.bucketStateRegistry, bucket);
        if (state && !conflictsWithInsertions(state.value())) {
            // Can archive a bucket if it's still eligible for insertions.
            archiveBucket(opCtx, catalog, stripe, stripeLock, *bucket, closedBuckets);
            stats.incNumBucketsArchivedDueToMemoryThreshold();
        } else if (state &&
                   (isBucketStateCleared(state.value()) || isBucketStateFrozen(state.value()))) {
            // Bucket was cleared and just needs to be removed from catalog.
            removeBucket(catalog, stripe, stripeLock, *bucket, RemovalMode::kAbort);
        } else {
            closeOpenBucket(opCtx, catalog, stripe, stripeLock, *bucket, closedBuckets);
            stats.incNumBucketsClosedDueToMemoryThreshold();
        }

        ++numExpired;
    }

    while (!stripe.archivedBuckets.empty() &&
           getMemoryUsage(catalog) > catalog.memoryUsageThreshold() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {

        auto& [hash, archivedSet] = *stripe.archivedBuckets.begin();
        invariant(!archivedSet.empty());

        auto& [timestamp, bucket] = *archivedSet.begin();
        closeArchivedBucket(catalog, bucket, closedBuckets);
        if (archivedSet.size() == 1) {
            // If this is the only entry, erase the whole map so we don't leave it empty.
            stripe.archivedBuckets.erase(stripe.archivedBuckets.begin());
        } else {
            // Otherwise just erase this bucket from the map.
            archivedSet.erase(archivedSet.begin());
        }
        catalog.numberOfActiveBuckets.fetchAndSubtract(1);

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

Bucket& allocateBucket(OperationContext* opCtx,
                       BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       const CreationInfo& info) {
    expireIdleBuckets(opCtx, catalog, stripe, stripeLock, info.stats, *info.closedBuckets);

    // In rare cases duplicate bucket _id fields can be generated in the same stripe and fail to be
    // inserted. We will perform a limited number of retries to minimize the probability of
    // collision.
    auto maxRetries = gTimeseriesInsertMaxRetriesOnDuplicates.load();
    OID oid;
    Date_t roundedTime;
    tracked_unordered_map<BucketId, unique_tracked_ptr<Bucket>, BucketHasher>::iterator it;
    bool inserted = false;
    for (int retryAttempts = 0; !inserted && retryAttempts < maxRetries; ++retryAttempts) {
        std::tie(oid, roundedTime) = generateBucketOID(info.time, info.options);
        auto bucketId = BucketId{info.key.collectionUUID, oid};
        std::tie(it, inserted) = stripe.openBucketsById.try_emplace(
            bucketId,
            make_unique_tracked<Bucket>(catalog.trackingContext,
                                        catalog.trackingContext,
                                        bucketId,
                                        info.key.cloneAsTracked(catalog.trackingContext),
                                        info.options.getTimeField(),
                                        roundedTime,
                                        catalog.bucketStateRegistry));
        if (!inserted) {
            resetBucketOIDCounter();
        }
    }
    uassert(6130900,
            "Unable to insert documents due to internal OID generation collision. Increase the "
            "value of server parameter 'timeseriesInsertMaxRetriesOnDuplicates' and try again",
            inserted);

    Bucket* bucket = it->second.get();
    stripe.openBucketsByKey[info.key.cloneAsUntracked()].emplace(bucket);

    auto status = initializeBucketState(catalog.bucketStateRegistry, bucket->bucketId);
    if (!status.isOK()) {
        // Don't track the memory usage for the bucket keys in this data structure because it is
        // already being tracked by the Bucket itself.
        stripe.openBucketsByKey[info.key.cloneAsUntracked()].erase(bucket);
        stripe.openBucketsById.erase(it);
        throwWriteConflictException(status.reason());
    }

    catalog.numberOfActiveBuckets.fetchAndAdd(1);
    // Make sure we set the control.min time field to match the rounded _id timestamp.
    auto controlDoc = buildControlMinTimestampDoc(info.options.getTimeField(), roundedTime);
    bucket->minmax.update(
        controlDoc, /*metaField=*/boost::none, bucket->key.metadata.getComparator());
    return *bucket;
}

Bucket& rollover(OperationContext* opCtx,
                 BucketCatalog& catalog,
                 Stripe& stripe,
                 WithLock stripeLock,
                 Bucket& bucket,
                 const CreationInfo& info,
                 RolloverAction action) {
    invariant(action != RolloverAction::kNone);
    if (allCommitted(bucket)) {
        // The bucket does not contain any measurements that are yet to be committed, so we can take
        // action now.
        if (action == RolloverAction::kArchive) {
            archiveBucket(opCtx, catalog, stripe, stripeLock, bucket, *info.closedBuckets);
        } else {
            closeOpenBucket(opCtx, catalog, stripe, stripeLock, bucket, *info.closedBuckets);
        }
    } else {
        // We must keep the bucket around until all measurements are committed committed, just mark
        // the action we chose now so it we know what to do when the last batch finishes.
        bucket.rolloverAction = action;
    }

    return allocateBucket(opCtx, catalog, stripe, stripeLock, info);
}

std::pair<RolloverAction, RolloverReason> determineRolloverAction(
    OperationContext* opCtx,
    const BSONObj& doc,
    CreationInfo& info,
    Bucket& bucket,
    uint32_t numberOfActiveBuckets,
    Bucket::NewFieldNames& newFieldNamesToBeInserted,
    int32_t& sizeToBeAdded,
    AllowBucketCreation mode) {
    // If the mode is enabled to create new buckets, then we should update stats for soft closures
    // accordingly. If we specify the mode to not allow bucket creation, it means we are not sure if
    // we want to soft close the bucket yet and should wait to update closure stats.
    const bool shouldUpdateStats = (mode == AllowBucketCreation::kYes);

    if (bucket.usingAlwaysCompressedBuckets !=
        feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // The user operation is executing with the always use compressed buckets feature flag in
        // the opposite mode from what the bucket was created with. We close the bucket as this
        // makes the incoming measurement incompatible with this bucket.
        // TODO SERVER-70605: remove this check.
        return {RolloverAction::kHardClose, RolloverReason::kIncompatible};
    }

    auto bucketTime = bucket.minTime;
    if (info.time - bucketTime >= Seconds(*info.options.getBucketMaxSpanSeconds())) {
        if (shouldUpdateStats) {
            info.stats.incNumBucketsClosedDueToTimeForward();
        }
        return {RolloverAction::kSoftClose, RolloverReason::kTimeForward};
    }
    if (info.time < bucketTime) {
        if (shouldUpdateStats) {
            info.stats.incNumBucketsArchivedDueToTimeBackward();
        }
        return {RolloverAction::kArchive, RolloverReason::kTimeBackward};
    }
    if (bucket.numMeasurements == static_cast<std::uint64_t>(gTimeseriesBucketMaxCount)) {
        info.stats.incNumBucketsClosedDueToCount();
        return {RolloverAction::kHardClose, RolloverReason::kCount};
    }

    // In scenarios where we have a high cardinality workload and face increased cache pressure we
    // will decrease the size of buckets before we close them.
    auto storageCacheSize = static_cast<uint64_t>(
        opCtx->getServiceContext()->getStorageEngine()->getEngine()->getCacheSizeMB() * 1024 *
        1024);
    auto [effectiveMaxSize, cacheDerivedBucketMaxSize] =
        getCacheDerivedBucketMaxSize(storageCacheSize, numberOfActiveBuckets);

    // Before we hit our bucket minimum count, we will allow for large measurements to be inserted
    // into buckets. Instead of packing the bucket to the BSON size limit, 16MB, we'll limit the max
    // bucket size to 12MB. This is to leave some space in the bucket if we need to add new internal
    // fields to existing, full buckets.
    static constexpr int32_t largeMeasurementsMaxBucketSize =
        BSONObjMaxUserSize - (4 * 1024 * 1024);
    // We restrict the ceiling of the bucket max size under cache pressure.
    int32_t absoluteMaxSize = std::min(largeMeasurementsMaxBucketSize, cacheDerivedBucketMaxSize);

    calculateBucketFieldsAndSizeChange(
        bucket, doc, info.options.getMetaField(), newFieldNamesToBeInserted, sizeToBeAdded);
    if (bucket.size + sizeToBeAdded > effectiveMaxSize) {
        bool keepBucketOpenForLargeMeasurements =
            bucket.numMeasurements < static_cast<std::uint64_t>(gTimeseriesBucketMinCount);
        if (keepBucketOpenForLargeMeasurements) {
            if (bucket.size + sizeToBeAdded > absoluteMaxSize) {
                if (absoluteMaxSize != largeMeasurementsMaxBucketSize) {
                    info.stats.incNumBucketsClosedDueToCachePressure();
                    return {RolloverAction::kHardClose, RolloverReason::kCachePressure};
                }
                info.stats.incNumBucketsClosedDueToSize();
                return {RolloverAction::kHardClose, RolloverReason::kSize};
            }

            // There's enough space to add this measurement and we're still below the large
            // measurement threshold.
            if (!bucket.keptOpenDueToLargeMeasurements) {
                // Only increment this metric once per bucket.
                bucket.keptOpenDueToLargeMeasurements = true;
                info.stats.incNumBucketsKeptOpenDueToLargeMeasurements();
            }
            return {RolloverAction::kNone, RolloverReason::kNone};
        } else {
            if (effectiveMaxSize == gTimeseriesBucketMaxSize) {
                info.stats.incNumBucketsClosedDueToSize();
                return {RolloverAction::kHardClose, RolloverReason::kSize};
            }
            info.stats.incNumBucketsClosedDueToCachePressure();
            return {RolloverAction::kHardClose, RolloverReason::kCachePressure};
        }
    }

    if (schemaIncompatible(
            bucket, doc, info.options.getMetaField(), info.key.metadata.getComparator())) {
        info.stats.incNumBucketsClosedDueToSchemaChange();
        return {RolloverAction::kHardClose, RolloverReason::kSchemaChange};
    }

    return {RolloverAction::kNone, RolloverReason::kNone};
}

ExecutionStatsController getOrInitializeExecutionStats(BucketCatalog& catalog,
                                                       const UUID& collectionUUID) {
    stdx::lock_guard catalogLock{catalog.mutex};
    auto it = catalog.executionStats.find(collectionUUID);
    if (it != catalog.executionStats.end()) {
        return {it->second, catalog.globalExecutionStats};
    }

    auto res = catalog.executionStats.emplace(
        collectionUUID, make_shared_tracked<ExecutionStats>(catalog.trackingContext));
    return {res.first->second, catalog.globalExecutionStats};
}

shared_tracked_ptr<ExecutionStats> getExecutionStats(const BucketCatalog& catalog,
                                                     const UUID& collectionUUID) {
    static const auto kEmptyStats{std::make_shared<ExecutionStats>()};

    stdx::lock_guard catalogLock{catalog.mutex};

    auto it = catalog.executionStats.find(collectionUUID);
    if (it != catalog.executionStats.end()) {
        return it->second;
    }
    return kEmptyStats;
}

std::pair<UUID, shared_tracked_ptr<ExecutionStats>> getSideBucketCatalogCollectionStats(
    BucketCatalog& sideBucketCatalog) {
    stdx::lock_guard catalogLock{sideBucketCatalog.mutex};
    invariant(sideBucketCatalog.executionStats.size() == 1);
    return *sideBucketCatalog.executionStats.begin();
}

void mergeExecutionStatsToBucketCatalog(BucketCatalog& catalog,
                                        shared_tracked_ptr<ExecutionStats> collStats,
                                        const UUID& collectionUUID) {
    ExecutionStatsController stats = getOrInitializeExecutionStats(catalog, collectionUUID);
    addCollectionExecutionStats(stats, *collStats);
}

Status getTimeseriesBucketClearedError(const NamespaceString& nss, const OID& oid) {
    return {ErrorCodes::TimeseriesBucketCleared,
            str::stream() << "Time-series bucket " << oid << " for collection "
                          << (nss.isTimeseriesBucketsCollection()
                                  ? nss.getTimeseriesViewNamespace().toStringForErrorMsg()
                                  : nss.toStringForErrorMsg())
                          << " was cleared"};
}

void closeOpenBucket(OperationContext* opCtx,
                     BucketCatalog& catalog,
                     Stripe& stripe,
                     WithLock stripeLock,
                     Bucket& bucket,
                     ClosedBuckets& closedBuckets) {
    // Skip creating a ClosedBucket when the bucket is already compressed. Check that
    // compressed is set because reopened uncompressed buckets can get closed without operations
    // against them.
    if (bucket.usingAlwaysCompressedBuckets && bucket.compressedBucketDoc) {
        // Remove the bucket from the bucket state registry.
        stopTrackingBucketState(catalog.bucketStateRegistry, bucket.bucketId);

        updateCompressionStatistics(catalog, bucket);
        removeBucket(catalog, stripe, stripeLock, bucket, RemovalMode::kClose);
        return;
    }

    invariant(!bucket.compressedBucketDoc);
    bool error = false;
    try {
        closedBuckets.emplace_back(
            &catalog.bucketStateRegistry,
            bucket.bucketId,
            bucket.timeField,
            bucket.numMeasurements,
            getOrInitializeExecutionStats(catalog, bucket.bucketId.collectionUUID));
    } catch (...) {
        error = true;
    }
    removeBucket(
        catalog, stripe, stripeLock, bucket, error ? RemovalMode::kAbort : RemovalMode::kClose);
}

void closeOpenBucket(OperationContext* opCtx,
                     BucketCatalog& catalog,
                     Stripe& stripe,
                     WithLock stripeLock,
                     Bucket& bucket,
                     boost::optional<ClosedBucket>& closedBucket) {
    // Skip creating a ClosedBucket when the bucket is already compressed. Check that
    // compressed is set because reopened uncompressed buckets can get closed without operations
    // against them.
    if (bucket.usingAlwaysCompressedBuckets && bucket.compressedBucketDoc) {
        // Remove the bucket from the bucket state registry.
        stopTrackingBucketState(catalog.bucketStateRegistry, bucket.bucketId);

        updateCompressionStatistics(catalog, bucket);
        removeBucket(catalog, stripe, stripeLock, bucket, RemovalMode::kClose);
        return;
    }

    invariant(!bucket.compressedBucketDoc);
    bool error = false;
    try {
        closedBucket =
            boost::in_place(&catalog.bucketStateRegistry,
                            bucket.bucketId,
                            bucket.timeField,
                            bucket.numMeasurements,
                            getOrInitializeExecutionStats(catalog, bucket.bucketId.collectionUUID));
    } catch (...) {
        closedBucket = boost::none;
        error = true;
    }
    removeBucket(
        catalog, stripe, stripeLock, bucket, error ? RemovalMode::kAbort : RemovalMode::kClose);
}

void closeArchivedBucket(BucketCatalog& catalog,
                         ArchivedBucket& bucket,
                         ClosedBuckets& closedBuckets) {
    if (feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // Remove the bucket from the bucket state registry.
        stopTrackingBucketState(catalog.bucketStateRegistry, bucket.bucketId);
        return;
    }

    try {
        closedBuckets.emplace_back(
            &catalog.bucketStateRegistry,
            bucket.bucketId,
            bucket.timeField.c_str(),
            boost::none,
            getOrInitializeExecutionStats(catalog, bucket.bucketId.collectionUUID));
    } catch (...) {
    }
}

void runPostCommitDebugChecks(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const Bucket& bucket,
                              const WriteBatch& batch) {
    // Check in-memory and disk state, caller still has commit rights.
    DBDirectClient client{opCtx};
    BSONObj queriedBucket = client.findOne(nss, BSON("_id" << batch.bucketHandle.bucketId.oid));
    if (!queriedBucket.isEmpty()) {
        uint32_t memCount = batch.numPreviouslyCommittedMeasurements + batch.measurements.size();
        uint32_t diskCount = isCompressedBucket(queriedBucket)
            ? static_cast<uint32_t>(queriedBucket.getObjectField(kBucketControlFieldName)
                                        .getIntField(kBucketControlCountFieldName))
            : static_cast<uint32_t>(queriedBucket.getObjectField(kBucketDataFieldName)
                                        .getObjectField(bucket.timeField)
                                        .nFields());
        invariant(memCount == diskCount,
                  str::stream() << "Expected in-memory (" << memCount << ") and on-disk ("
                                << diskCount
                                << ") measurement counts to match. Bucket contents on disk: "
                                << queriedBucket.toString());
    }
}

}  // namespace mongo::timeseries::bucket_catalog::internal
