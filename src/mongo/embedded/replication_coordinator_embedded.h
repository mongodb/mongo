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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/split_horizon.h"
#include "mongo/db/repl/split_prepare_session_manager.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

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

    void shutdown(OperationContext* opCtx, BSONObjBuilder* shutdownTimeElapsedBuilder) override;

    // Returns the ServiceContext where this instance runs.
    ServiceContext* getServiceContext() override {
        return _service;
    }

    const repl::ReplSettings& getSettings() const override;

    bool getMaintenanceMode() override;

    bool isWritablePrimaryForReportingPurposes() override;
    bool isInPrimaryOrSecondaryState(OperationContext* opCtx) const override;
    bool isInPrimaryOrSecondaryState_UNSAFE() const override;

    bool canAcceptWritesForDatabase(OperationContext* opCtx, const DatabaseName& dbName) override;
    bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                           const DatabaseName& dbName) override;

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

    Status waitForMemberState(Interruptible*, repl::MemberState, Milliseconds) override;

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

    void setMyLastWrittenOpTimeAndWallTimeForward(
        const repl::OpTimeAndWallTime& opTimeAndWallTime) override;
    void setMyLastAppliedOpTimeAndWallTimeForward(
        const repl::OpTimeAndWallTime& opTimeAndWallTime) override;
    void setMyLastDurableOpTimeAndWallTimeForward(
        const repl::OpTimeAndWallTime& opTimeAndWallTime) override;
    void setMyLastAppliedAndLastWrittenOpTimeAndWallTimeForward(
        const repl::OpTimeAndWallTime& opTimeAndWallTime) override;
    void setMyLastDurableAndLastWrittenOpTimeAndWallTimeForward(
        const repl::OpTimeAndWallTime& opTimeAndWallTime) override;

    void resetMyLastOpTimes() override;

    void setMyHeartbeatMessage(const std::string&) override;

    repl::OpTime getMyLastWrittenOpTime() const override;
    repl::OpTimeAndWallTime getMyLastWrittenOpTimeAndWallTime(
        bool rollbackSafe = false) const override;

    repl::OpTime getMyLastAppliedOpTime() const override;
    repl::OpTimeAndWallTime getMyLastAppliedOpTimeAndWallTime() const override;

    repl::OpTime getMyLastDurableOpTime() const override;
    repl::OpTimeAndWallTime getMyLastDurableOpTimeAndWallTime() const override;

    Status waitUntilMajorityOpTime(OperationContext* opCtx,
                                   repl::OpTime targetOpTime,
                                   boost::optional<Date_t> deadline) override;
    Status waitUntilOpTimeForReadUntil(OperationContext*,
                                       const repl::ReadConcernArgs&,
                                       boost::optional<Date_t>) override;
    Status waitUntilOpTimeWrittenUntil(OperationContext*,
                                       LogicalTime,
                                       boost::optional<Date_t>) override;

    Status waitUntilOpTimeForRead(OperationContext*, const repl::ReadConcernArgs&) override;
    Status awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) override;

    OID getElectionId() override;

    int getMyId() const override;

    HostAndPort getMyHostAndPort() const override;

    Status setFollowerMode(const repl::MemberState&) override;

    Status setFollowerModeRollback(OperationContext* opCtx) override;

    ApplierState getApplierState() override;

    void signalDrainComplete(OperationContext*, long long) noexcept override;

    void signalUpstreamUpdater() override;

    StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const override;

    Status processReplSetGetStatus(OperationContext* opCtx,
                                   BSONObjBuilder*,
                                   ReplSetGetStatusResponseStyle) override;

    void appendSecondaryInfoData(BSONObjBuilder*) override;

    repl::ReplSetConfig getConfig() const override;

    ConnectionString getConfigConnectionString() const override;

    Milliseconds getConfigElectionTimeoutPeriod() const override;

    std::vector<repl::MemberConfig> getConfigVotingMembers() const override;

    size_t getNumConfigVotingMembers() const override;

    std::int64_t getConfigTerm() const override;

    std::int64_t getConfigVersion() const override;

    repl::ConfigVersionAndTerm getConfigVersionAndTerm() const override;

    int getConfigNumMembers() const override;

    Milliseconds getConfigHeartbeatTimeoutPeriodMillis() const override;

    BSONObj getConfigBSON() const override;

    boost::optional<repl::MemberConfig> findConfigMemberByHostAndPort_deprecated(
        const HostAndPort& hap) const override;

    bool isConfigLocalHostAllowed() const override;

    Milliseconds getConfigHeartbeatInterval() const override;

    Status validateWriteConcern(const WriteConcernOptions& writeConcern) const override;

    void processReplSetGetConfig(BSONObjBuilder*,
                                 bool commitmentStatus = false,
                                 bool includeNewlyAdded = false) override;

    void processReplSetMetadata(const rpc::ReplSetMetadata&) override;

    void advanceCommitPoint(const repl::OpTimeAndWallTime& committedOpTimeAndWallTime,
                            bool fromSyncSource) override;

    void cancelAndRescheduleElectionTimeout() override;

    Status setMaintenanceMode(OperationContext*, bool) override;

    bool shouldDropSyncSourceAfterShardSplit(OID replicaSetId) const override;

    Status processReplSetSyncFrom(OperationContext*, const HostAndPort&, BSONObjBuilder*) override;

    Status processReplSetFreeze(int, BSONObjBuilder*) override;

    Status processReplSetReconfig(OperationContext*,
                                  const ReplSetReconfigArgs&,
                                  BSONObjBuilder*) override;

    Status doReplSetReconfig(OperationContext* opCtx,
                             GetNewConfigFn getNewConfig,
                             bool force) override;

    Status doOptimizedReconfig(OperationContext* opCtx, GetNewConfigFn getNewConfig) override;

    Status awaitConfigCommitment(OperationContext* opCtx,
                                 bool waitForOplogCommitment,
                                 long long term) override;

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

    void appendDiagnosticBSON(BSONObjBuilder*, StringData) override;

    void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    virtual void createWMajorityWriteAvailabilityDateWaiter(repl::OpTime opTime) override;

    Status stepUpIfEligible(bool skipDryRun) override;

    Status abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) override;

    void incrementNumCatchUpOpsIfCatchingUp(long numOps) override;

    boost::optional<Timestamp> getRecoveryTimestamp() override;

    bool setContainsArbiter() const override;

    void attemptToAdvanceStableTimestamp() override;

    void finishRecoveryIfEligible(OperationContext* opCtx) override;

    void updateAndLogStateTransitionMetrics(
        ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
        size_t numOpsKilled,
        size_t numOpsRunning) const override;

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
                                            const DatabaseName& dbName,
                                            const BSONObj& cmdObj,
                                            OnRemoteCmdScheduledFn onRemoteCmdScheduled,
                                            OnRemoteCmdCompleteFn onRemoteCmdComplete) final;

    virtual void restartScheduledHeartbeats_forTest() override;

    virtual void recordIfCWWCIsSetOnConfigServerOnStartup(OperationContext* opCtx) final;

    class WriteConcernTagChangesEmbedded : public WriteConcernTagChanges {
        virtual ~WriteConcernTagChangesEmbedded() = default;
        virtual bool reserveDefaultWriteConcernChange() {
            return false;
        };
        virtual void releaseDefaultWriteConcernChange() {}

        virtual bool reserveConfigWriteConcernTagChange() {
            return false;
        };
        virtual void releaseConfigWriteConcernTagChange() {}
    };

    virtual WriteConcernTagChanges* getWriteConcernTagChanges() override;

    virtual repl::SplitPrepareSessionManager* getSplitPrepareSessionManager() override;

    virtual bool isRetryableWrite(OperationContext* opCtx) const override;

    virtual boost::optional<UUID> getInitialSyncId(OperationContext* opCtx) override;

private:
    // Back pointer to the ServiceContext that has started the instance.
    ServiceContext* const _service;
};

}  // namespace embedded
}  // namespace mongo
