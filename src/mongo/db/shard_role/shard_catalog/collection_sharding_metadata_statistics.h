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

#include "mongo/base/counter.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Tracks statistics for work related to authoritative collection metadata recovery.
 */
class MONGO_MOD_PARENT_PRIVATE CollectionShardingMetadataStatistics {
public:
    void report(BSONObjBuilder& builder) const {
        builder.append("countRecoverersCreated", _countRecoverersCreated.get());
        builder.append("countDiskRecoveriesPerformed", _countDiskRecoveriesPerformed.get());
        builder.append("countVersionMismatchResolutionsBeforeRecovery",
                       _countVersionMismatchResolutionsBeforeRecovery.get());
        builder.append("countVersionMismatchResolutionsAfterRecovery",
                       _countVersionMismatchResolutionsAfterRecovery.get());
        builder.append("countPostRecoveryWaitsResolvedByConfigTime",
                       _countPostRecoveryWaitsResolvedByConfigTime.get());
        builder.append("countPostRecoveryWaitsResolvedByVersionChange",
                       _countPostRecoveryWaitsResolvedByVersionChange.get());
        builder.append("totalPostRecoveryWaitMillis", _totalPostRecoveryWaitMillis.get());
        builder.append("totalDiskRecoveryMillis", _totalDiskRecoveryMillis.get());
        builder.append("countDiskRecoveryNoProgressRetries",
                       _countDiskRecoveryNoProgressRetries.get());
        builder.append("countDiskRecoveryAttemptsExhausted",
                       _countDiskRecoveryAttemptsExhausted.get());
        builder.append("countInvalidateCollectionMetadataOplogEntriesApplied",
                       _countInvalidateCollectionMetadataOplogEntriesApplied.get());
        builder.append("countInvalidateCollectionMetadataOplogEntriesForDroppedCollections",
                       _countInvalidateCollectionMetadataOplogEntriesForDroppedCollections.get());
        builder.append("countApplyCollectionShardingStateDeltaOplogEntriesApplied",
                       _countApplyCollectionShardingStateDeltaOplogEntriesApplied.get());
        builder.append("countSetAllowChunkOperationsOplogEntriesApplied",
                       _countSetAllowChunkOperationsOplogEntriesApplied.get());
        builder.append("countLocalCollectionMetadataCommits",
                       _countLocalCollectionMetadataCommits.get());
        builder.append("countLocalCollectionMetadataClones",
                       _countLocalCollectionMetadataClones.get());
        builder.append("countLocalCollectionMetadataDrops",
                       _countLocalCollectionMetadataDrops.get());
        builder.append("countLocalCollectionMetadataRenames",
                       _countLocalCollectionMetadataRenames.get());
    }

    void registerRecovererCreated() {
        _countRecoverersCreated.incrementRelaxed();
    }

    void registerDiskRecovery() {
        _countDiskRecoveriesPerformed.incrementRelaxed();
    }

    void registerVersionMismatchResolvedBeforeRecovery() {
        _countVersionMismatchResolutionsBeforeRecovery.incrementRelaxed();
    }

    void registerVersionMismatchResolvedAfterRecovery() {
        _countVersionMismatchResolutionsAfterRecovery.incrementRelaxed();
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

    void registerDiskRecoveryNoProgressRetry() {
        _countDiskRecoveryNoProgressRetries.incrementRelaxed();
    }

    void registerDiskRecoveryAttemptsExhausted() {
        _countDiskRecoveryAttemptsExhausted.incrementRelaxed();
    }

    void registerInvalidateCollectionMetadataOplogEntryApplied(bool forDroppedCollection) {
        _countInvalidateCollectionMetadataOplogEntriesApplied.incrementRelaxed();
        if (forDroppedCollection) {
            _countInvalidateCollectionMetadataOplogEntriesForDroppedCollections.incrementRelaxed();
        }
    }

    void registerApplyCollectionShardingStateDeltaOplogEntryApplied() {
        _countApplyCollectionShardingStateDeltaOplogEntriesApplied.incrementRelaxed();
    }

    void registerSetAllowChunkOperationsOplogEntryApplied() {
        _countSetAllowChunkOperationsOplogEntriesApplied.incrementRelaxed();
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
    // CollectionCacheRecoverer instances registered under the CSR exclusive lock.
    Counter64 _countRecoverersCreated;
    // Completed on-disk authoritative collection metadata recovery rounds.
    Counter64 _countDiskRecoveriesPerformed;
    // Mismatches resolved without reading disk (cached CSS already sufficient).
    Counter64 _countVersionMismatchResolutionsBeforeRecovery;
    // Mismatches resolved after on-disk recovery (CSS now matches router view).
    Counter64 _countVersionMismatchResolutionsAfterRecovery;
    // Post-recovery waits that completed once cluster configTime advanced.
    Counter64 _countPostRecoveryWaitsResolvedByConfigTime;
    // Post-recovery waits that completed when the CSS placement version changed.
    Counter64 _countPostRecoveryWaitsResolvedByVersionChange;
    // Wall-clock time spent in post-recovery configTime/version waits.
    Counter64 _totalPostRecoveryWaitMillis;
    // Wall-clock time spent in on-disk recovery (snapshot wait + drain + install).
    Counter64 _totalDiskRecoveryMillis;
    // Recovery-loop iterations that made no progress against the no-progress budget.
    Counter64 _countDiskRecoveryNoProgressRetries;
    // Recoveries that exhausted the no-progress budget and surfaced StaleConfig.
    Counter64 _countDiskRecoveryAttemptsExhausted;
    // invalidateCollectionMetadata ('c') oplog entries applied on replication secondaries.
    Counter64 _countInvalidateCollectionMetadataOplogEntriesApplied;
    // Subset of the above for dropped/untracked collection clears.
    Counter64 _countInvalidateCollectionMetadataOplogEntriesForDroppedCollections;
    // applyCollectionShardingStateDelta ('c') oplog entries applied on replication secondaries.
    Counter64 _countApplyCollectionShardingStateDeltaOplogEntriesApplied;
    // setAllowChunkOperations ('c') oplog entries applied on replication secondaries.
    Counter64 _countSetAllowChunkOperationsOplogEntriesApplied;
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
