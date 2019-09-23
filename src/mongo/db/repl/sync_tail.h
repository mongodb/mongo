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

#include <deque>
#include <functional>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/session_update_tracker.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

class Database;
class OperationContext;
struct MultikeyPathInfo;

namespace repl {
class ReplicationCoordinator;
class OpTime;

/**
 * Used for oplog application on a replica set secondary.
 * Primarily used to apply batches of operations fetched from a sync source during steady state
 * replication and initial sync.
 *
 * When used for steady state replication, runs a thread that reads batches of operations from
 * an oplog buffer (through the BackgroundSync interface) and applies the batch of operations.
 */
class SyncTail {
public:
    using MultiSyncApplyFunc =
        std::function<Status(OperationContext* opCtx,
                             MultiApplier::OperationPtrs* ops,
                             SyncTail* st,
                             WorkerMultikeyPathInfo* workerMultikeyPathInfo)>;

    /**
     *
     * Constructs a SyncTail.
     * During steady state replication, oplogApplication() obtains batches of operations to apply
     * from 'observer'. It is not required to provide 'observer' at construction if we do not plan
     * on using oplogApplication(). During the oplog application phase, the batch of operations is
     * distributed across writer threads in 'writerPool'. Each writer thread applies its own vector
     * of operations using 'func'. The writer thread pool is not owned by us.
     */
    SyncTail(OplogApplier::Observer* observer,
             ReplicationConsistencyMarkers* consistencyMarkers,
             StorageInterface* storageInterface,
             MultiSyncApplyFunc func,
             ThreadPool* writerPool,
             const OplogApplier::Options& options);
    virtual ~SyncTail();

    /**
     * Returns options for oplog application.
     */
    const OplogApplier::Options& getOptions() const;

    /**
     * Shuts down oplogApplication() processing.
     */
    void shutdown();

    /**
     * Returns true if we are shutting down.
     */
    bool inShutdown() const;


    using BatchLimits = OplogApplier::BatchLimits;

    /**
     * Applies a batch of oplog entries by writing the oplog entries to the local oplog and then
     * using a set of threads to apply the operations. It will only apply (but will
     * still write to the oplog) oplog entries with a timestamp greater than or equal to the
     * beginApplyingTimestamp.
     *
     * If the batch application is successful, returns the optime of the last op applied, which
     * should be the last op in the batch.
     * Returns ErrorCodes::CannotApplyOplogWhilePrimary if the node has become primary.
     *
     * To provide crash resilience, this function will advance the persistent value of 'minValid'
     * to at least the last optime of the batch. If 'minValid' is already greater than or equal
     * to the last optime of this batch, it will not be updated.
     */
    StatusWith<OpTime> multiApply(OperationContext* opCtx, MultiApplier::Operations ops);

    void fillWriterVectors(OperationContext* opCtx,
                           MultiApplier::Operations* ops,
                           std::vector<MultiApplier::OperationPtrs>* writerVectors,
                           std::vector<MultiApplier::Operations>* derivedOps) noexcept;

private:
    OplogApplier::Observer* const _observer;
    ReplicationConsistencyMarkers* const _consistencyMarkers;
    StorageInterface* const _storageInterface;

    void _deriveOpsAndFillWriterVectors(OperationContext* opCtx,
                                        MultiApplier::Operations* ops,
                                        std::vector<MultiApplier::OperationPtrs>* writerVectors,
                                        std::vector<MultiApplier::Operations>* derivedOps,
                                        SessionUpdateTracker* sessionUpdateTracker) noexcept;
    // Function to use during applyOps
    MultiSyncApplyFunc _applyFunc;

    // Pool of worker threads for writing ops to the databases.
    // Not owned by us.
    ThreadPool* const _writerPool;

    // Used to configure multiApply() behavior.
    const OplogApplier::Options _options;

    // Protects member data of SyncTail.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("SyncTail::_mutex");

    // Set to true if shutdown() has been called.
    bool _inShutdown = false;
};

/**
 * Applies a batch of operations.
 */
Status syncApply(OperationContext* opCtx,
                 const OplogEntryBatch& batch,
                 OplogApplication::Mode oplogApplicationMode);

/**
 * This free function is used by the thread pool workers to write ops to the db.
 * This consumes the passed in OperationPtrs and callers should not make any assumptions about the
 * state of the container after calling. However, this function cannot modify the pointed-to
 * operations because the OperationPtrs container contains const pointers.
 */
Status multiSyncApply(OperationContext* opCtx,
                      MultiApplier::OperationPtrs* ops,
                      SyncTail* st,
                      WorkerMultikeyPathInfo* workerMultikeyPathInfo);

}  // namespace repl
}  // namespace mongo
