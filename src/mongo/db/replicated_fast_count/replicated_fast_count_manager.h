/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_committer.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/uuid.h"

#include <boost/container/flat_map.hpp>
#include <boost/optional/optional.hpp>


namespace mongo {

/**
 * Maintains an in-memory cache of the size and count of all collections.
 *
 * Terminology:
 * The term "fast count" is historical, but should be equivalent to "fast size"
 * and "fast sizecount" to refer to a cached, and therefore "fast", size or count
 * value for a collection.
 *
 *
 * This cache is intended to be accurate, and helps the server avoid expensive
 * collection scans to compute these values. The validate command can check and repair
 * the fast count and size with various flags to the command, like --enforceFastCount.
 *
 * Backing the in-memory cache is a pair of collections used for recovery scenarios.
 *
 * This class and its backing collections are a singleton that creates its internal
 * collection(s) once, the first time a mongod creates its data files. It is assumed
 * these backing collections exist from then on.
 *
 * The backing collections are expected to exist before starting up the ReplicatedFastCountManager
 * background thread.
 *
 * This class is thread-safe, and synchronizes access to the in-memory SizeCount cache,
 * i.e. _metadata.
 *
 * The write path should generally not depend directly on this class, because it relies on
 * the collection write path to persist fast SizeCounts. Instead, operations should
 * interact with this class through UncommittedFastCountChange.
 */
class MONGO_MOD_PUBLIC ReplicatedFastCountManager {
    struct StoredSizeCount {
        CollectionSizeCount sizeCount;
        bool dirty{false};  // Indicates if flush is needed.
        Timestamp validAsOf;
    };

    using FastSizeCountMap = absl::flat_hash_map<UUID, StoredSizeCount>;

public:
    static ReplicatedFastCountManager& get(ServiceContext* svcCtx);

    ReplicatedFastCountManager() {
        initializeFastCountCommitFn();
    }

    /**
     * Registers the fast count commit function that will be called on commit to apply the changes
     * to the in-memory metadata. This function is initialized in this way to avoid introducing a
     * circular dependency by having the UncommittedFastCountChange class depend directly on
     * ReplicatedFastCountManager, since the former is depended on by the collection write path and
     * the latter depends on the collection write path.
     */
    void initializeFastCountCommitFn();

    /**
     * Spawns fastcount thread.
     * Skips running thread when _isUnderTest.
     */
    void startup(OperationContext* opCtx);

    /**
     * Signals fastcount thread to stop and flushes final changes synchronously.
     *
     * WARNING: This function should be called exactly once per startup() call. Calling shutdown()
     * more than once on the same background thread is an assertion error.
     */
    void shutdown(OperationContext* opCtx);

    /**
     * Initializes state for the ReplicatedFastCountManager. Populates in-memory _metadata values
     * with those persisted on disk. Should be performed once per start-up.
     */
    void initializeMetadata(OperationContext* opCtx);

    /**
     * Records committed changes to the size and count for the collections in 'changes'.
     */
    void commit(const boost::container::flat_map<UUID, CollectionSizeCount>& changes,
                boost::optional<Timestamp> commitTime);

    /**
     * Given a collection UUID, returns the last committed value of size and count for that
     * collection.
     */
    CollectionSizeCount find(const UUID& uuid) const;

    /**
     * Returns the persisted number of records (count) and data size for the collection with `uuid`.
     */
    CollectionSizeCount findPersisted(OperationContext* opCtx, const UUID& uuid) const;

    /**
     * Signals the background thread to perform a flush.
     *
     * This flush involves snapshotting and writing dirty in-memory SizeCounts to the internal
     * fastcount collection on disk.
     */
    void flushAsync();

    /**
     * Flushes data synchronously on the caller's thread. The calling thread must be able to take a
     * MODE_IX lock.
     *
     * This function is useful in testing and during shutdown when flushing must happen
     * synchronously to ensure a predictable order of events.
     */
    void flushSync(OperationContext* opCtx);

    ReplicatedFastCountMetrics& getReplicatedFastCountMetrics() {
        return _metrics;
    }

    /**
     * Disables periodic background writes of metadata for testing purposes. Must be called before
     * startup().
     */
    void disablePeriodicWrites_ForTest();

    /**
     * Returns true if the fastcount thread is running.
     */
    bool isRunning_ForTest();

private:
    /**
     * Centralized point for flushing logic.
     * TODO SERVER-123284: Remove 'dirtyMetadata' parameter once there is only one flush mechanism.
     */
    void _doFlush(OperationContext* opCtx, const FastSizeCountMap& dirtyMetadata);

    /**
     * Return a copy of a subset of _metadata, only including the dirty entries. Clears the dirty
     * flags for all currently dirty entries.
     */
    FastSizeCountMap _getAndClearSnapshotOfDirtyMetadata(WithLock metadataLock);

    /**
     * Write out dirtyMetadata to the `config.fast_count_metadata_store`. TODO SERVER-123284: Remove
     * these methods.
     */
    void _flushDirtyMetadata(OperationContext* opCtx, const FastSizeCountMap& dirtyMetadata);

    /**
     * Runs background thread, performing final flush.
     */
    void _startBackgroundThread(ServiceContext* svcCtx);

    /**
     * Flushes dirty metadata when signalled.
     * Sleeps on a condition variable - _backgroundThreadReadyForFlush - waiting for _flushRequested
     * to be true.
     */
    void _flushPeriodicallyOnSignal();

    /**
     * Write one collection's sizeCount to disk. Note: These are specific to the legacy flush
     * mechanism turned on and off by '_useLegacyFlush'. TODO SERVER-123284: Remove these methods.
     */
    void _writeOneMetadata(OperationContext* opCtx,
                           const CollectionPtr& fastCountColl,
                           const UUID& uuid,
                           const CollectionSizeCount& sizeCount,
                           const Timestamp& validAsOfTS,
                           const RecordId& recordId);

    void _updateOneMetadata(OperationContext* opCtx,
                            const CollectionPtr& fastCountColl,
                            const Snapshotted<BSONObj>& doc,
                            const UUID& uuid,
                            const CollectionSizeCount& sizeCount,
                            const Timestamp& validAsOfTS,
                            const RecordId& recordId);
    void _insertOneMetadata(OperationContext* opCtx,
                            const CollectionPtr& fastCountColl,
                            const UUID& uuid,
                            const CollectionSizeCount& sizeCount,
                            const Timestamp& validAsOfTS);

    /**
     * Populates the in-memory values of _metadata with the values persisted in the internal fast
     * count collection. Returns the number of records that were scanned.
     */
    int _hydrateMetadataFromDisk(OperationContext* opCtx,
                                 const CollectionOrViewAcquisition& acquisition);

    /**
     * Formats and returns the document to write to the fastcount collection.
     */
    BSONObj _getDocForWrite(const UUID& uuid,
                            const CollectionSizeCount& sizeCount,
                            const Timestamp& validAsOfTS) const;

    /**
     * Generates a key (RecordId) into the fastcount collection given a user
     * collection uuid.
     */
    RecordId _keyForUUID(const UUID& uuid) const;
    UUID _UUIDForKey(RecordId key) const;

    // Metrics for the ReplicatedFastCountManager reported via both serverStatus and OTel.
    //
    // Metrics are shared between ReplicatedFastCountManager instances. We assume there is exactly
    // one ReplicatedFastCountManager per mongod process.
    static inline ReplicatedFastCountMetrics _metrics;

    StringData _threadName = "replicatedSizeCount"_sd;
    stdx::thread _backgroundThread;
    Atomic<bool> _isEnabled = false;
    stdx::condition_variable _backgroundThreadReadyForFlush;
    bool _isUnderTest = false;  // Used to force synchronous writes in tests.

    /**
     * When true, utilizes the legacy flush mechanism which flushes the dirtied in-memory `metadata`
     * to the `config.fast_count_metadata_store`. Notably, this does not update
     * `config.fast_count_metadata_timestamp_store`.
     *
     * When false, utilizes a more robust flush mechanism which advances the logical metadata
     * checkpoint by writing to both `config.fast_count_metadata_store` and
     * `config.fast_count_metadata_timestamp_store`.
     *
     * TODO SERVER-123284: Remove this flag and the legacy mechanism.
     */
    bool _useLegacyFlush{false};

    /**
     * Interface for reads / writes to the `config.fast_count_metadata_store`.
     */
    replicated_fast_count::SizeCountStore _sizeCountStore;

    /**
     * Interface for reads / writes to the `config.fast_count_metadata_timestamp_store`.
     */
    replicated_fast_count::SizeCountTimestampStore _timestampStore;

    /**
     * In-memory cache of committed fast sizes & counts since last checkpoint.
     * Implemented as a map of collection UUID to the last committed size and count.
     */
    mutable ObservableMutex<stdx::mutex> _metadataMutex;
    FastSizeCountMap _metadata;
    bool _flushRequested = false;  // Prevents spurious wakeups.
};

}  // namespace mongo
