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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/in_progress_time_accumulator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/chunk_operations_statistics.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_metadata_statistics.h"
#include "mongo/db/shard_role/shard_catalog/critical_section_statistics.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_metadata_statistics.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

namespace mongo {

class OperationContext;
class ServiceContext;

/**
 * Encapsulates per-process statistics for the sharding subsystem.
 */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardingStatistics {
    // Counts how many times threads hit stale config exception (which is what triggers metadata
    // refreshes).
    Atomic<long long> countStaleConfigErrors{0};

    // Cumulative, always-increasing counter of how many chunks this node has started to donate
    // (whether they succeeded or not).
    Atomic<long long> countDonorMoveChunkStarted{0};

    // Cumulative, always-increasing counter of how many chunks this node successfully committed.
    Atomic<long long> countDonorMoveChunkCommitted{0};

    // Cumulative, always-increasing counter of how many move chunks this node aborted.
    Atomic<long long> countDonorMoveChunkAborted{0};

    // Cumulative, always-increasing counter of how much time the entire move chunk operation took
    // (excluding range deletion).
    Atomic<long long> totalDonorMoveChunkTimeMillis{0};

    // Cumulative, always-increasing counter of how much time the clone phase took on the donor
    // node, before it was appropriate to enter the critical section.
    Atomic<long long> totalDonorChunkCloneTimeMillis{0};

    // Cumulative, always-increasing counter of how many documents have been cloned on the
    // recipient node.
    Atomic<long long> countDocsClonedOnRecipient{0};

    // Cumulative, always-increasing counter of how many documents have been cloned on the catch up
    // phase on the recipient node.
    Atomic<long long> countDocsClonedOnCatchUpOnRecipient{0};

    // Cumulative, always-increasing counter of how many bytes have been cloned on the catch up
    // phase on the recipient node.
    Atomic<long long> countBytesClonedOnCatchUpOnRecipient{0};

    // Cumulative, always-increasing counter of how many bytes have been cloned on the
    // recipient node.
    Atomic<long long> countBytesClonedOnRecipient{0};

    // Cumulative, always-increasing counter of how many documents have been cloned on the donor
    // node.
    Atomic<long long> countDocsClonedOnDonor{0};

    // Cumulative, always-increasing counter of how many bytes have been cloned on the donor
    // node.
    Atomic<long long> countBytesClonedOnDonor{0};

    // Cumulative, always-increasing counter of how many documents have been deleted by the
    // rangeDeleter.
    Atomic<long long> countDocsDeletedByRangeDeleter{0};

    // Cumulative, always-increasing counter of how many bytes have been deleted by the
    // rangeDeleter.
    Atomic<long long> countBytesDeletedByRangeDeleter{0};

    // Cumulative, always-increasing counters of how many execution tickets the rangeDeleter
    // acquired (including how many came from the low-priority pool, which the rangeDeleter uses
    // when background task deprioritization is enabled).
    Atomic<long long> rangeDeleterTicketAdmissions{0};
    Atomic<long long> rangeDeleterLowPriorityTicketAdmissions{0};

    // Total time rangeDeleter operations have spent queued waiting for an execution ticket, and
    // processing while holding one, each including the in-progress time of any deletion currently
    // in that state. The queue accumulator also exposes the "currently queued" gauge.
    admission::execution_control::InProgressTimeAccumulator rangeDeleterTicketQueueTime;
    admission::execution_control::InProgressTimeAccumulator rangeDeleterTicketProcessingTime;

    // Cumulative counter of range-deletion tasks for which the guard preserved (did not delete)
    // MaxKey-prefixed documents. One increment per task; the number of preserved documents may be
    // higher.
    AtomicWord<long long> countRangeDeletionTasksPreservingMaxKeyOrphans{0};

    // Cumulative, always-increasing counter of how many chunks this node started to receive
    // (whether the receiving succeeded or not)
    Atomic<long long> countRecipientMoveChunkStarted{0};

    // Cumulative, always-increasing counter of how much time the critical section's commit phase
    // took (this is the period of time when all operations on the collection are blocked, not just
    // the reads)
    Atomic<long long> totalCriticalSectionCommitTimeMillis{0};

    // Cumulative, always-increasing counter of how much time the entire critical section took. It
    // includes the time the recipient took to fetch the latest modifications from the donor and
    // persist them plus the critical section commit time.
    //
    // The value of totalCriticalSectionTimeMillis - totalCriticalSectionCommitTimeMillis gives the
    // duration of the catch-up phase of the critical section (where the last mods are transferred
    // from the donor to the recipient).
    Atomic<long long> totalCriticalSectionTimeMillis{0};

    // Cumulative, always-increasing counter of the number of migrations aborted on this node
    // after timing out waiting to acquire a lock.
    Atomic<long long> countDonorMoveChunkLockTimeout{0};

    // Cumulative, always-increasing counter of how much time the migration recipient critical
    // section took (this is the period of time when write operations on the collection on the
    // recipient are blocked).
    Atomic<long long> totalRecipientCriticalSectionTimeMillis{0};

    // Cumulative, always-increasing counter of the number of migrations aborted on this node
    // due to concurrent index operations.
    Atomic<long long> countDonorMoveChunkAbortConflictingIndexOperation{0};

    // Total number of migrations leftover from previous primaries that needs to be run to
    // completion. Valid only when this process is the repl set primary.
    Atomic<long long> unfinishedMigrationFromPreviousPrimary{0};

    // Total number of commands run directly against this shard without the directShardOperations
    // role.
    Atomic<long long> unauthorizedDirectShardOperations{0};

    // Total number of times the _configsvrTransitionToDedicatedConfigServer command has started.
    Atomic<long long> countTransitionToDedicatedConfigServerStarted{0};

    // Total number of times the _configsvrTransitionToDedicatedConfigServer command has completed.
    Atomic<long long> countTransitionToDedicatedConfigServerCompleted{0};

    // Total number of times the _configsvrTransitionFromDedicatedConfigServer command has
    // completed.
    Atomic<long long> countTransitionFromDedicatedConfigServerCompleted{0};

    // Cumulative, always-increasing total number of sharding metadata refreshes that have been
    // kicked off by the _flushReshardingStateChange command.
    Atomic<long long> countFlushReshardingStateChangeTotalShardingMetadataRefreshes{0};
    // Cumulative, always-increasing number of successful and failed sharding metadata refreshes
    // that have been kicked off by the _flushReshardingStateChange command.
    Atomic<long long> countFlushReshardingStateChangeSuccessfulShardingMetadataRefreshes{0};
    Atomic<long long> countFlushReshardingStateChangeFailedShardingMetadataRefreshes{0};

    // Total number of times a compound wildcard index prefixed by shard key has been detected
    // during a moveChunk, range deleter or any other operation which needs to fetch a valid shard
    // key index. This will help estimate the impact of SERVER-103774.
    //
    // TODO (SERVER-112793) Remove once v9.0 branches out.
    Atomic<long long> countHitsOfCompoundWildcardIndexesWithShardKeyPrefix{0};

    // Cumulative, always-increasing counter of how many chunk migrations had to wait for
    // reclaimed prepared transactions from precise checkpoint recovery to resolve before cloning.
    Atomic<long long> chunkMigrationWaitedOnReclaimedPreparedTxns{0};

    // Cumulative, always-increasing counter of total time (ms) chunk migrations spent waiting for
    // reclaimed prepared transactions from precise checkpoint recovery to resolve.
    Atomic<long long> chunkMigrationWaitForReclaimedPreparedTxnsMillis{0};

    // FTDC metrics for the MaxKey orphan detection sweep run on shard primaries.
    // The *Complete/*FoundUnownedMaxKey/*UnownedAlertEmitted fields are 0/1 flags describing the
    // last published sweep outcome on this process. *Errors counts non-fatal per-collection errors
    // encountered while running the sweep.
    AtomicWord<long long> maxKeyOrphanScanComplete{0};
    AtomicWord<long long> maxKeyOrphanScanFoundUnownedMaxKey{0};
    AtomicWord<long long> maxKeyOrphanScanUnownedAlertEmitted{0};
    AtomicWord<long long> maxKeyOrphanScanErrors{0};

    // An accessible (owned) MaxKey doc was found on this shard. Its version could be stale if it
    // was re-owned after some series of application operations. The
    // *FoundOwnedMaxKey/*OwnedAlertEmitted fields are 0/1 flags describing the last published sweep
    // outcome.
    AtomicWord<long long> maxKeyOrphanScanFoundOwnedMaxKey{0};
    AtomicWord<long long> maxKeyOrphanScanOwnedAlertEmitted{0};

    // FTDC metrics for the MaxKey zone inventory scan run by the balancer on config primaries.
    // The *Complete/*FoundBuggyZone/*AlertEmitted fields are 0/1 flags describing the last
    // published scan outcome on this process. *Errors counts non-fatal scan errors.
    Atomic<long long> maxKeyZoneScanComplete{0};
    Atomic<long long> maxKeyZoneScanFoundBuggyZone{0};
    Atomic<long long> maxKeyZoneScanAlertEmitted{0};
    Atomic<long long> maxKeyZoneScanErrors{0};

    CriticalSectionStatistics<DatabaseName> databaseCriticalSectionStatistics;
    CriticalSectionStatistics<NamespaceString> collectionCriticalSectionStatistics;

    DatabaseShardingMetadataStatistics databaseShardingMetadataStatistics;
    CollectionShardingMetadataStatistics collectionShardingMetadataStatistics;
    ChunkOperationsStatistics chunkOperationsStatistics;

    /**
     * Obtains the per-process instance of the sharding statistics object.
     */
    static ShardingStatistics& get(ServiceContext* serviceContext);
    static ShardingStatistics& get(OperationContext* opCtx);

    /**
     * Reports the accumulated statistics for serverStatus.
     */
    void report(BSONObjBuilder* builder) const;
};

}  // namespace mongo
