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

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/tracking/context.h"

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
        if (bucket.measurementMap.containsField(it->first)) {
            batch.newFieldNamesToBeInserted.erase(it++);
            continue;
        }

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
 * Updates stats to reflect the status of bucket fetches and queries based off of the
 * 'ReopeningContext' (which is populated when attempting to reopen a bucket).
 */
void updateBucketFetchAndQueryStats(const ReopeningContext& context,
                                    ExecutionStatsController& stats) {
    if (context.fetchedBucket) {
        if (context.bucketToReopen.has_value()) {
            stats.incNumBucketsFetched();
        } else {
            stats.incNumBucketFetchesFailed();
        }
    }

    if (context.queriedBucket) {
        if (context.bucketToReopen.has_value()) {
            stats.incNumBucketsQueried();
        } else {
            stats.incNumBucketQueriesFailed();
        }
    }
}

/**
 * Determines if 'measurement' will cause rollover to 'bucket'.
 * Returns the rollover reason and marks the bucket with rollover reason if it needs to be rolled
 * over.
 */
RolloverReason determineBucketRolloverForMeasurement(BucketCatalog& catalog,
                                                     Stripe& stripe,
                                                     WithLock stripeLock,
                                                     Bucket& bucket,
                                                     const BSONObj& measurement,
                                                     const Date_t& measurementTimestamp,
                                                     const TimeseriesOptions& options,
                                                     const StringDataComparator* comparator,
                                                     uint64_t storageCacheSizeBytes,
                                                     ExecutionStatsController& stats,
                                                     bool& bucketOpenedDueToMetadata) {
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
        bucket,
        catalog.globalExecutionStats.numActiveBuckets.loadRelaxed(),
        sizesToBeAdded,
        measurementTimestamp,
        storageCacheSizeBytes,
        comparator,
        stats);

    if (rolloverReason != RolloverReason::kNone) {
        // Update the bucket's 'rolloverReason'.
        bucket.rolloverReason = rolloverReason;
    }
    bucketOpenedDueToMetadata = false;
    return rolloverReason;
}

/**
 * Returns an open bucket from stripe that can fit 'measurement'. If none available, returns
 * nullptr.
 * Makes the decision to skip query-based reopening if 'measurementTimestamp' is later than
 * the bucket's time range.
 */
Bucket* findOpenBucketForMeasurement(BucketCatalog& catalog,
                                     Stripe& stripe,
                                     WithLock stripeLock,
                                     const BSONObj& measurement,
                                     const BucketKey& bucketKey,
                                     const Date_t& measurementTimestamp,
                                     const TimeseriesOptions& options,
                                     const StringDataComparator* comparator,
                                     uint64_t storageCacheSizeBytes,
                                     internal::AllowQueryBasedReopening& allowQueryBasedReopening,
                                     ExecutionStatsController& stats,
                                     bool& bucketOpenedDueToMetadata) {
    // Gets a vector of potential buckets, starting with kSoftClose/kArchived buckets, followed by
    // at most one kNone bucket.
    auto potentialBuckets = findAndRolloverOpenBuckets(catalog,
                                                       stripe,
                                                       stripeLock,
                                                       bucketKey,
                                                       measurementTimestamp,
                                                       Seconds(*options.getBucketMaxSpanSeconds()));
    if (potentialBuckets.empty()) {
        return nullptr;
    }

    for (const auto& potentialBucket : potentialBuckets) {
        // Check if the measurement can fit in the potential bucket.
        auto rolloverReason = determineBucketRolloverForMeasurement(catalog,
                                                                    stripe,
                                                                    stripeLock,
                                                                    *potentialBucket,
                                                                    measurement,
                                                                    measurementTimestamp,
                                                                    options,
                                                                    comparator,
                                                                    storageCacheSizeBytes,
                                                                    stats,
                                                                    bucketOpenedDueToMetadata);

        if (rolloverReason == RolloverReason::kNone) {
            // The measurement can be inserted into the open bucket.
            return potentialBucket;
        }

        // Skip query based reopening when 'measurementTimestamp' is later than the
        // current open bucket's time range.
        allowQueryBasedReopening = rolloverReason == RolloverReason::kTimeForward
            ? internal::AllowQueryBasedReopening::kDisallow
            : internal::AllowQueryBasedReopening::kAllow;
    }

    return nullptr;
}
}  // namespace

SuccessfulInsertion::SuccessfulInsertion(std::shared_ptr<WriteBatch>&& b) : batch{std::move(b)} {}

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

BSONObj getMetadata(BucketCatalog& catalog, const BucketId& bucketId) {
    auto const& stripe = *catalog.stripes[internal::getStripeNumber(catalog, bucketId)];
    stdx::lock_guard stripeLock{stripe.mutex};

    const Bucket* bucket =
        internal::findBucket(catalog.bucketStateRegistry, stripe, stripeLock, bucketId);
    if (!bucket) {
        return {};
    }

    return bucket->key.metadata.toBSON();
}

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
        catalog.trackingContexts.stats.allocated() + catalog.trackingContexts.summaries.allocated();
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
#endif
}

StatusWith<InsertResult> tryInsert(BucketCatalog& catalog,
                                   const StringDataComparator* comparator,
                                   const BSONObj& doc,
                                   OperationId opId,
                                   InsertContext& insertContext,
                                   const Date_t& time,
                                   uint64_t storageCacheSizeBytes) {
    // Save the catalog era value from before we make any further checks. This guarantees that we
    // don't miss a direct write that happens sometime in between our decision to potentially reopen
    // a bucket below, and actually reopening it in a subsequent reentrant call. Any direct write
    // will increment the era, so the reentrant call can check the current value and return a write
    // conflict if it sees a newer era.
    const auto catalogEra = getCurrentEra(catalog.bucketStateRegistry);

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto& stripe = *catalog.stripes[insertContext.stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket = internal::useBucket(catalog,
                                         stripe,
                                         stripeLock,
                                         insertContext,
                                         internal::AllowBucketCreation::kNo,
                                         time,
                                         comparator);
    // If there are no open buckets for our measurement that we can use, we return a
    // reopeningContext to try reopening a closed bucket from disk.
    if (!bucket) {
        return getReopeningContext(catalog,
                                   stripe,
                                   stripeLock,
                                   insertContext,
                                   catalogEra,
                                   internal::AllowQueryBasedReopening::kAllow,
                                   time,
                                   storageCacheSizeBytes);
    }

    auto insertionResult = internal::insertIntoBucket(catalog,
                                                      stripe,
                                                      stripeLock,
                                                      doc,
                                                      opId,
                                                      internal::AllowBucketCreation::kNo,
                                                      insertContext,
                                                      *bucket,
                                                      time,
                                                      storageCacheSizeBytes,
                                                      comparator);
    // If our insert was successful, return a SuccessfulInsertion with our
    // WriteBatch.
    if (auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult)) {
        return SuccessfulInsertion{std::move(*batch)};
    }

    auto* reason = get_if<RolloverReason>(&insertionResult);
    invariant(reason);
    if (allCommitted(*bucket)) {
        internal::markBucketIdle(stripe, stripeLock, *bucket);
    }

    // If we were time forward or backward, we might be able to "reopen" a bucket we still have
    // in memory that's set to be closed when pending operations finish.
    if ((*reason == RolloverReason::kTimeBackward || *reason == RolloverReason::kTimeForward)) {
        if (Bucket* alternate =
                internal::useAlternateBucket(catalog, stripe, stripeLock, insertContext, time)) {
            insertionResult = internal::insertIntoBucket(catalog,
                                                         stripe,
                                                         stripeLock,
                                                         doc,
                                                         opId,
                                                         internal::AllowBucketCreation::kNo,
                                                         insertContext,
                                                         *alternate,
                                                         time,
                                                         storageCacheSizeBytes,
                                                         comparator,
                                                         bucket,
                                                         *reason);
            if (auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult)) {
                return SuccessfulInsertion{std::move(*batch)};
            }

            // We weren't able to insert into the other bucket, so fall through to the regular
            // reopening procedure.
        }
    }

    return getReopeningContext(catalog,
                               stripe,
                               stripeLock,
                               insertContext,
                               catalogEra,
                               (*reason == RolloverReason::kTimeBackward)
                                   ? internal::AllowQueryBasedReopening::kAllow
                                   : internal::AllowQueryBasedReopening::kDisallow,
                               time,
                               storageCacheSizeBytes);
}

StatusWith<InsertResult> insertWithReopeningContext(BucketCatalog& catalog,
                                                    const StringDataComparator* comparator,
                                                    const BSONObj& doc,
                                                    OperationId opId,
                                                    ReopeningContext& reopeningContext,
                                                    InsertContext& insertContext,
                                                    const Date_t& time,
                                                    uint64_t storageCacheSizeBytes) {
    updateBucketFetchAndQueryStats(reopeningContext, insertContext.stats);

    // We try to create a bucket in-memory from one on disk that we can potentially insert our
    // measurement into.
    auto rehydratedBucket = (reopeningContext.bucketToReopen.has_value())
        ? internal::rehydrateBucket(catalog,
                                    reopeningContext.bucketToReopen->bucketDocument,
                                    insertContext.key,
                                    insertContext.options,
                                    reopeningContext.catalogEra,
                                    comparator,
                                    reopeningContext.bucketToReopen->validator,
                                    insertContext.stats)
        : StatusWith<tracking::unique_ptr<Bucket>>{ErrorCodes::BadValue, "No bucket to rehydrate"};
    if (rehydratedBucket.getStatus().code() == ErrorCodes::WriteConflict) {
        return rehydratedBucket.getStatus();
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto& stripe = *catalog.stripes[insertContext.stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    // Can safely clear reentrant coordination state now that we have acquired the lock.
    reopeningContext.clear(stripeLock);

    if (rehydratedBucket.isOK()) {
        hangTimeseriesInsertBeforeReopeningBucket.pauseWhileSet();

        auto existingIt = stripe.openBucketsById.find(rehydratedBucket.getValue()->bucketId);
        if (existingIt == stripe.openBucketsById.end()) {
            // No existing bucket matches this one, go ahead and try to reopen our rehydrated
            // bucket.
            auto swBucket = internal::loadBucketIntoCatalog(catalog,
                                                            stripe,
                                                            stripeLock,
                                                            insertContext.stats,
                                                            insertContext.key,
                                                            std::move(rehydratedBucket.getValue()),
                                                            reopeningContext.catalogEra);

            if (swBucket.isOK()) {
                // We reopened the bucket successfully. Now we'll use it directly as an optimization
                // to bypass normal bucket selection.
                Bucket& reopenedBucket = swBucket.getValue().get();
                auto insertionResult =
                    internal::insertIntoBucket(catalog,
                                               stripe,
                                               stripeLock,
                                               doc,
                                               opId,
                                               internal::AllowBucketCreation::kYes,
                                               insertContext,
                                               reopenedBucket,
                                               time,
                                               storageCacheSizeBytes,
                                               comparator);
                auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
                invariant(batch);
                return SuccessfulInsertion{std::move(*batch)};
            } else {
                insertContext.stats.incNumBucketReopeningsFailed();
                if (swBucket.getStatus().code() == ErrorCodes::WriteConflict) {
                    return swBucket.getStatus();
                }
                // If we had a different type of error, then we should fall through to normal bucket
                // selection/allocation.
            }
        } else {
            // We tried to reopen a bucket we already had open. Record the metric and then fall
            // through to normal bucket selection/allocation.
            insertContext.stats.incNumDuplicateBucketsReopened();
        }
    }

    Bucket* bucket = useBucket(catalog,
                               stripe,
                               stripeLock,
                               insertContext,
                               internal::AllowBucketCreation::kYes,
                               time,
                               comparator);
    invariant(bucket);

    auto insertionResult = internal::insertIntoBucket(catalog,
                                                      stripe,
                                                      stripeLock,
                                                      doc,
                                                      opId,
                                                      internal::AllowBucketCreation::kYes,
                                                      insertContext,
                                                      *bucket,
                                                      time,
                                                      storageCacheSizeBytes,
                                                      comparator);
    auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
    invariant(batch);
    return SuccessfulInsertion{std::move(*batch)};
}

StatusWith<InsertResult> insert(BucketCatalog& catalog,
                                const StringDataComparator* comparator,
                                const BSONObj& doc,
                                OperationId opId,
                                InsertContext& insertContext,
                                const Date_t& time,
                                uint64_t storageCacheSizeBytes) {
    auto& stripe = *catalog.stripes[insertContext.stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket = useBucket(catalog,
                               stripe,
                               stripeLock,
                               insertContext,
                               internal::AllowBucketCreation::kYes,
                               time,
                               comparator);
    invariant(bucket);

    auto insertionResult = internal::insertIntoBucket(catalog,
                                                      stripe,
                                                      stripeLock,
                                                      doc,
                                                      opId,
                                                      internal::AllowBucketCreation::kYes,
                                                      insertContext,
                                                      *bucket,
                                                      time,
                                                      storageCacheSizeBytes,
                                                      comparator);

    auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
    invariant(batch);
    return SuccessfulInsertion{std::move(*batch)};
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

void appendExecutionStats(const BucketCatalog& catalog,
                          const UUID& collectionUUID,
                          BSONObjBuilder& builder) {
    const tracking::shared_ptr<ExecutionStats> stats =
        internal::getCollectionExecutionStats(catalog, collectionUUID);
    if (stats) {
        appendExecutionStatsToBuilder(*stats, builder);
    }
}

StatusWith<std::tuple<InsertContext, Date_t>> prepareInsert(BucketCatalog& catalog,
                                                            const UUID& collectionUUID,
                                                            const TimeseriesOptions& options,
                                                            const BSONObj& measurementDoc) {
    auto res = internal::extractBucketingParameters(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kOpenBucketsByKey),
        collectionUUID,
        options,
        measurementDoc);
    if (!res.isOK()) {
        return res.getStatus();
    }
    auto& key = res.getValue().first;
    auto time = res.getValue().second;

    ExecutionStatsController stats =
        internal::getOrInitializeExecutionStats(catalog, collectionUUID);

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto stripeNumber = internal::getStripeNumber(catalog, key);
    InsertContext insertContext{std::move(key), stripeNumber, options, stats};

    return {std::make_pair(std::move(insertContext), std::move(time))};
}

std::vector<Bucket*> findAndRolloverOpenBuckets(BucketCatalog& catalog,
                                                Stripe& stripe,
                                                WithLock stripeLock,
                                                const BucketKey& bucketKey,
                                                const Date_t& time,
                                                const Seconds& bucketMaxSpanSeconds) {
    std::vector<Bucket*> potentialBuckets;
    auto openBuckets = internal::findOpenBuckets(stripe, stripeLock, bucketKey);
    Bucket* bucketWithoutRolloverAction = nullptr;
    for (const auto& openBucket : openBuckets) {
        auto reason = openBucket->rolloverReason;
        auto action = getRolloverAction(reason);
        switch (action) {
            case RolloverAction::kNone: {
                auto bucketState = internal::isBucketStateEligibleForInsertsAndCleanup(
                    catalog, stripe, stripeLock, openBucket);
                if (bucketState == internal::BucketStateForInsertAndCleanup::kEligibleForInsert) {
                    internal::markBucketNotIdle(stripe, stripeLock, *openBucket);
                    // Only one uncleared open bucket is allowed for each key.
                    invariant(bucketWithoutRolloverAction == nullptr);
                    // Save the bucket with 'RolloverAction::kNone' to add it to the end of
                    // 'potentialBuckets'.
                    bucketWithoutRolloverAction = openBucket;
                }
                break;
            }
            case RolloverAction::kHardClose:
                internal::rollover(catalog, stripe, stripeLock, *openBucket, reason);
                break;
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
                    potentialBuckets.push_back(openBucket);
                } else {
                    // We only want to rollover these buckets when we don't have an insertion
                    // conflict; otherwise, we will attempt to remove the bucket twice.
                    internal::rollover(catalog, stripe, stripeLock, *openBucket, reason);
                }
                break;
            }
        }
    }
    if (bucketWithoutRolloverAction) {
        potentialBuckets.push_back(bucketWithoutRolloverAction);
    }
    return potentialBuckets;
}

StatusWith<tracking::unique_ptr<Bucket>> getReopenedBucket(
    OperationContext* opCtx,
    BucketCatalog& catalog,
    const Collection* bucketsColl,
    const BucketKey& bucketKey,
    const TimeseriesOptions& options,
    const std::variant<OID, std::vector<BSONObj>>& reopeningCandidate,
    BucketStateRegistry::Era catalogEra,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    ExecutionStatsController& stats) {
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
                          BucketStateRegistry::Era era,
                          uint64_t storageCacheSizeBytes,
                          const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
                          ExecutionStatsController& stats,
                          bool& bucketOpenedDueToMetadata) {
    Status reopeningStatus = Status::OK();
    auto numReopeningsAttempted = 0;
    auto reopeningLimit = 3;
    do {
        auto allowQueryBasedReopening = internal::AllowQueryBasedReopening::kAllow;
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

        // 2. Attempt to reopen a bucket.
        // Explicitly pass in the lock which can be unlocked and relocked during reopening.
        auto swReopenedBucket = potentiallyReopenBucket(
            opCtx,
            catalog,
            stripe,
            stripeLock,
            bucketsColl,
            bucketKey,
            measurementTimestamp,
            options,
            era,
            allowQueryBasedReopening == internal::AllowQueryBasedReopening::kAllow,
            storageCacheSizeBytes,
            compressAndWriteBucketFunc,
            stats);
        if (swReopenedBucket.isOK() && swReopenedBucket.getValue()) {
            auto& reopenedBucket = *swReopenedBucket.getValue();
            auto rolloverReason = determineBucketRolloverForMeasurement(catalog,
                                                                        stripe,
                                                                        stripeLock,
                                                                        reopenedBucket,
                                                                        measurement,
                                                                        measurementTimestamp,
                                                                        options,
                                                                        comparator,
                                                                        storageCacheSizeBytes,
                                                                        stats,
                                                                        bucketOpenedDueToMetadata);
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
    auto allowQueryBasedReopening = internal::AllowQueryBasedReopening::kAllow;
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
    BucketStateRegistry::Era catalogEra,
    bool allowQueryBasedReopening,
    uint64_t storageCacheSizeBytes,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    ExecutionStatsController& stats) {
    // Get the information needed for reopening.
    boost::optional<std::variant<OID, std::vector<BSONObj>>> reopeningCandidate;
    boost::optional<InsertWaiter> reopeningConflict;
    if (const auto& archivedCandidate = internal::getArchiveReopeningCandidate(
            catalog, stripe, stripeLock, bucketKey, options, time)) {
        reopeningConflict =
            internal::checkForReopeningConflict(stripe, stripeLock, bucketKey, archivedCandidate);
        if (!reopeningConflict) {
            reopeningCandidate = archivedCandidate.get();
        }
    } else if (allowQueryBasedReopening) {
        reopeningConflict = internal::checkForReopeningConflict(stripe, stripeLock, bucketKey);
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

        bucket_catalog::waitToInsert(&reopeningConflict.get());
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
                                                  stats);

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
        stats.incNumBucketReopeningsFailed();
        return swBucket.getStatus();
    }

    return &swBucket.getValue().get();
}

}  // namespace mongo::timeseries::bucket_catalog
