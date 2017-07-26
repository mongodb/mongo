/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/oplog_interface_remote.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/rollback_impl.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class DBClientBase;
class OperationContext;

namespace repl {

class OplogInterface;
class ReplicationCoordinator;
class ReplicationCoordinatorExternalState;
class ReplicationProcess;
class StorageInterface;

class BackgroundSync {
    MONGO_DISALLOW_COPYING(BackgroundSync);

public:
    /**
     *   Stopped -> Starting -> Running
     *      ^          |            |
     *      |__________|____________|
     *
     * In normal cases: Stopped -> Starting -> Running -> Stopped.
     * It is also possible to transition directly from Starting to Stopped.
     *
     * We need a separate Starting state since part of the startup process involves reading from
     * disk and we want to do that disk I/O in the bgsync thread, rather than whatever thread calls
     * start().
     */
    enum class ProducerState { Starting, Running, Stopped };

    BackgroundSync(ReplicationCoordinatorExternalState* replicationCoordinatorExternalState,
                   ReplicationProcess* replicationProcess,
                   std::unique_ptr<OplogBuffer> oplogBuffer);

    // stop syncing (when this node becomes a primary, e.g.)
    // During stepdown, the last fetched optime is not reset in order to keep track of the lastest
    // optime in the buffer. However, the last fetched optime has to be reset after initial sync or
    // rollback.
    void stop(bool resetLastFetchedOptime);

    /**
     * Starts oplog buffer, task executor and producer thread, in that order.
     */
    void startup(OperationContext* opCtx);

    /**
     * Signals producer thread to stop.
     */
    void shutdown(OperationContext* opCtx);

    /**
     * Waits for producer thread to stop before shutting down the task executor and oplog buffer.
     */
    void join(OperationContext* opCtx);

    /**
     * Returns true if shutdown() has been called.
     * Once this returns true, nothing more will be added to the queue and consumers must shutdown.
     */
    bool inShutdown() const;

    // starts the sync target notifying thread
    void notifierThread();

    HostAndPort getSyncTarget() const;

    // Interface implementation

    bool peek(OperationContext* opCtx, BSONObj* op);
    void consume(OperationContext* opCtx);
    void clearSyncTarget();
    void waitForMore();

    // For monitoring
    BSONObj getCounters();

    // Clears any fetched and buffered oplog entries.
    void clearBuffer(OperationContext* opCtx);

    /**
     * Returns true if any of the following is true:
     * 1) We are shutting down;
     * 2) We are primary;
     * 3) We are in drain mode; or
     * 4) We are stopped.
     */
    bool shouldStopFetching() const;

    ProducerState getState() const;
    // Starts the producer if it's stopped. Otherwise, let it keep running.
    void startProducerIfStopped();

    // Adds a fake oplog entry to buffer. Used for testing only.
    void pushTestOpToBuffer(OperationContext* opCtx, const BSONObj& op);

private:
    bool _inShutdown_inlock() const;

    /**
     * Starts the producer thread which runs until shutdown. Upon resolving the current sync source
     * the producer thread uses the OplogFetcher (which requires the replication coordinator
     * external state at construction) to fetch oplog entries from the source's oplog via a long
     * running find query.
     */
    void _run();
    // Production thread inner loop.
    void _runProducer();
    void _produce(OperationContext* opCtx);

    /**
     * Checks current background sync state before pushing operations into blocking queue and
     * updating metrics. If the queue is full, might block.
     *
     * requiredRBID is reset to empty after the first call.
     */
    Status _enqueueDocuments(Fetcher::Documents::const_iterator begin,
                             Fetcher::Documents::const_iterator end,
                             const OplogFetcher::DocumentsInfo& info);

    /**
     * Executes a rollback.
     */
    void _runRollback(OperationContext* opCtx,
                      const Status& fetcherReturnStatus,
                      const HostAndPort& source,
                      int requiredRBID,
                      StorageInterface* storageInterface);

    /**
     * Executes a rollback with the recover to checkpoint algorithm. This is the default rollback
     * algorithm.
     */
    void _runRollbackViaRecoverToCheckpoint(OperationContext* opCtx,
                                            const HostAndPort& source,
                                            OplogInterface* localOplog,
                                            StorageInterface* storageInterface,
                                            OplogInterfaceRemote::GetConnectionFn getConnection);

    /**
     * Executes a rollback via refetch in either rs_rollback.cpp or rs_rollback_no_uuid.cpp
     *
     * We fall back on the rollback via refetch algorithm when:
     * 1)  the server parameter "rollbackMethod" is set to "rollbackViaRefetch" or
     *     "rollbackViaRefetchNoUUID"; or
     * 2)  the storage engine does not support "rollback to a checkpoint."
     *
     * Must be called from _runRollback() which ensures that all the conditions for entering
     * rollback have been met.
     */
    void _fallBackOnRollbackViaRefetch(OperationContext* opCtx,
                                       const HostAndPort& source,
                                       int requiredRBID,
                                       OplogInterface* localOplog,
                                       bool useUUID,
                                       OplogInterfaceRemote::GetConnectionFn getConnection);

    // restart syncing
    void start(OperationContext* opCtx);

    OpTimeWithHash _readLastAppliedOpTimeWithHash(OperationContext* opCtx);

    // Production thread
    std::unique_ptr<OplogBuffer> _oplogBuffer;

    // A pointer to the replication coordinator running the show.
    ReplicationCoordinator* _replCoord;

    // A pointer to the replication coordinator external state.
    ReplicationCoordinatorExternalState* _replicationCoordinatorExternalState;

    // A pointer to the replication process.
    ReplicationProcess* _replicationProcess;

    /**
      * All member variables are labeled with one of the following codes indicating the
      * synchronization rules for accessing them:
      *
      * (PR) Completely private to BackgroundSync. Can be read or written to from within the main
      *      BackgroundSync thread without synchronization. Shouldn't be accessed outside of this
      *      thread.
      *
      * (S)  Self-synchronizing; access in any way from any context.
      *
      * (M)  Reads and writes guarded by _mutex
      *
     */

    // Protects member data of BackgroundSync.
    // Never hold the BackgroundSync mutex when trying to acquire the ReplicationCoordinator mutex.
    mutable stdx::mutex _mutex;  // (S)

    OpTime _lastOpTimeFetched;  // (M)

    // lastFetchedHash is used to match ops to determine if we need to rollback, when a secondary.
    long long _lastFetchedHash = 0LL;  // (M)

    // Thread running producerThread().
    std::unique_ptr<stdx::thread> _producerThread;  // (M)

    // Set to true if shutdown() has been called.
    bool _inShutdown = false;  // (M)

    // Flag that marks whether a node's oplog has no common point with any
    // potential sync sources.
    bool _tooStale = false;  // (PR)

    ProducerState _state = ProducerState::Starting;  // (M)

    HostAndPort _syncSourceHost;  // (M)

    // Current sync source resolver validating sync source candidates.
    // Pointer may be read on any thread that locks _mutex or unlocked on the BGSync thread. It can
    // only be written to by the BGSync thread while holding _mutex.
    std::unique_ptr<SyncSourceResolver> _syncSourceResolver;  // (M)

    // Current oplog fetcher tailing the oplog on the sync source.
    std::unique_ptr<OplogFetcher> _oplogFetcher;

    // Current rollback process. If this component is active, we are currently reverting local
    // operations in the local oplog in order to bring this server to a consistent state relative
    // to the sync source.
    std::unique_ptr<RollbackImpl> _rollback;  // (PR)
};


}  // namespace repl
}  // namespace mongo
