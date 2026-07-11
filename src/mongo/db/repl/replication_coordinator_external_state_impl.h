// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/clang_checked/checked_mutex.h"
#include "mongo/db/repl/clang_checked/mutex.h"
#include "mongo/db/repl/clang_checked/thread_safety_annotations.h"
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
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <memory>
#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

class ReplicationProcess;
class StorageInterface;

class NoopWriter;

class [[MONGO_MOD_PUBLIC]] ReplicationCoordinatorExternalStateImpl final
    : public ReplicationCoordinatorExternalState,
      public JournalListener {
    ReplicationCoordinatorExternalStateImpl(const ReplicationCoordinatorExternalStateImpl&) =
        delete;
    ReplicationCoordinatorExternalStateImpl& operator=(
        const ReplicationCoordinatorExternalStateImpl&) = delete;

public:
    class [[MONGO_MOD_PRIVATE]] ReplDurabilityToken : public JournalListener::Token {
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
    bool isSelf(const HostAndPort& host,
                const boost::optional<int>& priorityPort,
                ServiceContext* service) override;
    bool isSelfFastPath(const HostAndPort& host, const boost::optional<int>& priorityPort) final;
    bool isSelfSlowPath(const HostAndPort& host,
                        const boost::optional<int>& priorityPort,
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
    std::unique_ptr<JournalListener::Token> getToken(OperationContext* opCtx,
                                                     TokenMode mode) override;
    void onDurable(const JournalListener::Token& token) override;

    void setupNoopWriter(Seconds waitTime) override;
    void startNoopWriter(OpTime) override;
    void stopNoopWriter() override;

    bool isCWWCSetOnConfigShard(OperationContext* opCtx) const final;

    bool isShardPartOfShardedCluster(OperationContext* opCtx) const final;

private:
    using ThreadMutex = clang_checked::CheckedMutex<std::mutex>;

    /**
     * Stops data replication and returns with 'lock' locked.
     */
    void _stopDataReplication(OperationContext* opCtx,
                              clang_checked::unique_lock<ThreadMutex>& lock);

    /**
     * Drops all temporary collections on all databases except "local".
     *
     * The implementation may assume that the caller has acquired the global exclusive lock
     * for "opCtx".
     */
    void _dropAllTempCollections(OperationContext* opCtx);

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

    /*
     * Signals the fast count manager's thread to stop.
     */
    void _stopReplicatedFastCountThread();

    ServiceContext* _service;

    // Guards starting threads and setting _startedThreads
    mutable ThreadMutex _threadMutex;

    // Flag for guarding against concurrent data replication stopping.
    bool _stoppingDataReplication MONGO_LOCKING_GUARDED_BY(_threadMutex) = false;
    stdx::condition_variable _dataReplicationStopped;

    StorageInterface* _storageInterface;

    ReplicationProcess* _replicationProcess;

    // True when the threads have been started
    bool _startedThreads MONGO_LOCKING_GUARDED_BY(_threadMutex) = false;

    // Set to true when we are in the process of shutting down replication.
    bool _inShutdown MONGO_LOCKING_GUARDED_BY(_threadMutex) = false;

    // The SyncSourceFeedback class is responsible for sending replSetUpdatePosition commands
    // for forwarding replication progress information upstream when there is chained
    // replication.
    SyncSourceFeedback _syncSourceFeedback;

    // This buffer is used to hold operations read from the sync source. During steady state
    // replication, BackgroundSync adds operations to this buffer while OplogWriter's batcher
    // consumes these operations from this buffer in batches.
    // Note: Only initialized and used when featureFlagReduceMajorityWriteLatency is enabled.
    std::unique_ptr<OplogBuffer> _oplogWriteBuffer MONGO_LOCKING_GUARDED_BY(_threadMutex);

    // When featureFlagReduceMajorityWriteLatency is enabled:
    // This buffer is used to hold operations written by the OplogWriter. During steady state
    // replication, the OplogWriter adds operations to this buffer while the applier's batcher
    // consumes these operations from this buffer in batches.
    //
    // When featureFlagReduceMajorityWriteLatency is disabled:
    // This buffer is used to hold operations read from the sync source. During steady state
    // replication, BackgroundSync adds operations to this buffer while OplogApplier's batcher
    // consumes these operations from this buffer in batches.
    std::unique_ptr<OplogBuffer> _oplogApplyBuffer MONGO_LOCKING_GUARDED_BY(_threadMutex);

    // The BackgroundSync class is responsible for pulling ops off the network from the sync source
    // and into a BlockingQueue.
    // We can't create it on construction because it needs a fully constructed
    // ReplicationCoordinator, but this ExternalState object is constructed prior to the
    // ReplicationCoordinator.
    std::unique_ptr<BackgroundSync> _bgSync MONGO_LOCKING_GUARDED_BY(_threadMutex);

    // Set by stopProducer() when _bgSync is null, so that startSteadyStateReplication() can stop
    // the producer immediately after creating _bgSync. Guarded by _threadMutex to prevent race
    // conditions.
    bool _stopProducerRequested MONGO_LOCKING_GUARDED_BY(_threadMutex) = false;

    // Thread running SyncSourceFeedback::run().
    std::unique_ptr<stdx::thread> _syncSourceFeedbackThread MONGO_LOCKING_GUARDED_BY(_threadMutex);

    // Thread running oplog write.
    // Note: Only initialized and used when featureFlagReduceMajorityWriteLatency is enabled.
    std::shared_ptr<executor::TaskExecutor> _oplogWriterTaskExecutor
        MONGO_LOCKING_GUARDED_BY(_threadMutex);
    std::unique_ptr<OplogWriter> _oplogWriter MONGO_LOCKING_GUARDED_BY(_threadMutex);
    Future<void> _oplogWriterShutdownFuture MONGO_LOCKING_GUARDED_BY(_threadMutex);

    // Thread running oplog application.
    std::shared_ptr<executor::TaskExecutor> _oplogApplierTaskExecutor
        MONGO_LOCKING_GUARDED_BY(_threadMutex);
    std::unique_ptr<OplogApplier> _oplogApplier MONGO_LOCKING_GUARDED_BY(_threadMutex);
    Future<void> _oplogApplierShutdownFuture MONGO_LOCKING_GUARDED_BY(_threadMutex);

    // Task executor used to run replication tasks.
    // Constructed at init time, so the pointer is immutable and can be read without a lock.
    // The executor's own internal synchronization handles startup()/shutdown()/join().
    const std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    // Used by OplogApplier::applyOplogBatch() to apply the sync source's operations in parallel.
    // Also used by database and collection cloners to perform storage operations.
    // Cloners and oplog application run in separate phases of initial sync so it is fine to share
    // this thread pool.
    // Constructed at init time, so the pointer is immutable and can be read without a lock.
    // The pool's own internal synchronization handles shutdown()/join().
    const std::unique_ptr<ThreadPool> _workerPool;

    // Writes a noop every 10 seconds.
    std::unique_ptr<NoopWriter> _noopWriter;
};

}  // namespace repl
}  // namespace mongo
