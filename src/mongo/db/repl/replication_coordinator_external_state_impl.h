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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/sync_source_feedback.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
class ServiceContext;

namespace repl {

class DropPendingCollectionReaper;
class ReplicationProcess;
class StorageInterface;
class NoopWriter;

class ReplicationCoordinatorExternalStateImpl final : public ReplicationCoordinatorExternalState,
                                                      public JournalListener {
    ReplicationCoordinatorExternalStateImpl(const ReplicationCoordinatorExternalStateImpl&) =
        delete;
    ReplicationCoordinatorExternalStateImpl& operator=(
        const ReplicationCoordinatorExternalStateImpl&) = delete;

public:
    ReplicationCoordinatorExternalStateImpl(
        ServiceContext* service,
        DropPendingCollectionReaper* dropPendingCollectionReaper,
        StorageInterface* storageInterface,
        ReplicationProcess* replicationProcess);
    virtual ~ReplicationCoordinatorExternalStateImpl();
    virtual void startThreads() override;
    virtual void startSteadyStateReplication(OperationContext* opCtx,
                                             ReplicationCoordinator* replCoord) override;
    virtual bool isInitialSyncFlagSet(OperationContext* opCtx) override;

    virtual void shutdown(OperationContext* opCtx);

    virtual executor::TaskExecutor* getTaskExecutor() const override;
    std::shared_ptr<executor::TaskExecutor> getSharedTaskExecutor() const override;
    virtual ThreadPool* getDbWorkThreadPool() const override;
    virtual Status initializeReplSetStorage(OperationContext* opCtx, const BSONObj& config);
    void onDrainComplete(OperationContext* opCtx) override;
    OpTime onTransitionToPrimary(OperationContext* opCtx) override;
    virtual void forwardSecondaryProgress();
    virtual bool isSelf(const HostAndPort& host, ServiceContext* service);
    Status createLocalLastVoteCollection(OperationContext* opCtx) final;
    virtual StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx);
    virtual Status storeLocalConfigDocument(OperationContext* opCtx,
                                            const BSONObj& config,
                                            bool writeOplog);
    virtual Status replaceLocalConfigDocument(OperationContext* opCtx, const BSONObj& config);
    virtual StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx);
    virtual Status storeLocalLastVoteDocument(OperationContext* opCtx, const LastVote& lastVote);
    virtual void setGlobalTimestamp(ServiceContext* service, const Timestamp& newTime);
    virtual Timestamp getGlobalTimestamp(ServiceContext* service);
    bool oplogExists(OperationContext* opCtx) final;
    virtual StatusWith<OpTimeAndWallTime> loadLastOpTimeAndWallTime(OperationContext* opCtx);
    virtual HostAndPort getClientHostAndPort(const OperationContext* opCtx);
    virtual void closeConnections();
    virtual void onStepDownHook();
    virtual void signalApplierToChooseNewSyncSource();
    virtual void stopProducer();
    virtual void startProducerIfStopped();
    void notifyOtherMemberDataChanged() final;
    virtual bool tooStale();
    void clearCommittedSnapshot() final;
    void updateCommittedSnapshot(const OpTime& newCommitPoint) final;
    void updateLastAppliedSnapshot(const OpTime& optime) final;
    virtual bool snapshotsEnabled() const;
    virtual void notifyOplogMetadataWaiters(const OpTime& committedOpTime);
    boost::optional<OpTime> getEarliestDropPendingOpTime() const final;
    virtual double getElectionTimeoutOffsetLimitFraction() const;
    virtual bool isReadCommittedSupportedByStorageEngine(OperationContext* opCtx) const;
    virtual bool isReadConcernSnapshotSupportedByStorageEngine(OperationContext* opCtx) const;
    virtual std::size_t getOplogFetcherSteadyStateMaxFetcherRestarts() const override;
    virtual std::size_t getOplogFetcherInitialSyncMaxFetcherRestarts() const override;
    JournalListener* getReplicationJournalListener() final;


    // Methods from JournalListener.
    virtual JournalListener::Token getToken(OperationContext* opCtx);
    virtual void onDurable(const JournalListener::Token& token);

    virtual void setupNoopWriter(Seconds waitTime);
    virtual void startNoopWriter(OpTime);
    virtual void stopNoopWriter();

    virtual bool isCWWCSetOnConfigShard(OperationContext* opCtx) const final;

    virtual bool isShardPartOfShardedCluster(OperationContext* opCtx) const final;

private:
    /**
     * Stops data replication and returns with 'lock' locked.
     */
    void _stopDataReplication_inlock(OperationContext* opCtx, stdx::unique_lock<Latch>& lock);

    /**
     * Called when the instance transitions to primary in order to notify a potentially sharded host
     * to perform respective state changes, such as starting the balancer, etc.
     *
     * Throws on errors.
     */
    void _shardingOnTransitionToPrimaryHook(OperationContext* opCtx);

    /**
     * Drops all temporary collections on all databases except "local".
     *
     * The implementation may assume that the caller has acquired the global exclusive lock
     * for "opCtx".
     */
    void _dropAllTempCollections(OperationContext* opCtx);

    /**
     * Resets any active sharding metadata on this server and stops any sharding-related threads
     * (such as the balancer). It is called after stepDown to ensure that if the node becomes
     * primary again in the future it will recover its state from a clean slate.
     */
    void _shardingOnStepDownHook();

    /**
     * Stops asynchronous updates to and then clears the oplogTruncateAfterPoint.
     *
     * Safe to call when there are no oplog writes, and therefore no oplog holes that must be
     * tracked by the oplogTruncateAfterPoint.
     *
     * Only primaries update the truncate point asynchronously; other replication states update the
     * truncate point manually as necessary. This function should be called whenever replication
     * leaves state PRIMARY: stepdown; and shutdown while in state PRIMARY. Otherwise, we might
     * leave a stale oplogTruncateAfterPoint set and cause unnecessary oplog truncation during
     * startup if the server gets restarted.
     */
    void _stopAsyncUpdatesOfAndClearOplogTruncateAfterPoint();

    ServiceContext* _service;

    // Guards starting threads and setting _startedThreads
    Mutex _threadMutex = MONGO_MAKE_LATCH("ReplicationCoordinatorExternalStateImpl::_threadMutex");

    // Flag for guarding against concurrent data replication stopping.
    bool _stoppingDataReplication = false;
    stdx::condition_variable _dataReplicationStopped;

    // Used to clean up drop-pending collections with drop optimes before the current replica set
    // committed OpTime.
    DropPendingCollectionReaper* _dropPendingCollectionReaper;

    StorageInterface* _storageInterface;

    ReplicationProcess* _replicationProcess;

    // True when the threads have been started
    bool _startedThreads = false;

    // Set to true when we are in the process of shutting down replication.
    bool _inShutdown = false;

    // The SyncSourceFeedback class is responsible for sending replSetUpdatePosition commands
    // for forwarding replication progress information upstream when there is chained
    // replication.
    SyncSourceFeedback _syncSourceFeedback;

    // The OplogBuffer is used to hold operations read from the sync source. During oplog
    // application, Backgrounds Sync adds operations to the OplogBuffer while the applier's
    // OplogBatcher consumes these operations from the buffer in batches.
    std::unique_ptr<OplogBuffer> _oplogBuffer;

    // The BackgroundSync class is responsible for pulling ops off the network from the sync source
    // and into a BlockingQueue.
    // We can't create it on construction because it needs a fully constructed
    // ReplicationCoordinator, but this ExternalState object is constructed prior to the
    // ReplicationCoordinator.
    std::unique_ptr<BackgroundSync> _bgSync;

    // Thread running SyncSourceFeedback::run().
    std::unique_ptr<stdx::thread> _syncSourceFeedbackThread;

    // Thread running oplog application.
    std::unique_ptr<executor::TaskExecutor> _oplogApplierTaskExecutor;
    std::unique_ptr<OplogApplier> _oplogApplier;
    Future<void> _oplogApplierShutdownFuture;

    // Mutex guarding the _nextThreadId value to prevent concurrent incrementing.
    Mutex _nextThreadIdMutex =
        MONGO_MAKE_LATCH("ReplicationCoordinatorExternalStateImpl::_nextThreadIdMutex");
    // Number used to uniquely name threads.
    long long _nextThreadId = 0;

    // Task executor used to run replication tasks.
    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    // Used by repl::applyOplogBatch() to apply the sync source's operations in parallel.
    // Also used by database and collection cloners to perform storage operations.
    // Cloners and oplog application run in separate phases of initial sync so it is fine to share
    // this thread pool.
    std::unique_ptr<ThreadPool> _writerPool;

    // Writes a noop every 10 seconds.
    std::unique_ptr<NoopWriter> _noopWriter;
};

}  // namespace repl
}  // namespace mongo
