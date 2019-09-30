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

#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/opqueue_batcher.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_tail.h"

namespace mongo {
namespace repl {

/**
 * Applies oplog entries.
 * Reads from an OplogBuffer batches of operations that may be applied in parallel.
 */
class OplogApplierImpl : public OplogApplier {
    OplogApplierImpl(const OplogApplierImpl&) = delete;
    OplogApplierImpl& operator=(const OplogApplierImpl&) = delete;

public:
    using MultiSyncApplyFunc =
        std::function<Status(OperationContext* opCtx,
                             MultiApplier::OperationPtrs* ops,
                             SyncTail* st,
                             WorkerMultikeyPathInfo* workerMultikeyPathInfo)>;
    /**
     * Constructs this OplogApplier with specific options.
     * Obtains batches of operations from the OplogBuffer to apply.
     * Reports oplog application progress using the Observer.
     */
    OplogApplierImpl(executor::TaskExecutor* executor,
                     OplogBuffer* oplogBuffer,
                     Observer* observer,
                     ReplicationCoordinator* replCoord,
                     ReplicationConsistencyMarkers* consistencyMarkers,
                     StorageInterface* storageInterface,
                     MultiSyncApplyFunc func,
                     const Options& options,
                     ThreadPool* writerPool);

private:
    void _run(OplogBuffer* oplogBuffer) override;

    /**
     * Runs oplog application in a loop until shutdown() is called.
     * Retrieves operations from the OplogBuffer in batches that will be applied in parallel using
     * multiApply().
     */
    void _runLoop(OplogBuffer* oplogBuffer,
                  OplogApplier::GetNextApplierBatchFn getNextApplierBatchFn);

    /**
     * Applies a batch of oplog entries by writing the oplog entries to the local oplog and then
     * using a set of threads to apply the operations. It writes all entries to the oplog, but only
     * applies entries with timestamp >= beginApplyingTimestamp.
     *
     * If the batch application is successful, returns the optime of the last op applied, which
     * should be the last op in the batch.
     * Returns ErrorCodes::CannotApplyOplogWhilePrimary if the node has become primary.
     *
     * To provide crash resilience, this function will advance the persistent value of 'minValid'
     * to at least the last optime of the batch. If 'minValid' is already greater than or equal
     * to the last optime of this batch, it will not be updated.
     */
    StatusWith<OpTime> _multiApply(OperationContext* opCtx, MultiApplier::Operations ops);

    // Not owned by us.
    ReplicationCoordinator* const _replCoord;

    // Pool of worker threads for writing ops to the databases.
    // Not owned by us.
    ThreadPool* const _writerPool;

    StorageInterface* _storageInterface;

    ReplicationConsistencyMarkers* const _consistencyMarkers;

    // Function to use during _multiApply
    MultiSyncApplyFunc _applyFunc;

    // Used to run oplog application loop.
    // TODO (SERVER-43651): Remove this member once sync_tail.cpp is fully merged in.
    SyncTail _syncTail;

    // Used to determine which operations should be applied during initial sync. If this is null,
    // we will apply all operations that were fetched.
    OpTime _beginApplyingOpTime = OpTime();
};

}  // namespace repl
}  // namespace mongo
