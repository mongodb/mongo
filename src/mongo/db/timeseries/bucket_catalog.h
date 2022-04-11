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

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/flat_bson.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BucketCatalog {
    // Number of new field names we can hold in NewFieldNames without needing to allocate memory.
    static constexpr std::size_t kNumStaticNewFields = 10;
    using NewFieldNames = boost::container::small_vector<StringMapHashedKey, kNumStaticNewFields>;

    using StripeNumber = std::uint8_t;

    struct BucketHandle {
        const OID id;
        const StripeNumber stripe;
    };

    struct ExecutionStats;
    class Bucket;
    struct CreationInfo;

public:
    enum class CombineWithInsertsFromOtherClients {
        kAllow,
        kDisallow,
    };

    struct CommitInfo {
        boost::optional<repl::OpTime> opTime;
        boost::optional<OID> electionId;
    };

    /**
     * Information of a Bucket that got closed while performing an operation on this BucketCatalog.
     */
    struct ClosedBucket {
        OID bucketId;
        std::string timeField;
        uint32_t numMeasurements;
    };
    using ClosedBuckets = std::vector<ClosedBucket>;

    /**
     * The basic unit of work for a bucket. Each insert will return a shared_ptr to a WriteBatch.
     * When a writer is finished with all their insertions, they should then take steps to ensure
     * each batch they wrote into is committed. To ensure a batch is committed, a writer should
     * first attempt to claimCommitRights(). If successful, the writer can proceed to commit (or
     * abort) the batch via BucketCatalog::prepareCommit and BucketCatalog::finish. If unsuccessful,
     * it means another writer is in the process of committing. The writer can proceed to do other
     * work (like commit another batch), and when they have no other work to do, they can wait for
     * this batch to be committed by executing the blocking operation getResult().
     */
    class WriteBatch {
        friend class BucketCatalog;

    public:
        WriteBatch() = delete;

        WriteBatch(const BucketHandle& bucketId,
                   OperationId opId,
                   const std::shared_ptr<ExecutionStats>& stats);

        /**
         * Attempts to claim the right to commit a batch. If it returns true, rights are
         * granted. If it returns false, rights are revoked, and the caller should get the result
         * of the batch with getResult(). Non-blocking.
         */
        bool claimCommitRights();

        /**
         * Retrieves the result of the write batch commit. Should be called by any interested party
         * that does not have commit rights. Blocking.
         */
        StatusWith<CommitInfo> getResult() const;

        /**
         * Returns a handle which can be used by the BucketCatalog internally to locate its record
         * for this bucket.
         */
        const BucketHandle& bucket() const;

        const std::vector<BSONObj>& measurements() const;
        const BSONObj& min() const;
        const BSONObj& max() const;
        const StringMap<std::size_t>& newFieldNamesToBeInserted() const;
        uint32_t numPreviouslyCommittedMeasurements() const;

        /**
         * Returns whether the batch has already been committed or aborted.
         */
        bool finished() const;

        BSONObj toBSON() const;

    private:
        /**
         * Adds a measurement. Active batches only.
         */
        void _addMeasurement(const BSONObj& doc);

        /**
         * Records a set of new-to-the-bucket fields. Active batches only.
         */
        void _recordNewFields(NewFieldNames&& fields);

        /**
         * Prepares the batch for commit. Sets min/max appropriately, records the number of
         * documents that have previously been committed to the bucket, and renders the batch
         * inactive. Must have commit rights.
         */
        void _prepareCommit(Bucket* bucket);

        /**
         * Reports the result and status of a commit, and notifies anyone waiting on getResult().
         * Must have commit rights. Inactive batches only.
         */
        void _finish(const CommitInfo& info);

        /**
         * Abandons the write batch and notifies any waiters that the bucket has been cleared.
         */
        void _abort(const Status& status);

        const BucketHandle _bucket;
        OperationId _opId;
        std::shared_ptr<ExecutionStats> _stats;

        std::vector<BSONObj> _measurements;
        BSONObj _min;  // Batch-local min; full if first batch, updates otherwise.
        BSONObj _max;  // Batch-local max; full if first batch, updates otherwise.
        uint32_t _numPreviouslyCommittedMeasurements = 0;
        StringMap<std::size_t> _newFieldNamesToBeInserted;  // Value is hash of string key

        AtomicWord<bool> _commitRights{false};
        SharedPromise<CommitInfo> _promise;
    };

    /**
     * Return type for the insert function. See insert() for more information.
     */
    struct InsertResult {
        std::shared_ptr<WriteBatch> batch;
        ClosedBuckets closedBuckets;
    };

    static BucketCatalog& get(ServiceContext* svcCtx);
    static BucketCatalog& get(OperationContext* opCtx);

    BucketCatalog() = default;

    BucketCatalog(const BucketCatalog&) = delete;
    BucketCatalog operator=(const BucketCatalog&) = delete;

    /**
     * Returns the metadata for the given bucket in the following format:
     *     {<metadata field name>: <value>}
     * All measurements in the given bucket share same metadata value.
     *
     * Returns an empty document if the given bucket cannot be found or if this time-series
     * collection was not created with a metadata field name.
     */
    BSONObj getMetadata(const BucketHandle& bucket) const;

    /**
     * Returns the WriteBatch into which the document was inserted and a list of any buckets that
     * were closed in order to make space to insert the document. Any caller who receives the same
     * batch may commit or abort the batch after claiming commit rights. See WriteBatch for more
     * details.
     */
    StatusWith<InsertResult> insert(OperationContext* opCtx,
                                    const NamespaceString& ns,
                                    const StringData::ComparatorInterface* comparator,
                                    const TimeseriesOptions& options,
                                    const BSONObj& doc,
                                    CombineWithInsertsFromOtherClients combine);

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
     * Marks any bucket with the specified OID as cleared and prevents any future inserts from
     * landing in that bucket.
     */
    void clear(const OID& oid);

    /**
     * Clears any bucket whose namespace satisfies the predicate.
     */
    void clear(const std::function<bool(const NamespaceString&)>& shouldClear);

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

private:
    enum class BucketState {
        // Bucket can be inserted into, and does not have an outstanding prepared commit
        kNormal,
        // Bucket can be inserted into, and has a prepared commit outstanding.
        kPrepared,
        // Bucket can no longer be inserted into, does not have an outstanding prepared
        // commit.
        kCleared,
        // Bucket can no longer be inserted into, but still has an outstanding
        // prepared commit. Any writer other than the one who prepared the
        // commit should receive a WriteConflictException.
        kPreparedAndCleared,
    };

    struct BucketMetadata {
    public:
        BucketMetadata() = default;
        BucketMetadata(BSONElement elem, const StringData::ComparatorInterface* comparator);

        bool operator==(const BucketMetadata& other) const;

        const BSONObj& toBSON() const;

        StringData getMetaField() const;

        const StringData::ComparatorInterface* getComparator() const;

        template <typename H>
        friend H AbslHashValue(H h, const BucketMetadata& metadata) {
            return H::combine(
                std::move(h),
                absl::Hash<absl::string_view>()(absl::string_view(
                    metadata._metadataElement.value(), metadata._metadataElement.valuesize())));
        }

    private:
        // Only the value of '_metadataElement' is used for hashing and comparison.
        BSONElement _metadataElement;

        // Empty if metadata field isn't present, owns a copy otherwise.
        BSONObj _metadata;

        const StringData::ComparatorInterface* _comparator = nullptr;
    };

    /**
     * Key to lookup open Bucket for namespace and metadata, with pre-computed hash.
     */
    struct BucketKey {
        BucketKey() = delete;
        BucketKey(const NamespaceString& nss, const BucketMetadata& meta);

        NamespaceString ns;
        BucketMetadata metadata;
        std::size_t hash;

        bool operator==(const BucketKey& other) const {
            return ns == other.ns && metadata == other.metadata;
        }

        template <typename H>
        friend H AbslHashValue(H h, const BucketKey& key) {
            return H::combine(std::move(h), key.ns, key.metadata);
        }
    };

    /**
     * Hasher to support pre-computed hash lookup for BucketKey.
     */
    struct BucketHasher {
        std::size_t operator()(const BucketKey& key) const;
    };

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
        stdx::unordered_map<OID, std::unique_ptr<Bucket>, OID::Hasher> allBuckets;

        // The current open bucket for each namespace and metadata pair.
        stdx::unordered_map<BucketKey, Bucket*, BucketHasher> openBuckets;

        // Buckets that do not have any outstanding writes.
        using IdleList = std::list<Bucket*>;
        IdleList idleBuckets;
    };

    StripeNumber _getStripeNumber(const BucketKey& key);

    /**
     * Mode enum to control whether the bucket retrieval methods below will return buckets that are
     * in kCleared or kPreparedAndCleared state.
     */
    enum class ReturnClearedBuckets { kYes, kNo };

    /**
     * Retrieve a bucket for read-only use.
     */
    const Bucket* _findBucket(const Stripe& stripe,
                              WithLock stripeLock,
                              const OID& id,
                              ReturnClearedBuckets mode = ReturnClearedBuckets::kNo) const;

    /**
     * Retrieve a bucket for write use.
     */
    Bucket* _useBucket(Stripe* stripe,
                       WithLock stripeLock,
                       const OID& id,
                       ReturnClearedBuckets mode);

    /**
     * Retrieve a bucket for write use, setting the state in the process.
     */
    Bucket* _useBucketInState(Stripe* stripe,
                              WithLock stripeLock,
                              const OID& id,
                              BucketState targetState);

    /**
     * Retrieve a bucket for write use, or create one if a suitable bucket doesn't already exist.
     */
    Bucket* _useOrCreateBucket(Stripe* stripe, WithLock stripeLock, const CreationInfo& info);

    /**
     * Wait for other batches to finish so we can prepare 'batch'
     */
    void _waitToCommitBatch(Stripe* stripe, const std::shared_ptr<WriteBatch>& batch);

    /**
     * Removes the given bucket from the bucket catalog's internal data structures.
     */
    bool _removeBucket(Stripe* stripe, WithLock stripeLock, Bucket* bucket);

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
                            ExecutionStats* stats,
                            ClosedBuckets* closedBuckets);

    /**
     * Allocates a new bucket and adds it to the catalog.
     */
    Bucket* _allocateBucket(Stripe* stripe, WithLock stripeLock, const CreationInfo& info);

    /**
     * Close the existing, full bucket and open a new one for the same metadata.
     *
     * Writes information about the closed bucket to the 'info' parameter.
     */
    Bucket* _rollover(Stripe* stripe,
                      WithLock stripeLock,
                      Bucket* bucket,
                      const CreationInfo& info);

    std::shared_ptr<ExecutionStats> _getExecutionStats(const NamespaceString& ns);
    std::shared_ptr<ExecutionStats> _getExecutionStats(const NamespaceString& ns) const;

    /**
     * Retreives the bucket state if it is tracked in the catalog.
     */
    boost::optional<BucketState> _getBucketState(const OID& id) const;

    /**
     * Initializes state for the given bucket to kNormal.
     */
    void _initializeBucketState(const OID& id);

    /**
     * Remove state for the given bucket from the catalog.
     */
    void _eraseBucketState(const OID& id);

    /**
     * Changes the bucket state, taking into account the current state, the specified target state,
     * and allowed state transitions. The return value, if set, is the final state of the bucket
     * with the given id; if no such bucket exists, the return value will not be set.
     *
     * Ex. For a bucket with state kPrepared, and a target of kCleared, the return will be
     * kPreparedAndCleared.
     */
    boost::optional<BucketState> _setBucketState(const OID& id, BucketState target);

    static constexpr std::size_t kNumberOfStripes = 32;
    std::array<Stripe, kNumberOfStripes> _stripes;

    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "BucketCatalog::_mutex");

    // Bucket state for synchronization with direct writes, protected by '_mutex'
    stdx::unordered_map<OID, BucketState, OID::Hasher> _bucketStates;

    // Per-namespace execution stats. This map is protected by '_mutex'. Once you complete your
    // lookup, you can keep the shared_ptr to an individual namespace's stats object and release the
    // lock. The object itself is thread-safe (using atomics).
    stdx::unordered_map<NamespaceString, std::shared_ptr<ExecutionStats>> _executionStats;

    // Approximate memory usage of the bucket catalog.
    AtomicWord<uint64_t> _memoryUsage;

    class ServerStatus;
};
}  // namespace mongo
