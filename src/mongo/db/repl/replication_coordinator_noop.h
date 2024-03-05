/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

    void startup(OperationContext* opCtx, StorageEngine::LastShutdownState lastShutdownState) final;

    void enterTerminalShutdown() final;

    bool enterQuiesceModeIfSecondary(Milliseconds quiesceTime) final;

    bool inQuiesceMode() const final;

    void shutdown(OperationContext* opCtx, BSONObjBuilder* shutdownTimeElapsedBuilder) final;

    ServiceContext* getServiceContext() final {
        return _service;
    }

    const ReplSettings& getSettings() const final;

    bool getMaintenanceMode() final;

    bool isWritablePrimaryForReportingPurposes() final;
    bool isInPrimaryOrSecondaryState(OperationContext* opCtx) const final;
    bool isInPrimaryOrSecondaryState_UNSAFE() const final;

    bool canAcceptWritesForDatabase(OperationContext* opCtx, const DatabaseName& dbName) final;
    bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                           const DatabaseName& dbName) final;

    bool canAcceptWritesFor(OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID) final;
    bool canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                   const NamespaceStringOrUUID& nsOrUUID) final;

    Status checkCanServeReadsFor(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 bool secondaryOk) final;
    Status checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        bool secondaryOk) final;

    bool shouldRelaxIndexConstraints(OperationContext* opCtx, const NamespaceString& ns) final;

    WriteConcernOptions getGetLastErrorDefault() final;

    WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(WriteConcernOptions wc) final;

    bool buildsIndexes() final;

    MemberState getMemberState() const final;

    bool canAcceptNonLocalWrites() const final;

    std::vector<MemberData> getMemberData() const final;

    Status waitForMemberState(Interruptible*, MemberState, Milliseconds) final;

    Seconds getSecondaryDelaySecs() const final;

    void clearSyncSourceDenylist() final;

    ReplicationCoordinator::StatusAndDuration awaitReplication(OperationContext*,
                                                               const OpTime&,
                                                               const WriteConcernOptions&) final;

    SharedSemiFuture<void> awaitReplicationAsyncNoWTimeout(
        const OpTime& opTime, const WriteConcernOptions& writeConcern) final;

    void stepDown(OperationContext*, bool, const Milliseconds&, const Milliseconds&) final;

    Status checkIfWriteConcernCanBeSatisfied(const WriteConcernOptions&) const final;

    Status checkIfCommitQuorumCanBeSatisfied(const CommitQuorumOptions& commitQuorum) const final;

    bool isCommitQuorumSatisfied(const CommitQuorumOptions& commitQuorum,
                                 const std::vector<mongo::HostAndPort>& members) const final;

    void setMyLastWrittenOpTimeAndWallTimeForward(const OpTimeAndWallTime& opTimeAndWallTime) final;
    void setMyLastAppliedOpTimeAndWallTimeForward(const OpTimeAndWallTime& opTimeAndWallTime) final;
    void setMyLastDurableOpTimeAndWallTimeForward(const OpTimeAndWallTime& opTimeAndWallTime) final;
    void setMyLastAppliedAndLastWrittenOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime) final;
    void setMyLastDurableAndLastWrittenOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime) final;

    void resetMyLastOpTimes() final;

    void setMyHeartbeatMessage(const std::string&) final;

    OpTime getMyLastWrittenOpTime() const final;
    OpTimeAndWallTime getMyLastWrittenOpTimeAndWallTime(bool rollbackSafe = false) const final;

    OpTime getMyLastAppliedOpTime() const final;
    OpTimeAndWallTime getMyLastAppliedOpTimeAndWallTime() const final;

    OpTime getMyLastDurableOpTime() const final;
    OpTimeAndWallTime getMyLastDurableOpTimeAndWallTime() const final;

    Status waitUntilMajorityOpTime(OperationContext* opCtx,
                                   OpTime targetOpTime,
                                   boost::optional<Date_t> deadline) final;

    Status waitUntilOpTimeForReadUntil(OperationContext*,
                                       const ReadConcernArgs&,
                                       boost::optional<Date_t>) final;
    Status waitUntilOpTimeWrittenUntil(OperationContext*,
                                       LogicalTime,
                                       boost::optional<Date_t>) final;

    Status waitUntilOpTimeForRead(OperationContext*, const ReadConcernArgs&) final;
    Status awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) final;

    OID getElectionId() final;

    int getMyId() const final;

    HostAndPort getMyHostAndPort() const final;

    Status setFollowerMode(const MemberState&) final;

    Status setFollowerModeRollback(OperationContext* opCtx) final;

    ApplierState getApplierState() final;

    void signalDrainComplete(OperationContext*, long long) noexcept final;

    void signalUpstreamUpdater() final;

    StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const final;

    Status processReplSetGetStatus(OperationContext* opCtx,
                                   BSONObjBuilder*,
                                   ReplSetGetStatusResponseStyle) final;

    void appendSecondaryInfoData(BSONObjBuilder*) final;

    ReplSetConfig getConfig() const final;

    ConnectionString getConfigConnectionString() const final;

    Milliseconds getConfigElectionTimeoutPeriod() const final;

    std::vector<MemberConfig> getConfigVotingMembers() const final;

    size_t getNumConfigVotingMembers() const final;

    std::int64_t getConfigTerm() const final;

    std::int64_t getConfigVersion() const final;

    ConfigVersionAndTerm getConfigVersionAndTerm() const final;

    int getConfigNumMembers() const final;

    Milliseconds getConfigHeartbeatTimeoutPeriodMillis() const final;

    BSONObj getConfigBSON() const final;

    boost::optional<MemberConfig> findConfigMemberByHostAndPort_deprecated(
        const HostAndPort& hap) const final;

    bool isConfigLocalHostAllowed() const final;

    Milliseconds getConfigHeartbeatInterval() const final;

    Status validateWriteConcern(const WriteConcernOptions& writeConcern) const final;

    void processReplSetGetConfig(BSONObjBuilder*,
                                 bool commitmentStatus = false,
                                 bool includeNewlyAdded = false) final;

    void processReplSetMetadata(const rpc::ReplSetMetadata&) final;

    void advanceCommitPoint(const OpTimeAndWallTime& committedOpTimeAndWallTime,
                            bool fromSyncSource) final;

    void cancelAndRescheduleElectionTimeout() final;

    Status setMaintenanceMode(OperationContext*, bool) final;

    bool shouldDropSyncSourceAfterShardSplit(OID replicaSetId) const final;

    Status processReplSetSyncFrom(OperationContext*, const HostAndPort&, BSONObjBuilder*) final;

    Status processReplSetFreeze(int, BSONObjBuilder*) final;

    Status processReplSetReconfig(OperationContext*,
                                  const ReplSetReconfigArgs&,
                                  BSONObjBuilder*) final;

    Status doReplSetReconfig(OperationContext* opCtx,
                             GetNewConfigFn getNewConfig,
                             bool force) final;

    Status doOptimizedReconfig(OperationContext* opCtx, GetNewConfigFn) final;

    Status awaitConfigCommitment(OperationContext* opCtx,
                                 bool waitForOplogCommitment,
                                 long long term) final;

    Status processReplSetInitiate(OperationContext*, const BSONObj&, BSONObjBuilder*) final;

    Status processReplSetUpdatePosition(const UpdatePositionArgs&) final;

    std::vector<HostAndPort> getHostsWrittenTo(const OpTime&, bool) final;

    Status checkReplEnabledForCommand(BSONObjBuilder*) final;

    HostAndPort chooseNewSyncSource(const OpTime&) final;

    void denylistSyncSource(const HostAndPort&, Date_t) final;

    void resetLastOpTimesFromOplog(OperationContext*) final;

    ChangeSyncSourceAction shouldChangeSyncSource(const HostAndPort&,
                                                  const rpc::ReplSetMetadata&,
                                                  const rpc::OplogQueryMetadata&,
                                                  const OpTime&,
                                                  const OpTime&) const final;

    ChangeSyncSourceAction shouldChangeSyncSourceOnError(const HostAndPort&,
                                                         const OpTime&) const final;

    OpTime getLastCommittedOpTime() const final;

    OpTimeAndWallTime getLastCommittedOpTimeAndWallTime() const final;

    Status processReplSetRequestVotes(OperationContext*,
                                      const ReplSetRequestVotesArgs&,
                                      ReplSetRequestVotesResponse*) final;

    void prepareReplMetadata(const BSONObj&, const OpTime&, BSONObjBuilder*) const final;

    Status processHeartbeatV1(const ReplSetHeartbeatArgsV1&, ReplSetHeartbeatResponse*) final;

    bool getWriteConcernMajorityShouldJournal() final;

    void clearCommittedSnapshot() final;

    long long getTerm() const final;

    Status updateTerm(OperationContext*, long long) final;

    OpTime getCurrentCommittedSnapshotOpTime() const final;

    void waitUntilSnapshotCommitted(OperationContext*, const Timestamp&) final;

    void appendDiagnosticBSON(BSONObjBuilder*, StringData) final;

    void appendConnectionStats(executor::ConnectionPoolStats* stats) const final;

    virtual void createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) final;

    Status stepUpIfEligible(bool skipDryRun) final;

    Status abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) final;

    void incrementNumCatchUpOpsIfCatchingUp(long numOps) final;

    boost::optional<Timestamp> getRecoveryTimestamp() final;

    bool setContainsArbiter() const final;

    void attemptToAdvanceStableTimestamp() final;

    void finishRecoveryIfEligible(OperationContext* opCtx) final;

    void incrementTopologyVersion() final;

    void updateAndLogStateTransitionMetrics(
        ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
        size_t numOpsKilled,
        size_t numOpsRunning) const final;

    TopologyVersion getTopologyVersion() const final;

    std::shared_ptr<const HelloResponse> awaitHelloResponse(
        OperationContext* opCtx,
        const SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> clientTopologyVersion,
        boost::optional<Date_t> deadline) final;

    SharedSemiFuture<std::shared_ptr<const HelloResponse>> getHelloResponseFuture(
        const SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> clientTopologyVersion) final;

    StatusWith<OpTime> getLatestWriteOpTime(OperationContext* opCtx) const noexcept override;

    HostAndPort getCurrentPrimaryHostAndPort() const override;

    void cancelCbkHandle(executor::TaskExecutor::CallbackHandle activeHandle) override;

    BSONObj runCmdOnPrimaryAndAwaitResponse(OperationContext* opCtx,
                                            const DatabaseName& dbName,
                                            const BSONObj& cmdObj,
                                            OnRemoteCmdScheduledFn onRemoteCmdScheduled,
                                            OnRemoteCmdCompleteFn onRemoteCmdComplete) override;

    virtual void restartScheduledHeartbeats_forTest() final;

    virtual void recordIfCWWCIsSetOnConfigServerOnStartup(OperationContext* opCtx) final;

    class WriteConcernTagChangesNoOp : public WriteConcernTagChanges {
        virtual ~WriteConcernTagChangesNoOp() = default;
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

    virtual SplitPrepareSessionManager* getSplitPrepareSessionManager() override;

    virtual bool isRetryableWrite(OperationContext* opCtx) const override;

    boost::optional<UUID> getInitialSyncId(OperationContext* opCtx) override;

private:
    ServiceContext* const _service;
    ReplSettings const _settings;
};

}  // namespace repl
}  // namespace mongo
