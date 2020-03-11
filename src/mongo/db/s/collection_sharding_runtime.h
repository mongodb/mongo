/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/decorable.h"

namespace mongo {

extern AtomicWord<int> migrationLockAcquisitionMaxWaitMS;

/**
 * See the comments for CollectionShardingState for more information on how this class fits in the
 * sharding architecture.
 */
class CollectionShardingRuntime final : public CollectionShardingState,
                                        public Decorable<CollectionShardingRuntime> {
public:
    CollectionShardingRuntime(ServiceContext* sc,
                              NamespaceString nss,
                              std::shared_ptr<executor::TaskExecutor> rangeDeleterExecutor);

    CollectionShardingRuntime(const CollectionShardingRuntime&) = delete;
    CollectionShardingRuntime& operator=(const CollectionShardingRuntime&) = delete;

    using CSRLock = ShardingStateLock<CollectionShardingRuntime>;

    /**
     * Obtains the sharding runtime state for the specified collection. If it does not exist, it
     * will be created and will remain active until the collection is dropped or unsharded.
     *
     * Must be called with some lock held on the specific collection being looked up and the
     * returned pointer should never be stored.
     */
    static CollectionShardingRuntime* get(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * It is the caller's responsibility to ensure that the collection locks for this namespace are
     * held when this is called. The returned pointer should never be stored.
     */
    static CollectionShardingRuntime* get_UNSAFE(ServiceContext* svcCtx,
                                                 const NamespaceString& nss);

    /**
     * Waits for all ranges deletion tasks with UUID 'collectionUuid' overlapping range
     * 'orphanRange' to be processed, even if the collection does not exist in the storage catalog.
     */
    static Status waitForClean(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const UUID& collectionUuid,
                               ChunkRange orphanRange);

    ScopedCollectionFilter getOwnershipFilter(OperationContext* opCtx,
                                              OrphanCleanupPolicy orphanCleanupPolicy) override;

    ScopedCollectionDescription getCollectionDescription() override;

    boost::optional<ScopedCollectionDescription> getCurrentMetadataIfKnown() override;

    boost::optional<ChunkVersion> getCurrentShardVersionIfKnown() override;

    void checkShardVersionOrThrow(OperationContext* opCtx) override;

    Status checkShardVersionNoThrow(OperationContext* opCtx) noexcept override;

    void enterCriticalSectionCatchUpPhase(OperationContext* opCtx) override;

    void enterCriticalSectionCommitPhase(OperationContext* opCtx) override;

    void exitCriticalSection(OperationContext* opCtx) override;

    std::shared_ptr<Notification<void>> getCriticalSectionSignal(
        ShardingMigrationCriticalSection::Operation op) const override;

    void setFilteringMetadata(OperationContext* opCtx, CollectionMetadata newMetadata) override;

    void appendInfoForServerStatus(BSONArrayBuilder* builder) override;

    /**
     * Marks the collection's filtering metadata as UNKNOWN, meaning that all attempts to check for
     * shard version match will fail with StaleConfig errors in order to trigger an update.
     *
     * It is safe to call this method with only an intent lock on the collection (as opposed to
     * setFilteringMetadata which requires exclusive), however note that clearing a collection's
     * filtering metadata will interrupt all in-progress orphan cleanups in which case orphaned data
     * will remain behind on disk.
     */
    void clearFilteringMetadata();

    /**
     * Schedules any documents in `range` for immediate cleanup iff no running queries can depend
     * on them, and adds the range to the list of ranges being received.
     *
     * Returns a future that will be resolved when the deletion has completed or failed.
     */
    SharedSemiFuture<void> beginReceive(ChunkRange const& range);

    /*
     * Removes `range` from the list of ranges being received, and schedules any documents in the
     * range for immediate cleanup. Does not block.
     */
    void forgetReceive(const ChunkRange& range);

    /**
     * Schedules documents in `range` for cleanup after any running queries that may depend on them
     * have terminated. Does not block. Fails if range overlaps any current local shard chunk.
     * Passed kDelayed, an additional delay (configured via server parameter orphanCleanupDelaySecs)
     * is added to permit (most) dependent queries on secondaries to complete, too.
     *
     * Returns a future that will be resolved when the deletion completes or fails. If that
     * succeeds, waitForClean can be called to ensure no other deletions are pending for the range.
     */
    enum CleanWhen { kNow, kDelayed };
    SharedSemiFuture<void> cleanUpRange(ChunkRange const& range,
                                        boost::optional<UUID> migrationId,
                                        CleanWhen when);

    /**
     * Returns a range _not_ owned by this shard that starts no lower than the specified
     * startingFrom key value, if any, or boost::none if there is no such range.
     */
    boost::optional<ChunkRange> getNextOrphanRange(BSONObj const& startingFrom);

    /**
     * BSON output of the pending metadata into a BSONArray
     */
    void toBSONPending(BSONArrayBuilder& bb) const override {
        _metadataManager->toBSONPending(bb);
    }

    std::uint64_t getNumMetadataManagerChanges_forTest() {
        return _numMetadataManagerChanges;
    }

private:
    friend CSRLock;

    /**
     * Returns the latest version of collection metadata with filtering configured for
     * atClusterTime if specified.
     */
    boost::optional<ScopedCollectionDescription> _getCurrentMetadataIfKnown(
        const boost::optional<LogicalTime>& atClusterTime);

    /**
     * Returns the latest version of collection metadata with filtering configured for
     * atClusterTime if specified. Throws StaleConfigInfo if the shard version attached to the
     * operation context does not match the shard version on the active metadata object.
     */
    boost::optional<ScopedCollectionDescription> _getMetadataWithVersionCheckAt(
        OperationContext* opCtx, const boost::optional<mongo::LogicalTime>& atClusterTime);

    // Namespace this state belongs to.
    const NamespaceString _nss;

    // The executor used for deleting ranges of orphan chunks.
    std::shared_ptr<executor::TaskExecutor> _rangeDeleterExecutor;

    // Object-wide ResourceMutex to protect changes to the CollectionShardingRuntime or objects held
    // within (including the MigrationSourceManager, which is a decoration on the CSR). Use only the
    // CSRLock to lock this mutex.
    Lock::ResourceMutex _stateChangeMutex;

    // Tracks the migration critical section state for this collection.
    ShardingMigrationCriticalSection _critSec;

    mutable Mutex _metadataManagerLock =
        MONGO_MAKE_LATCH("CollectionShardingRuntime::_metadataManagerLock");

    // Tracks whether the filtering metadata is unknown, unsharded, or sharded
    enum class MetadataType {
        kUnknown,
        kUnsharded,
        kSharded
    } _metadataType{MetadataType::kUnknown};

    // If the collection is sharded, contains all the metadata associated with this collection.
    //
    // If the collection is unsharded, the metadata has not been set yet, or the metadata has been
    // specifically reset by calling clearFilteringMetadata(), this will be nullptr;
    std::shared_ptr<MetadataManager> _metadataManager;

    // Used for testing to check the number of times a new MetadataManager has been installed.
    std::uint64_t _numMetadataManagerChanges{0};
};

/**
 * RAII-style class, which obtains a reference to the critical section for the specified collection.
 */
class CollectionCriticalSection {
    CollectionCriticalSection(const CollectionCriticalSection&) = delete;
    CollectionCriticalSection& operator=(const CollectionCriticalSection&) = delete;

public:
    CollectionCriticalSection(OperationContext* opCtx, NamespaceString ns);
    ~CollectionCriticalSection();

    /**
     * Enters the commit phase of the critical section and blocks reads.
     */
    void enterCommitPhase();

private:
    NamespaceString _nss;

    OperationContext* _opCtx;
};

}  // namespace mongo
