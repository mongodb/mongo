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
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/functional.h"

namespace mongo {

namespace executor {
struct ConnectionPoolStats;
}  // namespace executor

namespace repl {

/**
 * A mock ReplicationCoordinator.  Currently it is extremely simple and exists solely to link
 * into dbtests.
 */
class ReplicationCoordinatorMock : public ReplicationCoordinator {
    ReplicationCoordinatorMock(const ReplicationCoordinatorMock&) = delete;
    ReplicationCoordinatorMock& operator=(const ReplicationCoordinatorMock&) = delete;

public:
    ReplicationCoordinatorMock(ServiceContext* service, const ReplSettings& settings);

    ReplicationCoordinatorMock(ServiceContext* service, StorageInterface* storage);

    /**
     * Creates a ReplicationCoordinatorMock with ReplSettings for a one-node replica set.
     */
    explicit ReplicationCoordinatorMock(ServiceContext* service);

    virtual ~ReplicationCoordinatorMock();

    virtual void startup(OperationContext* opCtx);

    virtual void enterTerminalShutdown();

    virtual void shutdown(OperationContext* opCtx);

    virtual void appendDiagnosticBSON(BSONObjBuilder* bob) override {}

    virtual const ReplSettings& getSettings() const;

    virtual bool isReplEnabled() const;

    virtual Mode getReplicationMode() const;

    virtual MemberState getMemberState() const;

    virtual bool canAcceptNonLocalWrites() const;

    virtual Status waitForMemberState(MemberState expectedState, Milliseconds timeout) override;

    virtual bool isInPrimaryOrSecondaryState(OperationContext* opCtx) const;

    virtual bool isInPrimaryOrSecondaryState_UNSAFE() const;

    virtual Seconds getSlaveDelaySecs() const;

    virtual void clearSyncSourceBlacklist();

    virtual ReplicationCoordinator::StatusAndDuration awaitReplication(
        OperationContext* opCtx, const OpTime& opTime, const WriteConcernOptions& writeConcern);

    void stepDown(OperationContext* opCtx,
                  bool force,
                  const Milliseconds& waitTime,
                  const Milliseconds& stepdownTime) override;

    virtual bool isMasterForReportingPurposes();

    virtual bool canAcceptWritesForDatabase(OperationContext* opCtx, StringData dbName);

    virtual bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx, StringData dbName);

    bool canAcceptWritesFor(OperationContext* opCtx, const NamespaceString& ns) override;

    bool canAcceptWritesFor_UNSAFE(OperationContext* opCtx, const NamespaceString& ns) override;

    virtual Status checkIfWriteConcernCanBeSatisfied(const WriteConcernOptions& writeConcern) const;

    virtual Status checkIfCommitQuorumCanBeSatisfied(const CommitQuorumOptions& commitQuorum) const;

    virtual StatusWith<bool> checkIfCommitQuorumIsSatisfied(
        const CommitQuorumOptions& commitQuorum,
        const std::vector<HostAndPort>& commitReadyMembers) const;

    virtual Status checkCanServeReadsFor(OperationContext* opCtx,
                                         const NamespaceString& ns,
                                         bool slaveOk);
    virtual Status checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                const NamespaceString& ns,
                                                bool slaveOk);

    virtual bool shouldRelaxIndexConstraints(OperationContext* opCtx, const NamespaceString& ns);

    virtual void setMyLastAppliedOpTimeAndWallTime(const OpTimeAndWallTime& opTimeAndWallTime);
    virtual void setMyLastDurableOpTimeAndWallTime(const OpTimeAndWallTime& opTimeAndWallTime);

    virtual void setMyLastAppliedOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime, DataConsistency consistency);
    virtual void setMyLastDurableOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime);

    virtual void resetMyLastOpTimes();

    virtual void setMyHeartbeatMessage(const std::string& msg);

    virtual OpTimeAndWallTime getMyLastAppliedOpTimeAndWallTime() const;
    virtual OpTime getMyLastAppliedOpTime() const;

    virtual OpTimeAndWallTime getMyLastDurableOpTimeAndWallTime() const;
    virtual OpTime getMyLastDurableOpTime() const;

    virtual Status waitUntilOpTimeForRead(OperationContext* opCtx,
                                          const ReadConcernArgs& settings) override;

    virtual Status waitUntilOpTimeForReadUntil(OperationContext* opCtx,
                                               const ReadConcernArgs& settings,
                                               boost::optional<Date_t> deadline) override;
    virtual Status awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts);
    virtual OID getElectionId();

    virtual OID getMyRID() const;

    virtual int getMyId() const;

    virtual HostAndPort getMyHostAndPort() const;

    virtual Status setFollowerMode(const MemberState& newState);

    virtual Status setFollowerModeStrict(OperationContext* opCtx, const MemberState& newState);

    virtual ApplierState getApplierState();

    virtual void signalDrainComplete(OperationContext*, long long);

    virtual Status waitForDrainFinish(Milliseconds timeout) override;

    virtual void signalUpstreamUpdater();

    virtual Status resyncData(OperationContext* opCtx, bool waitUntilCompleted) override;

    virtual StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const override;

    virtual Status processReplSetGetStatus(BSONObjBuilder*, ReplSetGetStatusResponseStyle);

    void fillIsMasterForReplSet(IsMasterResponse* result,
                                const SplitHorizon::Parameters& horizon) override;

    virtual void appendSlaveInfoData(BSONObjBuilder* result);

    void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    virtual ReplSetConfig getConfig() const;

    virtual void processReplSetGetConfig(BSONObjBuilder* result);

    virtual void processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) override;

    virtual void advanceCommitPoint(const OpTimeAndWallTime& committedOptimeAndWallTime,
                                    bool fromSyncSource) override;

    virtual void cancelAndRescheduleElectionTimeout() override;

    virtual Status setMaintenanceMode(OperationContext* opCtx, bool activate);

    virtual bool getMaintenanceMode();

    virtual Status processReplSetSyncFrom(OperationContext* opCtx,
                                          const HostAndPort& target,
                                          BSONObjBuilder* resultObj);

    virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj);

    virtual Status processReplSetReconfig(OperationContext* opCtx,
                                          const ReplSetReconfigArgs& args,
                                          BSONObjBuilder* resultObj);

    virtual Status processReplSetInitiate(OperationContext* opCtx,
                                          const BSONObj& configObj,
                                          BSONObjBuilder* resultObj);

    virtual Status processReplSetUpdatePosition(const UpdatePositionArgs& updates,
                                                long long* configVersion);

    virtual bool buildsIndexes();

    virtual std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op, bool durablyWritten);

    virtual std::vector<HostAndPort> getOtherNodesInReplSet() const;

    virtual WriteConcernOptions getGetLastErrorDefault();

    virtual Status checkReplEnabledForCommand(BSONObjBuilder* result);

    virtual HostAndPort chooseNewSyncSource(const OpTime& lastOpTimeFetched);

    virtual void blacklistSyncSource(const HostAndPort& host, Date_t until);

    virtual void resetLastOpTimesFromOplog(OperationContext* opCtx, DataConsistency consistency);

    bool lastOpTimesWereReset() const;

    virtual bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                        const rpc::ReplSetMetadata& replMetadata,
                                        boost::optional<rpc::OplogQueryMetadata> oqMetadata);

    virtual OpTime getLastCommittedOpTime() const;

    virtual OpTimeAndWallTime getLastCommittedOpTimeAndWallTime() const;

    virtual std::vector<MemberData> getMemberData() const override;

    virtual Status processReplSetRequestVotes(OperationContext* opCtx,
                                              const ReplSetRequestVotesArgs& args,
                                              ReplSetRequestVotesResponse* response);

    void prepareReplMetadata(const BSONObj& metadataRequestObj,
                             const OpTime& lastOpTimeFromClient,
                             BSONObjBuilder* builder) const override;

    virtual Status processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                      ReplSetHeartbeatResponse* response);

    virtual bool getWriteConcernMajorityShouldJournal();

    virtual long long getTerm();

    virtual Status updateTerm(OperationContext* opCtx, long long term);

    virtual void dropAllSnapshots() override;

    virtual OpTime getCurrentCommittedSnapshotOpTime() const override;

    virtual OpTimeAndWallTime getCurrentCommittedSnapshotOpTimeAndWallTime() const override;

    virtual void waitUntilSnapshotCommitted(OperationContext* opCtx,
                                            const Timestamp& untilSnapshot) override;

    virtual size_t getNumUncommittedSnapshots() override;

    virtual void createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) override;

    virtual WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(
        WriteConcernOptions wc) override;

    virtual Status stepUpIfEligible(bool skipDryRun) override;

    /**
     * Sets the return value for calls to getConfig.
     */
    void setGetConfigReturnValue(ReplSetConfig returnValue);

    /**
     * Sets the function to generate the return value for calls to awaitReplication().
     * 'OperationContext' and 'opTime' are the parameters passed to awaitReplication().
     */
    using AwaitReplicationReturnValueFunction =
        stdx::function<StatusAndDuration(OperationContext*, const OpTime&)>;
    void setAwaitReplicationReturnValueFunction(
        AwaitReplicationReturnValueFunction returnValueFunction);

    /**
     * Always allow writes even if this node is a writable primary. Used by sharding unit tests.
     */
    void alwaysAllowWrites(bool allowWrites);

    virtual ServiceContext* getServiceContext() override {
        return _service;
    }

    virtual Status abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) override;

    virtual void incrementNumCatchUpOpsIfCatchingUp(long numOps) override;

    void signalDropPendingCollectionsRemovedFromStorage() final;

    virtual boost::optional<Timestamp> getRecoveryTimestamp() override;

    virtual bool setContainsArbiter() const override;

    virtual void attemptToAdvanceStableTimestamp() override;

    virtual void updateAndLogStateTransitionMetrics(
        const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
        const size_t numOpsKilled,
        const size_t numOpsRunning) const override;

    virtual void setCanAcceptNonLocalWrites(bool canAcceptNonLocalWrites);

private:
    AtomicWord<unsigned long long> _snapshotNameGenerator;
    ServiceContext* const _service;
    ReplSettings _settings;
    StorageInterface* _storage = nullptr;
    MemberState _memberState;
    OpTime _myLastDurableOpTime;
    Date_t _myLastDurableWallTime;
    OpTime _myLastAppliedOpTime;
    Date_t _myLastAppliedWallTime;
    ReplSetConfig _getConfigReturnValue;
    AwaitReplicationReturnValueFunction _awaitReplicationReturnValueFunction = [](OperationContext*,
                                                                                  const OpTime&) {
        return StatusAndDuration(Status::OK(), Milliseconds(0));
    };
    bool _alwaysAllowWrites = false;
    bool _resetLastOpTimesCalled = false;
    long long _term = OpTime::kInitialTerm;
    bool _canAcceptNonLocalWrites = false;
};

}  // namespace repl
}  // namespace mongo
