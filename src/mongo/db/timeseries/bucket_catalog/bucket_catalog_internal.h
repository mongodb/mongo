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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/timeseries/bucket_catalog/bucket.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::timeseries::bucket_catalog {
struct Stripe;
class BucketCatalog;
struct BatchedInsertContext;
}  // namespace mongo::timeseries::bucket_catalog

namespace mongo::timeseries::bucket_catalog::internal {

using StripeNumber = std::uint8_t;
using BatchedInsertTuple = std::tuple<BSONObj, Date_t, UserBatchIndex>;
/**
 * Function that should run validation against the bucket to ensure it's a proper bucket document.
 * Typically, this should execute Collection::checkValidation.
 */
using BucketDocumentValidator =
    std::function<std::pair<Collection::SchemaValidationResult, Status>(const BSONObj&)>;

enum class StageInsertBatchResult {
    Success,
    RolloverNeeded,
    NoMeasurementsStaged,
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
 * Mode enum to let us know what the isBucketStateEligibleForInsertsAndCleanup returns.
 */
enum class BucketStateForInsertAndCleanup { kNoState, kInsertionConflict, kEligibleForInsert };

/**
 * Maps bucket identifier to the stripe that is responsible for it.
 */
StripeNumber getStripeNumber(const BucketCatalog& catalog, const BucketKey& key);
StripeNumber getStripeNumber(const BucketCatalog& catalog, const BucketId& bucketId);

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
 * Retrieve all open buckets from 'stripe' given a bucket key.
 */
std::vector<Bucket*> findOpenBuckets(Stripe& stripe,
                                     WithLock stripeLock,
                                     const BucketKey& bucketKey);

/**
 * Returns kEligibleForInsert if a given bucket's state is eligible for new inserts. If the bucket
 * has no state, returns kNoState. Returns kInsertionConflict and cleans up the bucket from the
 * catalog if we have an insertion conflict.
 */
BucketStateForInsertAndCleanup isBucketStateEligibleForInsertsAndCleanup(BucketCatalog& catalog,
                                                                         Stripe& stripe,
                                                                         WithLock stripeLock,
                                                                         Bucket* bucket);

/**
 * Rollover 'bucket' according to 'reason' and will update rollover stats if the bucket does not
 * contain uncommitted measurements. Mark the bucket's 'rolloverReason' otherwise.
 */
void rollover(BucketCatalog& catalog,
              Stripe& stripe,
              WithLock stripeLock,
              Bucket& bucket,
              RolloverReason reason);

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

using CompressAndWriteBucketFunc =
    std::function<void(OperationContext*, const BucketId&, const NamespaceString&, StringData)>;

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
 * Wait for other batches to finish so we can prepare 'batch'
 */
void waitToCommitBatch(BucketStateRegistry& registry,
                       Stripe& stripe,
                       const std::shared_ptr<WriteBatch>& batch);

/**
 * Removes the given bucket from the bucket catalog's internal data structures,
 * including statistics.
 */
void removeBucket(BucketCatalog& catalog,
                  Stripe& stripe,
                  WithLock stripeLock,
                  Bucket& bucket,
                  ExecutionStatsController& stats);

/**
 * Removes the given bucket from the bucket catalog's internal data structures.
 */
void removeBucketWithoutStats(BucketCatalog& catalog,
                              Stripe& stripe,
                              WithLock stripeLock,
                              Bucket& bucket);

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
                                                         int64_t workloadCardinality);

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
 * Returns a prepared write batch matching the specified 'key' if one exists, by searching the set
 * of open buckets associated with 'key'.
 */
std::shared_ptr<WriteBatch> findPreparedBatch(const Stripe& stripe,
                                              WithLock stripeLock,
                                              const BucketKey& key,
                                              const boost::optional<OID>& oid);

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
 * Determines if and why 'bucket' needs to be rolled over to accommodate 'doc'.
 * Will also update the bucket catalog stats incNumBucketsKeptOpenDueToLargeMeasurements as
 * appropriate.
 */
RolloverReason determineRolloverReason(const BSONObj& doc,
                                       const TimeseriesOptions& timeseriesOptions,
                                       int64_t numberOfActiveBuckets,
                                       const Sizes& sizesToBeAdded,
                                       const Date_t& time,
                                       uint64_t storageCacheSizeBytes,
                                       const StringDataComparator* comparator,
                                       Bucket& bucket,
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
void closeArchivedBucket(BucketCatalog& catalog,
                         const BucketId& bucketId,
                         ExecutionStatsController& stats);

/**
 * Inserts measurements into the provided eligible bucket. On success of all measurements being
 * inserted into the provided bucket, returns true. Otherwise, returns false.
 * Also increments `currentPosition` to one past the index of the last measurement inserted.
 */
StageInsertBatchResult stageInsertBatchIntoEligibleBucket(BucketCatalog& catalog,
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
 * Given an already-selected 'bucket', inserts the measurement in 'batchedInsertTuple' to the bucket
 * if possible.
 * Returns true if successfully inserted.
 * Returns false if 'bucket' needs to be rolled over. Marks its 'rolloverReason' accordingly.
 */
bool tryToInsertIntoBucketWithoutRollover(BucketCatalog& catalog,
                                          Stripe& stripe,
                                          WithLock stripeLock,
                                          const BatchedInsertTuple& batchedInsertTuple,
                                          OperationId opId,
                                          const TimeseriesOptions& timeseriesOptions,
                                          const StripeNumber& stripeNumber,
                                          uint64_t storageCacheSizeBytes,
                                          const StringDataComparator* comparator,
                                          Bucket& bucket,
                                          ExecutionStatsController& stats,
                                          std::shared_ptr<WriteBatch>& writeBatch);

/**
 * Given a bucket 'bucket', a measurement 'doc', and the 'writeBatch', updates the 'writeBatch'
 * corresponding to the inputted bucket as well as the bucket itself to reflect the addition of the
 * measurement. This includes updating the batch/bucket estimated sizes and the bucket's schema.
 * We also store the index of the measurement in the original user batch, for retryability and
 * error-handling.
 */
void addMeasurementToBatchAndBucket(BucketCatalog& catalog,
                                    const BSONObj& measurement,
                                    const UserBatchIndex& index,
                                    OperationId opId,
                                    const TimeseriesOptions& timeseriesOptions,
                                    const StripeNumber& stripeNumber,
                                    const StringDataComparator* comparator,
                                    const Sizes& sizesToBeAdded,
                                    bool isNewlyOpenedBucket,
                                    const Bucket::NewFieldNames& newFieldNamesToBeInserted,
                                    Bucket& bucket,
                                    std::shared_ptr<WriteBatch>& writeBatch);
}  // namespace mongo::timeseries::bucket_catalog::internal
