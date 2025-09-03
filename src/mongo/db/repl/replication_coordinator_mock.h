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
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/split_horizon/split_horizon.h"
#include "mongo/db/repl/split_prepare_session_manager.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace executor {
struct ConnectionPoolStats;
}  // namespace executor

namespace repl {

MONGO_MOD_PUB inline repl::ReplSettings createServerlessReplSettings() {
    repl::ReplSettings settings;
    settings.setOplogSizeBytes(5 * 1024 * 1024);
    settings.setServerlessMode();
    return settings;
}

/**
 * A mock ReplicationCoordinator.  Currently it is extremely simple and exists solely to link
 * into dbtests.
 */
class MONGO_MOD_OPEN ReplicationCoordinatorMock : public ReplicationCoordinator {
    ReplicationCoordinatorMock(const ReplicationCoordinatorMock&) = delete;
    ReplicationCoordinatorMock& operator=(const ReplicationCoordinatorMock&) = delete;

public:
    ReplicationCoordinatorMock(ServiceContext* service, const ReplSettings& settings);

    ReplicationCoordinatorMock(ServiceContext* service, StorageInterface* storage);

    /**
     * Creates a ReplicationCoordinatorMock with ReplSettings for a one-node replica set.
     */
    explicit ReplicationCoordinatorMock(ServiceContext* service);

    ~ReplicationCoordinatorMock() override;

    void startup(OperationContext* opCtx,
                 StorageEngine::LastShutdownState lastShutdownState) override;

    void enterTerminalShutdown() override;

    bool enterQuiesceModeIfSecondary(Milliseconds quieseTime) override;

    bool inQuiesceMode() const override;

    void shutdown(OperationContext* opCtx, BSONObjBuilder* shutdownTimeElapsedBuilder) override;

    void appendDiagnosticBSON(BSONObjBuilder* bob, StringData leafName) override {}

    const ReplSettings& getSettings() const override;

    MemberState getMemberState() const override;

    bool canAcceptNonLocalWrites() const override;

    Status waitForMemberState(Interruptible* interruptible,
                              MemberState expectedState,
                              Milliseconds timeout) override;

    bool isInPrimaryOrSecondaryState(OperationContext* opCtx) const override;

    bool isInPrimaryOrSecondaryState_UNSAFE() const override;

    Seconds getSecondaryDelaySecs() const override;

    void clearSyncSourceDenylist() override;

    ReplicationCoordinator::StatusAndDuration awaitReplication(
        OperationContext* opCtx,
        const OpTime& opTime,
        const WriteConcernOptions& writeConcern) override;

    SharedSemiFuture<void> awaitReplicationAsyncNoWTimeout(
        const OpTime& opTime, const WriteConcernOptions& writeConcern) override;

    void stepDown(OperationContext* opCtx,
                  bool force,
                  const Milliseconds& waitTime,
                  const Milliseconds& stepdownTime) override;

    bool isWritablePrimaryForReportingPurposes() override;

    bool canAcceptWritesForDatabase(OperationContext* opCtx, const DatabaseName& dbName) override;

    MONGO_MOD_USE_REPLACEMENT(ReplicationCoordinatorMock::canAcceptWritesForDatabase)
    bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                           const DatabaseName& dbName) override;

    bool canAcceptWritesFor(OperationContext* opCtx,
                            const NamespaceStringOrUUID& nsOrUUID) override;

    MONGO_MOD_USE_REPLACEMENT(ReplicationCoordinatorMock::canAcceptWritesFor)
    bool canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                   const NamespaceStringOrUUID& nsOrUUID) override;

    Status checkIfWriteConcernCanBeSatisfied(
        const WriteConcernOptions& writeConcern) const override;

    Status checkIfCommitQuorumCanBeSatisfied(
        const CommitQuorumOptions& commitQuorum) const override;

    bool isCommitQuorumSatisfied(const CommitQuorumOptions& commitQuorum,
                                 const std::vector<mongo::HostAndPort>& members) const override;

    Status checkCanServeReadsFor(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 bool secondaryOk) override;
    MONGO_MOD_USE_REPLACEMENT(ReplicationCoordinatorMock::checkCanServeReadsFor)
    Status checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        bool secondaryOk) override;

    bool shouldRelaxIndexConstraints(OperationContext* opCtx, const NamespaceString& ns) override;

    void setMyLastWrittenOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime) override;
    void setMyLastAppliedOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime) override;
    void setMyLastDurableOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime) override;
    void setMyLastAppliedAndLastWrittenOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime) override;
    void setMyLastDurableAndLastWrittenOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime) override;

    void resetMyLastOpTimes() override;

    void setMyHeartbeatMessage(const std::string& msg) override;

    OpTime getMyLastWrittenOpTime() const override;
    OpTimeAndWallTime getMyLastWrittenOpTimeAndWallTime(bool rollbackSafe) const override;

    OpTimeAndWallTime getMyLastAppliedOpTimeAndWallTime() const override;
    OpTime getMyLastAppliedOpTime() const override;

    OpTimeAndWallTime getMyLastDurableOpTimeAndWallTime() const override;
    OpTime getMyLastDurableOpTime() const override;

    Status waitUntilMajorityOpTime(OperationContext* opCtx,
                                   OpTime targetOpTime,
                                   boost::optional<Date_t> deadline) override;

    Status waitUntilOpTimeForRead(OperationContext* opCtx,
                                  const ReadConcernArgs& settings) override;

    Status waitUntilOpTimeForReadUntil(OperationContext* opCtx,
                                       const ReadConcernArgs& settings,
                                       boost::optional<Date_t> deadline) override;
    Status waitUntilOpTimeWrittenUntil(OperationContext* opCtx,
                                       LogicalTime clusterTime,
                                       boost::optional<Date_t> deadline) override;
    Status awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) override;
    OID getElectionId() override;

    int getMyId() const override;

    HostAndPort getMyHostAndPort() const override;

    Status setFollowerMode(const MemberState& newState) override;

    Status setFollowerModeRollback(OperationContext* opCtx) override;

    OplogSyncState getOplogSyncState() override;

    void signalWriterDrainComplete(OperationContext*, long long) noexcept override;

    void signalApplierDrainComplete(OperationContext*, long long) noexcept override;

    void signalUpstreamUpdater() override;

    StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const override;

    Status processReplSetGetStatus(OperationContext* opCtx,
                                   BSONObjBuilder*,
                                   ReplSetGetStatusResponseStyle) override;

    void appendSecondaryInfoData(BSONObjBuilder* result) override;

    void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    ReplSetConfig getConfig() const override;

    ConnectionString getConfigConnectionString() const override;

    std::int64_t getConfigTerm() const override;

    std::int64_t getConfigVersion() const override;

    ConfigVersionAndTerm getConfigVersionAndTerm() const override;

    boost::optional<MemberConfig> findConfigMemberByHostAndPort_deprecated(
        const HostAndPort& hap) const override;

    Status validateWriteConcern(const WriteConcernOptions& writeConcern) const override;

    void processReplSetGetConfig(BSONObjBuilder* result,
                                 bool commitmentStatus = false,
                                 bool includeNewlyAdded = false) override;

    void processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) override;

    void advanceCommitPoint(const OpTimeAndWallTime& committedOptimeAndWallTime,
                            bool fromSyncSource) override;

    void cancelAndRescheduleElectionTimeout() override;

    Status setMaintenanceMode(OperationContext* opCtx, bool activate) override;

    bool getMaintenanceMode() override;

    Status processReplSetSyncFrom(OperationContext* opCtx,
                                  const HostAndPort& target,
                                  BSONObjBuilder* resultObj) override;

    Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj) override;

    Status processReplSetReconfig(OperationContext* opCtx,
                                  const ReplSetReconfigArgs& args,
                                  BSONObjBuilder* resultObj) override;

    Status doReplSetReconfig(OperationContext* opCtx,
                             GetNewConfigFn getNewConfig,
                             bool force) override;

    Status doOptimizedReconfig(OperationContext* opCtx, GetNewConfigFn getNewConfig) override;

    Status awaitConfigCommitment(OperationContext* opCtx,
                                 bool waitForOplogCommitment,
                                 long long term) override;

    Status processReplSetInitiate(OperationContext* opCtx,
                                  const BSONObj& configObj,
                                  BSONObjBuilder* resultObj) override;

    Status processReplSetUpdatePosition(const UpdatePositionArgs& updates) override;

    bool buildsIndexes() override;

    std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op, bool durablyWritten) override;

    WriteConcernOptions getGetLastErrorDefault() override;

    Status checkReplEnabledForCommand(BSONObjBuilder* result) override;

    HostAndPort chooseNewSyncSource(const OpTime& lastOpTimeFetched) override;

    void denylistSyncSource(const HostAndPort& host, Date_t until) override;

    void resetLastOpTimesFromOplog(OperationContext* opCtx) override;

    bool lastOpTimesWereReset() const;

    ChangeSyncSourceAction shouldChangeSyncSource(const HostAndPort& currentSource,
                                                  const rpc::ReplSetMetadata& replMetadata,
                                                  const rpc::OplogQueryMetadata& oqMetadata,
                                                  const OpTime& previousOpTimeFetched,
                                                  const OpTime& lastOpTimeFetched) const override;

    ChangeSyncSourceAction shouldChangeSyncSourceOnError(
        const HostAndPort& currentSource, const OpTime& lastOpTimeFetched) const override;

    OpTime getLastCommittedOpTime() const override;

    OpTimeAndWallTime getLastCommittedOpTimeAndWallTime() const override;

    std::vector<MemberData> getMemberData() const override;

    Status processReplSetRequestVotes(OperationContext* opCtx,
                                      const ReplSetRequestVotesArgs& args,
                                      ReplSetRequestVotesResponse* response) override;

    void prepareReplMetadata(const GenericArguments& genericArgs,
                             const OpTime& lastOpTimeFromClient,
                             BSONObjBuilder* builder) const override;

    Status processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                              ReplSetHeartbeatResponse* response) override;

    /**
     * Set the value for getWriteConcernMajorityShouldJournal()
     */
    void setWriteConcernMajorityShouldJournal(bool shouldJournal);

    bool getWriteConcernMajorityShouldJournal() override;

    long long getTerm() const override;

    Status updateTerm(OperationContext* opCtx, long long term) override;

    void clearCommittedSnapshot() override;

    void setCurrentCommittedSnapshotOpTime(OpTime time);

    OpTime getCurrentCommittedSnapshotOpTime() const override;

    void waitUntilSnapshotCommitted(OperationContext* opCtx,
                                    const Timestamp& untilSnapshot) override;

    void createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) override;

    Status waitForPrimaryMajorityReadsAvailable(OperationContext* opCtx) const override;

    WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(WriteConcernOptions wc) override;

    Status stepUpIfEligible(bool skipDryRun) override;

    /**
     * Sets the return value for calls to getConfig.
     */
    void setGetConfigReturnValue(ReplSetConfig returnValue);

    /**
     * Sets the function to generate the return value for calls to awaitReplication().
     * 'OperationContext' and 'opTime' are the parameters passed to awaitReplication().
     */
    using AwaitReplicationReturnValueFunction =
        std::function<StatusAndDuration(OperationContext*, const OpTime&)>;
    void setAwaitReplicationReturnValueFunction(
        AwaitReplicationReturnValueFunction returnValueFunction);

    using RunCmdOnPrimaryAndAwaitResponseFunction =
        std::function<BSONObj(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              OnRemoteCmdScheduledFn onRemoteCmdScheduled,
                              OnRemoteCmdCompleteFn onRemoteCmdComplete)>;

    /**
     * Always allow writes even if this node is a writable primary. Used by sharding unit tests.
     */
    void alwaysAllowWrites(bool allowWrites);

    ServiceContext* getServiceContext() override {
        return _service;
    }

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

    virtual void setCanAcceptNonLocalWrites(bool canAcceptNonLocalWrites);

    TopologyVersion getTopologyVersion() const override;

    void incrementTopologyVersion() override;

    std::shared_ptr<const HelloResponse> awaitHelloResponse(
        OperationContext* opCtx,
        const SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> clientTopologyVersion,
        boost::optional<Date_t> deadline) override;

    SharedSemiFuture<std::shared_ptr<const HelloResponse>> getHelloResponseFuture(
        const SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> clientTopologyVersion) override;

    StatusWith<OpTime> getLatestWriteOpTime(OperationContext* opCtx) const noexcept override;

    HostAndPort getCurrentPrimaryHostAndPort() const override;

    void cancelCbkHandle(executor::TaskExecutor::CallbackHandle activeHandle) override;
    BSONObj runCmdOnPrimaryAndAwaitResponse(OperationContext* opCtx,
                                            const DatabaseName& dbName,
                                            const BSONObj& cmdObj,
                                            OnRemoteCmdScheduledFn onRemoteCmdScheduled,
                                            OnRemoteCmdCompleteFn onRemoteCmdComplete) override;
    void restartScheduledHeartbeats_forTest() override;

    void recordIfCWWCIsSetOnConfigServerOnStartup(OperationContext* opCtx) final;

    class WriteConcernTagChangesMock : public WriteConcernTagChanges {
        ~WriteConcernTagChangesMock() override = default;
        bool reserveDefaultWriteConcernChange() override {
            return false;
        };
        void releaseDefaultWriteConcernChange() override {}

        bool reserveConfigWriteConcernTagChange() override {
            return false;
        };
        void releaseConfigWriteConcernTagChange() override {}
    };

    WriteConcernTagChanges* getWriteConcernTagChanges() override;

    SplitPrepareSessionManager* getSplitPrepareSessionManager() override;

    /**
     * If this is true, the mock will update the "committed snapshot" everytime the "last applied"
     * is updated. That behavior can be disabled for tests that need more control over what's
     * majority committed.
     */
    void setUpdateCommittedSnapshot(bool val) {
        _updateCommittedSnapshot = val;
    }

    bool isRetryableWrite(OperationContext* opCtx) const override {
        return false;
    }

    boost::optional<UUID> getInitialSyncId(OperationContext* opCtx) override;

    void setSecondaryDelaySecs(Seconds sec);

    void setOplogSyncState(const OplogSyncState& newState);

    void setConsistentDataAvailable(OperationContext* opCtx, bool isDataMajorityCommitted) override;
    bool isDataConsistent() const override;
    void clearSyncSource() override;

private:
    void _setMyLastAppliedOpTimeAndWallTime(WithLock lk,
                                            const OpTimeAndWallTime& opTimeAndWallTime);
    void _setCurrentCommittedSnapshotOpTime(WithLock lk, OpTime time);

    ServiceContext* const _service;
    ReplSettings _settings;
    StorageInterface* _storage = nullptr;
    AwaitReplicationReturnValueFunction _awaitReplicationReturnValueFunction = [](OperationContext*,
                                                                                  const OpTime&) {
        return StatusAndDuration(Status::OK(), Milliseconds(0));
    };
    RunCmdOnPrimaryAndAwaitResponseFunction _runCmdOnPrimaryAndAwaitResponseFn;

    // Guards all the variables below
    mutable stdx::mutex _mutex;

    MemberState _memberState;
    ReplSetConfig _getConfigReturnValue;
    OpTime _myLastWrittenOpTime;
    Date_t _myLastWrittenWallTime;
    OpTime _myLastDurableOpTime;
    Date_t _myLastDurableWallTime;
    OpTime _myLastAppliedOpTime;
    Date_t _myLastAppliedWallTime;
    OpTime _lastCommittedOpTime;
    Date_t _lastCommittedWallTime;
    OpTime _currentCommittedSnapshotOpTime;
    BSONObj _latestReconfig;

    long long _term = OpTime::kInitialTerm;
    bool _resetLastOpTimesCalled = false;
    bool _alwaysAllowWrites = false;
    bool _canAcceptNonLocalWrites = false;

    SplitPrepareSessionManager _splitSessionManager;
    bool _updateCommittedSnapshot = true;

    Seconds _secondaryDelaySecs = Seconds(0);
    OplogSyncState _oplogSyncState = OplogSyncState::Running;

    bool _writeConcernMajorityShouldJournal = true;
};

}  // namespace repl
}  // namespace mongo
