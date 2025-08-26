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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_writer.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_source_feedback.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

class ReplicationProcess;
class StorageInterface;

class NoopWriter;

class MONGO_MOD_PUB ReplicationCoordinatorExternalStateImpl final
    : public ReplicationCoordinatorExternalState,
      public JournalListener {
    ReplicationCoordinatorExternalStateImpl(const ReplicationCoordinatorExternalStateImpl&) =
        delete;
    ReplicationCoordinatorExternalStateImpl& operator=(
        const ReplicationCoordinatorExternalStateImpl&) = delete;

public:
    class ReplDurabilityToken : public JournalListener::Token {
    public:
        ReplDurabilityToken(OpTimeAndWallTime opTimeAndWallTime, bool isPrimary)
            : opTimeAndWallTime(opTimeAndWallTime), isPrimary(isPrimary) {}
        OpTimeAndWallTime opTimeAndWallTime;
        bool isPrimary;
    };

    ReplicationCoordinatorExternalStateImpl(ServiceContext* service,
                                            StorageInterface* storageInterface,
                                            ReplicationProcess* replicationProcess);
    ~ReplicationCoordinatorExternalStateImpl() override;
    void startThreads() override;
    void startSteadyStateReplication(OperationContext* opCtx,
                                     ReplicationCoordinator* replCoord) override;
    bool isInitialSyncFlagSet(OperationContext* opCtx) override;

    void shutdown(OperationContext* opCtx) override;

    executor::TaskExecutor* getTaskExecutor() const override;
    std::shared_ptr<executor::TaskExecutor> getSharedTaskExecutor() const override;
    ThreadPool* getDbWorkThreadPool() const override;
    Status initializeReplSetStorage(OperationContext* opCtx, const BSONObj& config) override;
    void onWriterDrainComplete(OperationContext* opCtx) override;
    void onApplierDrainComplete(OperationContext* opCtx) override;
    OpTime onTransitionToPrimary(OperationContext* opCtx) override;
    void forwardSecondaryProgress(bool prioritized = false) override;
    bool isSelf(const HostAndPort& host, ServiceContext* service) override;
    bool isSelfFastPath(const HostAndPort& host) final;
    bool isSelfSlowPath(const HostAndPort& host,
                        ServiceContext* service,
                        Milliseconds timeout) final;
    Status createLocalLastVoteCollection(OperationContext* opCtx) final;
    StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx) override;
    Status storeLocalConfigDocument(OperationContext* opCtx,
                                    const BSONObj& config,
                                    bool writeOplog) override;
    Status replaceLocalConfigDocument(OperationContext* opCtx, const BSONObj& config) override;
    StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx) override;
    Status storeLocalLastVoteDocument(OperationContext* opCtx, const LastVote& lastVote) override;
    void setGlobalTimestamp(ServiceContext* service, const Timestamp& newTime) override;
    Timestamp getGlobalTimestamp(ServiceContext* service) override;
    bool oplogExists(OperationContext* opCtx) final;
    StatusWith<OpTimeAndWallTime> loadLastOpTimeAndWallTime(OperationContext* opCtx) override;
    HostAndPort getClientHostAndPort(const OperationContext* opCtx) override;
    void closeConnections() override;
    void onStepDownHook() override;
    void signalApplierToChooseNewSyncSource() override;
    void stopProducer() override;
    void startProducerIfStopped() override;
    void notifyOtherMemberDataChanged() final;
    bool tooStale() override;
    void clearCommittedSnapshot() final;
    void updateCommittedSnapshot(const OpTime& newCommitPoint) final;
    void updateLastAppliedSnapshot(const OpTime& optime) final;
    bool snapshotsEnabled() const override;
    void notifyOplogMetadataWaiters(const OpTime& committedOpTime) override;
    double getElectionTimeoutOffsetLimitFraction() const override;
    bool isReadConcernSnapshotSupportedByStorageEngine(OperationContext* opCtx) const override;
    std::size_t getOplogFetcherSteadyStateMaxFetcherRestarts() const override;
    std::size_t getOplogFetcherInitialSyncMaxFetcherRestarts() const override;
    JournalListener* getReplicationJournalListener() final;


    // Methods from JournalListener.
    std::unique_ptr<JournalListener::Token> getToken(OperationContext* opCtx) override;
    void onDurable(const JournalListener::Token& token) override;

    void setupNoopWriter(Seconds waitTime) override;
    void startNoopWriter(OpTime) override;
    void stopNoopWriter() override;

    bool isCWWCSetOnConfigShard(OperationContext* opCtx) const final;

    bool isShardPartOfShardedCluster(OperationContext* opCtx) const final;

private:
    /**
     * Stops data replication and returns with 'lock' locked.
     */
    void _stopDataReplication(OperationContext* opCtx, stdx::unique_lock<stdx::mutex>& lock);

    /**
     * Called when the instance transitions to primary in order to notify a potentially sharded host
     * to perform respective state changes, such as starting the balancer, etc.
     *
     * Throws on errors.
     */
    void _shardingOnTransitionToPrimaryHook(OperationContext* opCtx, long long term);

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
    stdx::mutex _threadMutex;

    // Flag for guarding against concurrent data replication stopping.
    bool _stoppingDataReplication = false;
    stdx::condition_variable _dataReplicationStopped;

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

    // This buffer is used to hold operations read from the sync source. During steady state
    // replication, BackgroundSync adds operations to this buffer while OplogWriter's batcher
    // consumes these operations from this buffer in batches.
    // Note: Only initialized and used when featureFlagReduceMajorityWriteLatency is enabled.
    std::unique_ptr<OplogBuffer> _oplogWriteBuffer;

    // When featureFlagReduceMajorityWriteLatency is enabled:
    // This buffer is used to hold operations written by the OplogWriter. During steady state
    // replication, the OplogWriter adds operations to this buffer while the applier's batcher
    // consumes these operations from this buffer in batches.
    //
    // When featureFlagReduceMajorityWriteLatency is disabled:
    // This buffer is used to hold operations read from the sync source. During steady state
    // replication, BackgroundSync adds operations to this buffer while OplogApplier's batcher
    // consumes these operations from this buffer in batches.
    std::unique_ptr<OplogBuffer> _oplogApplyBuffer;

    // The BackgroundSync class is responsible for pulling ops off the network from the sync source
    // and into a BlockingQueue.
    // We can't create it on construction because it needs a fully constructed
    // ReplicationCoordinator, but this ExternalState object is constructed prior to the
    // ReplicationCoordinator.
    std::unique_ptr<BackgroundSync> _bgSync;

    // Thread running SyncSourceFeedback::run().
    std::unique_ptr<stdx::thread> _syncSourceFeedbackThread;

    // Thread running oplog write.
    // Note: Only initialized and used when featureFlagReduceMajorityWriteLatency is enabled.
    std::shared_ptr<executor::TaskExecutor> _oplogWriterTaskExecutor;
    std::unique_ptr<OplogWriter> _oplogWriter;
    Future<void> _oplogWriterShutdownFuture;

    // Thread running oplog application.
    std::shared_ptr<executor::TaskExecutor> _oplogApplierTaskExecutor;
    std::unique_ptr<OplogApplier> _oplogApplier;
    Future<void> _oplogApplierShutdownFuture;

    // Task executor used to run replication tasks.
    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    // Used by repl::applyOplogBatch() to apply the sync source's operations in parallel.
    // Also used by database and collection cloners to perform storage operations.
    // Cloners and oplog application run in separate phases of initial sync so it is fine to share
    // this thread pool.
    std::unique_ptr<ThreadPool> _workerPool;

    // Writes a noop every 10 seconds.
    std::unique_ptr<NoopWriter> _noopWriter;
};

}  // namespace repl
}  // namespace mongo
