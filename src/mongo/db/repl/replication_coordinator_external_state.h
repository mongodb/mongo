/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <cstddef>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/optime.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class OID;
class OldThreadPool;
class OperationContext;
class ServiceContext;
class SnapshotName;
class Status;
struct HostAndPort;
template <typename T>
class StatusWith;

namespace repl {

class LastVote;
class ReplSettings;
class ReplicationCoordinator;
class ReplicationExecutor;

/**
 * This class represents the interface the ReplicationCoordinator uses to interact with the
 * rest of the system.  All functionality of the ReplicationCoordinatorImpl that would introduce
 * dependencies on large sections of the server code and thus break the unit testability of
 * ReplicationCoordinatorImpl should be moved here.
 */
class ReplicationCoordinatorExternalState {
    MONGO_DISALLOW_COPYING(ReplicationCoordinatorExternalState);

public:
    ReplicationCoordinatorExternalState();
    virtual ~ReplicationCoordinatorExternalState();

    /**
     * Starts the journal listener, and snapshot threads
     *
     * NOTE: Only starts threads if they are not already started,
     */
    virtual void startThreads(const ReplSettings& settings) = 0;

    /**
     * Returns true if an incomplete initial sync is detected.
     */
    virtual bool isInitialSyncFlagSet(OperationContext* opCtx) = 0;

    /**
     * Starts steady state sync for replica set member.
     *
     * NOTE: Use either this or the Master/Slave version, but not both.
     */
    virtual void startSteadyStateReplication(OperationContext* opCtx,
                                             ReplicationCoordinator* replCoord) = 0;

    /**
     * Stops the data replication threads = bgsync, applier, reporter.
     */
    virtual void stopDataReplication(OperationContext* opCtx) = 0;

    /**
     * Starts the Master/Slave threads and sets up logOp
     */
    virtual void startMasterSlave(OperationContext* opCtx) = 0;

    /**
     * Performs any necessary external state specific shutdown tasks, such as cleaning up
     * the threads it started.
     */
    virtual void shutdown(OperationContext* opCtx) = 0;

    /**
     * Returns task executor for scheduling tasks to be run asynchronously.
     */
    virtual executor::TaskExecutor* getTaskExecutor() const = 0;

    /**
     * Returns shared db worker thread pool for collection cloning.
     */
    virtual OldThreadPool* getDbWorkThreadPool() const = 0;

    /**
     * Runs the repair database command on the "local" db, if the storage engine is MMapV1.
     * Note: Used after initial sync to compact the database files.
     */
    virtual Status runRepairOnLocalDB(OperationContext* opCtx) = 0;

    /**
     * Creates the oplog, writes the first entry and stores the replica set config document.
     */
    virtual Status initializeReplSetStorage(OperationContext* opCtx, const BSONObj& config) = 0;

    /**
     * Called when a node on way to becoming a primary is ready to leave drain mode. It is called
     * outside of the global X lock and the replication coordinator mutex.
     *
     * Throws on errors.
     */
    virtual void onDrainComplete(OperationContext* opCtx) = 0;

    /**
     * Called as part of the process of transitioning to primary and run with the global X lock and
     * the replication coordinator mutex acquired, so no majoirty writes are allowed while in this
     * state. See the call site in ReplicationCoordinatorImpl for details about when and how it is
     * called.
     *
     * Among other things, this writes a message about our transition to primary to the oplog if
     * isV1 and and returns the optime of that message. If !isV1, returns the optime of the last op
     * in the oplog.
     *
     * Throws on errors.
     */
    virtual OpTime onTransitionToPrimary(OperationContext* opCtx, bool isV1ElectionProtocol) = 0;

    /**
     * Simple wrapper around SyncSourceFeedback::forwardSlaveProgress.  Signals to the
     * SyncSourceFeedback thread that it needs to wake up and send a replSetUpdatePosition
     * command upstream.
     */
    virtual void forwardSlaveProgress() = 0;

    /**
     * Queries the singleton document in local.me.  If it exists and our hostname has not
     * changed since we wrote, returns the RID stored in the object.  If the document does not
     * exist or our hostname doesn't match what was recorded in local.me, generates a new OID
     * to use as our RID, stores it in local.me, and returns it.
     */
    virtual OID ensureMe(OperationContext*) = 0;

    /**
     * Returns true if "host" is one of the network identities of this node.
     */
    virtual bool isSelf(const HostAndPort& host, ServiceContext* service) = 0;

    /**
     * Gets the replica set config document from local storage, or returns an error.
     */
    virtual StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx) = 0;

    /**
     * Stores the replica set config document in local storage, or returns an error.
     */
    virtual Status storeLocalConfigDocument(OperationContext* opCtx, const BSONObj& config) = 0;

    /**
     * Gets the replica set lastVote document from local storage, or returns an error.
     */
    virtual StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx) = 0;

    /**
     * Stores the replica set lastVote document in local storage, or returns an error.
     */
    virtual Status storeLocalLastVoteDocument(OperationContext* opCtx,
                                              const LastVote& lastVote) = 0;

    /**
     * Sets the global opTime to be 'newTime'.
     */
    virtual void setGlobalTimestamp(ServiceContext* service, const Timestamp& newTime) = 0;

    /**
     * Gets the last optime of an operation performed on this host, from stable
     * storage.
     */
    virtual StatusWith<OpTime> loadLastOpTime(OperationContext* opCtx) = 0;

    /**
     * Cleaning up the oplog, by potentially truncating:
     * If we are recovering from a failed batch then minvalid.start though minvalid.end need
     * to be removed from the oplog before we can start applying operations.
     */
    virtual void cleanUpLastApplyBatch(OperationContext* opCtx) = 0;

    /**
     * Returns the HostAndPort of the remote client connected to us that initiated the operation
     * represented by "opCtx".
     */
    virtual HostAndPort getClientHostAndPort(const OperationContext* opCtx) = 0;

    /**
     * Closes all connections in the given TransportLayer except those marked with the
     * keepOpen property, which should just be connections used for heartbeating.
     * This is used during stepdown, and transition out of primary.
     */
    virtual void closeConnections() = 0;

    /**
     * Kills all operations that have a Client that is associated with an incoming user
     * connection.  Used during stepdown.
     */
    virtual void killAllUserOperations(OperationContext* opCtx) = 0;

    /**
     * Resets any active sharding metadata on this server and stops any sharding-related threads
     * (such as the balancer). It is called after stepDown to ensure that if the node becomes
     * primary again in the future it will recover its state from a clean slate.
     */
    virtual void shardingOnStepDownHook() = 0;

    /**
     * Notifies the bgsync and syncSourceFeedback threads to choose a new sync source.
     */
    virtual void signalApplierToChooseNewSyncSource() = 0;

    /**
     * Notifies the bgsync to stop fetching data.
     */
    virtual void stopProducer() = 0;

    /**
     * Start bgsync's producer if it's stopped.
     */
    virtual void startProducerIfStopped() = 0;

    /**
     * Drops all snapshots and clears the "committed" snapshot.
     */
    virtual void dropAllSnapshots() = 0;

    /**
     * Updates the committed snapshot to the newCommitPoint, and deletes older snapshots.
     *
     * It is illegal to call with a newCommitPoint that does not name an existing snapshot.
     */
    virtual void updateCommittedSnapshot(SnapshotName newCommitPoint) = 0;

    /**
     * Creates a new snapshot.
     */
    virtual void createSnapshot(OperationContext* opCtx, SnapshotName name) = 0;

    /**
     * Signals the SnapshotThread, if running, to take a forced snapshot even if the global
     * timestamp hasn't changed.
     *
     * Does not wait for the snapshot to be taken.
     */
    virtual void forceSnapshotCreation() = 0;

    /**
     * Returns whether or not the SnapshotThread is active.
     */
    virtual bool snapshotsEnabled() const = 0;

    virtual void notifyOplogMetadataWaiters() = 0;

    /**
     * Returns multiplier to apply to election timeout to obtain upper bound
     * on randomized offset.
     */
    virtual double getElectionTimeoutOffsetLimitFraction() const = 0;

    /**
     * Returns true if the current storage engine supports read committed.
     */
    virtual bool isReadCommittedSupportedByStorageEngine(OperationContext* opCtx) const = 0;

    /**
     * Applies the operations described in the oplog entries contained in "ops" using the
     * "applyOperation" function.
     */
    virtual StatusWith<OpTime> multiApply(OperationContext* opCtx,
                                          MultiApplier::Operations ops,
                                          MultiApplier::ApplyOperationFn applyOperation) = 0;

    /**
     * Used by multiApply() to writes operations to database during steady state replication.
     */
    virtual Status multiSyncApply(MultiApplier::OperationPtrs* ops) = 0;

    /**
     * Used by multiApply() to writes operations to database during initial sync. `fetchCount` is a
     * pointer to a counter that is incremented every time we fetch a missing document.
     *
     */
    virtual Status multiInitialSyncApply(MultiApplier::OperationPtrs* ops,
                                         const HostAndPort& source,
                                         AtomicUInt32* fetchCount) = 0;

    /**
     * This function creates an oplog buffer of the type specified at server startup.
     */
    virtual std::unique_ptr<OplogBuffer> makeInitialSyncOplogBuffer(
        OperationContext* opCtx) const = 0;

    /**
     * Creates an oplog buffer suitable for steady state replication.
     */
    virtual std::unique_ptr<OplogBuffer> makeSteadyStateOplogBuffer(
        OperationContext* opCtx) const = 0;

    /**
     * Returns maximum number of times that the oplog fetcher will consecutively restart the oplog
     * tailing query on non-cancellation errors.
     */
    virtual std::size_t getOplogFetcherMaxFetcherRestarts() const = 0;

    /*
     * Creates noop writer instance. Setting the _noopWriter member is not protected by a guard,
     * hence it must be called before multi-threaded operations start.
     */
    virtual void setupNoopWriter(Seconds waitTime) = 0;

    /*
     * Starts periodic noop writes to oplog.
     */
    virtual void startNoopWriter(OpTime) = 0;

    /*
     * Stops periodic noop writes to oplog.
     */
    virtual void stopNoopWriter() = 0;
};

}  // namespace repl
}  // namespace mongo
