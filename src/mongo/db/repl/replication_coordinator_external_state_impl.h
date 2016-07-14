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
#include "mongo/db/repl/sync_source_feedback.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/old_thread_pool.h"

namespace mongo {
class ServiceContext;

namespace repl {

class SnapshotThread;
class StorageInterface;

class ReplicationCoordinatorExternalStateImpl final : public ReplicationCoordinatorExternalState,
                                                      public JournalListener {
    MONGO_DISALLOW_COPYING(ReplicationCoordinatorExternalStateImpl);

public:
    ReplicationCoordinatorExternalStateImpl(StorageInterface* storageInterface);
    virtual ~ReplicationCoordinatorExternalStateImpl();
    virtual void startThreads(const ReplSettings& settings) override;
    virtual void startInitialSync(OnInitialSyncFinishedFn finished) override;
    virtual void startSteadyStateReplication(OperationContext* txn) override;
    virtual void runOnInitialSyncThread(stdx::function<void(OperationContext* txn)> run) override;

    virtual bool isInitialSyncFlagSet(OperationContext* txn) override;

    virtual void startMasterSlave(OperationContext* txn);
    virtual void shutdown(OperationContext* txn);
    virtual executor::TaskExecutor* getTaskExecutor() const override;
    virtual OldThreadPool* getDbWorkThreadPool() const override;
    virtual Status initializeReplSetStorage(OperationContext* txn, const BSONObj& config);
    virtual void logTransitionToPrimaryToOplog(OperationContext* txn);
    virtual void forwardSlaveProgress();
    virtual OID ensureMe(OperationContext* txn);
    virtual bool isSelf(const HostAndPort& host, ServiceContext* ctx);
    virtual StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* txn);
    virtual Status storeLocalConfigDocument(OperationContext* txn, const BSONObj& config);
    virtual StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* txn);
    virtual Status storeLocalLastVoteDocument(OperationContext* txn, const LastVote& lastVote);
    virtual void setGlobalTimestamp(const Timestamp& newTime);
    virtual StatusWith<OpTime> loadLastOpTime(OperationContext* txn);
    virtual void cleanUpLastApplyBatch(OperationContext* txn);
    virtual HostAndPort getClientHostAndPort(const OperationContext* txn);
    virtual void closeConnections();
    virtual void killAllUserOperations(OperationContext* txn);
    virtual void shardingOnStepDownHook();
    virtual void shardingOnDrainingStateHook(OperationContext* txn);
    virtual void signalApplierToChooseNewSyncSource();
    virtual void signalApplierToCancelFetcher();
    virtual void dropAllTempCollections(OperationContext* txn);
    void dropAllSnapshots() final;
    void updateCommittedSnapshot(SnapshotName newCommitPoint) final;
    void forceSnapshotCreation() final;
    virtual bool snapshotsEnabled() const;
    virtual void notifyOplogMetadataWaiters();
    virtual double getElectionTimeoutOffsetLimitFraction() const;
    virtual bool isReadCommittedSupportedByStorageEngine(OperationContext* txn) const;
    virtual StatusWith<OpTime> multiApply(OperationContext* txn,
                                          MultiApplier::Operations ops,
                                          MultiApplier::ApplyOperationFn applyOperation) override;
    virtual void multiSyncApply(MultiApplier::OperationPtrs* ops) override;
    virtual void multiInitialSyncApply(MultiApplier::OperationPtrs* ops,
                                       const HostAndPort& source) override;
    virtual std::unique_ptr<OplogBuffer> makeInitialSyncOplogBuffer(
        OperationContext* txn) const override;
    virtual std::unique_ptr<OplogBuffer> makeSteadyStateOplogBuffer(
        OperationContext* txn) const override;
    virtual bool shouldUseDataReplicatorInitialSync() const override;

    std::string getNextOpContextThreadName();

    // Methods from JournalListener.
    virtual JournalListener::Token getToken();
    virtual void onDurable(const JournalListener::Token& token);

private:
    // Guards starting threads and setting _startedThreads
    stdx::mutex _threadMutex;

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
    std::unique_ptr<stdx::thread> _applierThread;

    // Mutex guarding the _nextThreadId value to prevent concurrent incrementing.
    stdx::mutex _nextThreadIdMutex;
    // Number used to uniquely name threads.
    long long _nextThreadId = 0;

    std::unique_ptr<SnapshotThread> _snapshotThread;

    // Initial sync stuff
    StartInitialSyncFn _startInitialSyncIfNeededFn;
    StartSteadyReplicationFn _startSteadReplicationFn;
    std::unique_ptr<stdx::thread> _initialSyncThread;

    // Task executor used to run replication tasks.
    std::unique_ptr<executor::TaskExecutor> _taskExecutor;

    // Used by repl::multiApply() to apply the sync source's operations in parallel.
    std::unique_ptr<OldThreadPool> _writerPool;
};

}  // namespace repl
}  // namespace mongo
