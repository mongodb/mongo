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

#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class ServiceContext;

namespace repl {

class ReplicationCoordinatorExternalStateMock : public ReplicationCoordinatorExternalState {
    ReplicationCoordinatorExternalStateMock(const ReplicationCoordinatorExternalStateMock&) =
        delete;
    ReplicationCoordinatorExternalStateMock& operator=(
        const ReplicationCoordinatorExternalStateMock&) = delete;

public:
    class GlobalSharedLockAcquirer;

    ReplicationCoordinatorExternalStateMock();
    virtual ~ReplicationCoordinatorExternalStateMock();
    virtual void startThreads() override;
    virtual void startSteadyStateReplication(OperationContext* opCtx,
                                             ReplicationCoordinator* replCoord) override;
    virtual bool isInitialSyncFlagSet(OperationContext* opCtx) override;

    virtual void shutdown(OperationContext* opCtx);
    virtual executor::TaskExecutor* getTaskExecutor() const override;
    virtual std::shared_ptr<executor::TaskExecutor> getSharedTaskExecutor() const override;
    virtual ThreadPool* getDbWorkThreadPool() const override;
    virtual Status initializeReplSetStorage(OperationContext* opCtx, const BSONObj& config);
    void onDrainComplete(OperationContext* opCtx) override;
    OpTime onTransitionToPrimary(OperationContext* opCtx) override;
    virtual void forwardSecondaryProgress();
    virtual bool isSelf(const HostAndPort& host, ServiceContext* service);
    virtual HostAndPort getClientHostAndPort(const OperationContext* opCtx);
    virtual StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx);
    virtual Status storeLocalConfigDocument(OperationContext* opCtx,
                                            const BSONObj& config,
                                            bool writeOplog);
    virtual Status replaceLocalConfigDocument(OperationContext* opCtx, const BSONObj& config);
    virtual StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx);
    virtual Status storeLocalLastVoteDocument(OperationContext* opCtx, const LastVote& lastVote);
    virtual void setGlobalTimestamp(ServiceContext* service, const Timestamp& newTime);
    virtual Timestamp getGlobalTimestamp(ServiceContext* service);
    bool oplogExists(OperationContext* opCtx) override;
    virtual StatusWith<OpTimeAndWallTime> loadLastOpTimeAndWallTime(OperationContext* opCtx);
    virtual void closeConnections();
    virtual void onStepDownHook();
    virtual void signalApplierToChooseNewSyncSource();
    virtual void stopProducer();
    virtual void startProducerIfStopped();
    void notifyOtherMemberDataChanged() final;
    virtual bool tooStale();
    virtual void clearCommittedSnapshot();
    virtual void updateCommittedSnapshot(const OpTime& newCommitPoint);
    virtual void updateLastAppliedSnapshot(const OpTime& optime);
    virtual bool snapshotsEnabled() const;
    virtual void notifyOplogMetadataWaiters(const OpTime& committedOpTime);
    boost::optional<OpTime> getEarliestDropPendingOpTime() const final;
    virtual double getElectionTimeoutOffsetLimitFraction() const;
    virtual bool isReadCommittedSupportedByStorageEngine(OperationContext* opCtx) const;
    virtual bool isReadConcernSnapshotSupportedByStorageEngine(OperationContext* opCtx) const;
    virtual std::size_t getOplogFetcherSteadyStateMaxFetcherRestarts() const override;
    virtual std::size_t getOplogFetcherInitialSyncMaxFetcherRestarts() const override;

    /**
     * Adds "host" to the list of hosts that this mock will match when responding to "isSelf"
     * messages.
     */
    void addSelf(const HostAndPort& host);

    /**
     * Remove all hosts from the list of hosts that this mock will match when responding to "isSelf"
     * messages.
     */
    void clearSelfHosts();

    /**
     * Sets the return value for subsequent calls to loadLocalConfigDocument().
     */
    void setLocalConfigDocument(const StatusWith<BSONObj>& localConfigDocument);

    /**
     * Initializes the return value for subsequent calls to loadLocalLastVoteDocument().
     */
    Status createLocalLastVoteCollection(OperationContext* opCtx) final;

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
    void setLastOpTimeAndWallTime(const StatusWith<OpTime>& lastApplied,
                                  Date_t lastAppliedWall = Date_t());

    /**
     * Sets the return value for subsequent calls to storeLocalConfigDocument().
     * If "status" is Status::OK(), the subsequent calls will call the underlying funtion.
     */
    void setStoreLocalConfigDocumentStatus(Status status);

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

    void setFirstOpTimeOfMyTerm(const OpTime& opTime);

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

    /**
     * Sets the election timeout offset limit. Default is 0.15.
     */
    void setElectionTimeoutOffsetLimitFraction(double val);

    /**
     * Noop
     */
    virtual void setupNoopWriter(Seconds waitTime);

    /**
     * Noop
     */
    virtual void startNoopWriter(OpTime lastKnownOpTime);

    /**
     * Noop
     */
    virtual void stopNoopWriter();

    virtual bool isCWWCSetOnConfigShard(OperationContext* opCtx) const final;

    virtual bool isShardPartOfShardedCluster(OperationContext* opCtx) const final;

    /**
     * Clear the _otherMemberDataChanged flag so we can check it later.
     */
    void clearOtherMemberDataChanged();

    bool getOtherMemberDataChanged() const;

    JournalListener* getReplicationJournalListener() final;

private:
    StatusWith<BSONObj> _localRsConfigDocument;
    StatusWith<LastVote> _localRsLastVoteDocument;
    StatusWith<OpTime> _lastOpTime;
    StatusWith<Date_t> _lastWallTime;
    std::vector<HostAndPort> _selfHosts;
    bool _canAcquireGlobalSharedLock;
    Status _storeLocalConfigDocumentStatus;
    Status _storeLocalLastVoteDocumentStatus;
    // mutex and cond var for controlling stroeLocalLastVoteDocument()'s hanging
    Mutex _shouldHangLastVoteMutex =
        MONGO_MAKE_LATCH("ReplicationCoordinatorExternalStateMock::_shouldHangLastVoteMutex");
    stdx::condition_variable _shouldHangLastVoteCondVar;
    bool _storeLocalLastVoteDocumentShouldHang;
    bool _connectionsClosed;
    HostAndPort _clientHostAndPort;
    bool _threadsStarted;
    bool _isReadCommittedSupported = true;
    bool _areSnapshotsEnabled = true;
    bool _otherMemberDataChanged = false;
    OpTime _firstOpTimeOfMyTerm;
    double _electionTimeoutOffsetLimitFraction = 0.15;
    Timestamp _globalTimestamp;
};

}  // namespace repl
}  // namespace mongo
