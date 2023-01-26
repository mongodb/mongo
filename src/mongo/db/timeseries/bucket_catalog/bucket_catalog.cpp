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
#include <boost/utility/in_place_factory.hpp>

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
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
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeReopeningBucket);
MONGO_FAIL_POINT_DEFINE(alwaysUseSameBucketCatalogStripe);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationAfterStart);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationBeforeFinish);
MONGO_FAIL_POINT_DEFINE(hangWaitingForConflictingPreparedBatch);

OperationId getOpId(OperationContext* opCtx,
                    BucketCatalog::CombineWithInsertsFromOtherClients combine) {
    switch (combine) {
        case BucketCatalog::CombineWithInsertsFromOtherClients::kAllow:
            return 0;
        case BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow:
            invariant(opCtx->getOpID());
            return opCtx->getOpID();
    }
    MONGO_UNREACHABLE;
}

BSONObj buildControlMinTimestampDoc(StringData timeField, Date_t roundedTime) {
    BSONObjBuilder builder;
    builder.append(timeField, roundedTime);
    return builder.obj();
}

std::pair<OID, Date_t> generateBucketOID(const Date_t& time, const TimeseriesOptions& options) {
    OID oid = OID::gen();

    // We round the measurement timestamp down to the nearest minute, hour, or day depending on the
    // granularity. We do this for two reasons. The first is so that if measurements come in
    // slightly out of order, we don't have to close the current bucket due to going backwards in
    // time. The second, and more important reason, is so that we reliably group measurements
    // together into predictable chunks for sharding. This way we know from a measurement timestamp
    // what the bucket timestamp will be, so we can route measurements to the right shard chunk.
    auto roundedTime = roundTimestampToGranularity(time, options);
    int64_t const roundedSeconds = durationCount<Seconds>(roundedTime.toDurationSinceEpoch());
    oid.setTimestamp(roundedSeconds);

    // Now, if we stopped here we could end up with bucket OID collisions. Consider the case where
    // we have the granularity set to 'Hours'. This means we will round down to the nearest day, so
    // any bucket generated on the same machine on the same day will have the same timestamp portion
    // and unique instance portion of the OID. Only the increment will differ. Since we only use 3
    // bytes for the increment portion, we run a serious risk of overflow if we are generating lots
    // of buckets.
    //
    // To address this, we'll take the difference between the actual timestamp and the rounded
    // timestamp and add it to the instance portion of the OID to ensure we can't have a collision.
    // for timestamps generated on the same machine.
    //
    // This leaves open the possibility that in the case of step-down/step-up, we could get a
    // collision if the old primary and the new primary have unique instance bits that differ by
    // less than the maximum rounding difference. This is quite unlikely though, and can be resolved
    // by restarting the new primary. It remains an open question whether we can fix this in a
    // better way.
    // TODO (SERVER-61412): Avoid time-series bucket OID collisions after election
    auto instance = oid.getInstanceUnique();
    uint32_t sum = DataView(reinterpret_cast<char*>(instance.bytes)).read<uint32_t>(1) +
        (durationCount<Seconds>(time.toDurationSinceEpoch()) - roundedSeconds);
    DataView(reinterpret_cast<char*>(instance.bytes)).write<uint32_t>(sum, 1);
    oid.setInstanceUnique(instance);

    return {oid, roundedTime};
}

Status getTimeseriesBucketClearedError(const NamespaceString& ns, const OID& oid) {
    return {ErrorCodes::TimeseriesBucketCleared,
            str::stream() << "Time-series bucket " << oid << " for namespace " << ns
                          << " was cleared"};
}

/**
 * Caluculate the bucket max size constrained by the cache size and the cardinality of active
 * buckets.
 */
int32_t getCacheDerivedBucketMaxSize(StorageEngine* storageEngine, uint32_t workloadCardinality) {
    invariant(storageEngine);
    uint64_t storageCacheSize =
        static_cast<uint64_t>(storageEngine->getEngine()->getCacheSizeMB() * 1024 * 1024);

    if (!feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility) ||
        storageCacheSize == 0 || workloadCardinality == 0) {
        return INT_MAX;
    }

    uint64_t derivedMaxSize = storageCacheSize / (2 * workloadCardinality);
    uint64_t intMax = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    return std::min(derivedMaxSize, intMax);
}

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

Status BucketCatalog::reopenBucket(OperationContext* opCtx,
                                   const CollectionPtr& coll,
                                   const BSONObj& bucketDoc) {
    const NamespaceString ns = coll->ns().getTimeseriesViewNamespace();
    const boost::optional<TimeseriesOptions> options = coll->getTimeseriesOptions();
    invariant(options,
              str::stream() << "Attempting to reopen a bucket for a non-timeseries collection: "
                            << ns);

    BSONElement metadata;
    auto metaFieldName = options->getMetaField();
    if (metaFieldName) {
        metadata = bucketDoc.getField(*metaFieldName);
    }
    auto key = BucketKey{ns, BucketMetadata{metadata, coll->getDefaultCollator()}};

    // Validate the bucket document against the schema.
    auto validator = [&](OperationContext * opCtx, const BSONObj& bucketDoc) -> auto {
        return coll->checkValidation(opCtx, bucketDoc);
    };

    auto stats = _getExecutionStats(ns);

    auto res = _rehydrateBucket(opCtx,
                                ns,
                                coll->getDefaultCollator(),
                                *options,
                                BucketToReopen{bucketDoc, validator},
                                boost::none);
    if (!res.isOK()) {
        return res.getStatus();
    }
    auto bucket = std::move(res.getValue());

    auto stripeNumber = _getStripeNumber(key);

    // Register the reopened bucket with the catalog.
    auto& stripe = _stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    ClosedBuckets closedBuckets;
    return _reopenBucket(&stripe,
                         stripeLock,
                         stats,
                         key,
                         std::move(bucket),
                         _bucketStateManager.getEra(),
                         &closedBuckets)
        .getStatus();
}

BSONObj BucketCatalog::getMetadata(const BucketHandle& handle) {
    auto const& stripe = _stripes[handle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    const Bucket* bucket = _findBucket(stripe, stripeLock, handle.bucketId);
    if (!bucket) {
        return {};
    }

    return bucket->key.metadata.toBSON();
}

StatusWith<BucketCatalog::InsertResult> BucketCatalog::tryInsert(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine) {
    return _insert(opCtx, ns, comparator, options, doc, combine, AllowBucketCreation::kNo);
}

StatusWith<BucketCatalog::InsertResult> BucketCatalog::insert(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine,
    BucketFindResult bucketFindResult) {
    return _insert(
        opCtx, ns, comparator, options, doc, combine, AllowBucketCreation::kYes, bucketFindResult);
}

Status BucketCatalog::prepareCommit(std::shared_ptr<WriteBatch> batch) {
    auto getBatchStatus = [&] { return batch->promise.getFuture().getNoThrow().getStatus(); };

    if (isWriteBatchFinished(*batch)) {
        // In this case, someone else aborted the batch behind our back. Oops.
        return getBatchStatus();
    }

    auto& stripe = _stripes[batch->bucketHandle.stripe];
    _waitToCommitBatch(&stripe, batch);

    stdx::lock_guard stripeLock{stripe.mutex};
    Bucket* bucket = _useBucketAndChangeState(
        &stripe,
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
            _abort(&stripe, stripeLock, batch, getBatchStatus());
        }
        return getBatchStatus();
    } else if (!bucket) {
        _abort(&stripe,
               stripeLock,
               batch,
               getTimeseriesBucketClearedError(batch->bucketHandle.bucketId.ns,
                                               batch->bucketHandle.bucketId.oid));
        return getBatchStatus();
    }

    auto prevMemoryUsage = bucket->memoryUsage;
    prepareWriteBatchForCommit(*batch, *bucket);
    _memoryUsage.fetchAndAdd(bucket->memoryUsage - prevMemoryUsage);

    return Status::OK();
}

boost::optional<ClosedBucket> BucketCatalog::finish(std::shared_ptr<WriteBatch> batch,
                                                    const CommitInfo& info) {
    invariant(!isWriteBatchFinished(*batch));

    boost::optional<ClosedBucket> closedBucket;

    finishWriteBatch(*batch, info);

    auto& stripe = _stripes[batch->bucketHandle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket = _useBucketAndChangeState(
        &stripe,
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
        auto it = stripe.allBuckets.find(batch->bucketHandle.bucketId);
        if (it != stripe.allBuckets.end()) {
            bucket = it->second.get();
            bucket->preparedBatch.reset();
            _abort(&stripe,
                   stripeLock,
                   bucket,
                   nullptr,
                   getTimeseriesBucketClearedError(bucket->bucketId.ns, bucket->bucketId.oid));
        }
    } else if (allCommitted(*bucket)) {
        switch (bucket->rolloverAction) {
            case RolloverAction::kHardClose:
            case RolloverAction::kSoftClose: {
                const bool eligibleForReopening =
                    bucket->rolloverAction == RolloverAction::kSoftClose;
                closedBucket = boost::in_place(&_bucketStateManager,
                                               bucket->bucketId,
                                               bucket->timeField,
                                               bucket->numMeasurements,
                                               eligibleForReopening);
                _removeBucket(&stripe, stripeLock, bucket, RemovalMode::kClose);
                break;
            }
            case RolloverAction::kArchive: {
                ClosedBuckets closedBuckets;
                _archiveBucket(&stripe, stripeLock, bucket, &closedBuckets);
                if (!closedBuckets.empty()) {
                    closedBucket = std::move(closedBuckets[0]);
                }
                break;
            }
            case RolloverAction::kNone: {
                _markBucketIdle(&stripe, stripeLock, bucket);
                break;
            }
        }
    }
    return closedBucket;
}

void BucketCatalog::abort(std::shared_ptr<WriteBatch> batch, const Status& status) {
    invariant(batch);
    invariant(batch->commitRights.load());

    if (isWriteBatchFinished(*batch)) {
        return;
    }

    auto& stripe = _stripes[batch->bucketHandle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    _abort(&stripe, stripeLock, batch, status);
}

void BucketCatalog::directWriteStart(const NamespaceString& ns, const OID& oid) {
    invariant(!ns.isTimeseriesBucketsCollection());
    auto result = _bucketStateManager.changeBucketState(
        BucketId{ns, oid},
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            if (input.has_value()) {
                return input.value().setFlag(BucketStateFlag::kPendingDirectWrite);
            }
            // The underlying bucket isn't tracked by the catalog, but we need to insert a state
            // here so that we can conflict reopening this bucket until we've completed our write
            // and the reader has refetched.
            return BucketState{}
                .setFlag(BucketStateFlag::kPendingDirectWrite)
                .setFlag(BucketStateFlag::kUntracked);
        });
    if (result && result.value().isPrepared()) {
        hangTimeseriesDirectModificationBeforeWriteConflict.pauseWhileSet();
        throwWriteConflictException("Prepared bucket can no longer be inserted into.");
    }
    hangTimeseriesDirectModificationAfterStart.pauseWhileSet();
}

void BucketCatalog::directWriteFinish(const NamespaceString& ns, const OID& oid) {
    invariant(!ns.isTimeseriesBucketsCollection());
    hangTimeseriesDirectModificationBeforeFinish.pauseWhileSet();
    (void)_bucketStateManager.changeBucketState(
        BucketId{ns, oid},
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            if (!input.has_value()) {
                // We may have had multiple direct writes to this document in the same storage
                // transaction. If so, a previous call to directWriteFinish may have cleaned up the
                // state.
                return boost::none;
            }
            if (input.value().isSet(BucketStateFlag::kUntracked)) {
                // The underlying bucket is not tracked by the catalog, so we can clean up the
                // state.
                return boost::none;
            }
            return input.value()
                .unsetFlag(BucketStateFlag::kPendingDirectWrite)
                .setFlag(BucketStateFlag::kCleared);
        });
}

void BucketCatalog::clear(ShouldClearFn&& shouldClear) {
    if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        _bucketStateManager.clearSetOfBuckets(std::move(shouldClear));
        return;
    }
    for (auto& stripe : _stripes) {
        stdx::lock_guard stripeLock{stripe.mutex};
        for (auto it = stripe.allBuckets.begin(); it != stripe.allBuckets.end();) {
            auto nextIt = std::next(it);

            const auto& bucket = it->second;
            if (shouldClear(bucket->bucketId.ns)) {
                {
                    stdx::lock_guard catalogLock{_mutex};
                    _executionStats.erase(bucket->bucketId.ns);
                }
                _abort(&stripe,
                       stripeLock,
                       bucket.get(),
                       nullptr,
                       getTimeseriesBucketClearedError(bucket->bucketId.ns, bucket->bucketId.oid));
            }

            it = nextIt;
        }
    }
}

void BucketCatalog::clear(const NamespaceString& ns) {
    invariant(!ns.isTimeseriesBucketsCollection());
    clear([ns](const NamespaceString& bucketNs) { return bucketNs == ns; });
}

void BucketCatalog::clear(StringData dbName) {
    clear([dbName = dbName.toString()](const NamespaceString& bucketNs) {
        return bucketNs.db() == dbName;
    });
}

void BucketCatalog::_appendExecutionStatsToBuilder(const ExecutionStats* stats,
                                                   BSONObjBuilder* builder) const {
    builder->appendNumber("numBucketInserts", stats->numBucketInserts.load());
    builder->appendNumber("numBucketUpdates", stats->numBucketUpdates.load());
    builder->appendNumber("numBucketsOpenedDueToMetadata",
                          stats->numBucketsOpenedDueToMetadata.load());
    builder->appendNumber("numBucketsClosedDueToCount", stats->numBucketsClosedDueToCount.load());
    builder->appendNumber("numBucketsClosedDueToSchemaChange",
                          stats->numBucketsClosedDueToSchemaChange.load());
    builder->appendNumber("numBucketsClosedDueToSize", stats->numBucketsClosedDueToSize.load());
    builder->appendNumber("numBucketsClosedDueToTimeForward",
                          stats->numBucketsClosedDueToTimeForward.load());
    builder->appendNumber("numBucketsClosedDueToTimeBackward",
                          stats->numBucketsClosedDueToTimeBackward.load());
    builder->appendNumber("numBucketsClosedDueToMemoryThreshold",
                          stats->numBucketsClosedDueToMemoryThreshold.load());

    auto commits = stats->numCommits.load();
    builder->appendNumber("numCommits", commits);
    builder->appendNumber("numWaits", stats->numWaits.load());
    auto measurementsCommitted = stats->numMeasurementsCommitted.load();
    builder->appendNumber("numMeasurementsCommitted", measurementsCommitted);
    if (commits) {
        builder->appendNumber("avgNumMeasurementsPerCommit", measurementsCommitted / commits);
    }

    if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        builder->appendNumber("numBucketsClosedDueToReopening",
                              stats->numBucketsClosedDueToReopening.load());
        builder->appendNumber("numBucketsArchivedDueToMemoryThreshold",
                              stats->numBucketsArchivedDueToMemoryThreshold.load());
        builder->appendNumber("numBucketsArchivedDueToTimeBackward",
                              stats->numBucketsArchivedDueToTimeBackward.load());
        builder->appendNumber("numBucketsReopened", stats->numBucketsReopened.load());
        builder->appendNumber("numBucketsKeptOpenDueToLargeMeasurements",
                              stats->numBucketsKeptOpenDueToLargeMeasurements.load());
        builder->appendNumber("numBucketsClosedDueToCachePressure",
                              stats->numBucketsClosedDueToCachePressure.load());
        builder->appendNumber("numBucketsFetched", stats->numBucketsFetched.load());
        builder->appendNumber("numBucketsQueried", stats->numBucketsQueried.load());
        builder->appendNumber("numBucketFetchesFailed", stats->numBucketFetchesFailed.load());
        builder->appendNumber("numBucketQueriesFailed", stats->numBucketQueriesFailed.load());
        builder->appendNumber("numBucketReopeningsFailed", stats->numBucketReopeningsFailed.load());
        builder->appendNumber("numDuplicateBucketsReopened",
                              stats->numDuplicateBucketsReopened.load());
    }
}

void BucketCatalog::appendExecutionStats(const NamespaceString& ns, BSONObjBuilder* builder) const {
    invariant(!ns.isTimeseriesBucketsCollection());
    const std::shared_ptr<ExecutionStats> stats = _getExecutionStats(ns);
    _appendExecutionStatsToBuilder(stats.get(), builder);
}

void BucketCatalog::appendGlobalExecutionStats(BSONObjBuilder* builder) const {
    _appendExecutionStatsToBuilder(&_globalExecutionStats, builder);
}

void BucketCatalog::appendStateManagementStats(BSONObjBuilder* builder) const {
    _bucketStateManager.appendStats(builder);
}

long long BucketCatalog::memoryUsage() const {
    return _memoryUsage.load();
}

StatusWith<std::pair<BucketKey, Date_t>> BucketCatalog::_extractBucketingParameters(
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc) const {
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
    auto key = BucketKey{ns, BucketMetadata{metadata, comparator}};

    return {std::make_pair(key, time)};
}

BucketCatalog::StripeNumber BucketCatalog::_getStripeNumber(const BucketKey& key) const {
    if (MONGO_unlikely(alwaysUseSameBucketCatalogStripe.shouldFail())) {
        return 0;
    }
    return key.hash % kNumberOfStripes;
}

const Bucket* BucketCatalog::_findBucket(const Stripe& stripe,
                                         WithLock,
                                         const BucketId& bucketId,
                                         IgnoreBucketState mode) {
    auto it = stripe.allBuckets.find(bucketId);
    if (it != stripe.allBuckets.end()) {
        if (mode == IgnoreBucketState::kYes) {
            return it->second.get();
        }

        if (auto state = _bucketStateManager.getBucketState(it->second.get());
            state && !state.value().conflictsWithInsertion()) {
            return it->second.get();
        }
    }
    return nullptr;
}

Bucket* BucketCatalog::_useBucket(Stripe* stripe,
                                  WithLock stripeLock,
                                  const BucketId& bucketId,
                                  IgnoreBucketState mode) {
    return const_cast<Bucket*>(_findBucket(*stripe, stripeLock, bucketId, mode));
}

Bucket* BucketCatalog::_useBucketAndChangeState(Stripe* stripe,
                                                WithLock stripeLock,
                                                const BucketId& bucketId,
                                                const BucketStateManager::StateChangeFn& change) {
    auto it = stripe->allBuckets.find(bucketId);
    if (it != stripe->allBuckets.end()) {
        if (auto state = _bucketStateManager.changeBucketState(it->second.get(), change);
            state && !state.value().conflictsWithInsertion()) {
            return it->second.get();
        }
    }
    return nullptr;
}

Bucket* BucketCatalog::_useBucket(Stripe* stripe,
                                  WithLock stripeLock,
                                  const CreationInfo& info,
                                  AllowBucketCreation mode) {
    auto it = stripe->openBuckets.find(info.key);
    if (it == stripe->openBuckets.end()) {
        // No open bucket for this metadata.
        return mode == AllowBucketCreation::kYes ? _allocateBucket(stripe, stripeLock, info)
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
        return mode == AllowBucketCreation::kYes ? _allocateBucket(stripe, stripeLock, info)
                                                 : nullptr;
    }

    if (auto state = _bucketStateManager.getBucketState(bucket);
        state && !state.value().conflictsWithInsertion()) {
        _markBucketNotIdle(stripe, stripeLock, bucket);
        return bucket;
    }

    _abort(stripe,
           stripeLock,
           bucket,
           nullptr,
           getTimeseriesBucketClearedError(bucket->bucketId.ns, bucket->bucketId.oid));

    return mode == AllowBucketCreation::kYes ? _allocateBucket(stripe, stripeLock, info) : nullptr;
}

Bucket* BucketCatalog::_useAlternateBucket(Stripe* stripe,
                                           WithLock stripeLock,
                                           const CreationInfo& info) {
    auto it = stripe->openBuckets.find(info.key);
    if (it == stripe->openBuckets.end()) {
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

        auto state = _bucketStateManager.getBucketState(potentialBucket);
        invariant(state);
        if (!state.value().conflictsWithInsertion()) {
            invariant(!potentialBucket->idleListEntry.has_value());
            return potentialBucket;
        }

        // If we still have an entry for the bucket in the open set, but it conflicts with
        // insertion, then it must have been cleared, and we can clean it up.
        invariant(state.value().isSet(BucketStateFlag::kCleared));
        _abort(stripe,
               stripeLock,
               potentialBucket,
               nullptr,
               getTimeseriesBucketClearedError(potentialBucket->bucketId.ns,
                                               potentialBucket->bucketId.oid));
    }

    return nullptr;
}

StatusWith<std::unique_ptr<Bucket>> BucketCatalog::_rehydrateBucket(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BucketToReopen& bucketToReopen,
    boost::optional<const BucketKey&> expectedKey) {
    invariant(feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
        serverGlobalParams.featureCompatibility));
    const auto& [bucketDoc, validator, catalogEra] = bucketToReopen;
    if (catalogEra < _bucketStateManager.getEra()) {
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
    auto key = BucketKey{ns, BucketMetadata{metadata, comparator}};
    if (expectedKey.has_value() && key != expectedKey.value()) {
        return {ErrorCodes::BadValue, "Bucket metadata does not match (hash collision)"};
    }

    auto minTime = controlField.getObjectField(kBucketControlMinFieldName)
                       .getField(options.getTimeField())
                       .Date();
    BucketId bucketId{key.ns, bucketIdElem.OID()};
    std::unique_ptr<Bucket> bucket = std::make_unique<Bucket>(
        bucketId, key, options.getTimeField(), minTime, _bucketStateManager);

    const bool isCompressed = isCompressedBucket(bucketDoc);

    // Initialize the remaining member variables from the bucket document.
    if (isCompressed) {
        auto decompressed = decompressBucket(bucketDoc);
        if (!decompressed.has_value()) {
            return Status{ErrorCodes::BadValue, "Bucket could not be decompressed"};
        }
        bucket->size = decompressed.value().objsize();
        bucket->decompressed = DecompressionResult{bucketDoc, decompressed.value()};
        bucket->memoryUsage += (decompressed.value().objsize() + bucketDoc.objsize());
    } else {
        bucket->size = bucketDoc.objsize();
    }

    // Populate the top-level data field names.
    const BSONObj& dataObj = bucketDoc.getObjectField(kBucketDataFieldName);
    for (const BSONElement& dataElem : dataObj) {
        auto hashedKey = StringSet::hasher().hashed_key(dataElem.fieldName());
        bucket->fieldNames.emplace(hashedKey);
    }

    auto swMinMax = generateMinMaxFromBucketDoc(bucketDoc, comparator);
    if (!swMinMax.isOK()) {
        return swMinMax.getStatus();
    }
    bucket->minmax = std::move(swMinMax.getValue());

    auto swSchema = generateSchemaFromBucketDoc(bucketDoc, comparator);
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

    bucket->numMeasurements = numMeasurements;
    bucket->numCommittedMeasurements = numMeasurements;

    // The namespace is stored two times: the bucket itself and openBuckets. We don't have a great
    // approximation for the _schema or _minmax data structure size, so we use the control field
    // size as an approximation for _minmax, and half that size for _schema. Since the metadata
    // is stored in the bucket, we need to add that as well. A unique pointer to the bucket is
    // stored once: allBuckets. A raw pointer to the bucket is stored at most twice: openBuckets,
    // idleBuckets.
    bucket->memoryUsage += (key.ns.size() * 2) + 1.5 * controlField.objsize() +
        key.metadata.toBSON().objsize() + sizeof(Bucket) + sizeof(std::unique_ptr<Bucket>) +
        (sizeof(Bucket*) * 2);

    return {std::move(bucket)};
}

StatusWith<Bucket*> BucketCatalog::_reopenBucket(Stripe* stripe,
                                                 WithLock stripeLock,
                                                 ExecutionStatsController stats,
                                                 const BucketKey& key,
                                                 std::unique_ptr<Bucket>&& bucket,
                                                 std::uint64_t targetEra,
                                                 ClosedBuckets* closedBuckets) {
    invariant(bucket);

    _expireIdleBuckets(stripe, stripeLock, stats, closedBuckets);

    // We may need to initialize the bucket's state.
    bool conflicts = false;
    auto initializeStateFn =
        [targetEra, &conflicts](boost::optional<BucketState> input,
                                std::uint64_t currentEra) -> boost::optional<BucketState> {
        if (targetEra < currentEra ||
            (input.has_value() && input.value().conflictsWithReopening())) {
            conflicts = true;
            return input;
        }
        conflicts = false;
        return input.has_value() ? input.value() : BucketState{};
    };

    auto state = _bucketStateManager.changeBucketState(bucket->bucketId, initializeStateFn);
    if (conflicts) {
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    }

    // If this bucket was archived, we need to remove it from the set of archived buckets.
    if (auto setIt = stripe->archivedBuckets.find(key.hash);
        setIt != stripe->archivedBuckets.end()) {
        auto& archivedSet = setIt->second;
        if (auto bucketIt = archivedSet.find(bucket->minTime);
            bucketIt != archivedSet.end() && bucket->bucketId == bucketIt->second.bucketId) {
            long long memory =
                _marginalMemoryUsageForArchivedBucket(bucketIt->second, archivedSet.size() == 1);
            if (archivedSet.size() == 1) {
                stripe->archivedBuckets.erase(setIt);
            } else {
                archivedSet.erase(bucketIt);
            }
            _memoryUsage.fetchAndSubtract(memory);
            _numberOfActiveBuckets.fetchAndSubtract(1);
        }
    }

    // Pass ownership of the reopened bucket to the bucket catalog.
    auto [insertedIt, newlyInserted] =
        stripe->allBuckets.try_emplace(bucket->bucketId, std::move(bucket));
    invariant(newlyInserted);
    Bucket* unownedBucket = insertedIt->second.get();

    // If we already have an open bucket for this key, we need to close it.
    if (auto it = stripe->openBuckets.find(key); it != stripe->openBuckets.end()) {
        auto& openSet = it->second;
        for (Bucket* existingBucket : openSet) {
            if (existingBucket->rolloverAction == RolloverAction::kNone) {
                stats.incNumBucketsClosedDueToReopening();
                if (allCommitted(*existingBucket)) {
                    constexpr bool eligibleForReopening = true;
                    closedBuckets->emplace_back(ClosedBucket{&_bucketStateManager,
                                                             existingBucket->bucketId,
                                                             existingBucket->timeField,
                                                             existingBucket->numMeasurements,
                                                             eligibleForReopening});
                    _removeBucket(stripe, stripeLock, existingBucket, RemovalMode::kClose);
                } else {
                    existingBucket->rolloverAction = RolloverAction::kSoftClose;
                }
                // We should only have one open bucket at a time.
                break;
            }
        }
    }

    // Now actually mark this bucket as open.
    stripe->openBuckets[key].emplace(unownedBucket);
    stats.incNumBucketsReopened();

    _memoryUsage.addAndFetch(unownedBucket->memoryUsage);
    _numberOfActiveBuckets.fetchAndAdd(1);

    return unownedBucket;
}

StatusWith<Bucket*> BucketCatalog::_reuseExistingBucket(Stripe* stripe,
                                                        WithLock stripeLock,
                                                        ExecutionStatsController* stats,
                                                        const BucketKey& key,
                                                        Bucket* existingBucket,
                                                        std::uint64_t targetEra) {
    invariant(existingBucket);

    // If we have an existing bucket, passing the Bucket* will let us check if the bucket was
    // cleared as part of a set since the last time it was used. If we were to just check by
    // OID, we may miss if e.g. there was a move chunk operation.
    bool conflicts = false;
    auto state = _bucketStateManager.changeBucketState(
        existingBucket,
        [targetEra, &conflicts](boost::optional<BucketState> input,
                                std::uint64_t currentEra) -> boost::optional<BucketState> {
            if (targetEra < currentEra ||
                (input.has_value() && input.value().conflictsWithReopening())) {
                conflicts = true;
                return input;
            }
            conflicts = false;
            return input.has_value() ? input.value() : BucketState{};
        });
    if (state.has_value() && state.value().isSet(BucketStateFlag::kCleared)) {
        _abort(stripe,
               stripeLock,
               existingBucket,
               nullptr,
               getTimeseriesBucketClearedError(existingBucket->bucketId.ns,
                                               existingBucket->bucketId.oid));
        conflicts = true;
    }
    if (conflicts) {
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    }

    // It's possible to have two buckets with the same ID in different collections, so let's make
    // extra sure the existing bucket is the right one.
    if (existingBucket->bucketId.ns != key.ns) {
        return {ErrorCodes::BadValue, "Cannot re-use bucket: same ID but different namespace"};
    }

    // If the bucket was already open, wasn't cleared, the state didn't conflict with reopening, and
    // the namespace matches, then we can simply return it.
    stats->incNumDuplicateBucketsReopened();
    _markBucketNotIdle(stripe, stripeLock, existingBucket);

    return existingBucket;
}

StatusWith<BucketCatalog::InsertResult> BucketCatalog::_insert(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine,
    AllowBucketCreation mode,
    BucketFindResult bucketFindResult) {
    invariant(!ns.isTimeseriesBucketsCollection());

    auto res = _extractBucketingParameters(ns, comparator, options, doc);
    if (!res.isOK()) {
        return res.getStatus();
    }
    auto& key = res.getValue().first;
    auto time = res.getValue().second;

    ExecutionStatsController stats = _getExecutionStats(ns);
    _updateBucketFetchAndQueryStats(stats, bucketFindResult);

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto stripeNumber = _getStripeNumber(key);

    InsertResult result;
    result.catalogEra = _bucketStateManager.getEra();
    CreationInfo info{key, stripeNumber, time, options, stats, &result.closedBuckets};
    boost::optional<BucketToReopen> bucketToReopen = std::move(bucketFindResult.bucketToReopen);

    auto rehydratedBucket = bucketToReopen.has_value()
        ? _rehydrateBucket(opCtx, ns, comparator, options, bucketToReopen.value(), key)
        : StatusWith<std::unique_ptr<Bucket>>{ErrorCodes::BadValue, "No bucket to rehydrate"};
    if (rehydratedBucket.getStatus().code() == ErrorCodes::WriteConflict) {
        stats.incNumBucketReopeningsFailed();
        return rehydratedBucket.getStatus();
    }

    auto& stripe = _stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    if (rehydratedBucket.isOK()) {
        invariant(mode == AllowBucketCreation::kYes);
        hangTimeseriesInsertBeforeReopeningBucket.pauseWhileSet();

        StatusWith<Bucket*> swBucket{nullptr};
        auto existingIt = stripe.allBuckets.find(rehydratedBucket.getValue()->bucketId);
        if (existingIt != stripe.allBuckets.end()) {
            // First let's check the existing bucket if we have one.
            Bucket* existingBucket = existingIt->second.get();
            swBucket = _reuseExistingBucket(
                &stripe, stripeLock, &stats, key, existingBucket, bucketToReopen->catalogEra);
        } else {
            // No existing bucket to use, go ahead and try to reopen our rehydrated bucket.
            swBucket = _reopenBucket(&stripe,
                                     stripeLock,
                                     stats,
                                     key,
                                     std::move(rehydratedBucket.getValue()),
                                     bucketToReopen->catalogEra,
                                     &result.closedBuckets);
        }

        if (swBucket.isOK()) {
            Bucket* bucket = swBucket.getValue();
            invariant(bucket);
            auto insertionResult = _insertIntoBucket(
                opCtx, &stripe, stripeLock, stripeNumber, doc, combine, mode, &info, bucket);
            auto* batch = stdx::get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
            invariant(batch);
            result.batch = *batch;

            return std::move(result);
        } else {
            stats.incNumBucketReopeningsFailed();
            if (swBucket.getStatus().code() == ErrorCodes::WriteConflict) {
                return swBucket.getStatus();
            }
            // If we had a different type of error, then we should fall through and proceed to open
            // a new bucket.
        }
    }

    Bucket* bucket = _useBucket(&stripe, stripeLock, info, mode);
    if (!bucket) {
        invariant(mode == AllowBucketCreation::kNo);
        constexpr bool allowQueryBasedReopening = true;
        result.candidate =
            _getReopeningCandidate(&stripe, stripeLock, info, allowQueryBasedReopening);
        return std::move(result);
    }

    auto insertionResult = _insertIntoBucket(
        opCtx, &stripe, stripeLock, stripeNumber, doc, combine, mode, &info, bucket);
    if (auto* reason = stdx::get_if<RolloverReason>(&insertionResult)) {
        invariant(mode == AllowBucketCreation::kNo);
        if (allCommitted(*bucket)) {
            _markBucketIdle(&stripe, stripeLock, bucket);
        }

        // If we were time forward or backward, we might be able to "reopen" a bucket we still have
        // in memory that's set to be closed when pending operations finish.
        if ((*reason == RolloverReason::kTimeBackward || *reason == RolloverReason::kTimeForward)) {
            if (Bucket* alternate = _useAlternateBucket(&stripe, stripeLock, info)) {
                insertionResult = _insertIntoBucket(
                    opCtx, &stripe, stripeLock, stripeNumber, doc, combine, mode, &info, alternate);
                if (auto* batch = stdx::get_if<std::shared_ptr<WriteBatch>>(&insertionResult)) {
                    result.batch = *batch;
                    return std::move(result);
                }

                // We weren't able to insert into the other bucket, so fall through to the regular
                // reopening procedure.
            }
        }

        bool allowQueryBasedReopening = (*reason == RolloverReason::kTimeBackward);
        result.candidate =
            _getReopeningCandidate(&stripe, stripeLock, info, allowQueryBasedReopening);
    } else {
        result.batch = *stdx::get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
    }
    return std::move(result);
}

stdx::variant<std::shared_ptr<WriteBatch>, RolloverReason> BucketCatalog::_insertIntoBucket(
    OperationContext* opCtx,
    Stripe* stripe,
    WithLock stripeLock,
    StripeNumber stripeNumber,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine,
    AllowBucketCreation mode,
    CreationInfo* info,
    Bucket* bucket) {
    Bucket::NewFieldNames newFieldNamesToBeInserted;
    int32_t sizeToBeAdded = 0;
    const auto previousMemoryUsage = bucket->memoryUsage;

    bool isNewlyOpenedBucket = (bucket->size == 0);
    if (!isNewlyOpenedBucket) {
        auto [action, reason] = _determineRolloverAction(
            opCtx, doc, info, bucket, newFieldNamesToBeInserted, sizeToBeAdded, mode);
        if ((action == RolloverAction::kSoftClose || action == RolloverAction::kArchive) &&
            mode == AllowBucketCreation::kNo) {
            // We don't actually want to roll this bucket over yet, bail out.
            return reason;
        } else if (action != RolloverAction::kNone) {
            info->openedDuetoMetadata = false;
            bucket = _rollover(stripe, stripeLock, bucket, *info, action);
            isNewlyOpenedBucket = true;
        }
    }
    if (isNewlyOpenedBucket) {
        calculateBucketFieldsAndSizeChange(
            *bucket, doc, info->options.getMetaField(), newFieldNamesToBeInserted, sizeToBeAdded);
    }

    auto batch = activeBatch(*bucket, getOpId(opCtx, combine), stripeNumber, info->stats);
    batch->measurements.push_back(doc);
    for (auto&& field : newFieldNamesToBeInserted) {
        batch->newFieldNamesToBeInserted[field] = field.hash();
        bucket->uncommittedFieldNames.emplace(field);
    }

    bucket->numMeasurements++;
    bucket->size += sizeToBeAdded;
    if (isNewlyOpenedBucket) {
        // The namespace is stored two times: the bucket itself and openBuckets.
        // We don't have a great approximation for the
        // _schema size, so we use initial document size minus metadata as an approximation. Since
        // the metadata itself is stored once, in the bucket, we can combine the two and just use
        // the initial document size. A unique pointer to the bucket is stored once: allBuckets. A
        // raw pointer to the bucket is stored at most twice: openBuckets, idleBuckets.
        bucket->memoryUsage += (info->key.ns.size() * 2) + doc.objsize() + sizeof(Bucket) +
            sizeof(std::unique_ptr<Bucket>) + (sizeof(Bucket*) * 2);

        auto updateStatus = bucket->schema.update(
            doc, info->options.getMetaField(), info->key.metadata.getComparator());
        invariant(updateStatus == Schema::UpdateStatus::Updated);
    } else {
        _memoryUsage.fetchAndSubtract(previousMemoryUsage);
    }
    _memoryUsage.fetchAndAdd(bucket->memoryUsage);

    return batch;
}

void BucketCatalog::_waitToCommitBatch(Stripe* stripe, const std::shared_ptr<WriteBatch>& batch) {
    while (true) {
        std::shared_ptr<WriteBatch> current;

        {
            stdx::lock_guard stripeLock{stripe->mutex};
            Bucket* bucket = _useBucket(
                stripe, stripeLock, batch->bucketHandle.bucketId, IgnoreBucketState::kNo);
            if (!bucket || isWriteBatchFinished(*batch)) {
                return;
            }

            current = bucket->preparedBatch;
            if (!current) {
                // No other batches for this bucket are currently committing, so we can proceed.
                bucket->preparedBatch = batch;
                bucket->batches.erase(batch->opId);
                return;
            }
        }

        // We only hit this failpoint when there are conflicting prepared batches on the same
        // bucket.
        hangWaitingForConflictingPreparedBatch.pauseWhileSet();

        // We have to wait for someone else to finish.
        getWriteBatchResult(*current).getStatus().ignore();  // We don't care about the result.
    }
}

void BucketCatalog::_removeBucket(Stripe* stripe,
                                  WithLock stripeLock,
                                  Bucket* bucket,
                                  RemovalMode mode) {
    invariant(bucket->batches.empty());
    invariant(!bucket->preparedBatch);

    auto allIt = stripe->allBuckets.find(bucket->bucketId);
    invariant(allIt != stripe->allBuckets.end());

    _memoryUsage.fetchAndSubtract(bucket->memoryUsage);
    _markBucketNotIdle(stripe, stripeLock, bucket);

    // If the bucket was rolled over, then there may be a different open bucket for this metadata.
    auto openIt = stripe->openBuckets.find({bucket->bucketId.ns, bucket->key.metadata});
    if (openIt != stripe->openBuckets.end()) {
        auto& openSet = openIt->second;
        auto bucketIt = openSet.find(bucket);
        if (bucketIt != openSet.end()) {
            if (openSet.size() == 1) {
                stripe->openBuckets.erase(openIt);
            } else {
                openSet.erase(bucketIt);
            }
        }
    }

    // If we are cleaning up while archiving a bucket, then we want to preserve its state. Otherwise
    // we can remove the state from the catalog altogether.
    switch (mode) {
        case RemovalMode::kClose: {
            auto state = _bucketStateManager.getBucketState(bucket->bucketId);
            invariant(state.has_value());
            invariant(state.value().isSet(BucketStateFlag::kPendingCompression));
            break;
        }
        case RemovalMode::kAbort:
            _bucketStateManager.changeBucketState(
                bucket->bucketId,
                [](boost::optional<BucketState> input,
                   std::uint64_t) -> boost::optional<BucketState> {
                    invariant(input.has_value());
                    if (input->conflictsWithReopening()) {
                        return input.value().setFlag(BucketStateFlag::kUntracked);
                    }
                    return boost::none;
                });
            break;
        case RemovalMode::kArchive:
            // No state change
            break;
    }

    _numberOfActiveBuckets.fetchAndSubtract(1);
    stripe->allBuckets.erase(allIt);
}

void BucketCatalog::_archiveBucket(Stripe* stripe,
                                   WithLock stripeLock,
                                   Bucket* bucket,
                                   ClosedBuckets* closedBuckets) {
    bool archived = false;
    auto& archivedSet = stripe->archivedBuckets[bucket->key.hash];
    auto it = archivedSet.find(bucket->minTime);
    if (it == archivedSet.end()) {
        auto [it, inserted] = archivedSet.emplace(
            bucket->minTime, ArchivedBucket{bucket->bucketId, bucket->timeField});

        long long memory =
            _marginalMemoryUsageForArchivedBucket(it->second, archivedSet.size() == 1);
        _memoryUsage.fetchAndAdd(memory);
        archived = true;
    }

    RemovalMode mode = RemovalMode::kArchive;
    if (archived) {
        // If we have an archived bucket, we still want to account for it in numberOfActiveBuckets
        // so we will increase it here since removeBucket decrements the count.
        _numberOfActiveBuckets.fetchAndAdd(1);
    } else {
        // We had a meta hash collision, and already have a bucket archived with the same meta hash
        // and timestamp as this bucket. Since it's somewhat arbitrary which bucket we keep, we'll
        // keep the one that's already archived and just plain close this one.
        mode = RemovalMode::kClose;
        constexpr bool eligibleForReopening = true;
        closedBuckets->emplace_back(ClosedBucket{&_bucketStateManager,
                                                 bucket->bucketId,
                                                 bucket->timeField,
                                                 bucket->numMeasurements,
                                                 eligibleForReopening});
    }

    _removeBucket(stripe, stripeLock, bucket, mode);
}

boost::optional<OID> BucketCatalog::_findArchivedCandidate(Stripe* stripe,
                                                           WithLock stripeLock,
                                                           const CreationInfo& info) {
    auto setIt = stripe->archivedBuckets.find(info.key.hash);
    if (setIt == stripe->archivedBuckets.end()) {
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
        auto state = _bucketStateManager.getBucketState(candidateBucket.bucketId);
        if (state && !state.value().conflictsWithReopening()) {
            return candidateBucket.bucketId.oid;
        } else {
            if (state) {
                _bucketStateManager.changeBucketState(
                    candidateBucket.bucketId,
                    [](boost::optional<BucketState> input,
                       std::uint64_t) -> boost::optional<BucketState> {
                        if (!input.has_value()) {
                            return boost::none;
                        }
                        invariant(input.value().conflictsWithReopening());
                        return input.value().setFlag(BucketStateFlag::kUntracked);
                    });
            }
            long long memory =
                _marginalMemoryUsageForArchivedBucket(candidateBucket, archivedSet.size() == 1);
            if (archivedSet.size() == 1) {
                stripe->archivedBuckets.erase(setIt);
            } else {
                archivedSet.erase(it);
            }
            _memoryUsage.fetchAndSubtract(memory);
            _numberOfActiveBuckets.fetchAndSubtract(1);
        }
    }

    return boost::none;
}

stdx::variant<std::monostate, OID, BSONObj> BucketCatalog::_getReopeningCandidate(
    Stripe* stripe, WithLock stripeLock, const CreationInfo& info, bool allowQueryBasedReopening) {
    if (auto archived = _findArchivedCandidate(stripe, stripeLock, info)) {
        return archived.value();
    }

    if (!allowQueryBasedReopening) {
        return {};
    }

    boost::optional<BSONElement> metaElement;
    if (info.options.getMetaField().has_value()) {
        metaElement = info.key.metadata.element();
    }

    auto controlMinTimePath = kControlMinFieldNamePrefix.toString() + info.options.getTimeField();

    return generateReopeningFilters(
        info.time, metaElement, controlMinTimePath, *info.options.getBucketMaxSpanSeconds());
}

void BucketCatalog::_abort(Stripe* stripe,
                           WithLock stripeLock,
                           std::shared_ptr<WriteBatch> batch,
                           const Status& status) {
    // Before we access the bucket, make sure it's still there.
    Bucket* bucket =
        _useBucket(stripe, stripeLock, batch->bucketHandle.bucketId, IgnoreBucketState::kYes);
    if (!bucket) {
        // Special case, bucket has already been cleared, and we need only abort this batch.
        abortWriteBatch(*batch, status);
        return;
    }

    // Proceed to abort any unprepared batches and remove the bucket if possible
    _abort(stripe, stripeLock, bucket, batch, status);
}

void BucketCatalog::_abort(Stripe* stripe,
                           WithLock stripeLock,
                           Bucket* bucket,
                           std::shared_ptr<WriteBatch> batch,
                           const Status& status) {
    // Abort any unprepared batches. This should be safe since we have a lock on the stripe,
    // preventing anyone else from using these.
    for (const auto& [_, current] : bucket->batches) {
        abortWriteBatch(*current, status);
    }
    bucket->batches.clear();

    bool doRemove = true;  // We shouldn't remove the bucket if there's a prepared batch outstanding
                           // and it's not the one we manage. In that case, we don't know what the
                           // user is doing with it, but we need to keep the bucket around until
                           // that batch is finished.
    if (auto& prepared = bucket->preparedBatch) {
        if (batch && prepared == batch) {
            // We own the prepared batch, so we can go ahead and abort it and remove the bucket.
            abortWriteBatch(*prepared, status);
            prepared.reset();
        } else {
            doRemove = false;
        }
    }

    if (doRemove) {
        _removeBucket(stripe, stripeLock, bucket, RemovalMode::kAbort);
    } else {
        _bucketStateManager.changeBucketState(
            bucket->bucketId,
            [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
                invariant(input.has_value());
                return input.value().setFlag(BucketStateFlag::kCleared);
            });
    }
}

void BucketCatalog::_compressionDone(const BucketId& bucketId) {
    _bucketStateManager.changeBucketState(
        bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            return boost::none;
        });
}

void BucketCatalog::_markBucketIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket) {
    invariant(bucket);
    invariant(!bucket->idleListEntry.has_value());
    invariant(allCommitted(*bucket));
    stripe->idleBuckets.push_front(bucket);
    bucket->idleListEntry = stripe->idleBuckets.begin();
}

void BucketCatalog::_markBucketNotIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket) {
    invariant(bucket);
    if (bucket->idleListEntry.has_value()) {
        stripe->idleBuckets.erase(bucket->idleListEntry.value());
        bucket->idleListEntry = boost::none;
    }
}

void BucketCatalog::_expireIdleBuckets(Stripe* stripe,
                                       WithLock stripeLock,
                                       ExecutionStatsController& stats,
                                       ClosedBuckets* closedBuckets) {
    // As long as we still need space and have entries and remaining attempts, close idle buckets.
    int32_t numExpired = 0;

    const bool canArchive = feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
        serverGlobalParams.featureCompatibility);
    constexpr bool eligibleForReopening{true};

    while (!stripe->idleBuckets.empty() &&
           _memoryUsage.load() > getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {
        Bucket* bucket = stripe->idleBuckets.back();

        auto state = _bucketStateManager.getBucketState(bucket);
        if (canArchive && state && !state.value().conflictsWithInsertion()) {
            // Can archive a bucket if it's still eligible for insertions.
            _archiveBucket(stripe, stripeLock, bucket, closedBuckets);
            stats.incNumBucketsArchivedDueToMemoryThreshold();
        } else if (state && state.value().isSet(BucketStateFlag::kCleared)) {
            // Bucket was cleared and just needs to be removed from catalog.
            _removeBucket(stripe, stripeLock, bucket, RemovalMode::kAbort);
        } else {
            closedBuckets->emplace_back(ClosedBucket{&_bucketStateManager,
                                                     bucket->bucketId,
                                                     bucket->timeField,
                                                     bucket->numMeasurements,
                                                     eligibleForReopening});
            _removeBucket(stripe, stripeLock, bucket, RemovalMode::kClose);
            stats.incNumBucketsClosedDueToMemoryThreshold();
        }

        ++numExpired;
    }

    while (canArchive && !stripe->archivedBuckets.empty() &&
           _memoryUsage.load() > getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {

        auto& [hash, archivedSet] = *stripe->archivedBuckets.begin();
        invariant(!archivedSet.empty());

        auto& [timestamp, bucket] = *archivedSet.begin();
        closedBuckets->emplace_back(ClosedBucket{&_bucketStateManager,
                                                 bucket.bucketId,
                                                 bucket.timeField,
                                                 boost::none,
                                                 eligibleForReopening});

        long long memory = _marginalMemoryUsageForArchivedBucket(bucket, archivedSet.size() == 1);
        if (archivedSet.size() == 1) {
            // If this is the only entry, erase the whole map so we don't leave it empty.
            stripe->archivedBuckets.erase(stripe->archivedBuckets.begin());
        } else {
            // Otherwise just erase this bucket from the map.
            archivedSet.erase(archivedSet.begin());
        }
        _memoryUsage.fetchAndSubtract(memory);
        _numberOfActiveBuckets.fetchAndSubtract(1);

        stats.incNumBucketsClosedDueToMemoryThreshold();
        ++numExpired;
    }
}

Bucket* BucketCatalog::_allocateBucket(Stripe* stripe,
                                       WithLock stripeLock,
                                       const CreationInfo& info) {
    _expireIdleBuckets(stripe, stripeLock, info.stats, info.closedBuckets);

    auto [oid, roundedTime] = generateBucketOID(info.time, info.options);
    auto bucketId = BucketId{info.key.ns, oid};

    auto [it, inserted] = stripe->allBuckets.try_emplace(
        bucketId,
        std::make_unique<Bucket>(
            bucketId, info.key, info.options.getTimeField(), roundedTime, _bucketStateManager));
    tassert(6130900, "Expected bucket to be inserted", inserted);
    Bucket* bucket = it->second.get();
    stripe->openBuckets[info.key].emplace(bucket);

    auto state = _bucketStateManager.changeBucketState(
        bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            invariant(!input.has_value());
            return BucketState{};
        });
    invariant(state == BucketState{});
    _numberOfActiveBuckets.fetchAndAdd(1);

    if (info.openedDuetoMetadata) {
        info.stats.incNumBucketsOpenedDueToMetadata();
    }

    // Make sure we set the control.min time field to match the rounded _id timestamp.
    auto controlDoc = buildControlMinTimestampDoc(info.options.getTimeField(), roundedTime);
    bucket->minmax.update(
        controlDoc, bucket->key.metadata.getMetaField(), bucket->key.metadata.getComparator());
    return bucket;
}

std::pair<RolloverAction, RolloverReason> BucketCatalog::_determineRolloverAction(
    OperationContext* opCtx,
    const BSONObj& doc,
    CreationInfo* info,
    Bucket* bucket,
    Bucket::NewFieldNames& newFieldNamesToBeInserted,
    int32_t& sizeToBeAdded,
    AllowBucketCreation mode) {
    // If the mode is enabled to create new buckets, then we should update stats for soft closures
    // accordingly. If we specify the mode to not allow bucket creation, it means we are not sure if
    // we want to soft close the bucket yet and should wait to update closure stats.
    const bool shouldUpdateStats = (mode == AllowBucketCreation::kYes);

    auto bucketTime = bucket->minTime;
    if (info->time - bucketTime >= Seconds(*info->options.getBucketMaxSpanSeconds())) {
        if (shouldUpdateStats) {
            info->stats.incNumBucketsClosedDueToTimeForward();
        }
        return {RolloverAction::kSoftClose, RolloverReason::kTimeForward};
    }
    if (info->time < bucketTime) {
        const bool canArchive = feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility);
        if (shouldUpdateStats) {
            if (canArchive) {
                info->stats.incNumBucketsArchivedDueToTimeBackward();
            } else {
                info->stats.incNumBucketsClosedDueToTimeBackward();
            }
        }
        return {canArchive ? RolloverAction::kArchive : RolloverAction::kSoftClose,
                RolloverReason::kTimeBackward};
    }
    if (bucket->numMeasurements == static_cast<std::uint64_t>(gTimeseriesBucketMaxCount)) {
        info->stats.incNumBucketsClosedDueToCount();
        return {RolloverAction::kHardClose, RolloverReason::kCount};
    }

    // In scenarios where we have a high cardinality workload and face increased cache pressure we
    // will decrease the size of buckets before we close them.
    int32_t cacheDerivedBucketMaxSize = getCacheDerivedBucketMaxSize(
        opCtx->getServiceContext()->getStorageEngine(), _numberOfActiveBuckets.load());
    int32_t effectiveMaxSize = std::min(gTimeseriesBucketMaxSize, cacheDerivedBucketMaxSize);

    // Before we hit our bucket minimum count, we will allow for large measurements to be inserted
    // into buckets. Instead of packing the bucket to the BSON size limit, 16MB, we'll limit the max
    // bucket size to 12MB. This is to leave some space in the bucket if we need to add new internal
    // fields to existing, full buckets.
    static constexpr int32_t largeMeasurementsMaxBucketSize =
        BSONObjMaxUserSize - (4 * 1024 * 1024);
    // We restrict the ceiling of the bucket max size under cache pressure.
    int32_t absoluteMaxSize = std::min(largeMeasurementsMaxBucketSize, cacheDerivedBucketMaxSize);

    calculateBucketFieldsAndSizeChange(
        *bucket, doc, info->options.getMetaField(), newFieldNamesToBeInserted, sizeToBeAdded);
    if (bucket->size + sizeToBeAdded > effectiveMaxSize) {
        bool keepBucketOpenForLargeMeasurements =
            bucket->numMeasurements < static_cast<std::uint64_t>(gTimeseriesBucketMinCount) &&
            feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                serverGlobalParams.featureCompatibility);
        if (keepBucketOpenForLargeMeasurements) {
            if (bucket->size + sizeToBeAdded > absoluteMaxSize) {
                if (absoluteMaxSize != largeMeasurementsMaxBucketSize) {
                    info->stats.incNumBucketsClosedDueToCachePressure();
                    return {RolloverAction::kHardClose, RolloverReason::kCachePressure};
                }
                info->stats.incNumBucketsClosedDueToSize();
                return {RolloverAction::kHardClose, RolloverReason::kSize};
            }

            // There's enough space to add this measurement and we're still below the large
            // measurement threshold.
            if (!bucket->keptOpenDueToLargeMeasurements) {
                // Only increment this metric once per bucket.
                bucket->keptOpenDueToLargeMeasurements = true;
                info->stats.incNumBucketsKeptOpenDueToLargeMeasurements();
            }
            return {RolloverAction::kNone, RolloverReason::kNone};
        } else {
            if (effectiveMaxSize == gTimeseriesBucketMaxSize) {
                info->stats.incNumBucketsClosedDueToSize();
                return {RolloverAction::kHardClose, RolloverReason::kSize};
            }
            info->stats.incNumBucketsClosedDueToCachePressure();
            return {RolloverAction::kHardClose, RolloverReason::kCachePressure};
        }
    }

    if (schemaIncompatible(
            *bucket, doc, info->options.getMetaField(), info->key.metadata.getComparator())) {
        info->stats.incNumBucketsClosedDueToSchemaChange();
        return {RolloverAction::kHardClose, RolloverReason::kSchemaChange};
    }

    return {RolloverAction::kNone, RolloverReason::kNone};
}

Bucket* BucketCatalog::_rollover(Stripe* stripe,
                                 WithLock stripeLock,
                                 Bucket* bucket,
                                 const CreationInfo& info,
                                 RolloverAction action) {
    invariant(action != RolloverAction::kNone);
    if (allCommitted(*bucket)) {
        // The bucket does not contain any measurements that are yet to be committed, so we can take
        // action now.
        if (action == RolloverAction::kArchive) {
            _archiveBucket(stripe, stripeLock, bucket, info.closedBuckets);
        } else {
            const bool eligibleForReopening = action == RolloverAction::kSoftClose;
            info.closedBuckets->emplace_back(ClosedBucket{&_bucketStateManager,
                                                          bucket->bucketId,
                                                          bucket->timeField,
                                                          bucket->numMeasurements,
                                                          eligibleForReopening});

            _removeBucket(stripe, stripeLock, bucket, RemovalMode::kClose);
        }
    } else {
        // We must keep the bucket around until all measurements are committed committed, just mark
        // the action we chose now so it we know what to do when the last batch finishes.
        bucket->rolloverAction = action;
    }

    return _allocateBucket(stripe, stripeLock, info);
}

ExecutionStatsController BucketCatalog::_getExecutionStats(const NamespaceString& ns) {
    stdx::lock_guard catalogLock{_mutex};
    auto it = _executionStats.find(ns);
    if (it != _executionStats.end()) {
        return {it->second, _globalExecutionStats};
    }

    auto res = _executionStats.emplace(ns, std::make_shared<ExecutionStats>());
    return {res.first->second, _globalExecutionStats};
}

std::shared_ptr<ExecutionStats> BucketCatalog::_getExecutionStats(const NamespaceString& ns) const {
    static const auto kEmptyStats{std::make_shared<ExecutionStats>()};

    stdx::lock_guard catalogLock{_mutex};

    auto it = _executionStats.find(ns);
    if (it != _executionStats.end()) {
        return it->second;
    }
    return kEmptyStats;
}

long long BucketCatalog::_marginalMemoryUsageForArchivedBucket(const ArchivedBucket& bucket,
                                                               bool onlyEntryForMatchingMetaHash) {
    return sizeof(Date_t) +        // key in set of archived buckets for meta hash
        sizeof(ArchivedBucket) +   // main data for archived bucket
        bucket.timeField.size() +  // allocated space for timeField string, ignoring SSO
        (onlyEntryForMatchingMetaHash ? sizeof(std::size_t) +           // key in set (meta hash)
                 sizeof(decltype(Stripe::archivedBuckets)::value_type)  // set container
                                      : 0);
}

void BucketCatalog::_updateBucketFetchAndQueryStats(ExecutionStatsController& stats,
                                                    const BucketFindResult& findResult) {
    if (findResult.fetchedBucket) {
        if (findResult.bucketToReopen.has_value()) {
            stats.incNumBucketsFetched();
        } else {
            stats.incNumBucketFetchesFailed();
        }
    }

    if (findResult.queriedBucket) {
        if (findResult.bucketToReopen.has_value()) {
            stats.incNumBucketsQueried();
        } else {
            stats.incNumBucketQueriesFailed();
        }
    }
}

class BucketCatalog::ServerStatus : public ServerStatusSection {
    struct BucketCounts {
        BucketCounts& operator+=(const BucketCounts& other) {
            if (&other != this) {
                all += other.all;
                open += other.open;
                idle += other.idle;
            }
            return *this;
        }

        std::size_t all = 0;
        std::size_t open = 0;
        std::size_t idle = 0;
    };

    BucketCounts _getBucketCounts(const BucketCatalog& catalog) const {
        BucketCounts sum;
        for (auto const& stripe : catalog._stripes) {
            stdx::lock_guard stripeLock{stripe.mutex};
            sum += {stripe.allBuckets.size(), stripe.openBuckets.size(), stripe.idleBuckets.size()};
        }
        return sum;
    }

public:
    ServerStatus() : ServerStatusSection("bucketCatalog") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        const auto& bucketCatalog = BucketCatalog::get(opCtx);
        {
            stdx::lock_guard catalogLock{bucketCatalog._mutex};
            if (bucketCatalog._executionStats.empty()) {
                return {};
            }
        }

        auto counts = _getBucketCounts(bucketCatalog);
        auto numActive = bucketCatalog._numberOfActiveBuckets.load();
        BSONObjBuilder builder;
        builder.appendNumber("numBuckets", static_cast<long long>(numActive));
        builder.appendNumber("numOpenBuckets", static_cast<long long>(counts.open));
        builder.appendNumber("numIdleBuckets", static_cast<long long>(counts.idle));
        builder.appendNumber("numArchivedBuckets", static_cast<long long>(numActive - counts.open));
        builder.appendNumber("memoryUsage",
                             static_cast<long long>(bucketCatalog._memoryUsage.load()));

        // Append the global execution stats for all namespaces.
        bucketCatalog.appendGlobalExecutionStats(&builder);

        // Append the global state management stats for all namespaces.
        bucketCatalog.appendStateManagementStats(&builder);

        return builder.obj();
    }
} bucketCatalogServerStatus;
}  // namespace mongo::timeseries::bucket_catalog
