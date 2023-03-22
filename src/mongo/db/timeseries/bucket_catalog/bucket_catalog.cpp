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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/basic.h"

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"

#include <algorithm>
#include <boost/iterator/transform_iterator.hpp>

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries::bucket_catalog {
namespace {
const auto getBucketCatalog = ServiceContext::declareDecoration<BucketCatalog>();
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationBeforeWriteConflict);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationAfterStart);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationBeforeFinish);

/**
 * Prepares the batch for commit. Sets min/max appropriately, records the number of
 * documents that have previously been committed to the bucket, and renders the batch
 * inactive. Must have commit rights.
 */
void prepareWriteBatchForCommit(WriteBatch& batch, Bucket& bucket) {
    invariant(batch.commitRights.load());
    batch.numPreviouslyCommittedMeasurements = bucket.numCommittedMeasurements;

    // Filter out field names that were new at the time of insertion, but have since been committed
    // by someone else.
    for (auto it = batch.newFieldNamesToBeInserted.begin();
         it != batch.newFieldNamesToBeInserted.end();) {
        StringMapHashedKey fieldName(it->first, it->second);
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

        // Approximate minmax memory usage by taking sizes of initial commit. Subsequent updates may
        // add fields but are most likely just to update values.
        bucket.memoryUsage += batch.min.objsize();
        bucket.memoryUsage += batch.max.objsize();
    }

    if (bucket.decompressed.has_value()) {
        batch.decompressed = std::move(bucket.decompressed);
        bucket.decompressed.reset();
        bucket.memoryUsage -= (batch.decompressed.value().before.objsize() +
                               batch.decompressed.value().after.objsize());
    }
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
 * Abandons the write batch and notifies any waiters that the bucket has been cleared.
 */
void abortWriteBatch(WriteBatch& batch, const Status& status) {
    if (batch.promise.getFuture().isReady()) {
        return;
    }

    batch.promise.setError(status);
}
}  // namespace

BucketCatalog& BucketCatalog::get(ServiceContext* svcCtx) {
    return getBucketCatalog(svcCtx);
}

BucketCatalog& BucketCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

BSONObj getMetadata(BucketCatalog& catalog, const BucketHandle& handle) {
    auto const& stripe = catalog.stripes[handle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    const Bucket* bucket =
        internal::findBucket(catalog.bucketStateRegistry, stripe, stripeLock, handle.bucketId);
    if (!bucket) {
        return {};
    }

    return bucket->key.metadata.toBSON();
}

StatusWith<InsertResult> tryInsert(OperationContext* opCtx,
                                   BucketCatalog& catalog,
                                   const NamespaceString& ns,
                                   const StringData::ComparatorInterface* comparator,
                                   const TimeseriesOptions& options,
                                   const BSONObj& doc,
                                   CombineWithInsertsFromOtherClients combine) {
    return internal::insert(
        opCtx, catalog, ns, comparator, options, doc, combine, internal::AllowBucketCreation::kNo);
}

StatusWith<InsertResult> insert(OperationContext* opCtx,
                                BucketCatalog& catalog,
                                const NamespaceString& ns,
                                const StringData::ComparatorInterface* comparator,
                                const TimeseriesOptions& options,
                                const BSONObj& doc,
                                CombineWithInsertsFromOtherClients combine,
                                BucketFindResult bucketFindResult) {
    return internal::insert(opCtx,
                            catalog,
                            ns,
                            comparator,
                            options,
                            doc,
                            combine,
                            internal::AllowBucketCreation::kYes,
                            bucketFindResult);
}

Status prepareCommit(BucketCatalog& catalog, std::shared_ptr<WriteBatch> batch) {
    auto getBatchStatus = [&] {
        return batch->promise.getFuture().getNoThrow().getStatus();
    };

    if (isWriteBatchFinished(*batch)) {
        // In this case, someone else aborted the batch behind our back. Oops.
        return getBatchStatus();
    }

    auto& stripe = catalog.stripes[batch->bucketHandle.stripe];
    internal::waitToCommitBatch(catalog.bucketStateRegistry, stripe, batch);

    stdx::lock_guard stripeLock{stripe.mutex};
    Bucket* bucket = internal::useBucketAndChangeState(
        catalog.bucketStateRegistry,
        stripe,
        stripeLock,
        batch->bucketHandle.bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            invariant(input.has_value());
            return input.value().setFlag(BucketStateFlag::kPrepared);
        });

    if (isWriteBatchFinished(*batch)) {
        // Someone may have aborted it while we were waiting. Since we have the prepared batch, we
        // should now be able to fully abort the bucket.
        if (bucket) {
            internal::abort(catalog, stripe, stripeLock, batch, getBatchStatus());
        }
        return getBatchStatus();
    } else if (!bucket) {
        internal::abort(catalog,
                        stripe,
                        stripeLock,
                        batch,
                        internal::getTimeseriesBucketClearedError(
                            batch->bucketHandle.bucketId.ns, batch->bucketHandle.bucketId.oid));
        return getBatchStatus();
    }

    auto prevMemoryUsage = bucket->memoryUsage;
    prepareWriteBatchForCommit(*batch, *bucket);
    catalog.memoryUsage.fetchAndAdd(bucket->memoryUsage - prevMemoryUsage);

    return Status::OK();
}

boost::optional<ClosedBucket> finish(BucketCatalog& catalog,
                                     std::shared_ptr<WriteBatch> batch,
                                     const CommitInfo& info) {
    invariant(!isWriteBatchFinished(*batch));

    boost::optional<ClosedBucket> closedBucket;

    finishWriteBatch(*batch, info);

    auto& stripe = catalog.stripes[batch->bucketHandle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket = internal::useBucketAndChangeState(
        catalog.bucketStateRegistry,
        stripe,
        stripeLock,
        batch->bucketHandle.bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            invariant(input.has_value());
            return input.value().unsetFlag(BucketStateFlag::kPrepared);
        });
    if (bucket) {
        bucket->preparedBatch.reset();
    }

    auto& stats = batch->stats;
    stats.incNumCommits();
    if (batch->numPreviouslyCommittedMeasurements == 0) {
        stats.incNumBucketInserts();
    } else {
        stats.incNumBucketUpdates();
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
                            internal::getTimeseriesBucketClearedError(bucket->bucketId.ns,
                                                                      bucket->bucketId.oid));
        }
    } else if (allCommitted(*bucket)) {
        switch (bucket->rolloverAction) {
            case RolloverAction::kHardClose:
            case RolloverAction::kSoftClose: {
                internal::closeOpenBucket(catalog, stripe, stripeLock, *bucket, closedBucket);
                break;
            }
            case RolloverAction::kArchive: {
                ClosedBuckets closedBuckets;
                internal::archiveBucket(catalog, stripe, stripeLock, *bucket, closedBuckets);
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

    auto& stripe = catalog.stripes[batch->bucketHandle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    internal::abort(catalog, stripe, stripeLock, batch, status);
}

void directWriteStart(BucketStateRegistry& registry, const NamespaceString& ns, const OID& oid) {
    invariant(!ns.isTimeseriesBucketsCollection());
    auto result = changeBucketState(
        registry,
        BucketId{ns, oid},
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            if (input.has_value()) {
                if (input.value().isPrepared()) {
                    return input.value();
                }
                return input.value().addDirectWrite();
            }
            // The underlying bucket isn't tracked by the catalog, but we need to insert a state
            // here so that we can conflict reopening this bucket until we've completed our write
            // and the reader has refetched.
            return BucketState{}.setFlag(BucketStateFlag::kUntracked).addDirectWrite();
        });
    if (result.has_value() && result.value().isPrepared()) {
        hangTimeseriesDirectModificationBeforeWriteConflict.pauseWhileSet();
        throwWriteConflictException("Prepared bucket can no longer be inserted into.");
    }
    hangTimeseriesDirectModificationAfterStart.pauseWhileSet();
}

void directWriteFinish(BucketStateRegistry& registry, const NamespaceString& ns, const OID& oid) {
    invariant(!ns.isTimeseriesBucketsCollection());
    hangTimeseriesDirectModificationBeforeFinish.pauseWhileSet();
    (void)changeBucketState(
        registry,
        BucketId{ns, oid},
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            if (!input.has_value()) {
                // We may have had multiple direct writes to this document in the same storage
                // transaction. If so, a previous call to directWriteFinish may have cleaned up the
                // state.
                return boost::none;
            }

            auto& modified = input.value().removeDirectWrite();
            if (!modified.isSet(BucketStateFlag::kPendingDirectWrite) &&
                modified.isSet(BucketStateFlag::kUntracked)) {
                // The underlying bucket is no longer tracked by the catalog, so we can clean up the
                // state.
                return boost::none;
            }
            return modified;
        });
}

void clear(BucketCatalog& catalog, ShouldClearFn&& shouldClear) {
    if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        clearSetOfBuckets(catalog.bucketStateRegistry, std::move(shouldClear));
        return;
    }
    for (auto& stripe : catalog.stripes) {
        stdx::lock_guard stripeLock{stripe.mutex};
        for (auto it = stripe.openBucketsById.begin(); it != stripe.openBucketsById.end();) {
            auto nextIt = std::next(it);

            const auto& bucket = it->second;
            if (shouldClear(bucket->bucketId.ns)) {
                {
                    stdx::lock_guard catalogLock{catalog.mutex};
                    catalog.executionStats.erase(bucket->bucketId.ns);
                }
                internal::abort(catalog,
                                stripe,
                                stripeLock,
                                *bucket,
                                nullptr,
                                internal::getTimeseriesBucketClearedError(bucket->bucketId.ns,
                                                                          bucket->bucketId.oid));
            }

            it = nextIt;
        }
    }
}

void clear(BucketCatalog& catalog, const NamespaceString& ns) {
    invariant(!ns.isTimeseriesBucketsCollection());
    clear(catalog, [ns](const NamespaceString& bucketNs) { return bucketNs == ns; });
}

void clear(BucketCatalog& catalog, StringData dbName) {
    clear(catalog, [dbName = dbName.toString()](const NamespaceString& bucketNs) {
        return bucketNs.db() == dbName;
    });
}

void appendExecutionStats(const BucketCatalog& catalog,
                          const NamespaceString& ns,
                          BSONObjBuilder& builder) {
    invariant(!ns.isTimeseriesBucketsCollection());
    const std::shared_ptr<ExecutionStats> stats = internal::getExecutionStats(catalog, ns);
    appendExecutionStatsToBuilder(*stats, builder);
}

}  // namespace mongo::timeseries::bucket_catalog
