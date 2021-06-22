/**
 *    Copyright (C) 2019 MongoDB, Inc.
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

#include "mongo/db/repl/replication_coordinator.h"

namespace mongo {
namespace repl {

/**
 * Stub implementation for tests, or programs like mongocryptd, that want a non-null
 * ReplicationCoordinator but don't need any replication logic.
 */
class ReplicationCoordinatorNoOp final : public ReplicationCoordinator {

public:
    ReplicationCoordinatorNoOp(ServiceContext* serviceContext);
    ~ReplicationCoordinatorNoOp() = default;

    ReplicationCoordinatorNoOp(ReplicationCoordinatorNoOp&) = delete;
    ReplicationCoordinatorNoOp& operator=(ReplicationCoordinatorNoOp&) = delete;

    void startup(OperationContext* opCtx) final;

    void enterTerminalShutdown() final;

    void shutdown(OperationContext* opCtx) final;

    ServiceContext* getServiceContext() final {
        return _service;
    }

    const ReplSettings& getSettings() const final;

    Mode getReplicationMode() const final;
    bool getMaintenanceMode() final;

    bool isReplEnabled() const final;
    bool isMasterForReportingPurposes() final;
    bool isInPrimaryOrSecondaryState(OperationContext* opCtx) const final;
    bool isInPrimaryOrSecondaryState_UNSAFE() const final;

    bool canAcceptWritesForDatabase(OperationContext* opCtx, StringData dbName) final;
    bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx, StringData dbName) final;

    bool canAcceptWritesFor(OperationContext* opCtx, const NamespaceString& ns) final;
    bool canAcceptWritesFor_UNSAFE(OperationContext* opCtx, const NamespaceString& ns) final;

    Status checkCanServeReadsFor(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 bool slaveOk) final;
    Status checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        bool slaveOk) final;

    bool shouldRelaxIndexConstraints(OperationContext* opCtx, const NamespaceString& ns) final;

    WriteConcernOptions getGetLastErrorDefault() final;

    WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(WriteConcernOptions wc) final;

    bool buildsIndexes() final;

    MemberState getMemberState() const final;

    bool canAcceptNonLocalWrites() const final;

    std::vector<MemberData> getMemberData() const final;

    Status waitForMemberState(MemberState, Milliseconds) final;

    Seconds getSlaveDelaySecs() const final;

    void clearSyncSourceBlacklist() final;

    ReplicationCoordinator::StatusAndDuration awaitReplication(OperationContext*,
                                                               const OpTime&,
                                                               const WriteConcernOptions&) final;

    void stepDown(OperationContext*, bool, const Milliseconds&, const Milliseconds&) final;

    Status checkIfWriteConcernCanBeSatisfied(const WriteConcernOptions&) const final;

    Status checkIfCommitQuorumCanBeSatisfied(const CommitQuorumOptions& commitQuorum) const final;

    StatusWith<bool> checkIfCommitQuorumIsSatisfied(
        const CommitQuorumOptions& commitQuorum,
        const std::vector<HostAndPort>& commitReadyMembers) const final;

    void setMyLastAppliedOpTimeAndWallTime(const OpTimeAndWallTime& opTimeAndWallTime) final;
    void setMyLastDurableOpTimeAndWallTime(const OpTimeAndWallTime& opTimeAndWallTime) final;
    void setMyLastAppliedOpTimeAndWallTimeForward(const OpTimeAndWallTime& opTimeAndWallTime,
                                                  DataConsistency consistency) final;
    void setMyLastDurableOpTimeAndWallTimeForward(const OpTimeAndWallTime& opTimeAndWallTime) final;

    void resetMyLastOpTimes() final;

    void setMyHeartbeatMessage(const std::string&) final;

    OpTime getMyLastAppliedOpTime() const final;
    OpTimeAndWallTime getMyLastAppliedOpTimeAndWallTime() const final;

    OpTime getMyLastDurableOpTime() const final;
    OpTimeAndWallTime getMyLastDurableOpTimeAndWallTime() const final;

    Status waitUntilOpTimeForReadUntil(OperationContext*,
                                       const ReadConcernArgs&,
                                       boost::optional<Date_t>) final;

    Status waitUntilOpTimeForRead(OperationContext*, const ReadConcernArgs&) final;
    Status awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) final;

    OID getElectionId() final;

    int getMyId() const final;

    HostAndPort getMyHostAndPort() const final;

    Status setFollowerMode(const MemberState&) final;

    Status setFollowerModeStrict(OperationContext* opCtx, const MemberState&) final;

    ApplierState getApplierState() final;

    void signalDrainComplete(OperationContext*, long long) final;

    Status waitForDrainFinish(Milliseconds) final;

    void signalUpstreamUpdater() final;

    Status resyncData(OperationContext*, bool) final;

    StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const final;

    Status processReplSetGetStatus(BSONObjBuilder*, ReplSetGetStatusResponseStyle) final;

    void fillIsMasterForReplSet(IsMasterResponse*, const SplitHorizon::Parameters& horizon) final;

    void appendSlaveInfoData(BSONObjBuilder*) final;

    ReplSetConfig getConfig() const final;

    void processReplSetGetConfig(BSONObjBuilder*) final;

    void processReplSetMetadata(const rpc::ReplSetMetadata&) final;

    void advanceCommitPoint(const OpTimeAndWallTime& committedOpTimeAndWallTime,
                            bool fromSyncSource) final;

    void cancelAndRescheduleElectionTimeout() final;

    Status setMaintenanceMode(OperationContext*, bool) final;

    Status processReplSetSyncFrom(OperationContext*, const HostAndPort&, BSONObjBuilder*) final;

    Status processReplSetFreeze(int, BSONObjBuilder*) final;

    Status processReplSetReconfig(OperationContext*,
                                  const ReplSetReconfigArgs&,
                                  BSONObjBuilder*) final;

    Status processReplSetInitiate(OperationContext*, const BSONObj&, BSONObjBuilder*) final;

    Status processReplSetUpdatePosition(const UpdatePositionArgs&, long long*) final;

    std::vector<HostAndPort> getHostsWrittenTo(const OpTime&, bool) final;

    std::vector<HostAndPort> getOtherNodesInReplSet() const final;

    Status checkReplEnabledForCommand(BSONObjBuilder*) final;


    HostAndPort chooseNewSyncSource(const OpTime&) final;

    void blacklistSyncSource(const HostAndPort&, Date_t) final;

    void resetLastOpTimesFromOplog(OperationContext*, DataConsistency) final;

    bool shouldChangeSyncSource(const HostAndPort&,
                                const rpc::ReplSetMetadata&,
                                boost::optional<rpc::OplogQueryMetadata>) final;

    OpTime getLastCommittedOpTime() const final;

    OpTimeAndWallTime getLastCommittedOpTimeAndWallTime() const final;

    Status processReplSetRequestVotes(OperationContext*,
                                      const ReplSetRequestVotesArgs&,
                                      ReplSetRequestVotesResponse*) final;

    void prepareReplMetadata(const BSONObj&, const OpTime&, BSONObjBuilder*) const final;

    Status processHeartbeatV1(const ReplSetHeartbeatArgsV1&, ReplSetHeartbeatResponse*) final;

    bool getWriteConcernMajorityShouldJournal() final;

    void dropAllSnapshots() final;

    long long getTerm() final;

    Status updateTerm(OperationContext*, long long) final;

    OpTime getCurrentCommittedSnapshotOpTime() const final;

    OpTimeAndWallTime getCurrentCommittedSnapshotOpTimeAndWallTime() const final;

    void waitUntilSnapshotCommitted(OperationContext*, const Timestamp&) final;

    void appendDiagnosticBSON(BSONObjBuilder*) final;

    void appendConnectionStats(executor::ConnectionPoolStats* stats) const final;

    size_t getNumUncommittedSnapshots() final;

    virtual void createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) final;

    Status stepUpIfEligible(bool skipDryRun) final;

    Status abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) final;

    void incrementNumCatchUpOpsIfCatchingUp(long numOps) final;

    void signalDropPendingCollectionsRemovedFromStorage() final;

    boost::optional<Timestamp> getRecoveryTimestamp() final;

    bool setContainsArbiter() const final;

    void attemptToAdvanceStableTimestamp() final;

    void updateAndLogStateTransitionMetrics(
        const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
        const size_t numOpsKilled,
        const size_t numOpsRunning) const final;

private:
    ServiceContext* const _service;
};

}  // namespace repl
}  // namespace mongo
