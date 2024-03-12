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

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/timeseries/bucket_catalog/bucket.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include "mongo/db/timeseries/bucket_catalog/closed_bucket.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/time_support.h"

namespace mongo::timeseries::bucket_catalog::internal {

/**
 * Bundle of information that 'insert' needs to pass down to helper methods that may create a
 * new bucket.
 */
struct CreationInfo {
    const BucketKey& key;
    StripeNumber stripe;
    const Date_t& time;
    const TimeseriesOptions& options;
    ExecutionStatsController& stats;
    ClosedBuckets* closedBuckets;
    bool openedDuetoMetadata = true;
};

/**
 * Mode enum to control whether bucket retrieval methods will create new buckets if no suitable
 * bucket exists.
 */
enum class AllowBucketCreation { kYes, kNo };

/**
 * Mode to signal to 'removeBucket' what's happening to the bucket, and how to handle the bucket
 * state change.
 */
enum class RemovalMode {
    kClose,    // Normal closure, pending compression
    kArchive,  // Archive bucket, no state change
    kAbort,    // Bucket is being cleared, possibly due to error, erase state
};

/**
 * Mode enum to control whether the bucket retrieval methods will return buckets that have a state
 * that conflicts with insertion.
 */
enum class IgnoreBucketState { kYes, kNo };

/**
 * Mode enum to control whether we prepare or unprepare a bucket.
 */
enum class BucketPrepareAction { kPrepare, kUnprepare };

/**
 * Mode enum to control whether getReopeningCandidate() will allow query-based
 * reopening of buckets when attempting to accommodate a new measurement.
 */
enum class AllowQueryBasedReopening { kAllow, kDisallow };

/**
 * Maps bucket key to the stripe that is responsible for it.
 */
StripeNumber getStripeNumber(const BucketKey& key, size_t numberOfStripes);

/**
 * Extracts the information from the input 'doc' that is used to map the document to a bucket.
 */
StatusWith<std::pair<BucketKey, Date_t>> extractBucketingParameters(
    const UUID& collectionUUID,
    const StringDataComparator* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc);

/**
 * Retrieve a bucket for read-only use.
 */
const Bucket* findBucket(BucketStateRegistry& registry,
                         const Stripe& stripe,
                         WithLock stripeLock,
                         const BucketId& bucketId,
                         IgnoreBucketState mode = IgnoreBucketState::kNo);

/**
 * Retrieve a bucket for write use.
 */
Bucket* useBucket(BucketStateRegistry& registry,
                  Stripe& stripe,
                  WithLock stripeLock,
                  const BucketId& bucketId,
                  IgnoreBucketState mode);

/**
 * Retrieve a bucket for write use and prepare/unprepare the 'BucketState'.
 */
Bucket* useBucketAndChangePreparedState(BucketStateRegistry& registry,
                                        Stripe& stripe,
                                        WithLock stripeLock,
                                        const BucketId& bucketId,
                                        BucketPrepareAction prepare);

/**
 * Retrieve the open bucket for write use if one exists. If none exists and 'mode' is set to kYes,
 * then we will create a new bucket.
 */
Bucket* useBucket(OperationContext* opCtx,
                  BucketCatalog& catalog,
                  Stripe& stripe,
                  WithLock stripeLock,
                  const NamespaceString& nss,
                  const CreationInfo& info,
                  AllowBucketCreation mode);

/**
 * Retrieve a previously closed bucket for write use if one exists in the catalog. Considers buckets
 * that are pending closure or archival but which are still eligible to recieve new measurements.
 */
Bucket* useAlternateBucket(BucketCatalog& catalog,
                           Stripe& stripe,
                           WithLock stripeLock,
                           const NamespaceString& nss,
                           const CreationInfo& info);

/**
 * Given a bucket to reopen, performs validation and constructs the in-memory representation of the
 * bucket. If specified, 'expectedKey' is matched against the key extracted from the document to
 * validate that the bucket is expected (i.e. to help resolve hash collisions for archived buckets).
 * Does *not* hand ownership of the bucket to the catalog.
 */
StatusWith<unique_tracked_ptr<Bucket>> rehydrateBucket(OperationContext* opCtx,
                                                       BucketCatalog& catalog,
                                                       ExecutionStatsController& stats,
                                                       const UUID& collectionUUID,
                                                       const StringDataComparator* comparator,
                                                       const TimeseriesOptions& options,
                                                       const BucketToReopen& bucketToReopen,
                                                       uint64_t catalogEra,
                                                       const BucketKey* expectedKey);

/**
 * Given a rehydrated 'bucket', passes ownership of that bucket to the catalog, marking the bucket
 * as open.
 */
StatusWith<std::reference_wrapper<Bucket>> reopenBucket(OperationContext* opCtx,
                                                        BucketCatalog& catalog,
                                                        Stripe& stripe,
                                                        WithLock stripeLock,
                                                        ExecutionStatsController& stats,
                                                        const BucketKey& key,
                                                        unique_tracked_ptr<Bucket>&& bucket,
                                                        std::uint64_t targetEra,
                                                        ClosedBuckets& closedBuckets);

/**
 * Check to see if 'insert' can use existing bucket rather than reopening a candidate bucket. If
 * true, chances are the caller raced with another thread to reopen the same bucket, but if false,
 * there might be another bucket that had been cleared, or that has the same _id in a different
 * namespace.
 */
StatusWith<std::reference_wrapper<Bucket>> reuseExistingBucket(BucketCatalog& catalog,
                                                               Stripe& stripe,
                                                               WithLock stripeLock,
                                                               const NamespaceString& nss,
                                                               ExecutionStatsController& stats,
                                                               const BucketKey& key,
                                                               Bucket& existingBucket,
                                                               std::uint64_t targetEra);

/**
 * Given an already-selected 'bucket', inserts 'doc' to the bucket if possible. If not, and 'mode'
 * is set to 'kYes', we will create a new bucket and insert into that bucket.
 */
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
    Bucket& existingBucket);

/**
 * Wait for other batches to finish so we can prepare 'batch'
 */
void waitToCommitBatch(BucketStateRegistry& registry,
                       Stripe& stripe,
                       const std::shared_ptr<WriteBatch>& batch);

/**
 * Removes the given bucket from the bucket catalog's internal data structures.
 */
void removeBucket(
    BucketCatalog& catalog, Stripe& stripe, WithLock stripeLock, Bucket& bucket, RemovalMode mode);

/**
 * Archives the given bucket, minimizing the memory footprint but retaining the necessary
 * information required to efficiently identify it as a candidate for future insertions.
 */
void archiveBucket(OperationContext* opCtx,
                   BucketCatalog& catalog,
                   Stripe& stripe,
                   WithLock stripeLock,
                   Bucket& bucket,
                   ClosedBuckets& closedBuckets);

/**
 * Identifies a previously archived bucket that may be able to accommodate the measurement
 * represented by 'info', if one exists.
 */
boost::optional<OID> findArchivedCandidate(BucketCatalog& catalog,
                                           Stripe& stripe,
                                           WithLock stripeLock,
                                           const CreationInfo& info);

/**
 * Calculates the bucket max size constrained by the cache size and the cardinality of active
 * buckets. Returns a pair of the effective value that respects the absolute bucket max and min
 * sizes and the raw value.
 */
std::pair<int32_t, int32_t> getCacheDerivedBucketMaxSize(uint64_t storageCacheSize,
                                                         uint32_t workloadCardinality);

/**
 * Identifies a previously archived bucket that may be able to accommodate the measurement
 * represented by 'info', if one exists. Otherwise returns a pipeline to use for query-based
 * reopening if allowed.
 */
InsertResult getReopeningContext(OperationContext* opCtx,
                                 BucketCatalog& catalog,
                                 Stripe& stripe,
                                 WithLock stripeLock,
                                 const CreationInfo& info,
                                 uint64_t catalogEra,
                                 AllowQueryBasedReopening allowQueryBasedReopening);

/**
 * Aborts 'batch', and if the corresponding bucket still exists, proceeds to abort any other
 * unprepared batches and remove the bucket from the catalog if there is no unprepared batch.
 */
void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           std::shared_ptr<WriteBatch> batch,
           const Status& status);

/**
 * Aborts any unprepared batches for the given bucket, then removes the bucket if there is no
 * prepared batch. If 'batch' is non-null, it is assumed that the caller has commit rights for that
 * batch.
 */
void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           Bucket& bucket,
           std::shared_ptr<WriteBatch> batch,
           const Status& status);

/**
 * Adds the bucket to a list of idle buckets to be expired at a later date.
 */
void markBucketIdle(Stripe& stripe, WithLock stripeLock, Bucket& bucket);

/**
 * Remove the bucket from the list of idle buckets. The second parameter encodes whether the caller
 * holds a lock on _idleMutex.
 */
void markBucketNotIdle(Stripe& stripe, WithLock stripeLock, Bucket& bucket);

/**
 * Expires idle buckets until the bucket catalog's memory usage is below the expiry threshold.
 */
void expireIdleBuckets(OperationContext* opCtx,
                       BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       ExecutionStatsController& stats,
                       ClosedBuckets& closedBuckets);

/**
 * Generates an OID for the bucket _id field, setting the timestamp portion to a value determined by
 * rounding 'time' based on 'options'.
 */
std::pair<OID, Date_t> generateBucketOID(const Date_t& time, const TimeseriesOptions& options);

/**
 * Resets the counter used for bucket OID generation. Should be called after a collision.
 */
void resetBucketOIDCounter();

/**
 * Allocates a new bucket and adds it to the catalog.
 */
Bucket& allocateBucket(OperationContext* opCtx,
                       BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       const CreationInfo& info);

/**
 * Close the existing, full bucket and open a new one for the same metadata.
 *
 * Writes information about the closed bucket to the 'info' parameter.
 */
Bucket& rollover(OperationContext* opCtx,
                 BucketCatalog& catalog,
                 Stripe& stripe,
                 WithLock stripeLock,
                 Bucket& bucket,
                 const CreationInfo& info,
                 RolloverAction action);

/**
 * Determines if 'bucket' needs to be rolled over to accommodate 'doc'. If so, determines whether
 * to archive or close 'bucket'.
 */
std::pair<RolloverAction, RolloverReason> determineRolloverAction(
    OperationContext* opCtx,
    TrackingContext&,
    const BSONObj& doc,
    CreationInfo& info,
    Bucket& bucket,
    uint32_t numberOfActiveBuckets,
    Bucket::NewFieldNames& newFieldNamesToBeInserted,
    int32_t& sizeToBeAdded,
    bool& crossedLargeMeasurementThreshold,
    AllowBucketCreation mode);

/**
 * Retrieves or initializes the execution stats for the given namespace, for writing.
 */
ExecutionStatsController getOrInitializeExecutionStats(BucketCatalog& catalog,
                                                       const UUID& collectionUUID);

/**
 * Retrieves the execution stats for the given namespace, if they have already been initialized.
 */
shared_tracked_ptr<ExecutionStats> getExecutionStats(const BucketCatalog& catalog,
                                                     const UUID& collectionUUID);

/**
 * Retrieves the execution stats from the side bucket catalog.
 * Assumes the side bucket catalog has the stats of one collection.
 */
std::pair<UUID, shared_tracked_ptr<ExecutionStats>> getSideBucketCatalogCollectionStats(
    BucketCatalog& sideBucketCatalog);

/**
 * Merges the execution stats of a collection into the bucket catalog.
 */
void mergeExecutionStatsToBucketCatalog(BucketCatalog& catalog,
                                        shared_tracked_ptr<ExecutionStats> collStats,
                                        const UUID& collectionUUID);

/**
 * Generates a status with code TimeseriesBucketCleared and an appropriate error message.
 */
Status getTimeseriesBucketClearedError(const NamespaceString& nss, const OID& oid);

/**
 * Close an open bucket, setting the state appropriately and removing it from the catalog.
 */
void closeOpenBucket(OperationContext* opCtx,
                     BucketCatalog& catalog,
                     Stripe& stripe,
                     WithLock stripeLock,
                     Bucket& bucket,
                     ClosedBuckets& closedBuckets);
/**
 * Close an open bucket, setting the state appropriately and removing it from the catalog.
 */
void closeOpenBucket(OperationContext* opCtx,
                     BucketCatalog& catalog,
                     Stripe& stripe,
                     WithLock stripeLock,
                     Bucket& bucket,
                     boost::optional<ClosedBucket>& closedBucket);
/**
 * Close an archived bucket, setting the state appropriately and removing it from the catalog.
 */
void closeArchivedBucket(BucketCatalog& catalog,
                         ArchivedBucket& bucket,
                         ClosedBuckets& closedBuckets);

/**
 * Runs (slow) post commit debug checks to ensure we maintain expected invariants about the bucket
 * contents.
 *
 * Set of checks:
 *  - Measurement count on-disk matches in-memory state. (Helpful for detecting race conditions.)
 */
void runPostCommitDebugChecks(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const Bucket& bucket,
                              const WriteBatch& batch);

}  // namespace mongo::timeseries::bucket_catalog::internal
