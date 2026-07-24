// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/counter.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Tracks statistics for work related to authoritative collection metadata recovery.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] CollectionShardingMetadataStatistics {
public:
    void report(BSONObjBuilder& builder) const {
        builder.append("countMetadataSynchronizersCreated",
                       _countMetadataSynchronizersCreated.get());
        builder.append("countDiskRecoveriesPerformed", _countDiskRecoveriesPerformed.get());
        builder.append("countVersionMismatchResolutions", _countVersionMismatchResolutions.get());
        builder.append("countPostRecoveryWaitsResolvedByConfigTime",
                       _countPostRecoveryWaitsResolvedByConfigTime.get());
        builder.append("countPostRecoveryWaitsResolvedByVersionChange",
                       _countPostRecoveryWaitsResolvedByVersionChange.get());
        builder.append("totalPostRecoveryWaitMillis", _totalPostRecoveryWaitMillis.get());
        builder.append("totalDiskRecoveryMillis", _totalDiskRecoveryMillis.get());
        builder.append("countInvalidateCollectionMetadataOplogEntriesApplied",
                       _countInvalidateCollectionMetadataOplogEntriesApplied.get());
        builder.append("countInvalidateCollectionMetadataOplogEntriesForDroppedCollections",
                       _countInvalidateCollectionMetadataOplogEntriesForDroppedCollections.get());
        builder.append("countUpdateCollectionMetadataOplogEntriesApplied",
                       _countUpdateCollectionMetadataOplogEntriesApplied.get());
        builder.append("countUpdateCollectionMetadataChangedChunksApplied",
                       _countUpdateCollectionMetadataChangedChunksApplied.get());
        builder.append("countSetAllowChunkOperationsOplogEntriesApplied",
                       _countSetAllowChunkOperationsOplogEntriesApplied.get());
        builder.append("countCollectionMetadataCacheClears",
                       _countCollectionMetadataCacheClears.get());
        builder.append("countCollectionMetadataCacheSets", _countCollectionMetadataCacheSets.get());
        builder.append("countLocalCollectionMetadataCommits",
                       _countLocalCollectionMetadataCommits.get());
        builder.append("countLocalCollectionMetadataClones",
                       _countLocalCollectionMetadataClones.get());
        builder.append("countLocalCollectionMetadataDrops",
                       _countLocalCollectionMetadataDrops.get());
        builder.append("countLocalCollectionMetadataRenames",
                       _countLocalCollectionMetadataRenames.get());
    }

    void registerMetadataSynchronizerCreated() {
        _countMetadataSynchronizersCreated.incrementRelaxed();
    }

    void registerDiskRecovery() {
        _countDiskRecoveriesPerformed.incrementRelaxed();
    }

    void registerVersionMismatchResolved() {
        _countVersionMismatchResolutions.incrementRelaxed();
    }

    void registerPostRecoveryWaitResolvedByConfigTime() {
        _countPostRecoveryWaitsResolvedByConfigTime.incrementRelaxed();
    }

    void registerPostRecoveryWaitResolvedByVersionChange() {
        _countPostRecoveryWaitsResolvedByVersionChange.incrementRelaxed();
    }

    void registerPostRecoveryWaitMillis(long long millis) {
        _totalPostRecoveryWaitMillis.incrementRelaxed(millis);
    }

    void registerDiskRecoveryMillis(long long millis) {
        _totalDiskRecoveryMillis.incrementRelaxed(millis);
    }

    void registerInvalidateCollectionMetadataOplogEntryApplied(bool forDroppedCollection) {
        _countInvalidateCollectionMetadataOplogEntriesApplied.incrementRelaxed();
        if (forDroppedCollection) {
            _countInvalidateCollectionMetadataOplogEntriesForDroppedCollections.incrementRelaxed();
        }
    }

    void registerUpdateCollectionMetadataOplogEntryApplied(long long numChangedChunks) {
        _countUpdateCollectionMetadataOplogEntriesApplied.incrementRelaxed();
        _countUpdateCollectionMetadataChangedChunksApplied.incrementRelaxed(numChangedChunks);
    }

    void registerSetAllowChunkOperationsOplogEntryApplied() {
        _countSetAllowChunkOperationsOplogEntriesApplied.incrementRelaxed();
    }

    void registerCollectionMetadataCacheClear() {
        _countCollectionMetadataCacheClears.incrementRelaxed();
    }

    void registerCollectionMetadataCacheSet() {
        _countCollectionMetadataCacheSets.incrementRelaxed();
    }

    void registerLocalCollectionMetadataCommit() {
        _countLocalCollectionMetadataCommits.incrementRelaxed();
    }

    void registerLocalCollectionMetadataClone() {
        _countLocalCollectionMetadataClones.incrementRelaxed();
    }

    void registerLocalCollectionMetadataDrop() {
        _countLocalCollectionMetadataDrops.incrementRelaxed();
    }

    void registerLocalCollectionMetadataRename() {
        _countLocalCollectionMetadataRenames.incrementRelaxed();
    }

private:
    // CollectionMetadataSynchronizer instances registered under the CSR exclusive lock.
    Counter64 _countMetadataSynchronizersCreated;
    // Completed on-disk authoritative collection metadata recovery rounds.
    Counter64 _countDiskRecoveriesPerformed;
    // Authoritative shard-version mismatches resolved (cached CSS sufficient, or after recovery /
    // configTime wait).
    Counter64 _countVersionMismatchResolutions;
    // Post-recovery waits that completed once cluster configTime advanced.
    Counter64 _countPostRecoveryWaitsResolvedByConfigTime;
    // Post-recovery waits that completed when the CSS placement version changed.
    Counter64 _countPostRecoveryWaitsResolvedByVersionChange;
    // Wall-clock time spent in post-recovery configTime/version waits.
    Counter64 _totalPostRecoveryWaitMillis;
    // Wall-clock time spent in on-disk recovery (snapshot wait + drain + install).
    Counter64 _totalDiskRecoveryMillis;
    // invalidateCollectionMetadata ('c') oplog entries applied on replication secondaries.
    Counter64 _countInvalidateCollectionMetadataOplogEntriesApplied;
    // Subset of the above for dropped/untracked collection clears.
    Counter64 _countInvalidateCollectionMetadataOplogEntriesForDroppedCollections;
    // updateCollectionMetadata ('c') oplog entries applied on replication secondaries.
    Counter64 _countUpdateCollectionMetadataOplogEntriesApplied;
    // Total changed chunks merged by those updateCollectionMetadata delta applications.
    Counter64 _countUpdateCollectionMetadataChangedChunksApplied;
    // setAllowChunkOperations ('c') oplog entries applied on replication secondaries.
    Counter64 _countSetAllowChunkOperationsOplogEntriesApplied;
    // In-memory clears of the per-collection metadata cache (CSS).
    Counter64 _countCollectionMetadataCacheClears;
    // In-memory sets of the per-collection metadata cache (CSS).
    Counter64 _countCollectionMetadataCacheSets;
    // Local shard-catalog collection commits (shardCollection / reshard / refresh).
    Counter64 _countLocalCollectionMetadataCommits;
    // Local shard-catalog collection clones during FCV upgrade (cloneAuthoritativeMetadata).
    Counter64 _countLocalCollectionMetadataClones;
    // Local shard-catalog collection drops.
    Counter64 _countLocalCollectionMetadataDrops;
    // Local shard-catalog collection renames.
    Counter64 _countLocalCollectionMetadataRenames;
};
}  // namespace mongo
