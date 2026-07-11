// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_committer.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_coordinator.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/flush_all_files_observer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <mutex>
#include <string_view>

#include <boost/container/flat_map.hpp>
#include <boost/optional/optional.hpp>


namespace mongo::replicated_fast_count {

/**
 * Singleton `ServiceContext` decoration that facilitates initializing and committing collection
 * size and counts. This class also manages the lifetime of the background threads used for
 * writing collection size and count to disk.
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
class [[MONGO_MOD_PUBLIC]] ReplicatedFastCountManager : public FlushAllFilesObserver {
public:
    [[MONGO_MOD_PRIVATE]] ReplicatedFastCountManager(
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
     * Creates the checkpoint coordinator and starts its background threads. Skips starting the
     * coordinator's threads when _isUnderTest is true.
     *
     * This function is idempotent when the coordinator is already running.
     */
    void startup(OperationContext* opCtx);

    /**
     * Signals the checkpoint coordinator to stop and flushes final changes synchronously.
     *
     * This function is idempotent since shutdown() may be called when the coordinator was never
     * started or has already been shut down.
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
     * Signals the checkpointer thread to perform a flush.
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

    /**
     * Disables periodic background writes of metadata for testing purposes. Must be called before
     * startup().
     */
    void disablePeriodicWrites_ForTest();

    /**
     * Returns true if the checkpoint coordinator has been started and not yet shut down.
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

    /**
     * Decides how to start the fast count system when there is no persisted checkpoint timestamp
     * ('cold start'). Returns `{skipScan, startFrom}`:
     *
     * When the lag between the oldest entry in the oplog and the stable recovery timestamp is
     * within `replicatedFastCountMaxOplogScanLagSecs`, returns `{false, Timestamp::min()}` to
     * signal that we should catch up by scanning from the beginning of the oplog.
     *
     * Otherwise, returns `{true, ...}` to signal that we should skip the catch-up scan.
     *
     * 'returnTimestampToSeekFromIfSkippingScan' controls whether this function should return a
     * timestamp to seek from when we have too much oplog to scan. If true, it will return the
     * timestamp returned by seeking the stable recovery timestamp. If there is no
     * last applied optime, returns Timestamp::min() - indicating that we are assuming there is no
     * oplog to scan and it should be fine to scan from the beginning.
     *
     * If 'returnTimestampToSeekFromIfSkippingScan' is false and `skipScan` is true, returns
     * boost::none for the timestamp.
     *
     * TODO SERVER-130675: Remove this function and its usages once we never skip the oplog scan.
     */
    std::pair<bool, boost::optional<Timestamp>> _computeColdStartTimestamp(
        OperationContext* opCtx, bool returnTimestampToSeekFromIfSkippingScan);

    /**
     * Used to force synchronous writes in tests.
     */
    bool _isUnderTest = false;

    /**
     * Interface for reads / writes to the fast count metadata store.
     */
    std::unique_ptr<SizeCountStore> _sizeCountStore;

    /**
     * Interface for reads / writes to the fast count timestamp store.
     */
    std::unique_ptr<SizeCountTimestampStore> _timestampStore;

    /**
     * Guards _checkpointer.
     */
    mutable std::mutex _checkpointerMutex;

    /**
     * Maintains the oplog tailer and checkpoint flusher threads. The lifetime of _checkpointer
     * is equal to one primary term.
     */
    std::unique_ptr<SizeCountCheckpointCoordinator> _checkpointer;
};

}  // namespace mongo::replicated_fast_count
