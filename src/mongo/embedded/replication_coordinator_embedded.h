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

#include "mongo/db/repl/replication_coordinator.h"

namespace mongo {
namespace embedded {

class ReplicationCoordinatorEmbedded final : public repl::ReplicationCoordinator {

public:
    ReplicationCoordinatorEmbedded(ServiceContext* serviceContext);
    ~ReplicationCoordinatorEmbedded();

    ReplicationCoordinatorEmbedded(ReplicationCoordinatorEmbedded&) = delete;
    ReplicationCoordinatorEmbedded& operator=(ReplicationCoordinatorEmbedded&) = delete;

    // Members that are implemented and safe to call of public ReplicationCoordinator API

    void startup(OperationContext* opCtx,
                 StorageEngine::LastShutdownState lastShutdownState) override;

    void enterTerminalShutdown() override;

    bool enterQuiesceModeIfSecondary(Milliseconds quiesceTime) override;

    bool inQuiesceMode() const override;

    void shutdown(OperationContext* opCtx) override;

    void markAsCleanShutdownIfPossible(OperationContext* opCtx) override;

    // Returns the ServiceContext where this instance runs.
    ServiceContext* getServiceContext() override {
        return _service;
    }

    const repl::ReplSettings& getSettings() const override;

    Mode getReplicationMode() const override;
    bool getMaintenanceMode() override;

    bool isReplEnabled() const override;
    bool isWritablePrimaryForReportingPurposes() override;
    bool isInPrimaryOrSecondaryState(OperationContext* opCtx) const override;
    bool isInPrimaryOrSecondaryState_UNSAFE() const override;

    bool canAcceptWritesForDatabase(OperationContext* opCtx, StringData dbName) override;
    bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx, StringData dbName) override;

    bool canAcceptWritesFor(OperationContext* opCtx,
                            const NamespaceStringOrUUID& nsOrUUID) override;
    bool canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                   const NamespaceStringOrUUID& nsOrUUID) override;

    Status checkCanServeReadsFor(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 bool secondaryOk) override;
    Status checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        bool secondaryOk) override;

    bool shouldRelaxIndexConstraints(OperationContext* opCtx, const NamespaceString& ns) override;

    WriteConcernOptions getGetLastErrorDefault() override;

    WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(WriteConcernOptions wc) override;

    bool buildsIndexes() override;

    // Not implemented members that should not be called. Will assert or invariant.


    repl::MemberState getMemberState() const override;

    bool canAcceptNonLocalWrites() const override;

    std::vector<repl::MemberData> getMemberData() const override;

    Status waitForMemberState(repl::MemberState, Milliseconds) override;

    Seconds getSecondaryDelaySecs() const override;

    void clearSyncSourceDenylist() override;

    repl::ReplicationCoordinator::StatusAndDuration awaitReplication(
        OperationContext*, const repl::OpTime&, const WriteConcernOptions&) override;

    SharedSemiFuture<void> awaitReplicationAsyncNoWTimeout(const repl::OpTime&,
                                                           const WriteConcernOptions&) override;

    void stepDown(OperationContext*, bool, const Milliseconds&, const Milliseconds&) override;

    Status checkIfWriteConcernCanBeSatisfied(const WriteConcernOptions&) const override;

    Status checkIfCommitQuorumCanBeSatisfied(
        const CommitQuorumOptions& commitQuorum) const override;

    bool isCommitQuorumSatisfied(const CommitQuorumOptions& commitQuorum,
                                 const std::vector<mongo::HostAndPort>& members) const override;

    void setMyLastAppliedOpTimeAndWallTime(
        const repl::OpTimeAndWallTime& opTimeAndWallTime) override;
    void setMyLastDurableOpTimeAndWallTime(
        const repl::OpTimeAndWallTime& opTimeAndWallTime) override;
    void setMyLastAppliedOpTimeAndWallTimeForward(
        const repl::OpTimeAndWallTime& opTimeAndWallTime) override;
    void setMyLastDurableOpTimeAndWallTimeForward(
        const repl::OpTimeAndWallTime& opTimeAndWallTime) override;


    void resetMyLastOpTimes() override;

    void setMyHeartbeatMessage(const std::string&) override;

    repl::OpTime getMyLastAppliedOpTime() const override;
    repl::OpTimeAndWallTime getMyLastAppliedOpTimeAndWallTime(
        bool rollbackSafe = false) const override;

    repl::OpTime getMyLastDurableOpTime() const override;
    repl::OpTimeAndWallTime getMyLastDurableOpTimeAndWallTime() const override;

    Status waitUntilMajorityOpTime(OperationContext* opCtx,
                                   repl::OpTime targetOpTime,
                                   boost::optional<Date_t> deadline) override;
    Status waitUntilOpTimeForReadUntil(OperationContext*,
                                       const repl::ReadConcernArgs&,
                                       boost::optional<Date_t>) override;

    Status waitUntilOpTimeForRead(OperationContext*, const repl::ReadConcernArgs&) override;
    Status awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) override;

    OID getElectionId() override;

    int getMyId() const override;

    HostAndPort getMyHostAndPort() const override;

    Status setFollowerMode(const repl::MemberState&) override;

    Status setFollowerModeRollback(OperationContext* opCtx) override;

    ApplierState getApplierState() override;

    void signalDrainComplete(OperationContext*, long long) override;

    void signalUpstreamUpdater() override;

    StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const override;

    Status processReplSetGetStatus(BSONObjBuilder*, ReplSetGetStatusResponseStyle) override;

    void appendSecondaryInfoData(BSONObjBuilder*) override;

    repl::ReplSetConfig getConfig() const override;

    void processReplSetGetConfig(BSONObjBuilder*,
                                 bool commitmentStatus = false,
                                 bool includeNewlyAdded = false) override;

    void processReplSetMetadata(const rpc::ReplSetMetadata&) override;

    void advanceCommitPoint(const repl::OpTimeAndWallTime& committedOpTimeAndWallTime,
                            bool fromSyncSource) override;

    void cancelAndRescheduleElectionTimeout() override;

    Status setMaintenanceMode(OperationContext*, bool) override;

    Status processReplSetSyncFrom(OperationContext*, const HostAndPort&, BSONObjBuilder*) override;

    Status processReplSetFreeze(int, BSONObjBuilder*) override;

    Status processReplSetReconfig(OperationContext*,
                                  const ReplSetReconfigArgs&,
                                  BSONObjBuilder*) override;

    Status doReplSetReconfig(OperationContext* opCtx,
                             GetNewConfigFn getNewConfig,
                             bool force) override;

    Status doOptimizedReconfig(OperationContext* opCtx, GetNewConfigFn getNewConfig) override;

    Status awaitConfigCommitment(OperationContext* opCtx, bool waitForOplogCommitment) override;

    Status processReplSetInitiate(OperationContext*, const BSONObj&, BSONObjBuilder*) override;

    Status processReplSetUpdatePosition(const repl::UpdatePositionArgs&) override;

    std::vector<HostAndPort> getHostsWrittenTo(const repl::OpTime&, bool) override;

    Status checkReplEnabledForCommand(BSONObjBuilder*) override;

    HostAndPort chooseNewSyncSource(const repl::OpTime&) override;

    void denylistSyncSource(const HostAndPort&, Date_t) override;

    void resetLastOpTimesFromOplog(OperationContext*) override;

    repl::ChangeSyncSourceAction shouldChangeSyncSource(const HostAndPort&,
                                                        const rpc::ReplSetMetadata&,
                                                        const rpc::OplogQueryMetadata&,
                                                        const repl::OpTime&,
                                                        const repl::OpTime&) const override;

    repl::ChangeSyncSourceAction shouldChangeSyncSourceOnError(const HostAndPort&,
                                                               const repl::OpTime&) const override;

    repl::OpTime getLastCommittedOpTime() const override;

    repl::OpTimeAndWallTime getLastCommittedOpTimeAndWallTime() const override;

    Status processReplSetRequestVotes(OperationContext*,
                                      const repl::ReplSetRequestVotesArgs&,
                                      repl::ReplSetRequestVotesResponse*) override;

    void prepareReplMetadata(const BSONObj&, const repl::OpTime&, BSONObjBuilder*) const override;

    Status processHeartbeatV1(const repl::ReplSetHeartbeatArgsV1&,
                              repl::ReplSetHeartbeatResponse*) override;

    bool getWriteConcernMajorityShouldJournal() override;

    void clearCommittedSnapshot() override;

    long long getTerm() const override;

    Status updateTerm(OperationContext*, long long) override;

    repl::OpTime getCurrentCommittedSnapshotOpTime() const override;

    void waitUntilSnapshotCommitted(OperationContext*, const Timestamp&) override;

    void appendDiagnosticBSON(BSONObjBuilder*) override;

    void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    virtual void createWMajorityWriteAvailabilityDateWaiter(repl::OpTime opTime) override;

    Status stepUpIfEligible(bool skipDryRun) override;

    Status abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) override;

    void incrementNumCatchUpOpsIfCatchingUp(long numOps) override;

    void signalDropPendingCollectionsRemovedFromStorage() final;

    boost::optional<Timestamp> getRecoveryTimestamp() override;

    bool setContainsArbiter() const override;

    bool replSetContainsNewlyAddedMembers() const override;

    void attemptToAdvanceStableTimestamp() override;

    void finishRecoveryIfEligible(OperationContext* opCtx) override;

    void updateAndLogStateTransitionMetrics(
        const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
        const size_t numOpsKilled,
        const size_t numOpsRunning) const override;

    TopologyVersion getTopologyVersion() const override;

    void incrementTopologyVersion() override;

    std::shared_ptr<const repl::HelloResponse> awaitHelloResponse(
        OperationContext* opCtx,
        const repl::SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> previous,
        boost::optional<Date_t> deadline) override;

    virtual SharedSemiFuture<std::shared_ptr<const repl::HelloResponse>> getHelloResponseFuture(
        const repl::SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> clientTopologyVersion);

    StatusWith<repl::OpTime> getLatestWriteOpTime(OperationContext* opCtx) const noexcept override;

    HostAndPort getCurrentPrimaryHostAndPort() const override;

    void cancelCbkHandle(executor::TaskExecutor::CallbackHandle activeHandle) override;

    BSONObj runCmdOnPrimaryAndAwaitResponse(OperationContext* opCtx,
                                            const std::string& dbName,
                                            const BSONObj& cmdObj,
                                            OnRemoteCmdScheduledFn onRemoteCmdScheduled,
                                            OnRemoteCmdCompleteFn onRemoteCmdComplete) final;

    virtual void restartScheduledHeartbeats_forTest() override;

    virtual void recordIfCWWCIsSetOnConfigServerOnStartup(OperationContext* opCtx) final;

private:
    // Back pointer to the ServiceContext that has started the instance.
    ServiceContext* const _service;
};

}  // namespace embedded
}  // namespace mongo
