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
#include "mongo/db/replicated_fast_count/size_count_checkpoint_coordinator.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/flush_all_files_observer.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/uuid.h"

#include <mutex>
#include <string_view>

#include <boost/container/flat_map.hpp>
#include <boost/optional/optional.hpp>


namespace mongo::replicated_fast_count {

/**
 * Singleton `ServiceContext` decoration that facilitates committing and flushing replicated
 * collection size and count changes.
 *
 * Terminology:
 * - Collection "count" refers to the number of documents in a collection.
 * - Collection "size" refers to the sum of the number of bytes in each documentat in a collection.
 * - The size and count are "replicated" because they are persisted in the oplog.
 * - The term "fast count" is historical but synonymous with "fast size" and "fast size count." All
 *   three terms refer to a cached, and therefore "fast," size or count value for a collection.
 *
 * Committed collection size and counts are accessible through the `Collection` and `RecordStore`
 * APIs.
 *
 * In the event of unclean shutdown, the oplog and two backing stores, `fast_count_metadata_store`
 * and `fast_count_metadata_timestamps_store`, are used to recover the correct collection size and
 * counts.
 */
class MONGO_MOD_PUBLIC ReplicatedFastCountManager : public FlushAllFilesObserver {
public:
    MONGO_MOD_PRIVATE ReplicatedFastCountManager(
        std::unique_ptr<SizeCountStore> sizeCountStore,
        std::unique_ptr<SizeCountTimestampStore> timestampStore)
        : _sizeCountStore(std::move(sizeCountStore)), _timestampStore(std::move(timestampStore)) {
        invariant(_sizeCountStore);
        invariant(_timestampStore);
        initializeFastCountCommitFn();
    }

    static ReplicatedFastCountManager& get(ServiceContext* svcCtx);

    ReplicatedFastCountManager()
        : _sizeCountStore(std::make_unique<CollectionSizeCountStore>()),
          _timestampStore(std::make_unique<CollectionSizeCountTimestampStore>()) {
        initializeFastCountCommitFn();
    }

    /**
     * Initializes the stores in container mode with the given RecordStores. Ownership of each
     * RecordStore is transferred into the corresponding SizeCount[Timestamp]Store member. Must be
     * called before startup().
     */
    void initializeContainerStores(std::unique_ptr<RecordStore> metadataRS,
                                   std::unique_ptr<RecordStore> timestampsRS);

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
     *
     * This function is idempotent when the background thread is already running. See implementation
     * for more details.
     */
    void startup(OperationContext* opCtx);

    /**
     * Signals fastcount thread to stop and flushes final changes synchronously.
     *
     * This function is idempotent since we cannot gaurantee that the background thread is joinable
     * during shutdown(). See implementation for more details.
     */
    void shutdown(OperationContext* opCtx);

    /**
     * Initializes the in-memory collection size/count information stored in each collection's
     * `RecordStore`.
     *
     * This function combines the persisted size/count for each collection with any additional
     * size/count updates in the oplog since the last checkpoint.
     *
     * Should be called once per startup, after oplog recovery and `CollectionCatalog`
     * initialization. If no replicated size/count store exists, this function does nothing.
     */
    void initializeMetadata(OperationContext* opCtx);

    /**
     * Adjusts each collection's `RecordStore` by the corresponding delta in `changes`.
     *
     * This function updates the in-memory representation of each collection's size and count only.
     * It does not write anything to disk.
     *
     * Any UUID in `changes` not found in the collection catalog is skipped.
     */
    void commit(OperationContext* opCtx,
                const boost::container::flat_map<UUID, CollectionSizeCount>& changes);

    /**
     * Returns the persisted singleton timestamp from the timestamp store, or boost::none if the
     * store has no entry. Used by initial sync to read the donor's checkpoint timestamp.
     *
     * This returns an optional because it is possible that we try find the timestamp before the
     * first flush persists one to disk.
     *
     * The caller must hold a MODE_IS GlobalLock.
     */
    boost::optional<Timestamp> findPersistedTimestampStoreTs(OperationContext* opCtx) const;

    /**
     * Returns the persisted size/count and its `validAsOf` timestamp for the collection with
     * `uuid`, or boost::none if no entry exists for that UUID.
     *
     * This returns an optional because it is possible that we try to find a uuid that is present in
     * the catalog but doesn't have a persisted fast count entry because it hasn't been flushed yet.
     *
     * The caller must hold a MODE_IS GlobalLock.
     */
    boost::optional<std::pair<CollectionSizeCount, Timestamp>> findPersisted(
        OperationContext* opCtx, UUID uuid) const;

    /**
     * Signals the background thread to perform a flush.
     *
     * This flush involves snapshotting and writing dirty in-memory SizeCounts to the internal
     * fastcount collection on disk.
     */
    void flushAsync();

    /**
     * FlushAllFilesObserver hook. Triggers an asynchronous flush when invoked.
     */
    void onFlushAllFiles() override {
        flushAsync();
    }

    /**
     * Flushes data synchronously on the caller's thread. The calling thread must be able to take a
     * MODE_IX lock. Requires periodic writes to be disabled.
     */
    void flushSync_ForTest(OperationContext* opCtx);

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

    /**
     * Returns true if this manager is using the container-backed path for the size count store.
     * Intended for tests that need to pick which on-disk read path to exercise.
     */
    bool usesContainers_ForTest() const;

    /**
     * Returns raw pointers to the metadata and timestamp SizeCount[Timestamp]Store's. Intended for
     * tests that need access to the underlying RecordStore objects when testing the container path.
     */
    std::pair<SizeCountStore*, SizeCountTimestampStore*> getSizeCountStores_ForTest() const;

private:
    /**
     * Centralized point for flushing logic.
     */
    void _doFlush(OperationContext* opCtx);

    /**
     * Runs background thread, performing final flush.
     */
    void _startBackgroundThread(ServiceContext* svcCtx);

    /**
     * Flushes updates to size and count when signalled.
     * Sleeps on a condition variable - _backgroundThreadReadyForFlush - waiting for _flushRequested
     * to be true.
     */
    void _flushPeriodicallyOnSignal();

    using SizeCountAccumulator = absl::flat_hash_map<UUID, CollectionSizeCount>;

    /**
     * Populates the `accumulator` with the values persisted in the internal fast count collection.
     * Returns the number of records that were scanned.
     */
    int _hydrateMetadataFromCollection(OperationContext* opCtx,
                                       SizeCountAccumulator& accumulator,
                                       const CollectionOrViewAcquisition& acquisition);

    /**
     * Same as _hydrateMetadataFromCollection, but reads from the container's backing RecordStore.
     */
    int _hydrateMetadataFromContainer(OperationContext* opCtx,
                                      SizeCountAccumulator& accumulator,
                                      const RecordStore::RecordStoreContainer& recordStore);

    UUID _UUIDForKey(RecordId key) const;

    // Metrics for the ReplicatedFastCountManager reported via both serverStatus and OTel.
    //
    // Metrics are shared between ReplicatedFastCountManager instances. We assume there is exactly
    // one ReplicatedFastCountManager per mongod process.
    static inline ReplicatedFastCountMetrics _metrics;

    std::string_view _threadName = "replicatedSizeCount"_sd;
    stdx::thread _backgroundThread;
    Atomic<bool> _isEnabled = false;
    stdx::condition_variable _backgroundThreadReadyForFlush;
    bool _isUnderTest = false;  // Used to force synchronous writes in tests.

    /**
     * Guards _checkpointer, _backgroundThread, and _isEnabled across startup(), shutdown(), and
     * flushAsync(). Holding this lock through the idempotency check + state assignment in startup()
     * and through the move-to-claim in shutdown() makes those transitions self-protecting — no
     * reliance on external FCV/replication lock ordering for memory safety.
     *
     * Lock ordering: _lifecycleMutex is never acquired while _flushMutex is held. They are always
     * acquired separately, never nested.
     */
    mutable ObservableMutex<std::mutex> _lifecycleMutex;

    /**
     * Guarded by _lifecycleMutex.
     */
    std::unique_ptr<SizeCountCheckpointCoordinator> _checkpointer;

    /**
     * Interface for reads / writes to the fast count metadata store.
     */
    std::unique_ptr<SizeCountStore> _sizeCountStore;

    /**
     * Interface for reads / writes to the fast count timestamp store.
     */
    std::unique_ptr<SizeCountTimestampStore> _timestampStore;

    /**
     * Synchronizes the background flush thread with `flushAsync()` and `shutdown()`. Guards
     * `_flushRequested` and is paired with `_backgroundThreadReadyForFlush`.
     */
    mutable ObservableMutex<std::mutex> _flushMutex;
    bool _flushRequested = false;  // Prevents spurious wakeups.
};

}  // namespace mongo::replicated_fast_count
