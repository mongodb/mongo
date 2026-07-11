// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once


#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/initial_sync/all_database_cloner.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace repl {

/**
 * Holder of state for initial sync (InitialSyncer).
 */
struct InitialSyncState {
    InitialSyncState(std::unique_ptr<AllDatabaseCloner> cloner)
        : allDatabaseCloner(std::move(cloner)) {};

    std::unique_ptr<AllDatabaseCloner>
        allDatabaseCloner;                 // Cloner for all databases included in initial sync.
    Future<void> allDatabaseClonerFuture;  // Future for holding result of AllDatabaseCloner
    Timestamp beginApplyingTimestamp;  // Timestamp from the latest entry in oplog when started. It
                                       // is also the timestamp after which we will start applying
                                       // operations during initial sync.
    Timestamp beginFetchingTimestamp;  // Timestamp from the earliest active transaction that had an
                                       // oplog entry.
    Timestamp stopTimestamp;  // Referred to as minvalid, or the place we can transition states.
    Timer timer;              // Timer for timing how long each initial sync attempt takes.
    size_t appliedOps = 0;

    bool earliestOplogEntryIsInitiatingSet = false;
    Timestamp earliestOplogEntryTimestamp;
    Date_t waitForSyncSourceStableTimestampAdvanceStartTime;  // Time at which we started waiting
                                                              // for the sync source's last
                                                              // checkpoint to advance. Used with
                                                              // the retry period parameter to
                                                              // compute the deadline on each loop.
    int waitForSyncSourceStableTimestampAdvanceSleepMillis =
        100;  // How long to sleep in-between attempts. Increases exponentially.
};

}  // namespace repl
}  // namespace mongo
