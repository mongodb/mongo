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

#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
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
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/split_horizon.h"
#include "mongo/db/repl/split_prepare_session_manager.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace executor {
struct ConnectionPoolStats;
}  // namespace executor

namespace repl {

inline repl::ReplSettings createServerlessReplSettings() {
    repl::ReplSettings settings;
    settings.setOplogSizeBytes(5 * 1024 * 1024);
    settings.setServerlessMode();
    return settings;
}

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

    virtual void startup(OperationContext* opCtx,
                         StorageEngine::LastShutdownState lastShutdownState);

    virtual void enterTerminalShutdown();

    virtual bool enterQuiesceModeIfSecondary(Milliseconds quieseTime);

    virtual bool inQuiesceMode() const;

    virtual void shutdown(OperationContext* opCtx, BSONObjBuilder* shutdownTimeElapsedBuilder);

    void appendDiagnosticBSON(BSONObjBuilder* bob, StringData leafName) override {}

    virtual const ReplSettings& getSettings() const;

    virtual MemberState getMemberState() const;

    virtual bool canAcceptNonLocalWrites() const;

    virtual Status waitForMemberState(Interruptible* interruptible,
                                      MemberState expectedState,
                                      Milliseconds timeout) override;

    virtual bool isInPrimaryOrSecondaryState(OperationContext* opCtx) const;

    virtual bool isInPrimaryOrSecondaryState_UNSAFE() const;

    virtual Seconds getSecondaryDelaySecs() const;

    virtual void clearSyncSourceDenylist();

    virtual ReplicationCoordinator::StatusAndDuration awaitReplication(
        OperationContext* opCtx, const OpTime& opTime, const WriteConcernOptions& writeConcern);

    virtual SharedSemiFuture<void> awaitReplicationAsyncNoWTimeout(
        const OpTime& opTime, const WriteConcernOptions& writeConcern);

    void stepDown(OperationContext* opCtx,
                  bool force,
                  const Milliseconds& waitTime,
                  const Milliseconds& stepdownTime) override;

    virtual bool isWritablePrimaryForReportingPurposes();

    virtual bool canAcceptWritesForDatabase(OperationContext* opCtx, const DatabaseName& dbName);

    virtual bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                                   const DatabaseName& dbName);

    bool canAcceptWritesFor(OperationContext* opCtx,
                            const NamespaceStringOrUUID& nsOrUUID) override;

    bool canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                   const NamespaceStringOrUUID& nsOrUUID) override;

    virtual Status checkIfWriteConcernCanBeSatisfied(const WriteConcernOptions& writeConcern) const;

    virtual Status checkIfCommitQuorumCanBeSatisfied(const CommitQuorumOptions& commitQuorum) const;

    virtual bool isCommitQuorumSatisfied(const CommitQuorumOptions& commitQuorum,
                                         const std::vector<mongo::HostAndPort>& members) const;

    virtual Status checkCanServeReadsFor(OperationContext* opCtx,
                                         const NamespaceString& ns,
                                         bool secondaryOk);
    virtual Status checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                const NamespaceString& ns,
                                                bool secondaryOk);

    virtual bool shouldRelaxIndexConstraints(OperationContext* opCtx, const NamespaceString& ns);

    virtual void setMyLastWrittenOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime);
    virtual void setMyLastAppliedOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime);
    virtual void setMyLastDurableOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime);
    virtual void setMyLastAppliedAndLastWrittenOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime);
    virtual void setMyLastDurableAndLastWrittenOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime);

    virtual void resetMyLastOpTimes();

    virtual void setMyHeartbeatMessage(const std::string& msg);

    virtual OpTime getMyLastWrittenOpTime() const;
    virtual OpTimeAndWallTime getMyLastWrittenOpTimeAndWallTime(bool rollbackSafe) const;

    virtual OpTimeAndWallTime getMyLastAppliedOpTimeAndWallTime() const;
    virtual OpTime getMyLastAppliedOpTime() const;

    virtual OpTimeAndWallTime getMyLastDurableOpTimeAndWallTime() const;
    virtual OpTime getMyLastDurableOpTime() const;

    virtual Status waitUntilMajorityOpTime(OperationContext* opCtx,
                                           OpTime targetOpTime,
                                           boost::optional<Date_t> deadline) override;

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

    virtual Status setFollowerModeRollback(OperationContext* opCtx);

    virtual ApplierState getApplierState();

    virtual void signalDrainComplete(OperationContext*, long long) noexcept;

    virtual void signalUpstreamUpdater();

    virtual StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const override;

    virtual Status processReplSetGetStatus(OperationContext* opCtx,
                                           BSONObjBuilder*,
                                           ReplSetGetStatusResponseStyle);

    virtual void appendSecondaryInfoData(BSONObjBuilder* result);

    void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    virtual ReplSetConfig getConfig() const;

    virtual ConnectionString getConfigConnectionString() const override;

    virtual Milliseconds getConfigElectionTimeoutPeriod() const override;

    virtual std::vector<MemberConfig> getConfigVotingMembers() const override;

    virtual size_t getNumConfigVotingMembers() const override;

    virtual std::int64_t getConfigTerm() const override;

    virtual std::int64_t getConfigVersion() const override;

    virtual ConfigVersionAndTerm getConfigVersionAndTerm() const override;

    virtual int getConfigNumMembers() const override;

    virtual Milliseconds getConfigHeartbeatTimeoutPeriodMillis() const override;

    virtual BSONObj getConfigBSON() const override;

    virtual boost::optional<MemberConfig> findConfigMemberByHostAndPort_deprecated(
        const HostAndPort& hap) const override;

    virtual Status validateWriteConcern(const WriteConcernOptions& writeConcern) const override;

    virtual bool isConfigLocalHostAllowed() const override;

    virtual Milliseconds getConfigHeartbeatInterval() const override;

    virtual void processReplSetGetConfig(BSONObjBuilder* result,
                                         bool commitmentStatus = false,
                                         bool includeNewlyAdded = false);

    virtual void processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) override;

    virtual void advanceCommitPoint(const OpTimeAndWallTime& committedOptimeAndWallTime,
                                    bool fromSyncSource) override;

    virtual void cancelAndRescheduleElectionTimeout() override;

    virtual Status setMaintenanceMode(OperationContext* opCtx, bool activate);

    virtual bool getMaintenanceMode();

    virtual bool shouldDropSyncSourceAfterShardSplit(OID replicaSetId) const;

    virtual Status processReplSetSyncFrom(OperationContext* opCtx,
                                          const HostAndPort& target,
                                          BSONObjBuilder* resultObj);

    virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj);

    virtual Status processReplSetReconfig(OperationContext* opCtx,
                                          const ReplSetReconfigArgs& args,
                                          BSONObjBuilder* resultObj);

    BSONObj getLatestReconfig();

    virtual Status doReplSetReconfig(OperationContext* opCtx,
                                     GetNewConfigFn getNewConfig,
                                     bool force);

    virtual Status doOptimizedReconfig(OperationContext* opCtx, GetNewConfigFn getNewConfig);

    Status awaitConfigCommitment(OperationContext* opCtx,
                                 bool waitForOplogCommitment,
                                 long long term);

    virtual Status processReplSetInitiate(OperationContext* opCtx,
                                          const BSONObj& configObj,
                                          BSONObjBuilder* resultObj);

    virtual Status processReplSetUpdatePosition(const UpdatePositionArgs& updates);

    virtual bool buildsIndexes();

    virtual std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op, bool durablyWritten);

    virtual WriteConcernOptions getGetLastErrorDefault();

    virtual Status checkReplEnabledForCommand(BSONObjBuilder* result);

    virtual HostAndPort chooseNewSyncSource(const OpTime& lastOpTimeFetched);

    virtual void denylistSyncSource(const HostAndPort& host, Date_t until);

    virtual void resetLastOpTimesFromOplog(OperationContext* opCtx);

    bool lastOpTimesWereReset() const;

    virtual ChangeSyncSourceAction shouldChangeSyncSource(const HostAndPort& currentSource,
                                                          const rpc::ReplSetMetadata& replMetadata,
                                                          const rpc::OplogQueryMetadata& oqMetadata,
                                                          const OpTime& previousOpTimeFetched,
                                                          const OpTime& lastOpTimeFetched) const;

    virtual ChangeSyncSourceAction shouldChangeSyncSourceOnError(
        const HostAndPort& currentSource, const OpTime& lastOpTimeFetched) const;

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

    virtual long long getTerm() const;

    virtual Status updateTerm(OperationContext* opCtx, long long term);

    virtual void clearCommittedSnapshot() override;

    void setCurrentCommittedSnapshotOpTime(OpTime time);

    virtual OpTime getCurrentCommittedSnapshotOpTime() const override;

    virtual void waitUntilSnapshotCommitted(OperationContext* opCtx,
                                            const Timestamp& untilSnapshot) override;

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
        std::function<StatusAndDuration(OperationContext*, const OpTime&)>;
    void setAwaitReplicationReturnValueFunction(
        AwaitReplicationReturnValueFunction returnValueFunction);

    using RunCmdOnPrimaryAndAwaitResponseFunction =
        std::function<BSONObj(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              OnRemoteCmdScheduledFn onRemoteCmdScheduled,
                              OnRemoteCmdCompleteFn onRemoteCmdComplete)>;
    void setRunCmdOnPrimaryAndAwaitResponseFunction(
        RunCmdOnPrimaryAndAwaitResponseFunction runCmdFunction);

    /**
     * Always allow writes even if this node is a writable primary. Used by sharding unit tests.
     */
    void alwaysAllowWrites(bool allowWrites);

    virtual ServiceContext* getServiceContext() override {
        return _service;
    }

    virtual Status abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) override;

    virtual void incrementNumCatchUpOpsIfCatchingUp(long numOps) override;

    virtual boost::optional<Timestamp> getRecoveryTimestamp() override;

    virtual bool setContainsArbiter() const override;

    virtual void attemptToAdvanceStableTimestamp() override;

    virtual void finishRecoveryIfEligible(OperationContext* opCtx) override;

    virtual void updateAndLogStateTransitionMetrics(
        ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
        size_t numOpsKilled,
        size_t numOpsRunning) const override;

    virtual void setCanAcceptNonLocalWrites(bool canAcceptNonLocalWrites);

    virtual TopologyVersion getTopologyVersion() const;

    virtual void incrementTopologyVersion() override;

    virtual std::shared_ptr<const HelloResponse> awaitHelloResponse(
        OperationContext* opCtx,
        const SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> clientTopologyVersion,
        boost::optional<Date_t> deadline) override;

    virtual SharedSemiFuture<std::shared_ptr<const HelloResponse>> getHelloResponseFuture(
        const SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> clientTopologyVersion) override;

    virtual StatusWith<OpTime> getLatestWriteOpTime(
        OperationContext* opCtx) const noexcept override;

    virtual HostAndPort getCurrentPrimaryHostAndPort() const override;

    void cancelCbkHandle(executor::TaskExecutor::CallbackHandle activeHandle) override;
    BSONObj runCmdOnPrimaryAndAwaitResponse(OperationContext* opCtx,
                                            const DatabaseName& dbName,
                                            const BSONObj& cmdObj,
                                            OnRemoteCmdScheduledFn onRemoteCmdScheduled,
                                            OnRemoteCmdCompleteFn onRemoteCmdComplete) override;
    virtual void restartScheduledHeartbeats_forTest() override;

    virtual void recordIfCWWCIsSetOnConfigServerOnStartup(OperationContext* opCtx) final;

    class WriteConcernTagChangesMock : public WriteConcernTagChanges {
        virtual ~WriteConcernTagChangesMock() = default;
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

    void setApplierState(const ApplierState& newState);

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
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ReplicationCoordinatorExternalStateMock::_mutex");

    MemberState _memberState;
    ReplSetConfig _getConfigReturnValue;
    OpTime _myLastWrittenOpTime;
    Date_t _myLastWrittenWallTime;
    OpTime _myLastDurableOpTime;
    Date_t _myLastDurableWallTime;
    OpTime _myLastAppliedOpTime;
    Date_t _myLastAppliedWallTime;
    OpTime _currentCommittedSnapshotOpTime;
    BSONObj _latestReconfig;

    long long _term = OpTime::kInitialTerm;
    bool _resetLastOpTimesCalled = false;
    bool _alwaysAllowWrites = false;
    bool _canAcceptNonLocalWrites = false;

    SplitPrepareSessionManager _splitSessionManager;
    bool _updateCommittedSnapshot = true;

    Seconds _secondaryDelaySecs = Seconds(0);
    ApplierState _applierState = ApplierState::Running;
};

}  // namespace repl
}  // namespace mongo
