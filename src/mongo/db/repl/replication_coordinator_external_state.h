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

#include <boost/optional.hpp>
#include <cstddef>
#include <functional>

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class OID;
class OperationContext;
class ServiceContext;
class Status;
struct HostAndPort;
template <typename T>
class StatusWith;

namespace repl {

class LastVote;
class ReplSettings;
class ReplicationCoordinator;

/**
 * This class represents the interface the ReplicationCoordinator uses to interact with the
 * rest of the system.  All functionality of the ReplicationCoordinatorImpl that would introduce
 * dependencies on large sections of the server code and thus break the unit testability of
 * ReplicationCoordinatorImpl should be moved here.
 */
class ReplicationCoordinatorExternalState {
    ReplicationCoordinatorExternalState(const ReplicationCoordinatorExternalState&) = delete;
    ReplicationCoordinatorExternalState& operator=(const ReplicationCoordinatorExternalState&) =
        delete;

public:
    ReplicationCoordinatorExternalState() {}
    virtual ~ReplicationCoordinatorExternalState() {}

    /**
     * Starts the journal listener, and snapshot threads
     *
     * NOTE: Only starts threads if they are not already started,
     */
    virtual void startThreads() = 0;

    /**
     * Returns true if an incomplete initial sync is detected.
     */
    virtual bool isInitialSyncFlagSet(OperationContext* opCtx) = 0;

    /**
     * Starts steady state sync for replica set member.
     */
    virtual void startSteadyStateReplication(OperationContext* opCtx,
                                             ReplicationCoordinator* replCoord) = 0;

    /**
     * Performs any necessary external state specific shutdown tasks, such as cleaning up
     * the threads it started.
     */
    virtual void shutdown(OperationContext* opCtx) = 0;

    /**
     * Returns task executor for scheduling tasks to be run asynchronously.
     */
    virtual executor::TaskExecutor* getTaskExecutor() const = 0;
    virtual std::shared_ptr<executor::TaskExecutor> getSharedTaskExecutor() const = 0;

    /**
     * Returns shared db worker thread pool for collection cloning.
     */
    virtual ThreadPool* getDbWorkThreadPool() const = 0;

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
     * Among other things, this writes a message about our transition to primary to the oplog and
     * returns the optime of that message.
     *
     * Throws on errors.
     */
    virtual OpTime onTransitionToPrimary(OperationContext* opCtx) = 0;

    /**
     * Simple wrapper around SyncSourceFeedback::forwardSecondaryProgress.  Signals to the
     * SyncSourceFeedback thread that it needs to wake up and send a replSetUpdatePosition
     * command upstream.
     */
    virtual void forwardSecondaryProgress() = 0;

    /**
     * Returns true if "host" is one of the network identities of this node.
     */
    virtual bool isSelf(const HostAndPort& host, ServiceContext* service) = 0;

    /**
     * Gets the replica set config document from local storage, or returns an error.
     */
    virtual StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx) = 0;

    /**
     * Stores the replica set config document in local storage and writes a no-op in the oplog, or
     * returns an error.
     */
    virtual Status storeLocalConfigDocument(OperationContext* opCtx,
                                            const BSONObj& config,
                                            bool writeOplog) = 0;

    /**
     * Replaces the replica set config document in local storage, or returns an error.
     **/
    virtual Status replaceLocalConfigDocument(OperationContext* opCtx, const BSONObj& config) = 0;

    /**
     * Creates the collection for "lastVote" documents and initializes it, or returns an error.
     */
    virtual Status createLocalLastVoteCollection(OperationContext* opCtx) = 0;

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
     * Gets the global opTime timestamp, i.e. the latest cluster time.
     */
    virtual Timestamp getGlobalTimestamp(ServiceContext* service) = 0;

    /**
     * Checks if the oplog exists.
     */
    virtual bool oplogExists(OperationContext* opCtx) = 0;

    /**
     * Gets the last optime, and corresponding wall clock time, of an operation performed on this
     * host, from stable storage.
     */
    virtual StatusWith<OpTimeAndWallTime> loadLastOpTimeAndWallTime(OperationContext* opCtx) = 0;

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
     * Called after this node has stepped down. This includes stepDowns caused by rollback or node
     * removal, so this function must also be able to handle those situations.
     */
    virtual void onStepDownHook() = 0;

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
     * Notify interested parties that member data for other nodes has changed.
     */
    virtual void notifyOtherMemberDataChanged() = 0;

    /**
     * True if we have discovered that no sync source's oplog overlaps with ours.
     */
    virtual bool tooStale() = 0;

    /**
     * Clears the "committed" snapshot.
     */
    virtual void clearCommittedSnapshot() = 0;

    /**
     * Updates the committed snapshot to the newCommitPoint, and deletes older snapshots.
     *
     * It is illegal to call with a newCommitPoint that does not name an existing snapshot.
     */
    virtual void updateCommittedSnapshot(const OpTime& newCommitPoint) = 0;

    /**
     * Updates the lastApplied snapshot to a consistent point for secondary reads.
     *
     * It is illegal to call with a non-existent optime.
     */
    virtual void updateLastAppliedSnapshot(const OpTime& optime) = 0;

    /**
     * Returns whether or not the SnapshotThread is active.
     */
    virtual bool snapshotsEnabled() const = 0;

    /**
     * Notifies listeners of a change in the commit level.
     */
    virtual void notifyOplogMetadataWaiters(const OpTime& committedOpTime) = 0;

    /**
     * Returns earliest drop optime of drop pending collections.
     * Returns boost::none if there are no drop pending collections.
     */
    virtual boost::optional<OpTime> getEarliestDropPendingOpTime() const = 0;

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
     * Returns true if the current storage engine supports snapshot read concern.
     */
    virtual bool isReadConcernSnapshotSupportedByStorageEngine(OperationContext* opCtx) const = 0;

    /**
     * Returns maximum number of times that the oplog fetcher will consecutively restart the oplog
     * tailing query on non-cancellation errors during steady state replication.
     */
    virtual std::size_t getOplogFetcherSteadyStateMaxFetcherRestarts() const = 0;

    /**
     * Returns maximum number of times that the oplog fetcher will consecutively restart the oplog
     * tailing query on non-cancellation errors during initial sync.
     */
    virtual std::size_t getOplogFetcherInitialSyncMaxFetcherRestarts() const = 0;

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

    /**
     * Returns whether cluster-wide write concern is set on config server or not.
     *
     * Assert will be raised if running a command on the config server failed.
     */
    virtual bool isCWWCSetOnConfigShard(OperationContext* opCtx) const = 0;

    /**
     * Used to check if the server is a shardServer and has been added to a sharded cluster via
     * addShard.
     */
    virtual bool isShardPartOfShardedCluster(OperationContext* opCtx) const = 0;

    /**
     * Returns the JournalListener used by storage to inform replication of durability.
     */
    virtual JournalListener* getReplicationJournalListener() = 0;
};

}  // namespace repl
}  // namespace mongo
