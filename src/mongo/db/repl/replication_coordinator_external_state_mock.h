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

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class ServiceContext;

namespace repl {

class ReplicationCoordinatorExternalStateMock : public ReplicationCoordinatorExternalState {
    MONGO_DISALLOW_COPYING(ReplicationCoordinatorExternalStateMock);

public:
    class GlobalSharedLockAcquirer;

    ReplicationCoordinatorExternalStateMock();
    virtual ~ReplicationCoordinatorExternalStateMock();
    virtual void startThreads(const ReplSettings& settings) override;
    virtual void startInitialSync(OnInitialSyncFinishedFn finished) override;
    virtual void startSteadyStateReplication(OperationContext* txn) override;
    virtual void runOnInitialSyncThread(stdx::function<void(OperationContext* txn)> run) override;
    virtual bool isInitialSyncFlagSet(OperationContext* txn) override;

    virtual void startMasterSlave(OperationContext*);
    virtual void shutdown(OperationContext* txn);
    virtual executor::TaskExecutor* getTaskExecutor() const override;
    virtual OldThreadPool* getDbWorkThreadPool() const override;
    virtual Status initializeReplSetStorage(OperationContext* txn, const BSONObj& config);
    virtual void logTransitionToPrimaryToOplog(OperationContext* txn);
    virtual void forwardSlaveProgress();
    virtual OID ensureMe(OperationContext*);
    virtual bool isSelf(const HostAndPort& host, ServiceContext* ctx);
    virtual HostAndPort getClientHostAndPort(const OperationContext* txn);
    virtual StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* txn);
    virtual Status storeLocalConfigDocument(OperationContext* txn, const BSONObj& config);
    virtual StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* txn);
    virtual Status storeLocalLastVoteDocument(OperationContext* txn, const LastVote& lastVote);
    virtual void setGlobalTimestamp(const Timestamp& newTime);
    virtual StatusWith<OpTime> loadLastOpTime(OperationContext* txn);
    virtual void cleanUpLastApplyBatch(OperationContext* txn);
    virtual void closeConnections();
    virtual void killAllUserOperations(OperationContext* txn);
    virtual void shardingOnStepDownHook();
    virtual void shardingOnDrainingStateHook(OperationContext* txn);
    virtual void signalApplierToChooseNewSyncSource();
    virtual void signalApplierToCancelFetcher();
    virtual void dropAllTempCollections(OperationContext* txn);
    virtual void dropAllSnapshots();
    virtual void updateCommittedSnapshot(SnapshotName newCommitPoint);
    virtual void forceSnapshotCreation();
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

    /**
     * Adds "host" to the list of hosts that this mock will match when responding to "isSelf"
     * messages.
     */
    void addSelf(const HostAndPort& host);

    /**
     * Sets the return value for subsequent calls to loadLocalConfigDocument().
     */
    void setLocalConfigDocument(const StatusWith<BSONObj>& localConfigDocument);

    /**
     * Sets the return value for subsequent calls to loadLocalLastVoteDocument().
     */
    void setLocalLastVoteDocument(const StatusWith<LastVote>& localLastVoteDocument);

    /**
     * Sets the return value for subsequent calls to getClientHostAndPort().
     */
    void setClientHostAndPort(const HostAndPort& clientHostAndPort);

    /**
     * Sets the return value for subsequent calls to loadLastOpTimeApplied.
     */
    void setLastOpTime(const StatusWith<OpTime>& lastApplied);

    /**
     * Sets the return value for subsequent calls to storeLocalConfigDocument().
     * If "status" is Status::OK(), the subsequent calls will call the underlying funtion.
     */
    void setStoreLocalConfigDocumentStatus(Status status);

    /**
     * Sets whether or not subsequent calls to storeLocalConfigDocument() should hang
     * indefinitely or not based on the value of "hang".
     */
    void setStoreLocalConfigDocumentToHang(bool hang);

    /**
     * Sets the return value for subsequent calls to storeLocalLastVoteDocument().
     * If "status" is Status::OK(), the subsequent calls will call the underlying funtion.
     */
    void setStoreLocalLastVoteDocumentStatus(Status status);

    /**
     * Sets whether or not subsequent calls to storeLocalLastVoteDocument() should hang
     * indefinitely or not based on the value of "hang".
     */
    void setStoreLocalLastVoteDocumentToHang(bool hang);

    /**
     * Returns true if applier was signaled to cancel fetcher.
     */
    bool isApplierSignaledToCancelFetcher() const;

    /**
     * Returns true if startThreads() has been called.
     */
    bool threadsStarted() const;

    /**
     * Sets if the storage engine is configured to support ReadConcern::Majority (committed point).
     */
    void setIsReadCommittedEnabled(bool val);

    /**
     * Sets if we are taking snapshots for read concern majority use.
     */
    void setAreSnapshotsEnabled(bool val);

private:
    StatusWith<BSONObj> _localRsConfigDocument;
    StatusWith<LastVote> _localRsLastVoteDocument;
    StatusWith<OpTime> _lastOpTime;
    std::vector<HostAndPort> _selfHosts;
    bool _canAcquireGlobalSharedLock;
    Status _storeLocalConfigDocumentStatus;
    Status _storeLocalLastVoteDocumentStatus;
    // mutex and cond var for controlling stroeLocalConfigDocument()'s hanging
    stdx::mutex _shouldHangConfigMutex;
    stdx::condition_variable _shouldHangConfigCondVar;
    // mutex and cond var for controlling stroeLocalLastVoteDocument()'s hanging
    stdx::mutex _shouldHangLastVoteMutex;
    stdx::condition_variable _shouldHangLastVoteCondVar;
    bool _storeLocalConfigDocumentShouldHang;
    bool _storeLocalLastVoteDocumentShouldHang;
    bool _isApplierSignaledToCancelFetcher;
    bool _connectionsClosed;
    HostAndPort _clientHostAndPort;
    bool _threadsStarted;
    bool _isReadCommittedSupported = true;
    bool _areSnapshotsEnabled = true;
};

}  // namespace repl
}  // namespace mongo
