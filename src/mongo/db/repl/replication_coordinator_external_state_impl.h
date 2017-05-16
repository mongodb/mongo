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

#include <deque>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/sync_source_feedback.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/old_thread_pool.h"

namespace mongo {
class ServiceContext;

namespace repl {
namespace {
using UniqueLock = stdx::unique_lock<stdx::mutex>;
}  // namespace

class SnapshotThread;
class StorageInterface;
class NoopWriter;

class ReplicationCoordinatorExternalStateImpl final : public ReplicationCoordinatorExternalState,
                                                      public JournalListener {
    MONGO_DISALLOW_COPYING(ReplicationCoordinatorExternalStateImpl);

public:
    ReplicationCoordinatorExternalStateImpl(ServiceContext* service,
                                            StorageInterface* storageInterface);
    virtual ~ReplicationCoordinatorExternalStateImpl();
    virtual void startThreads(const ReplSettings& settings) override;
    virtual void startSteadyStateReplication(OperationContext* opCtx,
                                             ReplicationCoordinator* replCoord) override;
    virtual void stopDataReplication(OperationContext* opCtx) override;

    virtual bool isInitialSyncFlagSet(OperationContext* opCtx) override;

    virtual void startMasterSlave(OperationContext* opCtx);
    virtual void shutdown(OperationContext* opCtx);
    virtual executor::TaskExecutor* getTaskExecutor() const override;
    virtual OldThreadPool* getDbWorkThreadPool() const override;
    virtual Status runRepairOnLocalDB(OperationContext* opCtx) override;
    virtual Status initializeReplSetStorage(OperationContext* opCtx, const BSONObj& config);
    void onDrainComplete(OperationContext* opCtx) override;
    OpTime onTransitionToPrimary(OperationContext* opCtx, bool isV1ElectionProtocol) override;
    virtual void forwardSlaveProgress();
    virtual OID ensureMe(OperationContext* opCtx);
    virtual bool isSelf(const HostAndPort& host, ServiceContext* service);
    virtual StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx);
    virtual Status storeLocalConfigDocument(OperationContext* opCtx, const BSONObj& config);
    virtual StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx);
    virtual Status storeLocalLastVoteDocument(OperationContext* opCtx, const LastVote& lastVote);
    virtual void setGlobalTimestamp(ServiceContext* service, const Timestamp& newTime);
    virtual StatusWith<OpTime> loadLastOpTime(OperationContext* opCtx);
    virtual void cleanUpLastApplyBatch(OperationContext* opCtx);
    virtual HostAndPort getClientHostAndPort(const OperationContext* opCtx);
    virtual void closeConnections();
    virtual void killAllUserOperations(OperationContext* opCtx);
    virtual void shardingOnStepDownHook();
    virtual void signalApplierToChooseNewSyncSource();
    virtual void stopProducer();
    virtual void startProducerIfStopped();
    void dropAllSnapshots() final;
    void updateCommittedSnapshot(SnapshotName newCommitPoint) final;
    void createSnapshot(OperationContext* opCtx, SnapshotName name) final;
    void forceSnapshotCreation() final;
    virtual bool snapshotsEnabled() const;
    virtual void notifyOplogMetadataWaiters();
    virtual double getElectionTimeoutOffsetLimitFraction() const;
    virtual bool isReadCommittedSupportedByStorageEngine(OperationContext* opCtx) const;
    virtual StatusWith<OpTime> multiApply(OperationContext* opCtx,
                                          MultiApplier::Operations ops,
                                          MultiApplier::ApplyOperationFn applyOperation) override;
    virtual Status multiSyncApply(MultiApplier::OperationPtrs* ops) override;
    virtual Status multiInitialSyncApply(MultiApplier::OperationPtrs* ops,
                                         const HostAndPort& source,
                                         AtomicUInt32* fetchCount) override;
    virtual std::unique_ptr<OplogBuffer> makeInitialSyncOplogBuffer(
        OperationContext* opCtx) const override;
    virtual std::unique_ptr<OplogBuffer> makeSteadyStateOplogBuffer(
        OperationContext* opCtx) const override;
    virtual std::size_t getOplogFetcherMaxFetcherRestarts() const override;

    // Methods from JournalListener.
    virtual JournalListener::Token getToken();
    virtual void onDurable(const JournalListener::Token& token);

    virtual void setupNoopWriter(Seconds waitTime);
    virtual void startNoopWriter(OpTime);
    virtual void stopNoopWriter();

private:
    /**
     * Stops data replication and returns with 'lock' locked.
     */
    void _stopDataReplication_inlock(OperationContext* opCtx, UniqueLock* lock);

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

    ServiceContext* _service;

    // Guards starting threads and setting _startedThreads
    stdx::mutex _threadMutex;

    // Flag for guarding against concurrent data replication stopping.
    bool _stoppingDataReplication = false;
    stdx::condition_variable _dataReplicationStopped;

    StorageInterface* _storageInterface;
    // True when the threads have been started
    bool _startedThreads = false;

    // The SyncSourceFeedback class is responsible for sending replSetUpdatePosition commands
    // for forwarding replication progress information upstream when there is chained
    // replication.
    SyncSourceFeedback _syncSourceFeedback;

    // The BackgroundSync class is responsible for pulling ops off the network from the sync source
    // and into a BlockingQueue.
    // We can't create it on construction because it needs a fully constructed
    // ReplicationCoordinator, but this ExternalState object is constructed prior to the
    // ReplicationCoordinator.
    std::unique_ptr<BackgroundSync> _bgSync;

    // Thread running SyncSourceFeedback::run().
    std::unique_ptr<stdx::thread> _syncSourceFeedbackThread;

    // Thread running runSyncThread().
    std::unique_ptr<RSDataSync> _applierThread;

    // Mutex guarding the _nextThreadId value to prevent concurrent incrementing.
    stdx::mutex _nextThreadIdMutex;
    // Number used to uniquely name threads.
    long long _nextThreadId = 0;

    std::unique_ptr<SnapshotThread> _snapshotThread;

    // Task executor used to run replication tasks.
    std::unique_ptr<executor::TaskExecutor> _taskExecutor;

    // Used by repl::multiApply() to apply the sync source's operations in parallel.
    std::unique_ptr<OldThreadPool> _writerPool;

    // Writes a noop every 10 seconds.
    std::unique_ptr<NoopWriter> _noopWriter;
};

}  // namespace repl
}  // namespace mongo
