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
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/tracking_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries::bucket_catalog {
namespace {
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationAfterStart);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationBeforeFinish);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeReopeningBucket);

/**
 * Prepares the batch for commit. Sets min/max appropriately, records the number of
 * documents that have previously been committed to the bucket, and renders the batch
 * inactive. Must have commit rights.
 */
void prepareWriteBatchForCommit(TrackingContexts& trackingContexts,
                                WriteBatch& batch,
                                Bucket& bucket,
                                const StringDataComparator* comparator) {
    invariant(batch.commitRights.load());
    batch.numPreviouslyCommittedMeasurements = bucket.numCommittedMeasurements;

    // Filter out field names that were new at the time of insertion, but have since been committed
    // by someone else.
    for (auto it = batch.newFieldNamesToBeInserted.begin();
         it != batch.newFieldNamesToBeInserted.end();) {
        TrackedStringMapHashedKey fieldName(
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
    batch.generateCompressedDiff = bucket.usingAlwaysCompressedBuckets;
    batch.isReopened = bucket.isReopened;
}

/**
 * Reports the result and status of a commit, and notifies anyone waiting on getResult().
 * Must have commit rights. Inactive batches only.
 */
void finishWriteBatch(WriteBatch& batch, const CommitInfo& info) {
    invariant(batch.commitRights.load());
    batch.promise.emplaceValue(info);
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
}  // namespace

SuccessfulInsertion::SuccessfulInsertion(std::shared_ptr<WriteBatch>&& b, ClosedBuckets&& c)
    : batch{std::move(b)}, closedBuckets{std::move(c)} {}

Stripe::Stripe(TrackingContexts& trackingContexts)
    : openBucketsById(
          make_tracked_unordered_map<BucketId, unique_tracked_ptr<Bucket>, BucketHasher>(
              getTrackingContext(trackingContexts, TrackingScope::kOpenBucketsById))),
      openBucketsByKey(make_tracked_unordered_map<BucketKey, tracked_set<Bucket*>, BucketHasher>(
          getTrackingContext(trackingContexts, TrackingScope::kOpenBucketsByKey))),
      idleBuckets(make_tracked_list<Bucket*>(
          getTrackingContext(trackingContexts, TrackingScope::kIdleBuckets))),
      archivedBuckets(
          make_tracked_btree_map<ArchivedKey, ArchivedBucket, std::greater<ArchivedKey>>(
              getTrackingContext(trackingContexts, TrackingScope::kArchivedBuckets))),
      collectionTimeFields(make_tracked_unordered_map<UUID, std::tuple<tracked_string, int64_t>>(
          getTrackingContext(trackingContexts, TrackingScope::kArchivedBuckets))),
      outstandingReopeningRequests(
          make_tracked_unordered_map<
              BucketKey,
              tracked_inlined_vector<shared_tracked_ptr<ReopeningRequest>, kInlinedVectorSize>,
              BucketHasher>(
              getTrackingContext(trackingContexts, TrackingScope::kReopeningRequests))) {}

BucketCatalog::BucketCatalog(size_t numberOfStripes, std::function<uint64_t()> memoryUsageThreshold)
    : bucketStateRegistry(
          getTrackingContext(trackingContexts, TrackingScope::kBucketStateRegistry)),
      numberOfStripes(numberOfStripes),
      stripes(make_tracked_vector<unique_tracked_ptr<Stripe>>(
          getTrackingContext(trackingContexts, TrackingScope::kMiscellaneous))),
      executionStats(make_tracked_unordered_map<UUID, shared_tracked_ptr<ExecutionStats>>(
          getTrackingContext(trackingContexts, TrackingScope::kStats))),
      memoryUsageThreshold(memoryUsageThreshold) {
    stripes.reserve(numberOfStripes);
    std::generate_n(std::back_inserter(stripes), numberOfStripes, [&]() {
        return make_unique_tracked<Stripe>(
            getTrackingContext(trackingContexts, TrackingScope::kMiscellaneous), trackingContexts);
    });
}

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
                                   CombineWithInsertsFromOtherClients combine,
                                   InsertContext& insertContext,
                                   const Date_t& time,
                                   uint64_t storageCacheSize) {
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
                                   storageCacheSize);
    }

    auto insertionResult = insertIntoBucket(catalog,
                                            stripe,
                                            stripeLock,
                                            doc,
                                            opId,
                                            combine,
                                            internal::AllowBucketCreation::kNo,
                                            insertContext,
                                            *bucket,
                                            time,
                                            storageCacheSize,
                                            comparator);
    // If our insert was successful, return a SuccessfulInsertion with our
    // WriteBatch.
    if (auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult)) {
        return SuccessfulInsertion{std::move(*batch), std::move(insertContext.closedBuckets)};
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
            insertionResult = insertIntoBucket(catalog,
                                               stripe,
                                               stripeLock,
                                               doc,
                                               opId,
                                               combine,
                                               internal::AllowBucketCreation::kNo,
                                               insertContext,
                                               *alternate,
                                               time,
                                               storageCacheSize,
                                               comparator);
            if (auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult)) {
                return SuccessfulInsertion{std::move(*batch),
                                           std::move(insertContext.closedBuckets)};
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
                               storageCacheSize);
}

StatusWith<InsertResult> insertWithReopeningContext(BucketCatalog& catalog,
                                                    const StringDataComparator* comparator,
                                                    const BSONObj& doc,
                                                    OperationId opId,
                                                    CombineWithInsertsFromOtherClients combine,
                                                    ReopeningContext& reopeningContext,
                                                    InsertContext& insertContext,
                                                    const Date_t& time,
                                                    uint64_t storageCacheSize) {
    updateBucketFetchAndQueryStats(reopeningContext, insertContext.stats);

    // We try to create a bucket in-memory from one on disk that we can potentially insert our
    // measurement into.
    auto rehydratedBucket = (reopeningContext.bucketToReopen.has_value())
        ? internal::rehydrateBucket(catalog,
                                    insertContext.stats,
                                    insertContext.key.collectionUUID,
                                    comparator,
                                    insertContext.options,
                                    reopeningContext.bucketToReopen.value(),
                                    reopeningContext.catalogEra,
                                    &insertContext.key)
        : StatusWith<unique_tracked_ptr<Bucket>>{ErrorCodes::BadValue, "No bucket to rehydrate"};
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

        StatusWith<std::reference_wrapper<Bucket>> swBucket{ErrorCodes::BadValue, ""};
        auto existingIt = stripe.openBucketsById.find(rehydratedBucket.getValue()->bucketId);
        if (existingIt != stripe.openBucketsById.end()) {
            // First let's check the existing bucket if we have one.
            Bucket* existingBucket = existingIt->second.get();
            swBucket = internal::reuseExistingBucket(catalog,
                                                     stripe,
                                                     stripeLock,
                                                     insertContext.stats,
                                                     insertContext.key,
                                                     *existingBucket,
                                                     reopeningContext.catalogEra);
        } else {
            // No existing bucket to use, go ahead and try to reopen our rehydrated bucket.
            swBucket = internal::reopenBucket(catalog,
                                              stripe,
                                              stripeLock,
                                              insertContext.stats,
                                              insertContext.key,
                                              std::move(rehydratedBucket.getValue()),
                                              reopeningContext.catalogEra,
                                              insertContext.closedBuckets);
        }

        if (swBucket.isOK()) {
            Bucket& bucket = swBucket.getValue().get();
            auto insertionResult = insertIntoBucket(catalog,
                                                    stripe,
                                                    stripeLock,
                                                    doc,
                                                    opId,
                                                    combine,
                                                    internal::AllowBucketCreation::kYes,
                                                    insertContext,
                                                    bucket,
                                                    time,
                                                    storageCacheSize,
                                                    comparator);
            auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
            invariant(batch);
            return SuccessfulInsertion{std::move(*batch), std::move(insertContext.closedBuckets)};
        } else {
            insertContext.stats.incNumBucketReopeningsFailed();
            if (swBucket.getStatus().code() == ErrorCodes::WriteConflict) {
                return swBucket.getStatus();
            }
            // If we had a different type of error, then we should fall through and proceed to open
            // a new bucket.
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

    auto insertionResult = insertIntoBucket(catalog,
                                            stripe,
                                            stripeLock,
                                            doc,
                                            opId,
                                            combine,
                                            internal::AllowBucketCreation::kYes,
                                            insertContext,
                                            *bucket,
                                            time,
                                            storageCacheSize,
                                            comparator);
    auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
    invariant(batch);
    return SuccessfulInsertion{std::move(*batch), std::move(insertContext.closedBuckets)};
}

StatusWith<InsertResult> insert(BucketCatalog& catalog,
                                const StringDataComparator* comparator,
                                const BSONObj& doc,
                                OperationId opId,
                                CombineWithInsertsFromOtherClients combine,
                                InsertContext& insertContext,
                                const Date_t& time,
                                uint64_t storageCacheSize) {
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

    auto insertionResult = insertIntoBucket(catalog,
                                            stripe,
                                            stripeLock,
                                            doc,
                                            opId,
                                            combine,
                                            internal::AllowBucketCreation::kYes,
                                            insertContext,
                                            *bucket,
                                            time,
                                            storageCacheSize,
                                            comparator);

    auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
    invariant(batch);
    return SuccessfulInsertion{std::move(*batch), std::move(insertContext.closedBuckets)};
}

void waitToInsert(InsertWaiter* waiter) {
    if (auto* batch = get_if<std::shared_ptr<WriteBatch>>(waiter)) {
        getWriteBatchResult(**batch).getStatus().ignore();
    } else if (auto* request = get_if<std::shared_ptr<ReopeningRequest>>(waiter)) {
        waitForReopeningRequest(**request);
    }
}

Status prepareCommit(BucketCatalog& catalog,
                     std::shared_ptr<WriteBatch> batch,
                     const StringDataComparator* comparator) {
    auto getBatchStatus = [&] {
        return batch->promise.getFuture().getNoThrow().getStatus();
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

boost::optional<ClosedBucket> finish(
    BucketCatalog& catalog,
    std::shared_ptr<WriteBatch> batch,
    const CommitInfo& info,
    const std::function<void(const timeseries::bucket_catalog::WriteBatch&, StringData)>&
        runPostCommitDebugChecks) {
    invariant(!isWriteBatchFinished(*batch));

    boost::optional<ClosedBucket> closedBucket;

    finishWriteBatch(*batch, info);

    auto& stripe = *catalog.stripes[internal::getStripeNumber(catalog, batch->bucketId)];
    stdx::lock_guard stripeLock{stripe.mutex};

    if (MONGO_unlikely(runPostCommitDebugChecks)) {
        Bucket* bucket = internal::useBucket(catalog.bucketStateRegistry,
                                             stripe,
                                             stripeLock,
                                             batch->bucketId,
                                             internal::IgnoreBucketState::kYes);
        if (bucket) {
            runPostCommitDebugChecks(*batch, {bucket->timeField.data(), bucket->timeField.size()});
        }
    }

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

        if (bucket->usingAlwaysCompressedBuckets) {
            bucket->size -= batch->sizes.uncommittedMeasurementEstimate;
            bucket->size += batch->sizes.uncommittedVerifiedSize;
        }
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
        switch (bucket->rolloverAction) {
            case RolloverAction::kHardClose:
            case RolloverAction::kSoftClose: {
                internal::closeOpenBucket(
                    catalog, stripe, stripeLock, *bucket, stats, closedBucket);
                break;
            }
            case RolloverAction::kArchive: {
                ClosedBuckets closedBuckets;
                internal::archiveBucket(catalog, stripe, stripeLock, *bucket, stats, closedBuckets);
                if (!closedBuckets.empty()) {
                    closedBucket = std::move(closedBuckets[0]);
                }
                break;
            }
            case RolloverAction::kNone: {
                internal::markBucketIdle(stripe, stripeLock, *bucket);
                break;
            }
        }
    }
    return closedBucket;
}

void abort(BucketCatalog& catalog, std::shared_ptr<WriteBatch> batch, const Status& status) {
    invariant(batch);
    invariant(batch->commitRights.load());

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

void drop(BucketCatalog& catalog, tracked_vector<UUID> clearedCollectionUUIDs) {
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
    tracked_vector<UUID> clearedCollectionUUIDs = make_tracked_vector<UUID>(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kBucketStateRegistry));
    clearedCollectionUUIDs.push_back(collectionUUID);
    clearSetOfBuckets(catalog.bucketStateRegistry, std::move(clearedCollectionUUIDs));
}

void freeze(BucketCatalog& catalog, const BucketId& bucketId) {
    internal::getOrInitializeExecutionStats(catalog, bucketId.collectionUUID).incNumBucketsFrozen();
    freezeBucket(catalog.bucketStateRegistry, bucketId);
}

void freeze(BucketCatalog& catalog,
            const TimeseriesOptions& options,
            const StringDataComparator* comparator,
            const UUID& collectionUUID,
            const BSONObj& bucket) {
    freeze(catalog, extractBucketId(catalog, options, comparator, collectionUUID, bucket));
}

BucketId extractBucketId(BucketCatalog& bucketCatalog,
                         const TimeseriesOptions& options,
                         const StringDataComparator* comparator,
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
                                     const StringDataComparator* comparator,
                                     const UUID& collectionUUID,
                                     const BSONObj& metadataObj) {
    TrackingContext trackingContext;
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
    const shared_tracked_ptr<ExecutionStats> stats =
        internal::getCollectionExecutionStats(catalog, collectionUUID);
    if (stats) {
        appendExecutionStatsToBuilder(*stats, builder);
    }
}

void reportMeasurementsGroupCommitted(BucketCatalog& catalog,
                                      const UUID& collectionUUID,
                                      int64_t count) {
    auto stats = internal::getOrInitializeExecutionStats(catalog, collectionUUID);
    stats.incNumMeasurementsGroupCommitted(count);
}

StatusWith<std::tuple<InsertContext, Date_t>> prepareInsert(BucketCatalog& catalog,
                                                            const UUID& collectionUUID,
                                                            const StringDataComparator* comparator,
                                                            const TimeseriesOptions& options,
                                                            const BSONObj& doc) {
    auto res = internal::extractBucketingParameters(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kOpenBucketsByKey),
        collectionUUID,
        comparator,
        options,
        doc);
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

}  // namespace mongo::timeseries::bucket_catalog
