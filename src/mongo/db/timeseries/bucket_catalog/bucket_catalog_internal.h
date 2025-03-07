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
#include "mongo/db/timeseries/bucket_catalog/bucket.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/time_support.h"

namespace mongo::timeseries::bucket_catalog::internal {

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
    kClose,    // Normal closure
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
 * Maps bucket identifier to the stripe that is responsible for it.
 */
StripeNumber getStripeNumber(const BucketCatalog& catalog, const BucketKey& key);
StripeNumber getStripeNumber(const BucketCatalog& catalog, const BucketId& bucketId);

/**
 * Extracts the information from the input 'doc' that is used to map the document to a bucket.
 */
StatusWith<std::pair<BucketKey, Date_t>> extractBucketingParameters(
    tracking::Context&,
    const UUID& collectionUUID,
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
Bucket* useBucket(BucketCatalog& catalog,
                  Stripe& stripe,
                  WithLock stripeLock,
                  InsertContext& info,
                  AllowBucketCreation mode,
                  const Date_t& time,
                  const StringDataComparator* comparator);

/**
 * Retrieve all open buckets from 'stripe' given a bucket key.
 */
std::vector<Bucket*> findOpenBuckets(Stripe& stripe,
                                     WithLock stripeLock,
                                     const BucketKey& bucketKey);

/**
 * Return true if a given bucket's state is eligible for new inserts. Otherwise, return false.
 * Clean up the bucket from the catalog if there is a conflicting state.
 */
bool isBucketStateEligibleForInsertsAndCleanup(BucketCatalog& catalog,
                                               Stripe& stripe,
                                               WithLock stripeLock,
                                               Bucket* bucket);

/**
 * Rollover 'bucket' according to 'action' if the bucket does not contain uncommitted measurements.
 * Mark the bucket's 'rolloverAction' otherwise.
 */
void rollover(BucketCatalog& catalog,
              Stripe& stripe,
              WithLock stripeLock,
              Bucket& bucket,
              RolloverAction action);

/**
 * Retrieve a previously closed bucket for write use if one exists in the catalog. Considers buckets
 * that are pending closure or archival but which are still eligible to receive new measurements.
 */
Bucket* useAlternateBucket(BucketCatalog& catalog,
                           Stripe& stripe,
                           WithLock stripeLock,
                           InsertContext& info,
                           const Date_t& time);

/**
 * Perform archived-based reopening and returns the fetched bucket document.
 * Increments statistics accordingly.
 */
BSONObj reopenFetchedBucket(OperationContext* opCtx,
                            const Collection* bucketsColl,
                            const OID& bucketId,
                            ExecutionStatsController& stats);

/**
 * Perform query-based reopening and returns the fetched bucket document if the supporting index
 * exists.
 * Increments statistics accordingly.
 */
BSONObj reopenQueriedBucket(OperationContext* opCtx,
                            const Collection* bucketsColl,
                            const TimeseriesOptions& options,
                            const std::vector<BSONObj>& pipeline,
                            ExecutionStatsController& stats);

/**
 * Compress and write the bucket document to storage with 'compressAndWriteBucketFunc'. Return the
 * error status and freeze the bucket if the compression fails.
 */
Status compressAndWriteBucket(OperationContext* opCtx,
                              BucketCatalog& catalog,
                              const Collection* bucketsColl,
                              const BucketId& uncompressedBucketId,
                              StringData timeField,
                              const CompressAndWriteBucketFunc& compressAndWriteBucketFunc);

/**
 * Given a compressed bucket to reopen, performs validation and constructs the in-memory
 * representation of the bucket. Does *not* hand ownership of the bucket to the catalog.
 */
StatusWith<tracking::unique_ptr<Bucket>> rehydrateBucket(BucketCatalog& catalog,
                                                         const BSONObj& bucketDoc,
                                                         const BucketKey& bucketKey,
                                                         const TimeseriesOptions& options,
                                                         uint64_t catalogEra,
                                                         const StringDataComparator* comparator,
                                                         const BucketDocumentValidator& validator,
                                                         ExecutionStatsController& stats);

/**
 * Given a rehydrated 'bucket', passes ownership of that bucket to the catalog, marking the bucket
 * as open.
 */
StatusWith<std::reference_wrapper<Bucket>> loadBucketIntoCatalog(
    BucketCatalog& catalog,
    Stripe& stripe,
    WithLock stripeLock,
    ExecutionStatsController& stats,
    const BucketKey& key,
    tracking::unique_ptr<Bucket>&& bucket,
    std::uint64_t targetEra);

/**
 * Given an already-selected 'bucket', inserts 'doc' to the bucket if possible. If not, and 'mode'
 * is set to 'kYes', we will create a new bucket and insert into that bucket. If `existingBucket`
 * was selected via `useAlternateBucket`, then the previous bucket returned by `useBucket` should be
 * passed in as `excludedBucket`.
 */
std::variant<std::shared_ptr<WriteBatch>, RolloverReason> insertIntoBucket(
    BucketCatalog& catalog,
    Stripe& stripe,
    WithLock stripeLock,
    const BSONObj& doc,
    OperationId opId,
    AllowBucketCreation mode,
    InsertContext& insertContext,
    Bucket& existingBucket,
    const Date_t& time,
    uint64_t storageCacheSizeBytes,
    const StringDataComparator* comparator,
    Bucket* excludedBucket = nullptr,
    boost::optional<RolloverAction> excludedAction = boost::none);

/**
 * Wait for other batches to finish so we can prepare 'batch'
 */
void waitToCommitBatch(BucketStateRegistry& registry,
                       Stripe& stripe,
                       const std::shared_ptr<WriteBatch>& batch);

/**
 * Removes the given bucket from the bucket catalog's internal data structures.
 */
void removeBucket(BucketCatalog& catalog,
                  Stripe& stripe,
                  WithLock stripeLock,
                  Bucket& bucket,
                  ExecutionStatsController& stats,
                  RemovalMode mode);

/**
 * Archives the given bucket, minimizing the memory footprint but retaining the necessary
 * information required to efficiently identify it as a candidate for future insertions.
 */
void archiveBucket(BucketCatalog& catalog,
                   Stripe& stripe,
                   WithLock stripeLock,
                   Bucket& bucket,
                   ExecutionStatsController& stats);

/**
 * Identifies a previously archived bucket that may be able to accommodate a measurement with
 * 'time', if one exists.
 */
boost::optional<OID> findArchivedCandidate(BucketCatalog& catalog,
                                           Stripe& stripe,
                                           WithLock stripeLock,
                                           const BucketKey& bucketKey,
                                           const TimeseriesOptions& options,
                                           const Date_t& time);

/**
 * Calculates the bucket max size constrained by the cache size and the cardinality of active
 * buckets. Returns a pair of the effective value that respects the absolute bucket max and min
 * sizes and the raw value.
 */
std::pair<int32_t, int32_t> getCacheDerivedBucketMaxSize(uint64_t storageCacheSizeBytes,
                                                         uint32_t workloadCardinality);

/**
 * Identifies a previously archived bucket that may be able to accommodate the measurement
 * represented by 'info', if one exists. Otherwise returns a pipeline to use for query-based
 * reopening if allowed.
 */
InsertResult getReopeningContext(BucketCatalog& catalog,
                                 Stripe& stripe,
                                 WithLock stripeLock,
                                 InsertContext& info,
                                 uint64_t catalogEra,
                                 AllowQueryBasedReopening allowQueryBasedReopening,
                                 const Date_t& time,
                                 uint64_t storageCacheSizeBytes);

/**
 * Returns an archived bucket eligible for new insert with 'time'.
 */
boost::optional<OID> getArchiveReopeningCandidate(BucketCatalog& catalog,
                                                  Stripe& stripe,
                                                  WithLock stripeLock,
                                                  const BucketKey& bucketKey,
                                                  const TimeseriesOptions& options,
                                                  const Date_t& time);

/**
 * Returns an aggregation pipeline to reopen a bucket with 'time' using 'bucketKey' and 'options'.
 */
std::vector<BSONObj> getQueryReopeningCandidate(BucketCatalog& catalog,
                                                Stripe& stripe,
                                                WithLock stripeLock,
                                                const BucketKey& bucketKey,
                                                const TimeseriesOptions& options,
                                                uint64_t storageCacheSizeBytes,
                                                const Date_t& time);

/**
 * Returns a conflicting operation that needs to be waited for archive-based reopening (when
 * 'archivedCandidate' is passed) or query-based reopening.
 */
boost::optional<InsertWaiter> checkForReopeningConflict(
    Stripe& stripe,
    WithLock stripeLock,
    const BucketKey& bucketKey,
    boost::optional<OID> archivedCandidate = boost::none);
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
 * prepared batch.
 */
void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           Bucket& bucket,
           ExecutionStatsController& stats,
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
void expireIdleBuckets(BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       const UUID& collectionUUID,
                       ExecutionStatsController& collectionStats);

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
Bucket& allocateBucket(BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       const BucketKey& key,
                       const TimeseriesOptions& timeseriesOptions,
                       const Date_t& time,
                       const StringDataComparator* comparator,
                       ExecutionStatsController& stats);

/**
 * Close the existing, full bucket and open a new one for the same metadata.
 *
 * Writes information about the closed bucket to the 'info' parameter. Optionally, if `bucket` was
 * selected via `useAlternateBucket`, pass the current open bucket as `additionalBucket` to mark for
 * archival and preserve the invariant of only one open bucket per key.
 */
Bucket& rollover(BucketCatalog& catalog,
                 Stripe& stripe,
                 WithLock stripeLock,
                 Bucket& bucket,
                 const BucketKey& key,
                 const TimeseriesOptions& timeseriesOptions,
                 RolloverAction action,
                 const Date_t& time,
                 const StringDataComparator* comparator,
                 Bucket* additionalBucket,
                 boost::optional<RolloverAction> additionalAction,
                 ExecutionStatsController& stats);

/**
 * Determines if 'bucket' needs to be rolled over to accommodate 'doc'. If so, determines whether
 * to archive or close 'bucket'.
 */
std::pair<RolloverAction, RolloverReason> determineRolloverAction(
    const BSONObj& doc,
    InsertContext& info,
    Bucket& bucket,
    uint32_t numberOfActiveBuckets,
    Bucket::NewFieldNames& newFieldNamesToBeInserted,
    const Sizes& sizesToBeAdded,
    AllowBucketCreation mode,
    const Date_t& time,
    uint64_t storageCacheSizeBytes,
    const StringDataComparator* comparator);

/**
 * Determines if and why 'bucket' needs to be rolled over to accommodate 'doc'.
 * Will also update the bucket catalog stats incNumBucketsKeptOpenDueToLargeMeasurements as
 * appropriate.
 */
RolloverReason determineRolloverReason(const BSONObj& doc,
                                       const TimeseriesOptions& timeseriesOptions,
                                       Bucket& bucket,
                                       uint32_t numberOfActiveBuckets,
                                       const Sizes& sizesToBeAdded,
                                       const Date_t& time,
                                       uint64_t storageCacheSizeBytes,
                                       const StringDataComparator* comparator,
                                       ExecutionStatsController& stats);

/**
 * Updates the stats based on the RolloverReason.
 */
void updateRolloverStats(ExecutionStatsController& stats, RolloverReason reason);

/**
 * Retrieves or initializes the execution stats for the given namespace, for writing.
 */
ExecutionStatsController getOrInitializeExecutionStats(BucketCatalog& catalog,
                                                       const UUID& collectionUUID);

/**
 * Retrieves the execution stats for the given namespace, if they have already been initialized. A
 * valid instance is returned if the stats does not exist for the given namespace but any statistics
 * reported to it will not be reported to the catalog.
 */
ExecutionStatsController getExecutionStats(BucketCatalog& catalog, const UUID& collectionUUID);

/**
 * Retrieves the execution stats for the given namespace, if they have already been initialized.
 */
tracking::shared_ptr<ExecutionStats> getCollectionExecutionStats(const BucketCatalog& catalog,
                                                                 const UUID& collectionUUID);

/**
 * Release the execution stats of a collection from the bucket catalog.
 */
std::vector<tracking::shared_ptr<ExecutionStats>> releaseExecutionStatsFromBucketCatalog(
    BucketCatalog& catalog, std::span<const UUID> collectionUUIDs);

/**
 * Retrieves the execution stats from the side bucket catalog.
 * Assumes the side bucket catalog has the stats of one collection.
 */
std::pair<UUID, tracking::shared_ptr<ExecutionStats>> getSideBucketCatalogCollectionStats(
    BucketCatalog& sideBucketCatalog);

/**
 * Merges the execution stats of a collection into the bucket catalog.
 */
void mergeExecutionStatsToBucketCatalog(BucketCatalog& catalog,
                                        tracking::shared_ptr<ExecutionStats> collStats,
                                        const UUID& collectionUUID);

/**
 * Generates a status with code TimeseriesBucketCleared and an appropriate error message.
 */
Status getTimeseriesBucketClearedError(const OID& oid);

/**
 * Close an open bucket, setting the state appropriately and removing it from the catalog.
 */
void closeOpenBucket(BucketCatalog& catalog,
                     Stripe& stripe,
                     WithLock stripeLock,
                     Bucket& bucket,
                     ExecutionStatsController& stats);

/**
 * Close an archived bucket, setting the state appropriately and removing it from the catalog.
 */
void closeArchivedBucket(BucketCatalog& catalog, const BucketId& bucketId);

/**
 * Inserts measurements into the provided eligible bucket. On success of all measurements being
 * inserted into the provided bucket, returns true. Otherwise, returns false.
 * Also increments `currentPosition` to one past the index of the last measurement inserted.
 */
bool stageInsertBatchIntoEligibleBucket(BucketCatalog& catalog,
                                        OperationId opId,
                                        const StringDataComparator* comparator,
                                        BatchedInsertContext& batch,
                                        Stripe& stripe,
                                        WithLock stripeLock,
                                        uint64_t storageCacheSizeBytes,
                                        Bucket& eligibleBucket,
                                        size_t& currentPosition,
                                        std::shared_ptr<WriteBatch>& writeBatch);

/**
 * Given an already-selected 'bucket', inserts 'doc' to the bucket if possible. If not, we return
 * the reason for why attempting to insert the measurement into the bucket would result in the
 * bucket being rolled over.
 */
std::variant<std::shared_ptr<WriteBatch>, RolloverReason> tryToInsertIntoBucketWithoutRollover(
    BucketCatalog& catalog,
    Stripe& stripe,
    WithLock stripeLock,
    const BatchedInsertTuple& batchedInsertTuple,
    OperationId opId,
    const TimeseriesOptions& timeseriesOptions,
    const StripeNumber& stripeNumber,
    ExecutionStatsController& stats,
    uint64_t storageCacheSizeBytes,
    const StringDataComparator* comparator,
    Bucket& bucket);

/**
 * Given a bucket 'bucket' and a measurement 'doc', updates the WriteBatch corresponding to the
 * inputted bucket as well as the bucket itself to reflect the addition of the measurement. This
 * includes updating the batch/bucket estimated sizes and the bucket's schema.
 * Returns the WriteBatch for the bucket.
 */
std::shared_ptr<WriteBatch> addMeasurementToBatchAndBucket(
    BucketCatalog& catalog,
    const BSONObj& measurement,
    OperationId opId,
    const TimeseriesOptions& timeseriesOptions,
    const StripeNumber& stripeNumber,
    ExecutionStatsController& stats,
    const StringDataComparator* comparator,
    Bucket::NewFieldNames& newFieldNamesToBeInserted,
    const Sizes& sizesToBeAdded,
    bool isNewlyOpenedBucket,
    bool openedDueToMetadata,
    Bucket& bucket);


/**
 * Given a bucket 'bucket', a measurement 'doc', and the 'writeBatch', updates the 'writeBatch'
 * corresponding to the inputted bucket as well as the bucket itself to reflect the addition of the
 * measurement. This includes updating the batch/bucket estimated sizes and the bucket's schema.
 * We also store the index of the measurement in the original user batch, for retryability and
 * error-handling.
 * TODO(SERVER-100294) Remove the new prefix after deleting legacy timeseries write path code.
 */
void newAddMeasurementToBatchAndBucket(BucketCatalog& catalog,
                                       const BSONObj& measurement,
                                       const UserBatchIndex& index,
                                       OperationId opId,
                                       const TimeseriesOptions& timeseriesOptions,
                                       const StripeNumber& stripeNumber,
                                       ExecutionStatsController& stats,
                                       const StringDataComparator* comparator,
                                       Bucket::NewFieldNames& newFieldNamesToBeInserted,
                                       const Sizes& sizesToBeAdded,
                                       bool isNewlyOpenedBucket,
                                       bool openedDueToMetadata,
                                       Bucket& bucket,
                                       std::shared_ptr<WriteBatch>& writeBatch);
}  // namespace mongo::timeseries::bucket_catalog::internal
