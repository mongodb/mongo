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

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <queue>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/bucket_catalog/bucket.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_manager.h"
#include "mongo/db/timeseries/bucket_catalog/closed_bucket.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/string_map.h"

namespace mongo::timeseries::bucket_catalog {

class BucketCatalog {
protected:
    using StripeNumber = std::uint8_t;
    using ShouldClearFn = std::function<bool(const NamespaceString&)>;

public:
    enum class CombineWithInsertsFromOtherClients {
        kAllow,
        kDisallow,
    };

    /**
     * Return type for the insert function. See insert() for more information.
     */
    class InsertResult {
    public:
        InsertResult() = default;
        InsertResult(InsertResult&&) = default;
        InsertResult& operator=(InsertResult&&) = default;
        InsertResult(const InsertResult&) = delete;
        InsertResult& operator=(const InsertResult&) = delete;

        std::shared_ptr<WriteBatch> batch;
        ClosedBuckets closedBuckets;
        stdx::variant<std::monostate, OID, BSONObj> candidate;
        uint64_t catalogEra = 0;
    };

    static BucketCatalog& get(ServiceContext* svcCtx);
    static BucketCatalog& get(OperationContext* opCtx);

    BucketCatalog() = default;

    BucketCatalog(const BucketCatalog&) = delete;
    BucketCatalog operator=(const BucketCatalog&) = delete;

    /**
     * Reopens a closed bucket into the catalog given the bucket document.
     */
    Status reopenBucket(OperationContext* opCtx,
                        const CollectionPtr& coll,
                        const BSONObj& bucketDoc);

    /**
     * Returns the metadata for the given bucket in the following format:
     *     {<metadata field name>: <value>}
     * All measurements in the given bucket share same metadata value.
     *
     * Returns an empty document if the given bucket cannot be found or if this time-series
     * collection was not created with a metadata field name.
     */
    BSONObj getMetadata(const BucketHandle& bucket);

    /**
     * Tries to insert 'doc' into a suitable bucket. If an open bucket is full (or has incompatible
     * schema), but is otherwise suitable, we will close it and open a new bucket. If we find no
     * bucket with matching data and a time range that can accomodate 'doc', we will not open a new
     * bucket, but rather let the caller know to search for an archived or closed bucket that can
     * accomodate 'doc'.
     *
     * If a suitable bucket is found or opened, returns the WriteBatch into which 'doc' was
     * inserted and a list of any buckets that were closed to make space to insert 'doc'. Any
     * caller who receives the same batch may commit or abort the batch after claiming commit
     * rights. See WriteBatch for more details.
     *
     * If no suitable bucket is found or opened, returns an optional bucket ID. If set, the bucket
     * ID corresponds to an archived bucket which should be fetched; otherwise the caller should
     * search for a previously-closed bucket that can accomodate 'doc'. The caller should proceed to
     * call 'insert' to insert 'doc', passing any fetched bucket.
     */
    StatusWith<InsertResult> tryInsert(OperationContext* opCtx,
                                       const NamespaceString& ns,
                                       const StringData::ComparatorInterface* comparator,
                                       const TimeseriesOptions& options,
                                       const BSONObj& doc,
                                       CombineWithInsertsFromOtherClients combine);

    /**
     * Returns the WriteBatch into which the document was inserted and a list of any buckets that
     * were closed in order to make space to insert the document. Any caller who receives the same
     * batch may commit or abort the batch after claiming commit rights. See WriteBatch for more
     * details.
     *
     * If 'bucketToReopen' is passed, we will reopen that bucket and attempt to add 'doc' to that
     * bucket. Otherwise we will attempt to find a suitable open bucket, or open a new bucket if
     * none exists.
     */
    StatusWith<InsertResult> insert(OperationContext* opCtx,
                                    const NamespaceString& ns,
                                    const StringData::ComparatorInterface* comparator,
                                    const TimeseriesOptions& options,
                                    const BSONObj& doc,
                                    CombineWithInsertsFromOtherClients combine,
                                    BucketFindResult bucketFindResult = {});

    /**
     * Prepares a batch for commit, transitioning it to an inactive state. Caller must already have
     * commit rights on batch. Returns OK if the batch was successfully prepared, or a status
     * indicating why the batch was previously aborted by another operation.
     */
    Status prepareCommit(std::shared_ptr<WriteBatch> batch);

    /**
     * Records the result of a batch commit. Caller must already have commit rights on batch, and
     * batch must have been previously prepared.
     *
     * Returns bucket information of a bucket if one was closed.
     */
    boost::optional<ClosedBucket> finish(std::shared_ptr<WriteBatch> batch, const CommitInfo& info);

    /**
     * Aborts the given write batch and any other outstanding batches on the same bucket, using the
     * provided status.
     */
    void abort(std::shared_ptr<WriteBatch> batch, const Status& status);

    /**
     * Notifies the catalog of a direct write (that is, a write not initiated by the BucketCatalog)
     * that will be performed on the bucket document with the specified ID. If there is already an
     * internally-prepared operation on that bucket, this method will throw a
     * 'WriteConflictException'. This should be followed by a call to 'directWriteFinish' after the
     * write has been committed, rolled back, or otherwise finished.
     */
    void directWriteStart(const NamespaceString& ns, const OID& oid);

    /**
     * Notifies the catalog that a pending direct write to the bucket document with the specified ID
     * has finished or been abandoned, and normal operations on the bucket can resume. After this
     * point any in-memory representation of the on-disk bucket data from before the direct write
     * should have been cleared from the catalog, and it may be safely reopened from the on-disk
     * state.
     */
    void directWriteFinish(const NamespaceString& ns, const OID& oid);

    /**
     * Clears any bucket whose namespace satisfies the predicate.
     */
    void clear(ShouldClearFn&& shouldClear);

    /**
     * Clears the buckets for the given namespace.
     */
    void clear(const NamespaceString& ns);

    /**
     * Clears the buckets for the given database.
     */
    void clear(StringData dbName);

    /**
     * Appends the execution stats for the given namespace to the builder.
     */
    void appendExecutionStats(const NamespaceString& ns, BSONObjBuilder* builder) const;

    /**
     * Appends the global execution stats for all namespaces to the builder.
     */
    void appendGlobalExecutionStats(BSONObjBuilder* builder) const;

    /**
     * Appends the global bucket state management stats for all namespaces to the builder.
     */
    void appendStateManagementStats(BSONObjBuilder* builder) const;

    /**
     * Reports the current memory usage.
     */
    long long memoryUsage() const;

protected:
    /**
     * Struct to hold a portion of the buckets managed by the catalog.
     *
     * Each of the bucket lists, as well as the buckets themselves, are protected by 'mutex'.
     */
    struct Stripe {
        mutable Mutex mutex =
            MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(1), "BucketCatalog::Stripe::mutex");

        // All buckets currently in the catalog, including buckets which are full but not yet
        // committed.
        stdx::unordered_map<BucketId, std::unique_ptr<Bucket>, BucketHasher> allBuckets;

        // The current open bucket for each namespace and metadata pair.
        stdx::unordered_map<BucketKey, std::set<Bucket*>, BucketHasher> openBuckets;

        // Buckets that do not have any outstanding writes.
        using IdleList = std::list<Bucket*>;
        IdleList idleBuckets;

        // Buckets that are not currently in the catalog, but which are eligible to receive more
        // measurements. The top-level map is keyed by the hash of the BucketKey, while the stored
        // map is keyed by the bucket's minimum timestamp.
        //
        // We invert the key comparison in the inner map so that we can use lower_bound to
        // efficiently find an archived bucket that is a candidate for an incoming measurement.
        stdx::unordered_map<BucketKey::Hash,
                            std::map<Date_t, ArchivedBucket, std::greater<Date_t>>,
                            BucketHasher>
            archivedBuckets;
    };

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
     * Extracts the information from the input 'doc' that is used to map the document to a bucket.
     */
    StatusWith<std::pair<BucketKey, Date_t>> _extractBucketingParameters(
        const NamespaceString& ns,
        const StringData::ComparatorInterface* comparator,
        const TimeseriesOptions& options,
        const BSONObj& doc) const;

    /**
     * Maps bucket key to the stripe that is responsible for it.
     */
    StripeNumber _getStripeNumber(const BucketKey& key) const;

    /**
     * Mode enum to control whether the bucket retrieval methods below will return buckets that have
     * a state that conflicts with insertion.
     */
    enum class IgnoreBucketState { kYes, kNo };

    /**
     * Retrieve a bucket for read-only use.
     */
    const Bucket* _findBucket(const Stripe& stripe,
                              WithLock stripeLock,
                              const BucketId& bucketId,
                              IgnoreBucketState mode = IgnoreBucketState::kNo);

    /**
     * Retrieve a bucket for write use.
     */
    Bucket* _useBucket(Stripe* stripe,
                       WithLock stripeLock,
                       const BucketId& bucketId,
                       IgnoreBucketState mode);

    /**
     * Retrieve a bucket for write use, updating the state in the process.
     */
    Bucket* _useBucketAndChangeState(Stripe* stripe,
                                     WithLock stripeLock,
                                     const BucketId& bucketId,
                                     const BucketStateManager::StateChangeFn& change);

    /**
     * Mode enum to control whether the bucket retrieval methods below will create new buckets if no
     * suitable bucket exists.
     */
    enum class AllowBucketCreation { kYes, kNo };

    /**
     * Retrieve the open bucket for write use if one exists. If none exists and 'mode' is set to
     * kYes, then we will create a new bucket.
     */
    Bucket* _useBucket(Stripe* stripe,
                       WithLock stripeLock,
                       const CreationInfo& info,
                       AllowBucketCreation mode);

    /**
     * Retrieve a previously closed bucket for write use if one exists in the catalog. Considers
     * buckets that are pending closure or archival but which are still eligible to recieve new
     * measurements.
     */
    Bucket* _useAlternateBucket(Stripe* stripe, WithLock stripeLock, const CreationInfo& info);

    /**
     * Given a bucket to reopen, performs validation and constructs the in-memory representation of
     * the bucket. If specified, 'expectedKey' is matched against the key extracted from the
     * document to validate that the bucket is expected (i.e. to help resolve hash collisions for
     * archived buckets). Does *not* hand ownership of the bucket to the catalog.
     */
    StatusWith<std::unique_ptr<Bucket>> _rehydrateBucket(
        OperationContext* opCtx,
        const NamespaceString& ns,
        const StringData::ComparatorInterface* comparator,
        const TimeseriesOptions& options,
        const BucketToReopen& bucketToReopen,
        boost::optional<const BucketKey&> expectedKey);

    /**
     * Given a rehydrated 'bucket', passes ownership of that bucket to the catalog, marking the
     * bucket as open.
     */
    StatusWith<Bucket*> _reopenBucket(Stripe* stripe,
                                      WithLock stripeLock,
                                      ExecutionStatsController stats,
                                      const BucketKey& key,
                                      std::unique_ptr<Bucket>&& bucket,
                                      std::uint64_t targetEra,
                                      ClosedBuckets* closedBuckets);

    /**
     * Check to see if 'insert' can use existing bucket rather than reopening a candidate bucket. If
     * true, chances are the caller raced with another thread to reopen the same bucket, but if
     * false, there might be another bucket that had been cleared, or that has the same _id in a
     * different namespace.
     */
    StatusWith<Bucket*> _reuseExistingBucket(Stripe* stripe,
                                             WithLock stripeLock,
                                             ExecutionStatsController* stats,
                                             const BucketKey& key,
                                             Bucket* existingBucket,
                                             std::uint64_t targetEra);

    /**
     * Helper method to perform the heavy lifting for both 'tryInsert' and 'insert'. See
     * documentation on callers for more details.
     */
    StatusWith<InsertResult> _insert(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     const StringData::ComparatorInterface* comparator,
                                     const TimeseriesOptions& options,
                                     const BSONObj& doc,
                                     CombineWithInsertsFromOtherClients combine,
                                     AllowBucketCreation mode,
                                     BucketFindResult bucketFindResult = {});

    /**
     * Given an already-selected 'bucket', inserts 'doc' to the bucket if possible. If not, and
     * 'mode' is set to 'kYes', we will create a new bucket and insert into that bucket.
     */
    stdx::variant<std::shared_ptr<WriteBatch>, RolloverReason> _insertIntoBucket(
        OperationContext* opCtx,
        Stripe* stripe,
        WithLock stripeLock,
        StripeNumber stripeNumber,
        const BSONObj& doc,
        CombineWithInsertsFromOtherClients combine,
        AllowBucketCreation mode,
        CreationInfo* info,
        Bucket* bucket);

    /**
     * Wait for other batches to finish so we can prepare 'batch'
     */
    void _waitToCommitBatch(Stripe* stripe, const std::shared_ptr<WriteBatch>& batch);

    /**
     * Mode to signal to '_removeBucket' what's happening to the bucket, and how to handle the
     * bucket state change.
     */
    enum class RemovalMode {
        kClose,    // Normal closure, pending compression
        kArchive,  // Archive bucket, no state change
        kAbort,    // Bucket is being cleared, possibly due to error, erase state
    };

    /**
     * Removes the given bucket from the bucket catalog's internal data structures.
     */
    void _removeBucket(Stripe* stripe, WithLock stripeLock, Bucket* bucket, RemovalMode mode);

    /**
     * Archives the given bucket, minimizing the memory footprint but retaining the necessary
     * information required to efficiently identify it as a candidate for future insertions.
     */
    void _archiveBucket(Stripe* stripe,
                        WithLock stripeLock,
                        Bucket* bucket,
                        ClosedBuckets* closedBuckets);

    /**
     * Identifies a previously archived bucket that may be able to accomodate the measurement
     * represented by 'info', if one exists.
     */
    boost::optional<OID> _findArchivedCandidate(Stripe* stripe,
                                                WithLock stripeLock,
                                                const CreationInfo& info);

    /**
     * Identifies a previously archived bucket that may be able to accomodate the measurement
     * represented by 'info', if one exists.
     */
    stdx::variant<std::monostate, OID, BSONObj> _getReopeningCandidate(
        Stripe* stripe,
        WithLock stripeLock,
        const CreationInfo& info,
        bool allowQueryBasedReopening);

    /**
     * Aborts 'batch', and if the corresponding bucket still exists, proceeds to abort any other
     * unprepared batches and remove the bucket from the catalog if there is no unprepared batch.
     */
    void _abort(Stripe* stripe,
                WithLock stripeLock,
                std::shared_ptr<WriteBatch> batch,
                const Status& status);

    /**
     * Aborts any unprepared batches for the given bucket, then removes the bucket if there is no
     * prepared batch. If 'batch' is non-null, it is assumed that the caller has commit rights for
     * that batch.
     */
    void _abort(Stripe* stripe,
                WithLock stripeLock,
                Bucket* bucket,
                std::shared_ptr<WriteBatch> batch,
                const Status& status);

    /**
     * Records that compression for the given bucket has been completed, and the BucketCatalog can
     * forget about the bucket.
     */
    void _compressionDone(const BucketId& bucketId);

    /**
     * Adds the bucket to a list of idle buckets to be expired at a later date.
     */
    void _markBucketIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket);

    /**
     * Remove the bucket from the list of idle buckets. The second parameter encodes whether the
     * caller holds a lock on _idleMutex.
     */
    void _markBucketNotIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket);

    /**
     * Expires idle buckets until the bucket catalog's memory usage is below the expiry
     * threshold.
     */
    void _expireIdleBuckets(Stripe* stripe,
                            WithLock stripeLock,
                            ExecutionStatsController& stats,
                            ClosedBuckets* closedBuckets);

    /**
     * Allocates a new bucket and adds it to the catalog.
     */
    Bucket* _allocateBucket(Stripe* stripe, WithLock stripeLock, const CreationInfo& info);

    /**
     * Determines if 'bucket' needs to be rolled over to accomodate 'doc'. If so, determines whether
     * to archive or close 'bucket'.
     */
    std::pair<RolloverAction, RolloverReason> _determineRolloverAction(
        OperationContext* opCtx,
        const BSONObj& doc,
        CreationInfo* info,
        Bucket* bucket,
        Bucket::NewFieldNames& newFieldNamesToBeInserted,
        int32_t& sizeToBeAdded,
        AllowBucketCreation mode);

    /**
     * Close the existing, full bucket and open a new one for the same metadata.
     *
     * Writes information about the closed bucket to the 'info' parameter.
     */
    Bucket* _rollover(Stripe* stripe,
                      WithLock stripeLock,
                      Bucket* bucket,
                      const CreationInfo& info,
                      RolloverAction action);

    ExecutionStatsController _getExecutionStats(const NamespaceString& ns);
    std::shared_ptr<ExecutionStats> _getExecutionStats(const NamespaceString& ns) const;

    void _appendExecutionStatsToBuilder(const ExecutionStats* stats, BSONObjBuilder* builder) const;

    /**
     * Calculates the marginal memory usage for an archived bucket. The
     * 'onlyEntryForMatchingMetaHash' parameter indicates that the bucket will be (if inserting)
     * or was (if removing) the only bucket associated with it's meta hash value. If true, then
     * the returned value will attempt to account for the overhead of the map data structure for
     * the meta hash value.
     */
    static long long _marginalMemoryUsageForArchivedBucket(const ArchivedBucket& bucket,
                                                           bool onlyEntryForMatchingMetaHash);

    /**
     * Updates stats to reflect the status of bucket fetches and queries based off of the FindResult
     * (which is populated when attempting to reopen a bucket).
     */
    void _updateBucketFetchAndQueryStats(ExecutionStatsController& stats,
                                         const BucketFindResult& findResult);

    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "BucketCatalog::_mutex");

    BucketStateManager _bucketStateManager{&_mutex};

    static constexpr std::size_t kNumberOfStripes = 32;
    std::array<Stripe, kNumberOfStripes> _stripes;

    // Per-namespace execution stats. This map is protected by '_mutex'. Once you complete your
    // lookup, you can keep the shared_ptr to an individual namespace's stats object and release the
    // lock. The object itself is thread-safe (using atomics).
    stdx::unordered_map<NamespaceString, std::shared_ptr<ExecutionStats>> _executionStats;

    // Global execution stats used to report aggregated metrics in server status.
    ExecutionStats _globalExecutionStats;

    // Approximate memory usage of the bucket catalog.
    AtomicWord<uint64_t> _memoryUsage;

    // Approximate cardinality of opened and archived buckets.
    AtomicWord<uint32_t> _numberOfActiveBuckets;

    class ServerStatus;
};
}  // namespace mongo::timeseries::bucket_catalog
