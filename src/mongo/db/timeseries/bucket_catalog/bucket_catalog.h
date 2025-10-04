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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/timeseries/bucket_catalog/bucket.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/bucket_catalog/tracking_contexts.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/tracking/btree_map.h"
#include "mongo/util/tracking/flat_hash_set.h"
#include "mongo/util/tracking/inlined_vector.h"
#include "mongo/util/tracking/unordered_map.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <variant>

#include <absl/container/inlined_vector.h>
#include <boost/container/static_vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::timeseries::bucket_catalog {

using StripeNumber = std::uint8_t;
// Tuple that stores a measurement, the time value for that measurement, and the index of the
// measurement from the original insert request.
using BatchedInsertTuple = std::tuple<BSONObj, Date_t, UserBatchIndex>;
using TimeseriesWriteBatches = std::vector<std::shared_ptr<WriteBatch>>;

/**
 * Mode enum to control whether getReopeningCandidate() will allow query-based
 * reopening of buckets when attempting to accommodate a new measurement.
 */
enum class AllowQueryBasedReopening { kAllow, kDisallow };

struct WriteStageErrorAndIndex {
    Status error;
    size_t index;
};

/**
 * Represents a set of measurements that should target one bucket. The measurements contained in
 * this struct are a best-effort guess at a grouping based on, but not intended to be a guarantee as
 * to, what will fit in a bucket. The measurements stored within the struct should be sorted on
 * time, and are guaranteed only to share a metaField value.
 */
struct BatchedInsertContext {
    BucketKey key;
    StripeNumber stripeNumber;
    const TimeseriesOptions& options;
    ExecutionStatsController stats;
    std::vector<BatchedInsertTuple> measurementsTimesAndIndices;

    BatchedInsertContext(BucketKey&,
                         StripeNumber,
                         const TimeseriesOptions&,
                         ExecutionStatsController&,
                         std::vector<BatchedInsertTuple>&);
};

/**
 * An insert or reopening operation can conflict with an outstanding 'ReopeningRequest' or a
 * prepared 'WriteBatch' for a bucket in the series (same metaField value). Caller should wait using
 * 'waitToInsert'.
 */
using InsertWaiter = std::variant<std::shared_ptr<WriteBatch>, std::shared_ptr<ReopeningRequest>>;

/**
 * Struct to hold a portion of the buckets managed by the catalog.
 *
 * Each of the bucket lists, as well as the buckets themselves, are protected by 'mutex'.
 */
struct Stripe {
    // All access to a stripe should happen while 'mutex' is locked.
    mutable stdx::mutex mutex;

    // All buckets currently open in the catalog, including buckets which are full or pending
    // closure but not yet committed, indexed by BucketId. Owning pointers.
    tracking::unordered_map<BucketId, tracking::unique_ptr<Bucket>, BucketHasher> openBucketsById;

    // All buckets currently open in the catalog, including buckets which are full or pending
    // closure but not yet committed, indexed by BucketKey. Non-owning pointers.
    tracking::unordered_map<BucketKey, tracking::flat_hash_set<Bucket*>, BucketHasher>
        openBucketsByKey;

    // Open buckets that do not have any outstanding writes.
    using IdleList = tracking::list<Bucket*>;
    IdleList idleBuckets;

    // Buckets that are not currently in the catalog, but which are eligible to receive more
    // measurements. A btree with a compound key is used for maximum memory efficiency. The
    // comparison is inverted so we can use lower_bound to efficiently find an archived bucket that
    // is a candidate for an incoming measurement.
    using ArchivedKey = std::tuple<UUID, BucketKey::Hash, Date_t>;
    tracking::btree_map<ArchivedKey, ArchivedBucket, std::greater<ArchivedKey>> archivedBuckets;

    // All series currently with outstanding reopening operations. Used to coordinate disk access
    // between reopenings and regular writes to prevent stale reads and corrupted updates.
    static constexpr int kInlinedVectorSize = 4;
    tracking::unordered_map<
        BucketKey,
        tracking::inlined_vector<tracking::shared_ptr<ReopeningRequest>, kInlinedVectorSize>,
        BucketHasher>
        outstandingReopeningRequests;

    Stripe(TrackingContexts& trackingContextOpenId);
};

struct PotentialBucketOptions {
    absl::InlinedVector<Bucket*, 8> kArchivedBuckets;
    absl::InlinedVector<Bucket*, 8> kSoftClosedBuckets;
    // Only one uncleared open bucket is allowed for each key.
    Bucket* kNoneBucket = nullptr;
};

/**
 * This class holds all the data used to coordinate time series inserts amongst threads.
 */
class BucketCatalog {
public:
    BucketCatalog(size_t numberOfStripes, std::function<uint64_t()> memoryUsageThreshold);
    BucketCatalog(const BucketCatalog&) = delete;
    BucketCatalog operator=(const BucketCatalog&) = delete;

    // Stores an accurate count of the bytes allocated and deallocated for all the data held by
    // tracked members of the BucketCatalog.
    TrackingContexts trackingContexts;

    // Stores state information about all buckets managed by the catalog, across stripes.
    BucketStateRegistry bucketStateRegistry;

    // The actual buckets in the catalog are distributed across a number of 'Stripe's. Each can be
    // independently locked and operated on in parallel. The size of the stripe vector should not be
    // changed after initialization.
    const std::size_t numberOfStripes = 32;
    tracking::vector<tracking::unique_ptr<Stripe>> stripes;

    // Per-namespace execution stats. This map is protected by 'mutex'. Once you complete your
    // lookup, you can keep the shared_ptr to an individual namespace's stats object and release the
    // lock. The object itself is thread-safe (using atomics).
    mutable stdx::mutex mutex;
    tracking::unordered_map<UUID, tracking::shared_ptr<ExecutionStats>> executionStats;

    // Global execution stats used to report aggregated metrics in server status.
    ExecutionStats globalExecutionStats;

    // Memory usage threshold in bytes after which idle buckets will be expired.
    std::function<uint64_t()> memoryUsageThreshold;
};

/**
 * Returns the memory usage of the bucket catalog across all stripes from the approximated memory
 * usage, and the tracked memory usage from the tracking::Allocator.
 */
uint64_t getMemoryUsage(const BucketCatalog& catalog);

/**
 * Adds a 'memoryUsageDetails' section to the builder if details are available (debug builds only).
 */
void getDetailedMemoryUsage(const BucketCatalog& catalog, BSONObjBuilder& builder);

/**
 * If an insert or reopening returns a 'InsertWaiter' object, the caller should use this function to
 * wait before repeating their attempt.
 */
void waitToInsert(InsertWaiter* waiter);

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
 * Prepares a batch for commit, transitioning it to an inactive state. Returns OK if the batch was
 * successfully prepared, or a status indicating why the batch was previously aborted by another
 * operation. If another batch is already prepared on the same bucket, or there is an outstanding
 * 'ReopeningRequest' for the same series (metaField value), this operation will block waiting for
 * it to complete.
 */
Status prepareCommit(BucketCatalog& catalog,
                     std::shared_ptr<WriteBatch> batch,
                     const StringDataComparator* comparator);

/**
 * Finishes committing the batch and notifies other threads waiting for preparing their batches.
 * Batch must have been previously prepared.
 */
void finish(BucketCatalog& catalog, std::shared_ptr<WriteBatch> batch);

/**
 * Aborts the given write batch and any other outstanding (unprepared) batches on the same bucket,
 * using the provided status.
 */
void abort(BucketCatalog& catalog, std::shared_ptr<WriteBatch> batch, const Status& status);

/**
 * Notifies the catalog of a direct write (that is, a write not initiated by the BucketCatalog) that
 * will be performed on the bucket document with the specified ID. If there is already an
 * internally-prepared operation on that bucket, this method will throw a 'WriteConflictException'.
 * This should be followed by a call to 'directWriteFinish' after the write has been committed,
 * rolled back, or otherwise finished.
 */
void directWriteStart(BucketStateRegistry& registry, const BucketId& bucketId);

/**
 * Notifies the catalog that a pending direct write to the bucket document with the specified ID has
 * finished or been abandoned, and normal operations on the bucket can resume. After this point any
 * in-memory representation of the on-disk bucket data from before the direct write should have been
 * cleared from the catalog, and it may be safely reopened from the on-disk state.
 */
void directWriteFinish(BucketStateRegistry& registry, const BucketId& bucketId);

/**
 * Clears any bucket whose collection UUID has been cleared by removing the bucket from the catalog
 * asynchronously through the BucketStateRegistry. Drops statistics for the affected collections.
 */
void drop(BucketCatalog& catalog, tracking::vector<UUID> clearedCollectionUUIDs);

/**
 * Clears the buckets for the given collection UUID by removing the bucket from the catalog
 * asynchronously through the BucketStateRegistry. Drops statistics for the affected collection.
 */
void drop(BucketCatalog& catalog, const UUID& collectionUUID);

/**
 * Clears the buckets for the given collection UUID by removing the bucket from the catalog
 * asynchronously through the BucketStateRegistry.
 */
void clear(BucketCatalog& catalog, const UUID& collectionUUID);

/**
 * Freezes the given bucket in the registry so that this bucket will never be used in the future.
 */
void freeze(BucketCatalog&, const BucketId& bucketId);

/**
 * Increments an FTDC counter.
 * Denotes an event where a generated time-series bucket document for insert exceeded the BSON
 * size limit.
 */
void markBucketInsertTooLarge(BucketCatalog& catalog, const UUID& collectionUUID);

/**
 * Increments an FTDC counter.
 * Denotes an event where a generated time-series bucket document for update exceeded the BSON
 * size limit.
 */
void markBucketUpdateTooLarge(BucketCatalog& catalog, const UUID& collectionUUID);

/**
 * Extracts the BucketId from a bucket document.
 */
BucketId extractBucketId(BucketCatalog&,
                         const TimeseriesOptions& options,
                         const UUID& collectionUUID,
                         const BSONObj& bucket);

BucketKey::Signature getKeySignature(const TimeseriesOptions& options,
                                     const UUID& collectionUUID,
                                     const BSONObj& metadata);

/**
 * Resets the counter used for bucket OID generation. Should be called after a bucket _id
 * collision.
 */
void resetBucketOIDCounter();

/**
 * Generates an OID for the bucket _id field, setting the timestamp portion to a value determined by
 * rounding 'time' based on 'options'.
 */
std::pair<OID, Date_t> generateBucketOID(const Date_t& time, const TimeseriesOptions& options);

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
 * Appends the execution stats for the given namespace to the builder.
 */
void appendExecutionStats(const BucketCatalog& catalog,
                          const UUID& collectionUUID,
                          BSONObjBuilder& builder);

/**
 * Determines if 'measurement' will cause rollover to 'bucket'.
 * Returns the rollover reason and marks the bucket with rollover reason if it needs to be rolled
 * over.
 */
RolloverReason determineBucketRolloverForMeasurement(BucketCatalog& catalog,
                                                     const BSONObj& measurement,
                                                     const Date_t& measurementTimestamp,
                                                     const TimeseriesOptions& options,
                                                     const StringDataComparator* comparator,
                                                     uint64_t storageCacheSizeBytes,
                                                     Bucket& bucket,
                                                     ExecutionStatsController& stats);

/**
 * Returns a vector of buckets based on the two conditions in order:
 *  1. Prioritizes returning candidate buckets that have the kSoftClose rollover reason, then the
 *     kArchive rollover reason, and finally a kNone rollover reason.
 *  2. Prioritizes returning buckets with less measurements in them.
 */
std::vector<Bucket*> createOrderedPotentialBucketsVector(
    PotentialBucketOptions& potentialBucketOptions);

/**
 * Finds all buckets with 'bucketKey' by criteria in the following order:
 *  - A bucket with RolloverReason::kTimeForward or RolloverReason::kTimeBackward, and 'time' fits
 *    in its time range. The bucket's state is eligible for insert. There may be many such buckets.
 *  - A bucket with RolloverReason::kNone. The bucket's state is eligible for insert. There can be
 *    at most one such bucket, and it will be the last entry in the returned vector if it exists.
 * Rolls over buckets that don't satisfy the above requirements and cleans up buckets with states
 * conflicting with insert. Sets whether to skip query-based reopening.
 */
std::vector<Bucket*> findAndRolloverOpenBuckets(BucketCatalog& catalog,
                                                Stripe& stripe,
                                                WithLock stripeLock,
                                                const BucketKey& bucketKey,
                                                const Date_t& time,
                                                const Seconds& bucketMaxSpanSeconds,
                                                AllowQueryBasedReopening& allowQueryBasedReopening,
                                                bool& bucketOpenedDueToMetadata);

/**
 * Returns an open bucket from 'stripe' that can fit 'measurement'. If none available, returns
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
                                     AllowQueryBasedReopening& allowQueryBasedReopening,
                                     ExecutionStatsController& stats,
                                     bool& bucketOpenedDueToMetadata);

using CompressAndWriteBucketFunc =
    std::function<void(OperationContext*, const BucketId&, const NamespaceString&, StringData)>;

/**
 * Given the 'reopeningCandidate', returns:
 *      - An owned pointer of the bucket if successfully reopened and initialized in memory.
 *      - A nullptr if no eligible bucket is reopened.
 *      - An error if the reopened bucket cannot be used for new inserts.
 * Compresses the reopened bucket document if it's uncompressed.
 */
StatusWith<tracking::unique_ptr<Bucket>> getReopenedBucket(
    OperationContext* opCtx,
    BucketCatalog& catalog,
    const Collection* bucketsColl,
    const BucketKey& bucketKey,
    const TimeseriesOptions& options,
    const std::variant<OID, std::vector<BSONObj>>& reopeningCandidate,
    BucketStateRegistry::Era catalogEra,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    ExecutionStatsController& stats,
    bool& bucketOpenedDueToMetadata);

/**
 * Reopens and loads a bucket eligible for inserts into the bucket catalog. Checks for conflicting
 * operations with reopening. Returns:
 *      - A pointer of the bucket if successfully reopened and loaded into the bucket catalog.
 *      - A nullptr if no eligible bucket is reopened.
 *      - An error if the reopened bucket cannot be used for new inserts.
 * Called with a stripe lock. May release the lock for reopening. Returns holding the lock.
 * Manages the lifetime of the reopening request in 'stripe'.
 */
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
    const AllowQueryBasedReopening& allowQueryBasedReopening,
    uint64_t storageCacheSizeBytes,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    ExecutionStatsController& stats,
    bool& bucketOpenedDueToMetadata);

/**
 * Returns a bucket where 'measurement' can be inserted. Attempts to get the bucket with the steps:
 *  1. Finds an open bucket from 'stripe'.
 *  2. Reopens a bucket from 'bucketsColl'.
 *  3. Allocates a new bucket.
 * May release the stripeLock in the middle but will reacquire it for such cases.
 * Side effects:
 *  - Performs rollover on open buckets of 'stripe'.
 *  - Compresses uncompressed reopened bucket documents.
 */
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
                          uint64_t storageCacheSizeBytes,
                          const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
                          AllowQueryBasedReopening allowQueryBasedReopening,
                          ExecutionStatsController& stats,
                          bool& bucketOpenedDueToMetadata);

/**
 * Given a batch of user measurements for a collection that does not have a metaField value, returns
 * a BatchedInsertContext for all of the user measurements. This is a special case - all time-series
 * without a metafield value are grouped within the same batch.
 *
 * Passes through the inputted measurements twice, once to record the index of the measurement in
 * the original user batch for error reporting, and then again to sort the measurements based on
 * their time field.
 *
 * This is slightly more efficient and requires fewer maps/data structures than the metaField
 * variant, because we do not need to split up the measurements into different batches according to
 * their metaField value.
 */
std::vector<BatchedInsertContext> buildBatchedInsertContextsNoMetaField(
    const BucketCatalog& bucketCatalog,
    const UUID& collectionUUID,
    const TimeseriesOptions& timeseriesOptions,
    const std::vector<BSONObj>& userMeasurementsBatch,
    size_t startIndex,
    size_t numDocs,
    const std::vector<size_t>& indices,
    ExecutionStatsController& stats,
    tracking::Context& trackingContext,
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices);

/**
 * Given a batch of user measurements for a collection that does have a metaField value, returns a
 * vector of BatchedInsertContexts with each BatchedInsertContext storing the measurements for a
 * particular metaField value.
 *
 * Passes through the inputted measurements twice, once to record the index of the measurement in
 * the original user batch for error reporting, and then again to sort the measurements based on
 * their time field.
 */
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
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices);

/**
 * Given a set of measurements, splits up the measurements into batches based on the metaField.
 * Returns a vector of BatchedInsertContext where each BatchedInsertContext will contain the batch
 * of measurements for a particular metaField value, sorted on time, as well as other bucket-level
 * metadata.
 *
 * If the time-series collection has no metaField value, then all of the measurements will be
 * batched into one BatchedInsertContext.
 *
 * Any inserted measurements that are malformed (i.e. missing the proper time field) will have their
 * error Status and their index recorded in errorsAndIndices. Callers should check that no errors
 * occurred while processing measurements by checking that errorsAndIndices is empty.
 */
std::vector<BatchedInsertContext> buildBatchedInsertContexts(
    BucketCatalog& bucketCatalog,
    const UUID& collectionUUID,
    const TimeseriesOptions& timeseriesOptions,
    const std::vector<BSONObj>& userMeasurementsBatch,
    size_t startIndex,
    size_t numDocsToStage,
    const std::vector<size_t>& indices,
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices);

/**
 * Given a BatchedInsertContext, will stage writes to eligible buckets until all measurements have
 * been staged into an eligible bucket. When there is any RolloverReason that isn't kNone when
 * attempting to stage a measurement into a bucket, the function will find another eligible
 * buckets until all measurements are inserted.
 */
TimeseriesWriteBatches stageInsertBatch(
    OperationContext* opCtx,
    BucketCatalog& bucketCatalog,
    const Collection* bucketsColl,
    const OperationId& opId,
    const StringDataComparator* comparator,
    uint64_t storageCacheSizeBytes,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    AllowQueryBasedReopening allowQueryBasedReopening,
    BatchedInsertContext& batch);

/**
 * Stages compatible measurements into appropriate bucket(s).
 * Returns a non-success status if any measurements are malformed, and further
 * returns the index into 'userMeasurementsBatch' of each failure in 'errorsAndIndices'.
 * Returns a write batch per bucket that the measurements are staged to.
 * 'earlyReturnOnError' decides whether or not staging should happen in the case of any malformed
 * measurements.
 */
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
    AllowQueryBasedReopening allowQueryBasedReopening,
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices);

/**
 * Extracts the information from the input 'doc' that is used to map the document to a bucket.
 */
StatusWith<std::pair<BucketKey, Date_t>> extractBucketingParameters(
    tracking::Context&,
    const UUID& collectionUUID,
    const TimeseriesOptions& options,
    const BSONObj& doc);
}  // namespace mongo::timeseries::bucket_catalog
