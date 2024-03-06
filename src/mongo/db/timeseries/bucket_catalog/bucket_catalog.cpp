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
#include <iterator>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_tracking_context.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries::bucket_catalog {
namespace {
const auto getBucketCatalog = ServiceContext::declareDecoration<BucketCatalog>();
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationBeforeWriteConflict);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationAfterStart);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationBeforeFinish);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeReopeningBucket);
MONGO_FAIL_POINT_DEFINE(runPostCommitDebugChecks);

const std::size_t kDefaultNumberOfStripes = 32;

/**
 * Prepares the batch for commit. Sets min/max appropriately, records the number of
 * documents that have previously been committed to the bucket, and renders the batch
 * inactive. Must have commit rights.
 */
void prepareWriteBatchForCommit(TrackingContext& trackingContext,
                                WriteBatch& batch,
                                Bucket& bucket) {
    invariant(batch.commitRights.load());
    batch.numPreviouslyCommittedMeasurements = bucket.numCommittedMeasurements;

    // Filter out field names that were new at the time of insertion, but have since been committed
    // by someone else.
    for (auto it = batch.newFieldNamesToBeInserted.begin();
         it != batch.newFieldNamesToBeInserted.end();) {
        TrackedStringMapHashedKey fieldName(trackingContext, it->first, it->second);
        bucket.uncommittedFieldNames.erase(fieldName);
        if (bucket.fieldNames.contains(fieldName)) {
            batch.newFieldNamesToBeInserted.erase(it++);
            continue;
        }

        bucket.fieldNames.emplace(fieldName);
        ++it;
    }

    for (const auto& doc : batch.measurements) {
        bucket.minmax.update(
            doc, bucket.key.metadata.getMetaField(), bucket.key.metadata.getComparator());
    }

    const bool isUpdate = batch.numPreviouslyCommittedMeasurements > 0;
    if (isUpdate) {
        batch.min = bucket.minmax.minUpdates();
        batch.max = bucket.minmax.maxUpdates();
    } else {
        batch.min = bucket.minmax.min();
        batch.max = bucket.minmax.max();
    }

    batch.generateCompressedDiff = bucket.usingAlwaysCompressedBuckets &&
        bucket.movableState.uncompressedBucketDoc.get().get().isEmpty();

    // See corollary in finish().
    batch.movableState = std::move(bucket.movableState);
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

BucketCatalog& BucketCatalog::get(ServiceContext* svcCtx) {
    return getBucketCatalog(svcCtx);
}

Stripe::Stripe(TrackingContext& trackingContext)
    : openBucketsById(
          make_tracked_unordered_map<BucketId, unique_tracked_ptr<Bucket>, BucketHasher>(
              trackingContext)),
      openBucketsByKey(make_tracked_unordered_map<BucketKey, tracked_set<Bucket*>, BucketHasher>(
          trackingContext)),
      idleBuckets(make_tracked_list<Bucket*>(trackingContext)),
      archivedBuckets(
          make_tracked_unordered_map<BucketKey::Hash,
                                     tracked_map<Date_t, ArchivedBucket, std::greater<Date_t>>,
                                     BucketHasher>(trackingContext)),
      outstandingReopeningRequests(
          make_tracked_unordered_map<
              BucketKey,
              tracked_inlined_vector<shared_tracked_ptr<ReopeningRequest>, kInlinedVectorSize>,
              BucketHasher>(trackingContext)) {}

BucketCatalog::BucketCatalog()
    : BucketCatalog(kDefaultNumberOfStripes,
                    getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes) {}

BucketCatalog::BucketCatalog(size_t numberOfStripes, std::function<uint64_t()> memoryUsageThreshold)
    : bucketStateRegistry(trackingContext),
      numberOfStripes(numberOfStripes),
      stripes(make_tracked_vector<unique_tracked_ptr<Stripe>>(trackingContext)),
      executionStats(
          make_tracked_unordered_map<UUID, shared_tracked_ptr<ExecutionStats>>(trackingContext)),
      memoryUsageThreshold(memoryUsageThreshold) {
    stripes.reserve(numberOfStripes);
    std::generate_n(std::back_inserter(stripes), numberOfStripes, [&]() {
        return make_unique_tracked<Stripe>(trackingContext, trackingContext);
    });
}

BucketCatalog& BucketCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

BSONObj getMetadata(BucketCatalog& catalog, const BucketHandle& handle) {
    auto const& stripe = *catalog.stripes[handle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    const Bucket* bucket =
        internal::findBucket(catalog.bucketStateRegistry, stripe, stripeLock, handle.bucketId);
    if (!bucket) {
        return {};
    }

    return bucket->key.metadata.toBSON();
}

uint64_t getMemoryUsage(const BucketCatalog& catalog) {
    return catalog.memoryUsage.load() + catalog.trackingContext.allocated();
}

StatusWith<InsertResult> tryInsert(OperationContext* opCtx,
                                   BucketCatalog& catalog,
                                   const NamespaceString& nss,
                                   const UUID& collectionUUID,
                                   const StringDataComparator* comparator,
                                   const TimeseriesOptions& options,
                                   const BSONObj& doc,
                                   CombineWithInsertsFromOtherClients combine) {
    auto res = internal::extractBucketingParameters(collectionUUID, comparator, options, doc);
    if (!res.isOK()) {
        return res.getStatus();
    }
    auto& key = res.getValue().first;
    auto time = res.getValue().second;

    ExecutionStatsController stats =
        internal::getOrInitializeExecutionStats(catalog, collectionUUID);
    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto stripeNumber = internal::getStripeNumber(key, catalog.numberOfStripes);

    // Save the catalog era value from before we make any further checks. This guarantees that we
    // don't miss a direct write that happens sometime in between our decision to potentially reopen
    // a bucket below, and actually reopening it in a subsequent reentrant call. Any direct write
    // will increment the era, so the reentrant call can check the current value and return a write
    // conflict if it sees a newer era.
    const auto catalogEra = getCurrentEra(catalog.bucketStateRegistry);

    ClosedBuckets closedBuckets;
    internal::CreationInfo info{key, stripeNumber, time, options, stats, &closedBuckets};
    auto& stripe = *catalog.stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket = internal::useBucket(
        opCtx, catalog, stripe, stripeLock, nss, info, internal::AllowBucketCreation::kNo);
    // If there are no open buckets for our measurement that we can use, we return a
    // reopeningContext to try reopening a closed bucket from disk.
    if (!bucket) {
        return getReopeningContext(opCtx,
                                   catalog,
                                   stripe,
                                   stripeLock,
                                   info,
                                   catalogEra,
                                   internal::AllowQueryBasedReopening::kAllow);
    }

    auto insertionResult = insertIntoBucket(opCtx,
                                            catalog,
                                            stripe,
                                            stripeLock,
                                            stripeNumber,
                                            doc,
                                            combine,
                                            internal::AllowBucketCreation::kNo,
                                            info,
                                            *bucket);
    // If our insert was successful, return a SuccessfulInsertion with our
    // WriteBatch.
    if (auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult)) {
        return SuccessfulInsertion{std::move(*batch), std::move(closedBuckets)};
    }

    auto* reason = get_if<RolloverReason>(&insertionResult);
    invariant(reason);
    if (allCommitted(*bucket)) {
        internal::markBucketIdle(stripe, stripeLock, *bucket);
    }

    // If we were time forward or backward, we might be able to "reopen" a bucket we still have
    // in memory that's set to be closed when pending operations finish.
    if ((*reason == RolloverReason::kTimeBackward || *reason == RolloverReason::kTimeForward)) {
        if (Bucket* alternate = useAlternateBucket(catalog, stripe, stripeLock, nss, info)) {
            insertionResult = insertIntoBucket(opCtx,
                                               catalog,
                                               stripe,
                                               stripeLock,
                                               stripeNumber,
                                               doc,
                                               combine,
                                               internal::AllowBucketCreation::kNo,
                                               info,
                                               *alternate);
            if (auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult)) {
                return SuccessfulInsertion{std::move(*batch), std::move(closedBuckets)};
            }

            // We weren't able to insert into the other bucket, so fall through to the regular
            // reopening procedure.
        }
    }

    return getReopeningContext(opCtx,
                               catalog,
                               stripe,
                               stripeLock,
                               info,
                               catalogEra,
                               (*reason == RolloverReason::kTimeBackward)
                                   ? internal::AllowQueryBasedReopening::kAllow
                                   : internal::AllowQueryBasedReopening::kDisallow);
}

StatusWith<InsertResult> insertWithReopeningContext(OperationContext* opCtx,
                                                    BucketCatalog& catalog,
                                                    const NamespaceString& nss,
                                                    const UUID& collectionUUID,
                                                    const StringDataComparator* comparator,
                                                    const TimeseriesOptions& options,
                                                    const BSONObj& doc,
                                                    CombineWithInsertsFromOtherClients combine,
                                                    ReopeningContext& reopeningContext) {
    auto res = internal::extractBucketingParameters(collectionUUID, comparator, options, doc);
    invariant(res.isOK());
    auto& key = res.getValue().first;
    auto time = res.getValue().second;

    ExecutionStatsController stats =
        internal::getOrInitializeExecutionStats(catalog, collectionUUID);

    updateBucketFetchAndQueryStats(reopeningContext, stats);

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto stripeNumber = internal::getStripeNumber(key, catalog.numberOfStripes);
    ClosedBuckets closedBuckets;
    internal::CreationInfo info{key, stripeNumber, time, options, stats, &closedBuckets};

    // We try to create a bucket in-memory from one on disk that we can potentially insert our
    // measurement into.
    auto rehydratedBucket = (reopeningContext.bucketToReopen.has_value())
        ? internal::rehydrateBucket(opCtx,
                                    catalog,
                                    stats,
                                    collectionUUID,
                                    comparator,
                                    options,
                                    reopeningContext.bucketToReopen.value(),
                                    reopeningContext.catalogEra,
                                    &key)
        : StatusWith<unique_tracked_ptr<Bucket>>{ErrorCodes::BadValue, "No bucket to rehydrate"};
    if (rehydratedBucket.getStatus().code() == ErrorCodes::WriteConflict) {
        return rehydratedBucket.getStatus();
    }

    auto& stripe = *catalog.stripes[stripeNumber];
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
                                                     nss,
                                                     stats,
                                                     key,
                                                     *existingBucket,
                                                     reopeningContext.catalogEra);
        } else {
            // No existing bucket to use, go ahead and try to reopen our rehydrated bucket.
            swBucket = internal::reopenBucket(opCtx,
                                              catalog,
                                              stripe,
                                              stripeLock,
                                              stats,
                                              key,
                                              std::move(rehydratedBucket.getValue()),
                                              reopeningContext.catalogEra,
                                              closedBuckets);
        }

        if (swBucket.isOK()) {
            Bucket& bucket = swBucket.getValue().get();
            auto insertionResult = insertIntoBucket(opCtx,
                                                    catalog,
                                                    stripe,
                                                    stripeLock,
                                                    stripeNumber,
                                                    doc,
                                                    combine,
                                                    internal::AllowBucketCreation::kYes,
                                                    info,
                                                    bucket);
            auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
            invariant(batch);
            return SuccessfulInsertion{std::move(*batch), std::move(closedBuckets)};
        } else {
            stats.incNumBucketReopeningsFailed();
            if (swBucket.getStatus().code() == ErrorCodes::WriteConflict) {
                return swBucket.getStatus();
            }
            // If we had a different type of error, then we should fall through and proceed to open
            // a new bucket.
        }
    }

    Bucket* bucket = useBucket(
        opCtx, catalog, stripe, stripeLock, nss, info, internal::AllowBucketCreation::kYes);
    invariant(bucket);

    auto insertionResult = insertIntoBucket(opCtx,
                                            catalog,
                                            stripe,
                                            stripeLock,
                                            stripeNumber,
                                            doc,
                                            combine,
                                            internal::AllowBucketCreation::kYes,
                                            info,
                                            *bucket);
    auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
    invariant(batch);
    return SuccessfulInsertion{std::move(*batch), std::move(closedBuckets)};
}

StatusWith<InsertResult> insert(OperationContext* opCtx,
                                BucketCatalog& catalog,
                                const NamespaceString& nss,
                                const UUID& collectionUUID,
                                const StringDataComparator* comparator,
                                const TimeseriesOptions& options,
                                const BSONObj& doc,
                                CombineWithInsertsFromOtherClients combine) {
    auto res = internal::extractBucketingParameters(collectionUUID, comparator, options, doc);
    if (!res.isOK()) {
        return res.getStatus();
    }
    auto& key = res.getValue().first;
    auto time = res.getValue().second;

    ExecutionStatsController stats =
        internal::getOrInitializeExecutionStats(catalog, collectionUUID);

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto stripeNumber = internal::getStripeNumber(key, catalog.numberOfStripes);
    ClosedBuckets closedBuckets;
    internal::CreationInfo info{key, stripeNumber, time, options, stats, &closedBuckets};
    auto& stripe = *catalog.stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket = useBucket(
        opCtx, catalog, stripe, stripeLock, nss, info, internal::AllowBucketCreation::kYes);
    invariant(bucket);

    auto insertionResult = insertIntoBucket(opCtx,
                                            catalog,
                                            stripe,
                                            stripeLock,
                                            stripeNumber,
                                            doc,
                                            combine,
                                            internal::AllowBucketCreation::kYes,
                                            info,
                                            *bucket);

    auto* batch = get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
    invariant(batch);
    return SuccessfulInsertion{std::move(*batch), std::move(closedBuckets)};
}

void waitToInsert(InsertWaiter* waiter) {
    if (auto* batch = get_if<std::shared_ptr<WriteBatch>>(waiter)) {
        getWriteBatchResult(**batch).getStatus().ignore();
    } else if (auto* request = get_if<std::shared_ptr<ReopeningRequest>>(waiter)) {
        waitForReopeningRequest(**request);
    }
}

Status prepareCommit(BucketCatalog& catalog,
                     const NamespaceString& nss,
                     std::shared_ptr<WriteBatch> batch) {
    auto getBatchStatus = [&] {
        return batch->promise.getFuture().getNoThrow().getStatus();
    };

    if (isWriteBatchFinished(*batch)) {
        // In this case, someone else aborted the batch behind our back. Oops.
        return getBatchStatus();
    }

    auto& stripe = *catalog.stripes[batch->bucketHandle.stripe];
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
                                                  batch->bucketHandle.bucketId,
                                                  internal::BucketPrepareAction::kPrepare);

    if (!bucket) {
        internal::abort(
            catalog,
            stripe,
            stripeLock,
            batch,
            internal::getTimeseriesBucketClearedError(nss, batch->bucketHandle.bucketId.oid));
        return getBatchStatus();
    }

    prepareWriteBatchForCommit(catalog.trackingContext, *batch, *bucket);

    return Status::OK();
}

boost::optional<ClosedBucket> finish(OperationContext* opCtx,
                                     BucketCatalog& catalog,
                                     const NamespaceString& nss,
                                     std::shared_ptr<WriteBatch> batch,
                                     const CommitInfo& info) {
    invariant(!isWriteBatchFinished(*batch));

    boost::optional<ClosedBucket> closedBucket;

    finishWriteBatch(*batch, info);

    auto& stripe = *catalog.stripes[batch->bucketHandle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    if (MONGO_unlikely(runPostCommitDebugChecks.shouldFail() && opCtx)) {
        Bucket* bucket = internal::useBucket(catalog.bucketStateRegistry,
                                             stripe,
                                             stripeLock,
                                             batch->bucketHandle.bucketId,
                                             internal::IgnoreBucketState::kYes);
        if (bucket) {
            internal::runPostCommitDebugChecks(opCtx, nss, *bucket, *batch);
        }
    }

    Bucket* bucket =
        internal::useBucketAndChangePreparedState(catalog.bucketStateRegistry,
                                                  stripe,
                                                  stripeLock,
                                                  batch->bucketHandle.bucketId,
                                                  internal::BucketPrepareAction::kUnprepare);
    if (bucket) {
        // See corollary in prepareWriteBatchForCommit().
        bucket->movableState = std::move(batch->movableState);

        // Re-opened V1 buckets get compressed during commit so we no longer need the uncompressed
        // bucket document.
        // TODO SERVER-86542: remove this line.
        bucket->movableState.uncompressedBucketDoc = makeTrackedBson(catalog.trackingContext, {});

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
        auto it = stripe.openBucketsById.find(batch->bucketHandle.bucketId);
        if (it != stripe.openBucketsById.end()) {
            bucket = it->second.get();
            bucket->preparedBatch.reset();
            internal::abort(catalog,
                            stripe,
                            stripeLock,
                            *bucket,
                            nullptr,
                            internal::getTimeseriesBucketClearedError(nss, bucket->bucketId.oid));
        }
    } else if (allCommitted(*bucket)) {
        switch (bucket->rolloverAction) {
            case RolloverAction::kHardClose:
            case RolloverAction::kSoftClose: {
                internal::closeOpenBucket(
                    opCtx, catalog, stripe, stripeLock, *bucket, closedBucket);
                break;
            }
            case RolloverAction::kArchive: {
                ClosedBuckets closedBuckets;
                internal::archiveBucket(opCtx, catalog, stripe, stripeLock, *bucket, closedBuckets);
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

    auto& stripe = *catalog.stripes[batch->bucketHandle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    internal::abort(catalog, stripe, stripeLock, batch, status);
}

void directWriteStart(BucketStateRegistry& registry, const UUID& collectionUUID, const OID& oid) {
    auto state = addDirectWrite(registry, BucketId{collectionUUID, oid});
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
    hangTimeseriesDirectModificationBeforeWriteConflict.pauseWhileSet();
    throwWriteConflictException("Prepared bucket can no longer be inserted into.");
}

void directWriteFinish(BucketStateRegistry& registry, const UUID& collectionUUID, const OID& oid) {
    hangTimeseriesDirectModificationBeforeFinish.pauseWhileSet();
    removeDirectWrite(registry, BucketId{collectionUUID, oid});
}

void clear(BucketCatalog& catalog, tracked_vector<UUID> clearedCollectionUUIDs) {
    clearSetOfBuckets(catalog.bucketStateRegistry, std::move(clearedCollectionUUIDs));
}

void clear(BucketCatalog& catalog, const UUID& collectionUUID) {
    tracked_vector<UUID> clearedCollectionUUIDs =
        make_tracked_vector<UUID>(catalog.trackingContext);
    clearedCollectionUUIDs.push_back(collectionUUID);
    clear(catalog, std::move(clearedCollectionUUIDs));
}

void freeze(BucketCatalog& catalog, const UUID& collectionUUID, const OID& oid) {
    internal::getOrInitializeExecutionStats(catalog, collectionUUID).incNumBucketsFrozen();
    freezeBucket(catalog.bucketStateRegistry, {collectionUUID, oid});
}

void resetBucketOIDCounter() {
    internal::resetBucketOIDCounter();
}

void appendExecutionStats(const BucketCatalog& catalog,
                          const UUID& collectionUUID,
                          BSONObjBuilder& builder) {
    const shared_tracked_ptr<ExecutionStats> stats =
        internal::getExecutionStats(catalog, collectionUUID);
    appendExecutionStatsToBuilder(*stats, builder);
}

void reportMeasurementsGroupCommitted(BucketCatalog& catalog,
                                      const UUID& collectionUUID,
                                      int64_t count) {
    auto stats = internal::getOrInitializeExecutionStats(catalog, collectionUUID);
    stats.incNumMeasurementsGroupCommitted(count);
}

}  // namespace mongo::timeseries::bucket_catalog
