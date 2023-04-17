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

#include "mongo/platform/atomic_word.h"

namespace mongo {

class BSONObjBuilder;
class OperationContext;
class ServiceContext;

/**
 * Encapsulates per-process statistics for the sharding subsystem.
 */
struct ShardingStatistics {
    // Counts how many times threads hit stale config exception (which is what triggers metadata
    // refreshes).
    AtomicWord<long long> countStaleConfigErrors{0};

    // Cumulative, always-increasing counter of how many chunks this node has started to donate
    // (whether they succeeded or not).
    AtomicWord<long long> countDonorMoveChunkStarted{0};

    // Cumulative, always-increasing counter of how many chunks this node successfully committed.
    AtomicWord<long long> countDonorMoveChunkCommitted{0};

    // Cumulative, always-increasing counter of how many move chunks this node aborted.
    AtomicWord<long long> countDonorMoveChunkAborted{0};

    // Cumulative, always-increasing counter of how much time the entire move chunk operation took
    // (excluding range deletion).
    AtomicWord<long long> totalDonorMoveChunkTimeMillis{0};

    // Cumulative, always-increasing counter of how much time the clone phase took on the donor
    // node, before it was appropriate to enter the critical section.
    AtomicWord<long long> totalDonorChunkCloneTimeMillis{0};

    // Cumulative, always-increasing counter of how many documents have been cloned on the
    // recipient node.
    AtomicWord<long long> countDocsClonedOnRecipient{0};

    // Cumulative, always-increasing counter of how many documents have been cloned on the catch up
    // phase on the recipient node.
    AtomicWord<long long> countDocsClonedOnCatchUpOnRecipient{0};

    // Cumulative, always-increasing counter of how many bytes have been cloned on the catch up
    // phase on the recipient node.
    AtomicWord<long long> countBytesClonedOnCatchUpOnRecipient{0};

    // Cumulative, always-increasing counter of how many bytes have been cloned on the
    // recipient node.
    AtomicWord<long long> countBytesClonedOnRecipient{0};

    // Cumulative, always-increasing counter of how many documents have been cloned on the donor
    // node.
    AtomicWord<long long> countDocsClonedOnDonor{0};

    // Cumulative, always-increasing counter of how many documents have been deleted by the
    // rangeDeleter.
    AtomicWord<long long> countDocsDeletedOnDonor{0};

    // Cumulative, always-increasing counter of how many chunks this node started to receive
    // (whether the receiving succeeded or not)
    AtomicWord<long long> countRecipientMoveChunkStarted{0};

    // Cumulative, always-increasing counter of how much time the critical section's commit phase
    // took (this is the period of time when all operations on the collection are blocked, not just
    // the reads)
    AtomicWord<long long> totalCriticalSectionCommitTimeMillis{0};

    // Cumulative, always-increasing counter of how much time the entire critical section took. It
    // includes the time the recipient took to fetch the latest modifications from the donor and
    // persist them plus the critical section commit time.
    //
    // The value of totalCriticalSectionTimeMillis - totalCriticalSectionCommitTimeMillis gives the
    // duration of the catch-up phase of the critical section (where the last mods are transferred
    // from the donor to the recipient).
    AtomicWord<long long> totalCriticalSectionTimeMillis{0};

    // Cumulative, always-increasing counter of the number of migrations aborted on this node
    // after timing out waiting to acquire a lock.
    AtomicWord<long long> countDonorMoveChunkLockTimeout{0};

    // Cumulative, always-increasing counter of how much time the migration recipient critical
    // section took (this is the period of time when write operations on the collection on the
    // recipient are blocked).
    AtomicWord<long long> totalRecipientCriticalSectionTimeMillis{0};

    // Cumulative, always-increasing counter of the number of migrations aborted on this node
    // due to concurrent index operations.
    AtomicWord<long long> countDonorMoveChunkAbortConflictingIndexOperation{0};

    // Total number of migrations leftover from previous primaries that needs to be run to
    // completion. Valid only when this process is the repl set primary.
    AtomicWord<long long> unfinishedMigrationFromPreviousPrimary{0};

    // Current number for chunkMigrationConcurrency that defines concurrent fetchers and inserters
    // used for _migrateClone(step 4) of chunk migration
    AtomicWord<int> chunkMigrationConcurrencyCnt{1};
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
