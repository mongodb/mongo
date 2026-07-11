// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class ServiceContext;

namespace repl {

class [[MONGO_MOD_PUBLIC]] ReplicationCoordinatorExternalStateMock
    : public ReplicationCoordinatorExternalState {
    ReplicationCoordinatorExternalStateMock(const ReplicationCoordinatorExternalStateMock&) =
        delete;
    ReplicationCoordinatorExternalStateMock& operator=(
        const ReplicationCoordinatorExternalStateMock&) = delete;

public:
    class GlobalSharedLockAcquirer;

    ReplicationCoordinatorExternalStateMock();
    ~ReplicationCoordinatorExternalStateMock() override;
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
    void forwardSecondaryProgress(bool prioritized) override;
    bool isSelf(const HostAndPort& host,
                const boost::optional<int>& priorityPort,
                ServiceContext* service) override;
    bool isSelfFastPath(const HostAndPort& host, const boost::optional<int>& priorityPort) final;
    bool isSelfSlowPath(const HostAndPort& host,
                        const boost::optional<int>& priorityPort,
                        ServiceContext* service,
                        Milliseconds timeout) final;
    HostAndPort getClientHostAndPort(const OperationContext* opCtx) override;
    StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx) override;
    Status storeLocalConfigDocument(OperationContext* opCtx,
                                    const BSONObj& config,
                                    bool writeOplog) override;
    Status replaceLocalConfigDocument(OperationContext* opCtx, const BSONObj& config) override;
    StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx) override;
    Status storeLocalLastVoteDocument(OperationContext* opCtx, const LastVote& lastVote) override;
    void setGlobalTimestamp(ServiceContext* service, const Timestamp& newTime) override;
    Timestamp getGlobalTimestamp(ServiceContext* service) override;
    bool oplogExists(OperationContext* opCtx) override;
    StatusWith<OpTimeAndWallTime> loadLastOpTimeAndWallTime(OperationContext* opCtx) override;
    void closeConnections() override;
    void onStepDownHook() override;
    void signalApplierToChooseNewSyncSource() override;
    void stopProducer() override;
    void startProducerIfStopped() override;
    void notifyOtherMemberDataChanged() final;
    bool tooStale() override;
    void clearCommittedSnapshot() override;
    void updateCommittedSnapshot(const OpTime& newCommitPoint) override;
    void updateLastAppliedSnapshot(const OpTime& optime) override;
    bool snapshotsEnabled() const override;
    void notifyOplogMetadataWaiters(const OpTime& committedOpTime) override;
    double getElectionTimeoutOffsetLimitFraction() const override;
    bool isReadConcernSnapshotSupportedByStorageEngine(OperationContext* opCtx) const override;
    std::size_t getOplogFetcherSteadyStateMaxFetcherRestarts() const override;
    std::size_t getOplogFetcherInitialSyncMaxFetcherRestarts() const override;

    /**
     * Adds "host" to the list of hosts that this mock will match when responding to "isSelf"
     * messages, including "isSelfFastPath" and "isSelfSlowPath".
     */
    void addSelf(const HostAndPort& host);

    /**
     * Adds "host" to the list of hosts that this mock will match when responding to
     * "isSelfSlowPath" messages with a timeout less than or equal to that given,
     * but not "isSelfFastPath" messages.
     */
    void addSelfSlow(const HostAndPort& host, Milliseconds timeout);

    /**
     * Remove all hosts from the list of hosts that this mock will match when responding to "isSelf"
     * messages.  Clears both regular and slow hosts.
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
    void setupNoopWriter(Seconds waitTime) override;

    /**
     * Noop
     */
    void startNoopWriter(OpTime lastKnownOpTime) override;

    /**
     * Noop
     */
    void stopNoopWriter() override;

    bool isCWWCSetOnConfigShard(OperationContext* opCtx) const final;

    bool isShardPartOfShardedCluster(OperationContext* opCtx) const final;

    /**
     * Clear the _otherMemberDataChanged flag so we can check it later.
     */
    void clearOtherMemberDataChanged();

    bool getOtherMemberDataChanged() const;

    JournalListener* getReplicationJournalListener() final;

private:
    bool _selfHostsContains(const HostAndPort& host) const;

    StatusWith<BSONObj> _localRsConfigDocument;
    StatusWith<LastVote> _localRsLastVoteDocument;
    StatusWith<OpTime> _lastOpTime;
    StatusWith<Date_t> _lastWallTime;
    std::vector<HostAndPort> _selfHosts;
    stdx::unordered_map<HostAndPort, Milliseconds> _selfHostsSlow;
    bool _canAcquireGlobalSharedLock;
    Status _storeLocalConfigDocumentStatus;
    Status _storeLocalLastVoteDocumentStatus;
    // mutex and cond var for controlling stroeLocalLastVoteDocument()'s hanging
    std::mutex _shouldHangLastVoteMutex;
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
