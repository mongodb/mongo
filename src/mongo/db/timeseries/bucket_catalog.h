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
#include "mongo/db/timeseries/minmax.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BucketCatalog {
    struct ExecutionStats;

    // Number of new field names we can hold in NewFieldNames without needing to allocate memory.
    static constexpr std::size_t kNumStaticNewFields = 10;
    using NewFieldNames = boost::container::small_vector<StringMapHashedKey, kNumStaticNewFields>;

public:
    class Bucket;

    enum class CombineWithInsertsFromOtherClients {
        kAllow,
        kDisallow,
    };

    struct CommitInfo {
        boost::optional<repl::OpTime> opTime;
        boost::optional<OID> electionId;
    };

    /**
     * Information of a Bucket that got closed when performing an operation on this BucketCatalog.
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

        WriteBatch(const OID& bucketId,
                   OperationId opId,
                   const std::shared_ptr<ExecutionStats>& stats);

        /**
         * Attempt to claim the right to commit (or abort) a batch. If it returns true, rights are
         * granted. If it returns false, rights are revoked, and the caller should get the result
         * of the batch with getResult(). Non-blocking.
         */
        bool claimCommitRights();

        /**
         * Retrieve the result of the write batch commit. Should be called by any interested party
         * that does not have commit rights. Blocking.
         */
        StatusWith<CommitInfo> getResult() const;

        const OID& bucketId() const;

        const std::vector<BSONObj>& measurements() const;
        const BSONObj& min() const;
        const BSONObj& max() const;
        const StringMap<std::size_t>& newFieldNamesToBeInserted() const;
        uint32_t numPreviouslyCommittedMeasurements() const;

        /**
         * Whether the batch is active and can be written to.
         */
        bool active() const;

        /**
         * Whether the batch has been committed or aborted.
         */
        bool finished() const;

        BSONObj toBSON() const;

    private:
        /**
         * Add a measurement. Active batches only.
         */
        void _addMeasurement(const BSONObj& doc);

        /**
         * Record a set of new-to-the-bucket fields. Active batches only.
         */
        void _recordNewFields(NewFieldNames&& fields);

        /**
         * Prepare the batch for commit. Sets min/max appropriately, records the number of documents
         * that have previously been committed to the bucket, and marks the batch inactive. Must
         * have commit rights.
         */
        void _prepareCommit(Bucket* bucket);

        /**
         * Report the result and status of a commit, and notify anyone waiting on getResult(). Must
         * have commit rights. Inactive batches only.
         */
        void _finish(const CommitInfo& info);

        /**
         * Abandon the write batch and notify any waiters that the bucket has been cleared. Must
         * have commit rights. Parameter 'bucket' provides a pointer to the bucket if still
         * available, nullptr otherwise.
         */
        void _abort(const boost::optional<Status>& status, const Bucket* bucket);

        const OID _bucketId;
        OperationId _opId;
        std::shared_ptr<ExecutionStats> _stats;

        std::vector<BSONObj> _measurements;
        BSONObj _min;  // Batch-local min; full if first batch, updates otherwise.
        BSONObj _max;  // Batch-local max; full if first batch, updates otherwise.
        uint32_t _numPreviouslyCommittedMeasurements = 0;
        StringMap<std::size_t> _newFieldNamesToBeInserted;  // Value is hash of string key

        bool _active = true;

        AtomicWord<bool> _commitRights{false};
        SharedPromise<CommitInfo> _promise;
    };

    /**
     * Return type for the insert function.
     * See comment above insert() for more information.
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
    BSONObj getMetadata(const OID& bucketId) const;

    /**
     * Returns the WriteBatch into which the document was inserted and optional information about a
     * bucket if one was closed. Any caller who receives the same batch may commit or abort the
     * batch after claiming commit rights. See WriteBatch for more details.
     */
    StatusWith<InsertResult> insert(OperationContext* opCtx,
                                    const NamespaceString& ns,
                                    const StringData::ComparatorInterface* comparator,
                                    const TimeseriesOptions& options,
                                    const BSONObj& doc,
                                    CombineWithInsertsFromOtherClients combine);

    /**
     * Prepares a batch for commit, transitioning it to an inactive state. Caller must already have
     * commit rights on batch. Returns true if the batch was successfully prepared, or false if the
     * batch was aborted.
     */
    bool prepareCommit(std::shared_ptr<WriteBatch> batch);

    /**
     * Records the result of a batch commit. Caller must already have commit rights on batch, and
     * batch must have been previously prepared.
     *
     * Returns bucket information of a bucket if one was closed.
     */
    boost::optional<ClosedBucket> finish(std::shared_ptr<WriteBatch> batch, const CommitInfo& info);

    /**
     * Aborts the given write batch and any other outstanding batches on the same bucket. Caller
     * must already have commit rights on batch. Uses the provided status when clearing the bucket,
     * or TimeseriesBucketCleared if not provided.
     */
    void abort(std::shared_ptr<WriteBatch> batch,
               const boost::optional<Status>& status = boost::none);

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
    /**
     * This class provides a mutex with shared and exclusive locking semantics. Unlike some shared
     * mutex implementations, it does not allow for writer starvation (assuming the underlying
     * Mutex implemenation does not allow for starvation). The underlying mechanism is simply an
     * array of Mutex instances. To take a shared lock, a thread's ID is hashed, mapping the thread
     * to a particular mutex, which is then locked. To take an exclusive lock, all mutexes are
     * locked.
     *
     * This behavior makes it easy to allow concurrent read access while still allowing writes to
     * occur safely with exclusive access. It should only be used for situations where observed
     * access patterns are read-mostly.
     *
     * A shared lock *cannot* be upgraded to an exclusive lock.
     */
    class StripedMutex {
    public:
        static constexpr std::size_t kNumStripes = 16;
        StripedMutex() = default;

        using SharedLock = stdx::unique_lock<Mutex>;
        SharedLock lockShared() const;

        class ExclusiveLock {
        public:
            ExclusiveLock() = default;
            explicit ExclusiveLock(const StripedMutex&);

        private:
            std::array<stdx::unique_lock<Mutex>, kNumStripes> _locks;
        };
        ExclusiveLock lockExclusive() const;

    private:
        mutable std::array<Mutex, kNumStripes> _mutexes;
    };

    struct BucketMetadata {
    public:
        BucketMetadata() = default;
        BucketMetadata(BSONElement elem, const StringData::ComparatorInterface* comparator);

        // Constructs with a copy of the metadata.
        BucketMetadata(BSONElement elem,
                       BSONObj obj,
                       const StringData::ComparatorInterface* comparator,
                       bool normalized = false,
                       bool copied = true);

        bool normalized() const {
            return _normalized;
        }
        void normalize();

        bool operator==(const BucketMetadata& other) const;

        const BSONObj& toBSON() const;

        const BSONElement getMetaElement() const;

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
        // When BucketMetadata does not own the '_metadata', only '_metadataElement' will be present
        // and used to look up buckets. After owning the '_metadata,' the field should refer to the
        // BSONElement within '_metadata'.
        BSONElement _metadataElement;

        // Empty when just looking up buckets. Owns a copy when the field is present.
        BSONObj _metadata;
        const StringData::ComparatorInterface* _comparator = nullptr;
        bool _normalized = false;
        bool _copied = false;
    };

    using IdleList = std::list<Bucket*>;

public:
    class Bucket {
    public:
        friend class BucketAccess;
        friend class BucketCatalog;

        Bucket(const OID& id);

        /**
         * Returns the ID for the underlying bucket.
         */
        const OID& id() const;

        /**
         * Returns the timefield for the underlying bucket.
         */
        StringData getTimeField();

        /**
         * Returns whether all measurements have been committed.
         */
        bool allCommitted() const;

        /**
         * Returns total number of measurements in the bucket.
         */
        uint32_t numMeasurements() const;

    private:
        /**
         * Determines the effect of adding 'doc' to this bucket. If adding 'doc' causes this bucket
         * to overflow, we will create a new bucket and recalculate the change to the bucket size
         * and data fields.
         */
        void _calculateBucketFieldsAndSizeChange(const BSONObj& doc,
                                                 boost::optional<StringData> metaField,
                                                 NewFieldNames* newFieldNamesToBeInserted,
                                                 uint32_t* newFieldNamesSize,
                                                 uint32_t* sizeToBeAdded) const;

        /**
         * Returns whether BucketCatalog::commit has been called at least once on this bucket.
         */
        bool _hasBeenCommitted() const;

        /**
         * Return a pointer to the current, open batch.
         */
        std::shared_ptr<WriteBatch> _activeBatch(OperationId opId,
                                                 const std::shared_ptr<ExecutionStats>& stats);

        // Access to the bucket is controlled by this lock
        mutable Mutex _mutex =
            MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(2), "BucketCatalog::Bucket::_mutex");

        // The bucket ID for the underlying document
        const OID _id;

        // The namespace that this bucket is used for.
        NamespaceString _ns;

        // The metadata of the data that this bucket contains.
        BucketMetadata _metadata;

        // Extra metadata combinations that are supported without normalizing the metadata object.
        static constexpr std::size_t kNumFieldOrderCombinationsWithoutNormalizing = 1;
        boost::container::static_vector<BSONObj, kNumFieldOrderCombinationsWithoutNormalizing>
            _nonNormalizedKeyMetadatas;

        // Top-level field names of the measurements that have been inserted into the bucket.
        StringSet _fieldNames;

        // Time field for the measurements that have been inserted into the bucket.
        std::string _timeField;

        // The minimum and maximum values for each field in the bucket.
        timeseries::MinMax _minmax;

        // The latest time that has been inserted into the bucket.
        Date_t _latestTime;

        // The total size in bytes of the bucket's BSON serialization, including measurements to be
        // inserted.
        uint64_t _size = 0;

        // The total number of measurements in the bucket, including uncommitted measurements and
        // measurements to be inserted.
        uint32_t _numMeasurements = 0;

        // The number of committed measurements in the bucket.
        uint32_t _numCommittedMeasurements = 0;

        // Whether the bucket is full. This can be due to number of measurements, size, or time
        // range.
        bool _full = false;

        // The batch that has been prepared and is currently in the process of being committed, if
        // any.
        std::shared_ptr<WriteBatch> _preparedBatch;

        // Batches, per operation, that haven't been committed or aborted yet.
        stdx::unordered_map<OperationId, std::shared_ptr<WriteBatch>> _batches;

        // If the bucket is in _idleBuckets, then its position is recorded here.
        boost::optional<IdleList::iterator> _idleListEntry = boost::none;

        // Approximate memory usage of this bucket.
        uint64_t _memoryUsage = sizeof(*this);
    };

private:
    struct ExecutionStats {
        AtomicWord<long long> numBucketInserts;
        AtomicWord<long long> numBucketUpdates;
        AtomicWord<long long> numBucketsOpenedDueToMetadata;
        AtomicWord<long long> numBucketsClosedDueToCount;
        AtomicWord<long long> numBucketsClosedDueToSize;
        AtomicWord<long long> numBucketsClosedDueToTimeForward;
        AtomicWord<long long> numBucketsClosedDueToTimeBackward;
        AtomicWord<long long> numBucketsClosedDueToMemoryThreshold;
        AtomicWord<long long> numCommits;
        AtomicWord<long long> numWaits;
        AtomicWord<long long> numMeasurementsCommitted;
    };

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

    /**
     * Key to lookup open Bucket for namespace and metadata.
     */
    struct BucketKey {
        NamespaceString ns;
        BucketMetadata metadata;

        /**
         * Creates a new BucketKey with a different internal metadata object.
         */
        BucketKey withCopiedMetadata(BSONObj meta) const {
            return {ns, {meta.firstElement(), meta, metadata.getComparator()}};
        }

        bool operator==(const BucketKey& other) const {
            return ns == other.ns && metadata == other.metadata;
        }

        template <typename H>
        friend H AbslHashValue(H h, const BucketKey& key) {
            return H::combine(std::move(h), key.ns, key.metadata);
        }
    };

    /**
     * BucketKey with pre-calculated hash. To avoiding calculating the hash while holding locks.
     *
     * The unhashed BucketKey is stored inside HashedBucketKey by reference and must not go out of
     * scope for the lifetime of the returned HashedBucketKey.
     */
    struct HashedBucketKey {
        operator BucketKey() const {
            return *key;
        }
        const BucketKey* key;
        std::size_t hash;
    };

    /**
     * Hasher to support heterogeneous lookup for BucketKey and HashedBucketKey.
     */
    struct BucketHasher {
        // This using directive activates heterogeneous lookup in the hash table
        using is_transparent = void;

        std::size_t operator()(const BucketKey& key) const {
            // Use the default absl hasher.
            return absl::Hash<BucketKey>{}(key);
        }

        std::size_t operator()(const HashedBucketKey& key) const {
            return key.hash;
        }

        /**
         * Pre-calculates a hashed BucketKey.
         */
        HashedBucketKey hashed_key(const BucketKey& key) {
            return HashedBucketKey{&key, operator()(key)};
        }
    };

    /**
     * Equality, provides comparison between hashed and unhashed bucket keys.
     */
    struct BucketEq {
        // This using directive activates heterogeneous lookup in the hash table
        using is_transparent = void;

        bool operator()(const BucketKey& lhs, const BucketKey& rhs) const {
            return lhs == rhs;
        }
        bool operator()(const BucketKey& lhs, const HashedBucketKey& rhs) const {
            return lhs == *rhs.key;
        }
        bool operator()(const HashedBucketKey& lhs, const BucketKey& rhs) const {
            return *lhs.key == rhs;
        }
        bool operator()(const HashedBucketKey& lhs, const HashedBucketKey& rhs) const {
            return *lhs.key == *rhs.key;
        }
    };

    /**
     * Helper class to handle all the locking necessary to lookup and lock a bucket for use. This
     * is intended primarily for using a single bucket, including replacing it when it becomes full.
     * If the usage pattern iterates over several buckets, you will instead want to use raw access
     * using the different mutexes with the locking semantics described below.
     */
    class BucketAccess {
    public:
        BucketAccess() = delete;
        BucketAccess(BucketCatalog* catalog,
                     BucketKey& key,
                     const TimeseriesOptions& options,
                     ExecutionStats* stats,
                     ClosedBuckets* closedBuckets,
                     const Date_t& time);
        BucketAccess(BucketCatalog* catalog,
                     const OID& bucketId,
                     boost::optional<BucketState> targetState = boost::none);
        ~BucketAccess();

        bool isLocked() const;
        Bucket* operator->();
        operator bool() const;
        operator Bucket*() const;

        // Release the bucket lock, typically in order to reacquire the catalog lock.
        void release();

        /**
         * Close the existing, full bucket and open a new one for the same metadata.
         * Parameter is a function which should check that the bucket is indeed still full after
         * reacquiring the necessary locks. The first parameter will give the function access to
         * this BucketAccess instance, with the bucket locked.
         *
         * Returns bucket information of a bucket if one was closed.
         */
        void rollover(const std::function<bool(BucketAccess*)>& isBucketFull,
                      ClosedBuckets* closedBuckets);

        // Retrieve the time associated with the bucket (id)
        Date_t getTime() const;

    private:
        /**
         * Returns the state of the bucket, or boost::none if there is no state for the bucket.
         */
        boost::optional<BucketState> _getBucketState() const;

        /**
         * Helper to find and lock an open bucket for the given metadata if it exists. Takes a
         * shared lock on the catalog. Returns the state of the bucket if it is locked and usable.
         * In case the bucket does not exist or was previously cleared and thus is not usable, the
         * return value will be BucketState::kCleared.
         */
        BucketState _findOpenBucketThenLock(const HashedBucketKey& key);

        /**
         * Same as _findOpenBucketThenLock above but takes an exclusive lock on the catalog. In
         * addition to finding the bucket it also store a non-normalized key if there are available
         * slots in the bucket.
         */
        BucketState _findOpenBucketThenLockAndStoreKey(const HashedBucketKey& normalizedKey,
                                                       const HashedBucketKey& key,
                                                       BSONObj metadata);

        /**
         * Helper to determine the state of the bucket that is found by _findOpenBucketThenLock and
         * _findOpenBucketThenLockAndStoreKey. Requires the bucket lock to be acquired before
         * calling this function and it may release the lock depending on the state.
         */
        BucketState _confirmStateForAcquiredBucket();

        // Helper to find an open bucket for the given metadata if it exists, create it if it
        // doesn't, and lock it. Requires an exclusive lock on the catalog.
        void _findOrCreateOpenBucketThenLock(const HashedBucketKey& normalizedKey,
                                             const HashedBucketKey& key,
                                             ClosedBuckets* closedBuckets);

        // Lock _bucket.
        void _acquire();

        // Allocate a new bucket in the catalog, set the local state to that bucket, and acquire
        // a lock on it.
        void _create(const HashedBucketKey& normalizedKey,
                     const HashedBucketKey& key,
                     ClosedBuckets* closedBuckets,
                     bool openedDuetoMetadata = true);

        BucketCatalog* _catalog;
        BucketKey* _key = nullptr;
        const TimeseriesOptions* _options = nullptr;
        ExecutionStats* _stats = nullptr;
        const Date_t* _time = nullptr;

        Bucket* _bucket = nullptr;
        stdx::unique_lock<Mutex> _guard;
    };

    class ServerStatus;

    StripedMutex::SharedLock _lockShared() const;
    StripedMutex::ExclusiveLock _lockExclusive() const;

    void _waitToCommitBatch(const std::shared_ptr<WriteBatch>& batch);

    /**
     * Removes the given bucket from the bucket catalog's internal data structures.
     */
    bool _removeBucket(Bucket* bucket, bool expiringBuckets);

    /**
     * Removes extra non-normalized BucketKey's for the given bucket from the
     * bucket catalog's internal data structures.
     */
    void _removeNonNormalizedKeysForBucket(Bucket* bucket);

    /**
     * Aborts any batches it can for the given bucket, then removes the bucket. If batch is
     * non-null, it is assumed that the caller has commit rights for that batch.
     */
    void _abort(stdx::unique_lock<Mutex>& lk,
                Bucket* bucket,
                std::shared_ptr<WriteBatch> batch,
                const boost::optional<Status>& status);

    /**
     * Adds the bucket to a list of idle buckets to be expired at a later date
     */
    void _markBucketIdle(Bucket* bucket);

    /**
     * Remove the bucket from the list of idle buckets. The second parameter encodes whether the
     * caller holds a lock on _idleMutex.
     */
    void _markBucketNotIdle(Bucket* bucket, bool locked);

    /**
     * Verify the bucket is currently unused by taking a lock on it. Must hold exclusive lock from
     * the outside for the result to be meaningful.
     */
    void _verifyBucketIsUnused(Bucket* bucket) const;

    /**
     * Expires idle buckets until the bucket catalog's memory usage is below the expiry threshold.
     */
    void _expireIdleBuckets(ExecutionStats* stats, ClosedBuckets* closedBuckets);

    std::size_t _numberOfIdleBuckets() const;

    // Allocate a new bucket (and ID) and add it to the catalog
    Bucket* _allocateBucket(const BucketKey& key,
                            const Date_t& time,
                            const TimeseriesOptions& options,
                            ExecutionStats* stats,
                            ClosedBuckets* closedBuckets,
                            bool openedDuetoMetadata);

    std::shared_ptr<ExecutionStats> _getExecutionStats(const NamespaceString& ns);
    const std::shared_ptr<ExecutionStats> _getExecutionStats(const NamespaceString& ns) const;

    /**
     * Changes the bucket state, taking into account the current state, the specified target state,
     * and allowed state transitions. The return value, if set, is the final state of the bucket
     * with the given id; if no such bucket exists, the return value will not be set.
     *
     * Ex. For a bucket with state kPrepared, and a target of kCleared, the return will be
     * kPreparedAndCleared.
     */
    boost::optional<BucketState> _setBucketState(const OID& id, BucketState target);

    /**
     * You must hold a lock on _bucketMutex when accessing _allBuckets or _openBuckets.
     * While holding a lock on _bucketMutex, you can take a lock on an individual bucket, then
     * release _bucketMutex. Any iterators on the protected structures should be considered invalid
     * once the lock is released. Any subsequent access to the structures requires relocking
     * _bucketMutex. You must *not* be holding a lock on a bucket when you attempt to acquire the
     * lock on _mutex, as this can result in deadlock.
     *
     * The StripedMutex class has both shared (read-only) and exclusive (write) locks. If you are
     * going to write to any of the protected structures, you must hold an exclusive lock.
     *
     * Typically, if you want to acquire a bucket, you should use the BucketAccess RAII
     * class to do so, as it will take care of most of this logic for you. Only use the _bucketMutex
     * directly for more global maintenance where you want to take the lock once and interact with
     * multiple buckets atomically.
     */
    mutable StripedMutex _bucketMutex;

    // All buckets currently in the catalog, including buckets which are full but not yet committed.
    stdx::unordered_map<OID, std::unique_ptr<Bucket>, OID::Hasher> _allBuckets;

    // The current open bucket for each namespace and metadata pair.
    stdx::unordered_map<BucketKey, Bucket*, BucketHasher, BucketEq> _openBuckets;

    // Bucket state
    mutable Mutex _statesMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "BucketCatalog::_statesMutex");
    stdx::unordered_map<OID, BucketState, OID::Hasher> _bucketStates;

    // This mutex protects access to _idleBuckets
    mutable Mutex _idleMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(1), "BucketCatalog::_idleMutex");

    // Buckets that do not have any writers.
    IdleList _idleBuckets;

    /**
     * This mutex protects access to the _executionStats map. Once you complete your lookup, you
     * can keep the shared_ptr to an individual namespace's stats object and release the lock. The
     * object itself is thread-safe (using atomics).
     */
    mutable StripedMutex _statsMutex;

    // Per-namespace execution stats.
    stdx::unordered_map<NamespaceString, std::shared_ptr<ExecutionStats>> _executionStats;

    // A placeholder to be returned in case a namespace has no allocated statistics object
    static const std::shared_ptr<ExecutionStats> kEmptyStats;

    // Counter for buckets created by the bucket catalog.
    uint64_t _bucketNum = 0;

    // Approximate memory usage of the bucket catalog.
    AtomicWord<uint64_t> _memoryUsage;
};
}  // namespace mongo
