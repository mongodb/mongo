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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/bucket_compression_failure.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/tracking/context.h"

#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries::bucket_catalog {
namespace {
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationAfterStart);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationBeforeFinish);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeReopeningBucket);

/**
 * Prepares the batch for commit. Sets min/max appropriately, records the number of
 * documents that have previously been committed to the bucket, and renders the batch
 * inactive.
 */
void prepareWriteBatchForCommit(TrackingContexts& trackingContexts,
                                WriteBatch& batch,
                                Bucket& bucket,
                                const StringDataComparator* comparator) {
    batch.numPreviouslyCommittedMeasurements = bucket.numCommittedMeasurements;

    // Filter out field names that were new at the time of insertion, but have since been committed
    // by someone else.
    for (auto it = batch.newFieldNamesToBeInserted.begin();
         it != batch.newFieldNamesToBeInserted.end();) {
        tracking::StringMapHashedKey fieldName(
            getTrackingContext(trackingContexts, TrackingScope::kMiscellaneous),
            it->first,
            it->second);
        bucket.uncommittedFieldNames.erase(fieldName);
        if (bucket.fieldNames.contains(fieldName)) {
            batch.newFieldNamesToBeInserted.erase(it++);
            continue;
        }

        bucket.fieldNames.emplace(fieldName);
        ++it;
    }

    for (const auto& doc : batch.measurements) {
        bucket.minmax.update(doc, bucket.key.metadata.getMetaField(), comparator);
    }

    const bool isUpdate = batch.numPreviouslyCommittedMeasurements > 0;
    if (isUpdate) {
        batch.min = bucket.minmax.minUpdates();
        batch.max = bucket.minmax.maxUpdates();
    } else {
        batch.min = bucket.minmax.min();
        batch.max = bucket.minmax.max();
    }

    // Move BSONColumnBuilders from Bucket to WriteBatch.
    // See corollary in finish().
    batch.measurementMap = std::move(bucket.measurementMap);
    batch.bucketIsSortedByTime = bucket.bucketIsSortedByTime;
    batch.isReopened = bucket.isReopened;
}

/**
 * Notifies anyone waiting on the promise.
 * Inactive batches only.
 */
void finishWriteBatch(WriteBatch& batch) {
    batch.promise.emplaceValue();
}

/**
 * Disallows query-based reopening if the rollover reason is not time-backward.
 * Once 'allowQueryBasedReopening' is disabled, it cannot be re-enabled.
 */
void decideQueryBasedReopening(const RolloverReason& rolloverReason,
                               AllowQueryBasedReopening& allowQueryBasedReopening) {
    switch (allowQueryBasedReopening) {
        case AllowQueryBasedReopening::kDisallow: {
            return;
        }
        case AllowQueryBasedReopening::kAllow: {
            if (rolloverReason != RolloverReason::kTimeBackward) {
                allowQueryBasedReopening = AllowQueryBasedReopening::kDisallow;
            }
            return;
        }
    }
    MONGO_UNREACHABLE;
}
}  // namespace

Stripe::Stripe(TrackingContexts& trackingContexts)
    : openBucketsById(
          tracking::make_unordered_map<BucketId, tracking::unique_ptr<Bucket>, BucketHasher>(
              getTrackingContext(trackingContexts, TrackingScope::kOpenBucketsById))),
      openBucketsByKey(
          tracking::make_unordered_map<BucketKey, tracking::flat_hash_set<Bucket*>, BucketHasher>(
              getTrackingContext(trackingContexts, TrackingScope::kOpenBucketsByKey))),
      idleBuckets(tracking::make_list<Bucket*>(
          getTrackingContext(trackingContexts, TrackingScope::kIdleBuckets))),
      archivedBuckets(
          tracking::make_btree_map<ArchivedKey, ArchivedBucket, std::greater<ArchivedKey>>(
              getTrackingContext(trackingContexts, TrackingScope::kArchivedBuckets))),
      outstandingReopeningRequests(
          tracking::make_unordered_map<
              BucketKey,
              tracking::inlined_vector<tracking::shared_ptr<ReopeningRequest>, kInlinedVectorSize>,
              BucketHasher>(
              getTrackingContext(trackingContexts, TrackingScope::kReopeningRequests))) {}

BucketCatalog::BucketCatalog(size_t numberOfStripes, std::function<uint64_t()> memoryUsageThreshold)
    : bucketStateRegistry(
          getTrackingContext(trackingContexts, TrackingScope::kBucketStateRegistry)),
      numberOfStripes(numberOfStripes),
      stripes(tracking::make_vector<tracking::unique_ptr<Stripe>>(
          getTrackingContext(trackingContexts, TrackingScope::kMiscellaneous))),
      executionStats(tracking::make_unordered_map<UUID, tracking::shared_ptr<ExecutionStats>>(
          getTrackingContext(trackingContexts, TrackingScope::kStats))),
      memoryUsageThreshold(memoryUsageThreshold) {
    stripes.reserve(numberOfStripes);
    std::generate_n(std::back_inserter(stripes), numberOfStripes, [&]() {
        return tracking::make_unique<Stripe>(
            getTrackingContext(trackingContexts, TrackingScope::kMiscellaneous), trackingContexts);
    });
}

BatchedInsertContext::BatchedInsertContext(
    BucketKey& bucketKey,
    StripeNumber stripeNumber,
    const TimeseriesOptions& options,
    ExecutionStatsController& stats,
    std::vector<BatchedInsertTuple>& measurementsTimesAndIndices)
    : key(std::move(bucketKey)),
      stripeNumber(stripeNumber),
      options(options),
      stats(stats),
      measurementsTimesAndIndices(measurementsTimesAndIndices) {};

uint64_t getMemoryUsage(const BucketCatalog& catalog) {
#ifndef MONGO_CONFIG_DEBUG_BUILD
    return catalog.trackingContexts.global.allocated();
#else
    return catalog.trackingContexts.archivedBuckets.allocated() +
        catalog.trackingContexts.bucketStateRegistry.allocated() +
        catalog.trackingContexts.columnBuilders.allocated() +
        catalog.trackingContexts.idleBuckets.allocated() +
        catalog.trackingContexts.miscellaneous.allocated() +
        catalog.trackingContexts.openBucketsById.allocated() +
        catalog.trackingContexts.openBucketsByKey.allocated() +
        catalog.trackingContexts.reopeningRequests.allocated() +
        catalog.trackingContexts.stats.allocated() +
        catalog.trackingContexts.summaries.allocated() +
        catalog.trackingContexts.measurementBatching.allocated();
#endif
}

void getDetailedMemoryUsage(const BucketCatalog& catalog, BSONObjBuilder& builder) {
#ifndef MONGO_CONFIG_DEBUG_BUILD
    return;
#else
    BSONObjBuilder subBuilder(builder.subobjStart("memoryUsageDetails"_sd));

    subBuilder.appendNumber(
        "archivedBuckets",
        static_cast<long long>(catalog.trackingContexts.archivedBuckets.allocated()));
    subBuilder.appendNumber(
        "bucketStateRegistry",
        static_cast<long long>(catalog.trackingContexts.bucketStateRegistry.allocated()));
    subBuilder.appendNumber(
        "columnBuilders",
        static_cast<long long>(catalog.trackingContexts.columnBuilders.allocated()));
    subBuilder.appendNumber(
        "idleBuckets", static_cast<long long>(catalog.trackingContexts.idleBuckets.allocated()));
    subBuilder.appendNumber(
        "miscellaneous",
        static_cast<long long>(catalog.trackingContexts.miscellaneous.allocated()));
    subBuilder.appendNumber(
        "openBucketsById",
        static_cast<long long>(catalog.trackingContexts.openBucketsById.allocated()));
    subBuilder.appendNumber(
        "openBucketsByKey",
        static_cast<long long>(catalog.trackingContexts.openBucketsByKey.allocated()));
    subBuilder.appendNumber(
        "reopeningRequests",
        static_cast<long long>(catalog.trackingContexts.reopeningRequests.allocated()));
    subBuilder.appendNumber("stats",
                            static_cast<long long>(catalog.trackingContexts.stats.allocated()));
    subBuilder.appendNumber("summaries",
                            static_cast<long long>(catalog.trackingContexts.summaries.allocated()));
    subBuilder.appendNumber(
        "measurementBatching",
        static_cast<long long>(catalog.trackingContexts.measurementBatching.allocated()));
#endif
}

boost::optional<InsertWaiter> checkForReopeningConflict(Stripe& stripe,
                                                        WithLock stripeLock,
                                                        const BucketKey& bucketKey,
                                                        boost::optional<OID> archivedCandidate) {
    if (auto batch =
            internal::findPreparedBatch(stripe, stripeLock, bucketKey, archivedCandidate)) {
        return InsertWaiter{batch};
    }

    if (auto it = stripe.outstandingReopeningRequests.find(bucketKey);
        it != stripe.outstandingReopeningRequests.end()) {
        auto& requests = it->second;
        invariant(!requests.empty());

        if (!archivedCandidate.has_value()) {
            // We are trying to perform a query-based reopening. This conflicts with any reopening
            // for the key.
            return InsertWaiter{requests.front()};
        }

        // We are about to attempt an archive-based reopening. This conflicts with any query-based
        // reopening for the key, or another archive-based reopening for this bucket.
        for (auto&& request : requests) {
            if (!request->oid.has_value() ||
                (request->oid.has_value() && request->oid.value() == archivedCandidate.value())) {
                return InsertWaiter{request};
            }
        }
    }

    return boost::none;
}

void waitToInsert(InsertWaiter* waiter) {
    if (auto* batch = get_if<std::shared_ptr<WriteBatch>>(waiter)) {
        getWriteBatchStatus(**batch).ignore();
    } else if (auto* request = get_if<std::shared_ptr<ReopeningRequest>>(waiter)) {
        waitForReopeningRequest(**request);
    }
}

Status prepareCommit(BucketCatalog& catalog,
                     std::shared_ptr<WriteBatch> batch,
                     const StringDataComparator* comparator) {
    auto getBatchStatus = [&] {
        return batch->promise.getFuture().getNoThrow();
    };

    if (isWriteBatchFinished(*batch)) {
        // In this case, someone else aborted the batch behind our back. Oops.
        return getBatchStatus();
    }

    auto& stripe = *catalog.stripes[internal::getStripeNumber(catalog, batch->bucketId)];
    internal::waitToCommitBatch(catalog.bucketStateRegistry, stripe, batch);

    stdx::lock_guard stripeLock{stripe.mutex};

    if (isWriteBatchFinished(*batch)) {
        // Someone may have aborted it while we were waiting. Since we have the prepared batch, we
        // should now be able to fully abort the bucket.
        internal::abort(catalog, stripe, stripeLock, batch, getBatchStatus());
        return getBatchStatus();
    }

    Bucket* bucket =
        internal::useBucketAndChangePreparedState(catalog.bucketStateRegistry,
                                                  stripe,
                                                  stripeLock,
                                                  batch->bucketId,
                                                  internal::BucketPrepareAction::kPrepare);

    if (!bucket) {
        internal::abort(catalog,
                        stripe,
                        stripeLock,
                        batch,
                        internal::getTimeseriesBucketClearedError(batch->bucketId.oid));
        return getBatchStatus();
    }

    prepareWriteBatchForCommit(catalog.trackingContexts, *batch, *bucket, comparator);

    return Status::OK();
}

void finish(BucketCatalog& catalog, std::shared_ptr<WriteBatch> batch) {
    invariant(!isWriteBatchFinished(*batch));

    finishWriteBatch(*batch);

    auto& stripe = *catalog.stripes[internal::getStripeNumber(catalog, batch->bucketId)];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket =
        internal::useBucketAndChangePreparedState(catalog.bucketStateRegistry,
                                                  stripe,
                                                  stripeLock,
                                                  batch->bucketId,
                                                  internal::BucketPrepareAction::kUnprepare);
    if (bucket) {
        // Move BSONColumnBuilders from WriteBatch to Bucket.
        // See corollary in prepareWriteBatchForCommit().
        bucket->bucketIsSortedByTime = batch->bucketIsSortedByTime;

        bucket->size -= batch->sizes.uncommittedMeasurementEstimate;
        bucket->size += batch->sizes.uncommittedVerifiedSize;
        bucket->measurementMap = std::move(batch->measurementMap);
        bucket->preparedBatch.reset();
    }

    auto& stats = batch->stats;
    stats.incNumCommits();
    if (batch->numPreviouslyCommittedMeasurements == 0) {
        stats.incNumBucketInserts();
    } else {
        stats.incNumBucketUpdates();
    }

    if (batch->openedDueToMetadata) {
        stats.incNumBucketsOpenedDueToMetadata();
    }

    stats.incNumMeasurementsCommitted(batch->measurements.size());
    if (bucket) {
        bucket->numCommittedMeasurements += batch->measurements.size();
    }

    if (!bucket) {
        // It's possible that we cleared the bucket in between preparing the commit and finishing
        // here. In this case, we should abort any other ongoing batches and clear the bucket from
        // the catalog so it's not hanging around idle.
        auto it = stripe.openBucketsById.find(batch->bucketId);
        if (it != stripe.openBucketsById.end()) {
            bucket = it->second.get();
            bucket->preparedBatch.reset();
            internal::abort(catalog,
                            stripe,
                            stripeLock,
                            *bucket,
                            stats,
                            nullptr,
                            internal::getTimeseriesBucketClearedError(bucket->bucketId.oid));
        }
    } else if (allCommitted(*bucket)) {
        auto action = getRolloverAction(bucket->rolloverReason);

        if (action == RolloverAction::kNone) {
            internal::markBucketIdle(stripe, stripeLock, *bucket);
        } else {
            internal::rollover(catalog, stripe, stripeLock, *bucket, bucket->rolloverReason);
        }
    }
}

void abort(BucketCatalog& catalog, std::shared_ptr<WriteBatch> batch, const Status& status) {
    invariant(batch);

    if (isWriteBatchFinished(*batch)) {
        return;
    }

    auto& stripe = *catalog.stripes[internal::getStripeNumber(catalog, batch->bucketId)];
    stdx::lock_guard stripeLock{stripe.mutex};

    internal::abort(catalog, stripe, stripeLock, batch, status);
}

void directWriteStart(BucketStateRegistry& registry, const BucketId& bucketId) {
    auto state = addDirectWrite(registry, bucketId);
    hangTimeseriesDirectModificationAfterStart.pauseWhileSet();

    if (holds_alternative<DirectWriteCounter>(state)) {
        // The direct write count was successfully incremented.
        return;
    }

    if (isBucketStateFrozen(state)) {
        // It's okay to perform a direct write on a frozen bucket. Multiple direct writes will
        // coordinate via the storage engine's conflict handling. We just need to make sure that
        // direct writes aren't potentially conflicting with normal writes that go through the
        // bucket catalog.
        return;
    }

    // We cannot perform direct writes on prepared buckets.
    invariant(isBucketStatePrepared(state));
    throwWriteConflictException("Prepared bucket can no longer be inserted into.");
}

void directWriteFinish(BucketStateRegistry& registry, const BucketId& bucketId) {
    hangTimeseriesDirectModificationBeforeFinish.pauseWhileSet();
    removeDirectWrite(registry, bucketId);
}

void drop(BucketCatalog& catalog, tracking::vector<UUID> clearedCollectionUUIDs) {
    auto stats = internal::releaseExecutionStatsFromBucketCatalog(catalog, clearedCollectionUUIDs);
    clearSetOfBuckets(catalog.bucketStateRegistry, std::move(clearedCollectionUUIDs));

    for (auto&& collStats : stats) {
        removeCollectionExecutionGauges(catalog.globalExecutionStats, *collStats);
    }
}

void drop(BucketCatalog& catalog, const UUID& collectionUUID) {
    auto stats = internal::releaseExecutionStatsFromBucketCatalog(
        catalog, std::span<const UUID>(&collectionUUID, 1));
    clear(catalog, collectionUUID);

    for (auto&& collStats : stats) {
        removeCollectionExecutionGauges(catalog.globalExecutionStats, *collStats);
    }
}

void clear(BucketCatalog& catalog, const UUID& collectionUUID) {
    tracking::vector<UUID> clearedCollectionUUIDs = tracking::make_vector<UUID>(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kBucketStateRegistry));
    clearedCollectionUUIDs.push_back(collectionUUID);
    clearSetOfBuckets(catalog.bucketStateRegistry, std::move(clearedCollectionUUIDs));
}

void freeze(BucketCatalog& catalog, const BucketId& bucketId) {
    internal::getOrInitializeExecutionStats(catalog, bucketId.collectionUUID).incNumBucketsFrozen();
    freezeBucket(catalog.bucketStateRegistry, bucketId);
}

void markBucketInsertTooLarge(BucketCatalog& catalog, const UUID& collectionUUID) {
    internal::getOrInitializeExecutionStats(catalog, collectionUUID)
        .incNumBucketDocumentsTooLargeInsert();
}

void markBucketUpdateTooLarge(BucketCatalog& catalog, const UUID& collectionUUID) {
    internal::getOrInitializeExecutionStats(catalog, collectionUUID)
        .incNumBucketDocumentsTooLargeUpdate();
}

BucketId extractBucketId(BucketCatalog& bucketCatalog,
                         const TimeseriesOptions& options,
                         const UUID& collectionUUID,
                         const BSONObj& bucket) {
    const OID bucketOID = bucket[kBucketIdFieldName].OID();
    const BSONElement metadata = bucket[kBucketMetaFieldName];
    const BucketKey key{collectionUUID,
                        BucketMetadata{getTrackingContext(bucketCatalog.trackingContexts,
                                                          TrackingScope::kOpenBucketsByKey),
                                       metadata,
                                       options.getMetaField()}};
    return {collectionUUID, bucketOID, key.signature()};
}

BucketKey::Signature getKeySignature(const TimeseriesOptions& options,
                                     const UUID& collectionUUID,
                                     const BSONObj& metadataObj) {
    tracking::Context trackingContext;
    auto metaField = options.getMetaField();
    const BSONElement metadata = metaField ? metadataObj[metaField.value()] : BSONElement();
    const BucketKey key{collectionUUID,
                        BucketMetadata{trackingContext, metadata, options.getMetaField()}};
    return key.signature();
}

void resetBucketOIDCounter() {
    internal::resetBucketOIDCounter();
}

std::pair<OID, Date_t> generateBucketOID(const Date_t& time, const TimeseriesOptions& options) {
    return internal::generateBucketOID(time, options);
}

std::pair<UUID, tracking::shared_ptr<ExecutionStats>> getSideBucketCatalogCollectionStats(
    BucketCatalog& sideBucketCatalog) {
    stdx::lock_guard catalogLock{sideBucketCatalog.mutex};
    invariant(sideBucketCatalog.executionStats.size() == 1);
    return *sideBucketCatalog.executionStats.begin();
}

void mergeExecutionStatsToBucketCatalog(BucketCatalog& catalog,
                                        tracking::shared_ptr<ExecutionStats> collStats,
                                        const UUID& collectionUUID) {
    ExecutionStatsController stats =
        internal::getOrInitializeExecutionStats(catalog, collectionUUID);
    addCollectionExecutionCounters(stats, *collStats);
}

void appendExecutionStats(const BucketCatalog& catalog,
                          const UUID& collectionUUID,
                          BSONObjBuilder& builder) {
    const tracking::shared_ptr<ExecutionStats> stats =
        internal::getCollectionExecutionStats(catalog, collectionUUID);
    if (stats) {
        appendExecutionStatsToBuilder(*stats, builder);
    }
}

RolloverReason determineBucketRolloverForMeasurement(BucketCatalog& catalog,
                                                     const BSONObj& measurement,
                                                     const Date_t& measurementTimestamp,
                                                     const TimeseriesOptions& options,
                                                     const StringDataComparator* comparator,
                                                     const uint64_t storageCacheSizeBytes,
                                                     Bucket& bucket,
                                                     ExecutionStatsController& stats) {
    Bucket::NewFieldNames newFieldNamesToBeInserted;
    Sizes sizesToBeAdded;
    calculateBucketFieldsAndSizeChange(catalog.trackingContexts,
                                       bucket,
                                       measurement,
                                       options.getMetaField(),
                                       newFieldNamesToBeInserted,
                                       sizesToBeAdded);
    auto rolloverReason = internal::determineRolloverReason(
        measurement,
        options,
        catalog.globalExecutionStats.numActiveBuckets.loadRelaxed(),
        sizesToBeAdded,
        measurementTimestamp,
        storageCacheSizeBytes,
        comparator,
        bucket,
        stats);

    if (rolloverReason != RolloverReason::kNone) {
        // Update the bucket's 'rolloverReason'.
        bucket.rolloverReason = rolloverReason;
    }
    return rolloverReason;
}

void sortOnNumMeasurements(absl::InlinedVector<Bucket*, 8>& vec) {
    std::sort(vec.begin(), vec.end(), [](const Bucket* lhs, const Bucket* rhs) {
        return lhs->numMeasurements < rhs->numMeasurements;
    });
}

std::vector<Bucket*> createOrderedPotentialBucketsVector(
    PotentialBucketOptions& potentialBucketOptions) {
    auto& kSoftClosedBuckets = potentialBucketOptions.kSoftClosedBuckets;
    auto& kArchivedBuckets = potentialBucketOptions.kArchivedBuckets;
    bool kNoneBucketExists = (potentialBucketOptions.kNoneBucket != nullptr);

    // Sort in increasing order of the the number of measurements in the buckets.
    // We prioritize buckets with less data in them to improve fill ratios.
    sortOnNumMeasurements(kSoftClosedBuckets);
    sortOnNumMeasurements(kArchivedBuckets);

    // Create the potentialBuckets vector.
    size_t totalSize = kSoftClosedBuckets.size() + kArchivedBuckets.size() + kNoneBucketExists;
    std::vector<Bucket*> potentialBuckets;
    potentialBuckets.reserve(totalSize);

    // We can prioritize kSoftClose candidates over kArchive, since we can easily reopen the
    // kArchive ones whereas we're about to rollover kSoftClose buckets (ignoring query-based
    // reopening).
    potentialBuckets.insert(
        potentialBuckets.end(), kSoftClosedBuckets.begin(), kSoftClosedBuckets.end());
    potentialBuckets.insert(
        potentialBuckets.end(), kArchivedBuckets.begin(), kArchivedBuckets.end());
    if (kNoneBucketExists) {
        potentialBuckets.push_back(potentialBucketOptions.kNoneBucket);
    }
    return potentialBuckets;
}

std::vector<Bucket*> findAndRolloverOpenBuckets(BucketCatalog& catalog,
                                                Stripe& stripe,
                                                WithLock stripeLock,
                                                const BucketKey& bucketKey,
                                                const Date_t& time,
                                                const Seconds& bucketMaxSpanSeconds,
                                                AllowQueryBasedReopening& allowQueryBasedReopening,
                                                bool& bucketOpenedDueToMetadata) {
    PotentialBucketOptions potentialBucketOptions;
    auto openBuckets = internal::findOpenBuckets(stripe, stripeLock, bucketKey);
    for (const auto& openBucket : openBuckets) {
        // We found at least one bucket with the same metadata.
        bucketOpenedDueToMetadata = false;
        auto reason = openBucket->rolloverReason;
        auto action = getRolloverAction(reason);
        switch (action) {
            case RolloverAction::kNone: {
                auto bucketState = internal::isBucketStateEligibleForInsertsAndCleanup(
                    catalog, stripe, stripeLock, openBucket);
                if (bucketState == internal::BucketStateForInsertAndCleanup::kEligibleForInsert) {
                    internal::markBucketNotIdle(stripe, stripeLock, *openBucket);
                    // Only one uncleared open bucket is allowed for each key.
                    invariant(potentialBucketOptions.kNoneBucket == nullptr);
                    // Save the bucket with 'RolloverAction::kNone' to add it to the end of
                    // 'potentialBuckets'.
                    potentialBucketOptions.kNoneBucket = openBucket;
                }
                break;
            }
            case RolloverAction::kHardClose: {
                internal::rollover(catalog, stripe, stripeLock, *openBucket, reason);
                decideQueryBasedReopening(reason, allowQueryBasedReopening);
                break;
            }
            case RolloverAction::kArchive:
            case RolloverAction::kSoftClose: {
                auto bucketMinTime = openBucket->minTime;
                auto bucketState = internal::isBucketStateEligibleForInsertsAndCleanup(
                    catalog, stripe, stripeLock, openBucket);
                if (bucketState == internal::BucketStateForInsertAndCleanup::kInsertionConflict) {
                    // We will have aborted the bucket within
                    // isBucketStateEligibleForInsertsAndCleanup. No further action is required.
                    break;
                }

                if (time >= bucketMinTime && time - bucketMinTime < bucketMaxSpanSeconds &&
                    bucketState == internal::BucketStateForInsertAndCleanup::kEligibleForInsert) {
                    // The time range and the state of the bucket are eligible.
                    if (action == RolloverAction::kArchive) {
                        potentialBucketOptions.kArchivedBuckets.push_back(openBucket);
                    } else {
                        potentialBucketOptions.kSoftClosedBuckets.push_back(openBucket);
                    }
                } else {
                    // We only want to rollover these buckets when we don't have an insertion
                    // conflict; otherwise, we will attempt to remove the bucket twice.
                    internal::rollover(catalog, stripe, stripeLock, *openBucket, reason);
                    decideQueryBasedReopening(reason, allowQueryBasedReopening);
                }
                break;
            }
        }
    }
    // Create vector with all potential buckets.
    return createOrderedPotentialBucketsVector(potentialBucketOptions);
}

Bucket* findOpenBucketForMeasurement(BucketCatalog& catalog,
                                     Stripe& stripe,
                                     WithLock stripeLock,
                                     const BSONObj& measurement,
                                     const BucketKey& bucketKey,
                                     const Date_t& measurementTimestamp,
                                     const TimeseriesOptions& options,
                                     const StringDataComparator* comparator,
                                     const uint64_t storageCacheSizeBytes,
                                     AllowQueryBasedReopening& allowQueryBasedReopening,
                                     ExecutionStatsController& stats,
                                     bool& bucketOpenedDueToMetadata) {
    // Gets a vector of potential buckets, starting with kSoftClose/kArchived buckets, followed by
    // at most one kNone bucket.
    auto potentialBuckets = findAndRolloverOpenBuckets(catalog,
                                                       stripe,
                                                       stripeLock,
                                                       bucketKey,
                                                       measurementTimestamp,
                                                       Seconds(*options.getBucketMaxSpanSeconds()),
                                                       allowQueryBasedReopening,
                                                       bucketOpenedDueToMetadata);
    if (potentialBuckets.empty()) {
        return nullptr;
    }

    for (const auto& potentialBucket : potentialBuckets) {
        // Check if the measurement can fit in the potential bucket.
        auto rolloverReason = determineBucketRolloverForMeasurement(catalog,
                                                                    measurement,
                                                                    measurementTimestamp,
                                                                    options,
                                                                    comparator,
                                                                    storageCacheSizeBytes,
                                                                    *potentialBucket,
                                                                    stats);

        if (rolloverReason == RolloverReason::kNone) {
            // The measurement can be inserted into the open bucket.
            return potentialBucket;
        }

        decideQueryBasedReopening(rolloverReason, allowQueryBasedReopening);
    }

    return nullptr;
}

StatusWith<tracking::unique_ptr<Bucket>> getReopenedBucket(
    OperationContext* opCtx,
    BucketCatalog& catalog,
    const Collection* bucketsColl,
    const BucketKey& bucketKey,
    const TimeseriesOptions& options,
    const std::variant<OID, std::vector<BSONObj>>& reopeningCandidate,
    const BucketStateRegistry::Era catalogEra,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    ExecutionStatsController& stats,
    bool& bucketOpenedDueToMetadata) {
    BSONObj reopenedBucketDoc = visit(
        OverloadedVisitor{
            [&](const OID& bucketId) {
                return internal::reopenFetchedBucket(opCtx, bucketsColl, bucketId, stats);
            },
            [&](const std::vector<BSONObj>& pipeline) {
                return internal::reopenQueriedBucket(opCtx, bucketsColl, options, pipeline, stats);
            },
        },
        reopeningCandidate);

    if (reopenedBucketDoc.isEmpty()) {
        // We couldn't find an eligible bucket document with the 'reopeningCandidate'.
        return {tracking::unique_ptr<Bucket>(
            getTrackingContext(catalog.trackingContexts, TrackingScope::kReopeningRequests),
            nullptr)};
    }
    bucketOpenedDueToMetadata = false;

    if (!timeseries::isCompressedBucket(reopenedBucketDoc)) {
        // Compress the uncompressed bucket document and return.
        auto uncompressedBucketId =
            extractBucketId(catalog, options, bucketsColl->uuid(), reopenedBucketDoc);
        if (const auto& status = internal::compressAndWriteBucket(opCtx,
                                                                  catalog,
                                                                  bucketsColl,
                                                                  uncompressedBucketId,
                                                                  options.getTimeField(),
                                                                  compressAndWriteBucketFunc);
            !status.isOK()) {
            return status;
        }
        return Status{
            ErrorCodes::WriteConflict,
            "existing uncompressed bucket was compressed, retry insert on compressed bucket"};
    }

    // Instantiate the in-memory bucket representation of the reopened bucket document.
    auto bucketDocumentValidator = [&](const BSONObj& bucketDoc) {
        return bucketsColl->checkValidation(opCtx, bucketDoc);
    };
    return internal::rehydrateBucket(catalog,
                                     reopenedBucketDoc,
                                     bucketKey,
                                     options,
                                     catalogEra,
                                     bucketsColl->getDefaultCollator(),
                                     bucketDocumentValidator,
                                     stats);
}

Bucket& getEligibleBucket(OperationContext* opCtx,
                          BucketCatalog& catalog,
                          Stripe& stripe,
                          stdx::unique_lock<stdx::mutex>& stripeLock,
                          const Collection* bucketsColl,
                          const BSONObj& measurement,
                          const BucketKey& bucketKey,
                          const Date_t& measurementTimestamp,
                          const TimeseriesOptions& options,
                          const StringDataComparator* comparator,
                          const uint64_t storageCacheSizeBytes,
                          const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
                          AllowQueryBasedReopening allowQueryBasedReopening,
                          ExecutionStatsController& stats,
                          bool& bucketOpenedDueToMetadata) {
    Status reopeningStatus = Status::OK();
    auto numReopeningsAttempted = 0;
    auto reopeningLimit = gTimeseriesMaxRetriesForWriteConflictsOnReopening.load();
    do {
        // This can be disabled for the lifetime of the opCtx to avoid writing to the same bucket
        // that already failed.
        // 1. Try to find an eligible open bucket for the next measurement.
        if (auto eligibleBucket = findOpenBucketForMeasurement(catalog,
                                                               stripe,
                                                               stripeLock,
                                                               measurement,
                                                               bucketKey,
                                                               measurementTimestamp,
                                                               options,
                                                               comparator,
                                                               storageCacheSizeBytes,
                                                               allowQueryBasedReopening,
                                                               stats,
                                                               bucketOpenedDueToMetadata)) {
            return *eligibleBucket;
        }

        // Do not reopen existing buckets if not using the main bucket catalog.
        if (MONGO_unlikely(&catalog != &GlobalBucketCatalog::get(opCtx->getServiceContext()))) {
            return internal::allocateBucket(catalog,
                                            stripe,
                                            stripeLock,
                                            bucketKey,
                                            options,
                                            measurementTimestamp,
                                            comparator,
                                            stats);
        }

        // 2. Attempt to reopen a bucket.
        // Save the catalog era value from before trying to reopen a bucket. This guarantees that we
        // don't miss a direct write that happens sometime in between our decision to potentially
        // reopen a bucket below, and actually reopening it in a subsequent reentrant call. Any
        // direct write will increment the era, so the reentrant call can check the current value
        // and return a write conflict if it sees a newer era.
        const auto catalogEra = getCurrentEra(catalog.bucketStateRegistry);
        // Explicitly pass in the lock which can be unlocked and relocked during reopening.
        auto swReopenedBucket = potentiallyReopenBucket(opCtx,
                                                        catalog,
                                                        stripe,
                                                        stripeLock,
                                                        bucketsColl,
                                                        bucketKey,
                                                        measurementTimestamp,
                                                        options,
                                                        catalogEra,
                                                        allowQueryBasedReopening,
                                                        storageCacheSizeBytes,
                                                        compressAndWriteBucketFunc,
                                                        stats,
                                                        bucketOpenedDueToMetadata);
        if (swReopenedBucket.isOK() && swReopenedBucket.getValue()) {
            auto& reopenedBucket = *swReopenedBucket.getValue();
            auto rolloverReason = determineBucketRolloverForMeasurement(catalog,
                                                                        measurement,
                                                                        measurementTimestamp,
                                                                        options,
                                                                        comparator,
                                                                        storageCacheSizeBytes,
                                                                        reopenedBucket,
                                                                        stats);
            if (rolloverReason == RolloverReason::kNone) {
                // Use the reopened bucket if the measurement can fit there.
                return reopenedBucket;
            }
        }

        reopeningStatus = swReopenedBucket.getStatus();
        // Try again when reopening or the reopened bucket encounters a conflict.
    } while (reopeningStatus.code() == ErrorCodes::WriteConflict &&
             ++numReopeningsAttempted < reopeningLimit);

    // 3. Reopening can release and reacquire the stripe lock. Look for an eligible open bucket
    // again. If not found, allocate a new bucket this time.

    // This variable is intended to only be consumed by potentiallyReopenBucket(). Its value
    // doesn't indicate anything about whether a reopening has happened, but one will not be
    // happening after this point, so it is set to kDisallow to reinforce this fact.
    auto allowQueryBasedReopeningUnused = AllowQueryBasedReopening::kDisallow;
    if (auto eligibleBucket = findOpenBucketForMeasurement(catalog,
                                                           stripe,
                                                           stripeLock,
                                                           measurement,
                                                           bucketKey,
                                                           measurementTimestamp,
                                                           options,
                                                           comparator,
                                                           storageCacheSizeBytes,
                                                           allowQueryBasedReopeningUnused,
                                                           stats,
                                                           bucketOpenedDueToMetadata)) {
        return *eligibleBucket;
    }

    return internal::allocateBucket(
        catalog, stripe, stripeLock, bucketKey, options, measurementTimestamp, comparator, stats);
}

StatusWith<Bucket*> potentiallyReopenBucket(
    OperationContext* opCtx,
    BucketCatalog& catalog,
    Stripe& stripe,
    stdx::unique_lock<stdx::mutex>& stripeLock,
    const Collection* bucketsColl,
    const BucketKey& bucketKey,
    const Date_t& time,
    const TimeseriesOptions& options,
    const BucketStateRegistry::Era catalogEra,
    const AllowQueryBasedReopening& allowQueryBasedReopening,
    const uint64_t storageCacheSizeBytes,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    ExecutionStatsController& stats,
    bool& bucketOpenedDueToMetadata) {
    // Get the information needed for reopening.
    boost::optional<std::variant<OID, std::vector<BSONObj>>> reopeningCandidate;
    boost::optional<InsertWaiter> reopeningConflict;
    if (const auto& archivedCandidate = internal::getArchiveReopeningCandidate(
            catalog, stripe, stripeLock, bucketKey, options, time)) {
        reopeningConflict =
            checkForReopeningConflict(stripe, stripeLock, bucketKey, archivedCandidate);
        if (!reopeningConflict) {
            reopeningCandidate = archivedCandidate.get();
        }
    } else if (allowQueryBasedReopening == AllowQueryBasedReopening::kAllow) {
        reopeningConflict = checkForReopeningConflict(stripe, stripeLock, bucketKey);
        if (!reopeningConflict) {
            reopeningCandidate = internal::getQueryReopeningCandidate(
                catalog, stripe, stripeLock, bucketKey, options, storageCacheSizeBytes, time);
        }
    }

    if (reopeningConflict) {
        // Need to wait for another operation to finish. This could be another reopening request or
        // a previously prepared write batch for the same series (metaField value).

        // Release the stripe lock to wait for the conflicting operation.
        ScopedUnlock unlockGuard(stripeLock);

        waitToInsert(&reopeningConflict.get());
        return Status{ErrorCodes::WriteConflict, "waited to retry"};
    }

    if (!reopeningCandidate) {
        // No suitable bucket can be found for reopening.
        return nullptr;
    }

    ReopeningScope reopeningScope(catalog, stripe, stripeLock, bucketKey, reopeningCandidate.get());

    tracking::unique_ptr<Bucket> reopenedBucket(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kReopeningRequests), nullptr);
    {
        // Reopening can take some time. Release the stripe lock first.
        ScopedUnlock unlockGuard(stripeLock);

        // Reopen the bucket and initialize its in-memory states.
        auto swReopenedBucket = getReopenedBucket(opCtx,
                                                  catalog,
                                                  bucketsColl,
                                                  bucketKey,
                                                  options,
                                                  reopeningCandidate.get(),
                                                  catalogEra,
                                                  compressAndWriteBucketFunc,
                                                  stats,
                                                  bucketOpenedDueToMetadata);

        if (!swReopenedBucket.isOK()) {
            return swReopenedBucket.getStatus();
        }
        if (!swReopenedBucket.getValue()) {
            return nullptr;
        }

        hangTimeseriesInsertBeforeReopeningBucket.pauseWhileSet();
        reopenedBucket = std::move(swReopenedBucket.getValue());
        // Reacquire the stripe lock to load the bucket back into the catalog.
    }

    // It's possible to re-open an opened bucket. This behavior isn't limited to rollover.
    auto existingIt = stripe.openBucketsById.find(reopenedBucket->bucketId);
    if (existingIt != stripe.openBucketsById.end()) {
        stats.incNumDuplicateBucketsReopened();
        return nullptr;
    }
    auto swBucket = internal::loadBucketIntoCatalog(
        catalog, stripe, stripeLock, stats, bucketKey, std::move(reopenedBucket), catalogEra);
    if (!swBucket.isOK()) {
        return swBucket.getStatus();
    }

    return &swBucket.getValue().get();
}

std::vector<BatchedInsertContext> buildBatchedInsertContextsNoMetaField(
    const BucketCatalog& bucketCatalog,
    const UUID& collectionUUID,
    const TimeseriesOptions& timeseriesOptions,
    const std::vector<BSONObj>& userMeasurementsBatch,
    size_t startIndex,
    size_t numDocsToStage,
    const std::vector<size_t>& indices,
    ExecutionStatsController& stats,
    tracking::Context& trackingContext,
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices) {

    std::vector<BatchedInsertTuple> batchedInsertTupleVector;

    auto processMeasurement = [&](size_t index) {
        invariant(index < userMeasurementsBatch.size());
        auto swTime = extractTime(userMeasurementsBatch[index], timeseriesOptions.getTimeField());
        if (!swTime.isOK()) {
            errorsAndIndices.push_back(
                WriteStageErrorAndIndex{std::move(swTime.getStatus()), index});
            return;
        }
        batchedInsertTupleVector.emplace_back(
            userMeasurementsBatch[index], swTime.getValue(), index);
    };

    // As part of the InsertBatchTuple struct we store the index of the measurement in the original
    // user batch for error reporting and retryability purposes.
    if (!indices.empty()) {
        std::for_each(indices.begin(), indices.end(), processMeasurement);
    } else {
        for (size_t i = startIndex; i < startIndex + numDocsToStage; i++) {
            processMeasurement(i);
        }
    }

    // Empty metadata.
    BSONElement metadata;
    auto bucketKey =
        BucketKey{collectionUUID, BucketMetadata{trackingContext, metadata, boost::none}};
    auto stripeNumber = internal::getStripeNumber(bucketCatalog, bucketKey);

    std::sort(
        batchedInsertTupleVector.begin(), batchedInsertTupleVector.end(), [](auto& lhs, auto& rhs) {
            // Sort measurements on their timeField.
            return std::get<Date_t>(lhs) < std::get<Date_t>(rhs);
        });

    std::vector<BatchedInsertContext> batchedInsertContexts;

    // Only create a BatchedInsertContext if at least one measurement got processed successfully.
    if (!batchedInsertTupleVector.empty()) {
        batchedInsertContexts.emplace_back(
            bucketKey, stripeNumber, timeseriesOptions, stats, batchedInsertTupleVector);
    };

    return batchedInsertContexts;
};

std::vector<BatchedInsertContext> buildBatchedInsertContextsWithMetaField(
    const BucketCatalog& bucketCatalog,
    const UUID& collectionUUID,
    const TimeseriesOptions& timeseriesOptions,
    const std::vector<BSONObj>& userMeasurementsBatch,
    size_t startIndex,
    size_t numDocsToStage,
    const std::vector<size_t>& indices,
    ExecutionStatsController& stats,
    tracking::Context& trackingContext,
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices) {
    auto timeField = timeseriesOptions.getTimeField();
    auto metaField = timeseriesOptions.getMetaField().get();

    // Maps distinct metaField values using the BucketMetadata as the key to a vector of
    // BatchedInsertTuples whose measurements have that same metaField value.
    stdx::unordered_map<BucketMetadata, std::vector<BatchedInsertTuple>>
        metaFieldToBatchedInsertTuples;

    auto processMeasurement = [&](size_t index) {
        invariant(index < userMeasurementsBatch.size());
        auto swTimeAndMeta = extractTimeAndMeta(userMeasurementsBatch[index], timeField, metaField);
        if (!swTimeAndMeta.isOK()) {
            errorsAndIndices.push_back(
                WriteStageErrorAndIndex{std::move(swTimeAndMeta.getStatus()), index});
            return;
        }
        auto time = std::get<Date_t>(swTimeAndMeta.getValue());
        auto meta = std::get<BSONElement>(swTimeAndMeta.getValue());

        BucketMetadata metadata = BucketMetadata{trackingContext, meta, metaField};
        metaFieldToBatchedInsertTuples.try_emplace(metadata, std::vector<BatchedInsertTuple>{});

        metaFieldToBatchedInsertTuples[metadata].emplace_back(
            userMeasurementsBatch[index], time, index);
    };
    // Go through the vector of user measurements and create a map from each distinct metaField
    // value using BucketMetadata to a vector of InsertBatchTuples for that metaField. As part of
    // the InsertBatchTuple struct we store the index of the measurement in the original user batch
    // for error reporting and retryability purposes.
    if (!indices.empty()) {
        std::for_each(indices.begin(), indices.end(), processMeasurement);
    } else {
        for (size_t i = startIndex; i < startIndex + numDocsToStage; i++) {
            processMeasurement(i);
        }
    }

    std::vector<BatchedInsertContext> batchedInsertContexts;

    // Go through all unique meta batches, sort by time, and fill result
    for (auto& [metadata, batchedInsertTupleVector] : metaFieldToBatchedInsertTuples) {
        std::sort(batchedInsertTupleVector.begin(),
                  batchedInsertTupleVector.end(),
                  [](auto& lhs, auto& rhs) {
                      // Sort measurements on their timeField.
                      return std::get<Date_t>(lhs) < std::get<Date_t>(rhs);
                  });
        auto bucketKey = BucketKey{collectionUUID, metadata};
        auto stripeNumber = internal::getStripeNumber(bucketCatalog, bucketKey);
        batchedInsertContexts.emplace_back(
            bucketKey, stripeNumber, timeseriesOptions, stats, batchedInsertTupleVector);
    }

    return batchedInsertContexts;
}

std::vector<BatchedInsertContext> buildBatchedInsertContexts(
    BucketCatalog& bucketCatalog,
    const UUID& collectionUUID,
    const TimeseriesOptions& timeseriesOptions,
    const std::vector<BSONObj>& userMeasurementsBatch,
    size_t startIndex,
    size_t numDocsToStage,
    const std::vector<size_t>& indices,
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices) {

    invariant(indices.size() <= userMeasurementsBatch.size());

    auto metaFieldName = timeseriesOptions.getMetaField();
    auto& trackingContext =
        getTrackingContext(bucketCatalog.trackingContexts, TrackingScope::kMeasurementBatching);
    auto stats = internal::getOrInitializeExecutionStats(bucketCatalog, collectionUUID);

    return (metaFieldName) ? buildBatchedInsertContextsWithMetaField(bucketCatalog,
                                                                     collectionUUID,
                                                                     timeseriesOptions,
                                                                     userMeasurementsBatch,
                                                                     startIndex,
                                                                     numDocsToStage,
                                                                     indices,
                                                                     stats,
                                                                     trackingContext,
                                                                     errorsAndIndices)
                           : buildBatchedInsertContextsNoMetaField(bucketCatalog,
                                                                   collectionUUID,
                                                                   timeseriesOptions,
                                                                   userMeasurementsBatch,
                                                                   startIndex,
                                                                   numDocsToStage,
                                                                   indices,
                                                                   stats,
                                                                   trackingContext,
                                                                   errorsAndIndices);
}

TimeseriesWriteBatches stageInsertBatch(
    OperationContext* opCtx,
    BucketCatalog& bucketCatalog,
    const Collection* bucketsColl,
    const OperationId& opId,
    const StringDataComparator* comparator,
    uint64_t storageCacheSizeBytes,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    const AllowQueryBasedReopening allowQueryBasedReopening,
    BatchedInsertContext& batch) {
    auto& stripe = *bucketCatalog.stripes[batch.stripeNumber];
    stdx::unique_lock<stdx::mutex> stripeLock{stripe.mutex};
    TimeseriesWriteBatches writeBatches;
    size_t currentPosition = 0;
    bool needsAnotherBucket = true;

    while (needsAnotherBucket) {
        bool bucketOpenedDueToMetadata = true;
        auto [measurement, measurementTimestamp, _] =
            batch.measurementsTimesAndIndices[currentPosition];
        auto& eligibleBucket = getEligibleBucket(opCtx,
                                                 bucketCatalog,
                                                 stripe,
                                                 stripeLock,
                                                 bucketsColl,
                                                 measurement,
                                                 batch.key,
                                                 measurementTimestamp,
                                                 batch.options,
                                                 comparator,
                                                 storageCacheSizeBytes,
                                                 compressAndWriteBucketFunc,
                                                 allowQueryBasedReopening,
                                                 batch.stats,
                                                 bucketOpenedDueToMetadata);

        // getEligibleBucket guarantees that we will successfully insert at least one measurement
        // (batch.measurementsTimesAndIndices[currentPosition]) into the provided bucket without
        // rolling it over, which allows us to unconditionally initialize the writeBatch.
        std::shared_ptr<WriteBatch> writeBatch = activeBatch(
            bucketCatalog.trackingContexts, eligibleBucket, opId, batch.stripeNumber, batch.stats);
        writeBatch->openedDueToMetadata = bucketOpenedDueToMetadata;
        internal::StageInsertBatchResult result =
            internal::stageInsertBatchIntoEligibleBucket(bucketCatalog,
                                                         opId,
                                                         comparator,
                                                         batch,
                                                         stripe,
                                                         stripeLock,
                                                         storageCacheSizeBytes,
                                                         eligibleBucket,
                                                         currentPosition,
                                                         writeBatch);

        /**
         * Though rare, it is possible that the bucket provided by getEligibleBucket
         * is not considered to be eligible when re-checking for rollover inside
         * stageInsertBatchIntoEligibleBucket. For example, there is a global
         * statistic, numActiveBuckets, that is not protected by the stripe lock
         * and can be independently updated between the two checks. To avoid a
         * crash, the write path will ignore an ineligible bucket and try again.
         */
        if (result != internal::StageInsertBatchResult::NoMeasurementsStaged) {
            writeBatches.emplace_back(writeBatch);
        }

        needsAnotherBucket = (result != internal::StageInsertBatchResult::Success);
    }

    invariant(currentPosition == batch.measurementsTimesAndIndices.size());
    return writeBatches;
}

StatusWith<TimeseriesWriteBatches> prepareInsertsToBuckets(
    OperationContext* opCtx,
    BucketCatalog& bucketCatalog,
    const Collection* bucketsColl,
    const TimeseriesOptions& timeseriesOptions,
    OperationId opId,
    const StringDataComparator* comparator,
    uint64_t storageCacheSizeBytes,
    bool earlyReturnOnError,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    const std::vector<BSONObj>& userMeasurementsBatch,
    size_t startIndex,
    size_t numDocsToStage,
    const std::vector<size_t>& indices,
    const AllowQueryBasedReopening allowQueryBasedReopening,
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices) {
    auto batchedInsertContexts = buildBatchedInsertContexts(bucketCatalog,
                                                            bucketsColl->uuid(),
                                                            timeseriesOptions,
                                                            userMeasurementsBatch,
                                                            startIndex,
                                                            numDocsToStage,
                                                            indices,
                                                            errorsAndIndices);

    if (earlyReturnOnError && !errorsAndIndices.empty()) {
        // Any errors in the user batch will early-exit and be attempted one-at-a-time.
        return errorsAndIndices.front().error;
    }

    TimeseriesWriteBatches results;

    for (auto& batchedInsertContext : batchedInsertContexts) {
        auto writeBatches = stageInsertBatch(opCtx,
                                             bucketCatalog,
                                             bucketsColl,
                                             opId,
                                             comparator,
                                             storageCacheSizeBytes,
                                             compressAndWriteBucketFunc,
                                             allowQueryBasedReopening,
                                             batchedInsertContext);

        // Append all returned write batches to results, since multiple buckets may have been
        // targeted.
        results.insert(results.end(), writeBatches.begin(), writeBatches.end());
    }

    return results;
}

StatusWith<std::pair<BucketKey, Date_t>> extractBucketingParameters(
    tracking::Context& trackingContext,
    const UUID& collectionUUID,
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
    auto key = BucketKey{collectionUUID,
                         BucketMetadata{trackingContext, metadata, options.getMetaField()}};

    return {std::make_pair(std::move(key), time)};
}
}  // namespace mongo::timeseries::bucket_catalog
