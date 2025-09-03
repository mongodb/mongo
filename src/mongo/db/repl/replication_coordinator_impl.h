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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/auto_get_rstl_for_stepup_stepdown.h"
#include "mongo/db/repl/delayable_timeout_callback.h"
#include "mongo/db/repl/hello/hello_response.h"
#include "mongo/db/repl/initial_sync/initial_syncer.h"
#include "mongo/db/repl/initial_sync/initial_syncer_interface.h"
#include "mongo/db/repl/intent_registry.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/member_id.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/replication_metrics_gen.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/split_horizon/split_horizon.h"
#include "mongo/db/repl/split_prepare_session_manager.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/db/replication_state_transition_lock_guard.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"
#include "mongo/util/versioned_value.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {

class Timer;
template <typename T>
class StatusWith;

namespace executor {
struct ConnectionPoolStats;
}  // namespace executor

namespace rpc {
class OplogQueryMetadata;
class ReplSetMetadata;
}  // namespace rpc

namespace repl {
// HashWriteConcernForReplication and EqualWriteConcernForReplication are used to make a hash
// map of write concerns.  They should include all the fields, and only the fields which are
// relevant to _doneWaitingForReplication -- syncMode, w, and checkCondition.
// They are declared here, rather than inside ReplCoordinatorImpl, because it is not possible
// to use the IsTrustedHasher template for an inner class.
class HashWriteConcernForReplication {
public:
    std::size_t operator()(const WriteConcernOptions& a) const {
        std::size_t seed = 0;
        boost::hash_combine(seed, stdx::to_underlying(a.syncMode));
        boost::hash_combine(seed, a.checkCondition);
        std::visit(OverloadedVisitor{[&](const std::string& s) { boost::hash_combine(seed, s); },
                                     [&](std::int64_t n) { boost::hash_combine(seed, n); },
                                     [&](const WTags& tags) {
                                         for (const auto& tag : tags) {
                                             boost::hash_combine(seed, tag.first);
                                             boost::hash_combine(seed, tag.second);
                                         }
                                     }},
                   a.w);
        return seed;
    }
};

class EqualWriteConcernForReplication {
public:
    bool operator()(const WriteConcernOptions& a, const WriteConcernOptions& b) const {
        return a.syncMode == b.syncMode && a.checkCondition == b.checkCondition && a.w == b.w;
    }
};
}  // namespace repl

template <>
struct IsTrustedHasher<repl::HashWriteConcernForReplication, WriteConcernOptions> : std::true_type {
};

namespace repl {

class HeartbeatResponseAction;
class LastVote;
class ReplicationProcess;
class ReplSetRequestVotesArgs;
class ReplSetConfig;
class SyncSourceFeedback;
class StorageInterface;
class TopologyCoordinator;

class MONGO_MOD_PUB ReplicationCoordinatorImpl : public ReplicationCoordinator,
                                                 public StepUpStepDownCoordinator {
    ReplicationCoordinatorImpl(const ReplicationCoordinatorImpl&) = delete;
    ReplicationCoordinatorImpl& operator=(const ReplicationCoordinatorImpl&) = delete;

public:
    ReplicationCoordinatorImpl(ServiceContext* serviceContext,
                               const ReplSettings& settings,
                               std::unique_ptr<ReplicationCoordinatorExternalState> externalState,
                               std::shared_ptr<executor::TaskExecutor> executor,
                               std::unique_ptr<TopologyCoordinator> topoCoord,
                               ReplicationProcess* replicationProcess,
                               StorageInterface* storage,
                               int64_t prngSeed);

    ~ReplicationCoordinatorImpl() override;

    // ================== Members of public ReplicationCoordinator API ===================

    void startup(OperationContext* opCtx,
                 StorageEngine::LastShutdownState lastShutdownState) override;

    void enterTerminalShutdown() override;

    bool enterQuiesceModeIfSecondary(Milliseconds quiesceTime) override;

    bool inQuiesceMode() const override;

    void shutdown(OperationContext* opCtx, BSONObjBuilder* shutdownTimeElapsedBuilder) override;

    const ReplSettings& getSettings() const override;

    MemberState getMemberState() const override;

    std::vector<MemberData> getMemberData() const override;

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
    bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                           const DatabaseName& dbName) override;

    bool canAcceptWritesFor(OperationContext* opCtx,
                            const NamespaceStringOrUUID& nsorUUID) override;
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
    OpTimeAndWallTime getMyLastWrittenOpTimeAndWallTime(bool rollbackSafe = false) const override;

    OpTime getMyLastAppliedOpTime() const override;
    OpTimeAndWallTime getMyLastAppliedOpTimeAndWallTime() const override;

    OpTime getMyLastDurableOpTime() const override;
    OpTimeAndWallTime getMyLastDurableOpTimeAndWallTime() const override;

    Status waitUntilMajorityOpTime(OperationContext* opCtx,
                                   OpTime targetOpTime,
                                   boost::optional<Date_t> deadline = boost::none) override;

    Status waitUntilOpTimeForReadUntil(OperationContext* opCtx,
                                       const ReadConcernArgs& readConcern,
                                       boost::optional<Date_t> deadline) override;
    Status waitUntilOpTimeWrittenUntil(OperationContext* opCtx,
                                       LogicalTime clusterTime,
                                       boost::optional<Date_t> deadline) override;

    Status waitUntilOpTimeForRead(OperationContext* opCtx,
                                  const ReadConcernArgs& readConcern) override;
    Status awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) override;
    OID getElectionId() override;

    int getMyId() const override;

    HostAndPort getMyHostAndPort() const override;

    Status setFollowerMode(const MemberState& newState) override;

    Status setFollowerModeRollback(OperationContext* opCtx) override;

    OplogSyncState getOplogSyncState() override;

    void signalWriterDrainComplete(OperationContext* opCtx,
                                   long long termWhenExhausted) noexcept override;

    void signalApplierDrainComplete(OperationContext* opCtx,
                                    long long termWhenExhausted) noexcept override;


    void signalUpstreamUpdater() override;

    StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const override;

    Status processReplSetGetStatus(OperationContext* opCtx,
                                   BSONObjBuilder* result,
                                   ReplSetGetStatusResponseStyle responseStyle) override;

    void appendSecondaryInfoData(BSONObjBuilder* result) override;

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

    void advanceCommitPoint(const OpTimeAndWallTime& committedOpTimeAndWallTime,
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

    Status doOptimizedReconfig(OperationContext* opCtx, GetNewConfigFn) override;

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

    ChangeSyncSourceAction shouldChangeSyncSource(const HostAndPort& currentSource,
                                                  const rpc::ReplSetMetadata& replMetadata,
                                                  const rpc::OplogQueryMetadata& oqMetadata,
                                                  const OpTime& previousOpTimeFetched,
                                                  const OpTime& lastOpTimeFetched) const override;

    ChangeSyncSourceAction shouldChangeSyncSourceOnError(
        const HostAndPort& currentSource, const OpTime& lastOpTimeFetched) const override;

    OpTime getLastCommittedOpTime() const override;
    OpTimeAndWallTime getLastCommittedOpTimeAndWallTime() const override;

    Status processReplSetRequestVotes(OperationContext* opCtx,
                                      const ReplSetRequestVotesArgs& args,
                                      ReplSetRequestVotesResponse* response) override;

    void prepareReplMetadata(const GenericArguments& genericArgs,
                             const OpTime& lastOpTimeFromClient,
                             BSONObjBuilder* builder) const override;


    void setOldestTimestamp(const Timestamp& timestamp) override;

    Status processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                              ReplSetHeartbeatResponse* response) override;

    bool getWriteConcernMajorityShouldJournal() override;

    void clearCommittedSnapshot() override;
    /**
     * Get current term from topology coordinator
     */
    long long getTerm() const override;

    // Returns the ServiceContext where this instance runs.
    ServiceContext* getServiceContext() override {
        return _service;
    }

    Status updateTerm(OperationContext* opCtx, long long term) override;

    OpTime getCurrentCommittedSnapshotOpTime() const override;

    void waitUntilSnapshotCommitted(OperationContext* opCtx,
                                    const Timestamp& untilSnapshot) override;

    void appendDiagnosticBSON(BSONObjBuilder*, StringData) override;

    void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    void createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) override;

    Status waitForPrimaryMajorityReadsAvailable(OperationContext* opCtx) const override;

    WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(WriteConcernOptions wc) override;

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

    using SharedHelloResponse = std::shared_ptr<const HelloResponse>;

    SharedSemiFuture<SharedHelloResponse> getHelloResponseFuture(
        const SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> clientTopologyVersion) override;

    std::shared_ptr<const HelloResponse> awaitHelloResponse(
        OperationContext* opCtx,
        const SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> clientTopologyVersion,
        boost::optional<Date_t> deadline) override;

    StatusWith<OpTime> getLatestWriteOpTime(OperationContext* opCtx) const noexcept override;

    HostAndPort getCurrentPrimaryHostAndPort() const override;

    void cancelCbkHandle(executor::TaskExecutor::CallbackHandle activeHandle) override;

    BSONObj runCmdOnPrimaryAndAwaitResponse(OperationContext* opCtx,
                                            const DatabaseName& dbName,
                                            const BSONObj& cmdObj,
                                            OnRemoteCmdScheduledFn onRemoteCmdScheduled,
                                            OnRemoteCmdCompleteFn onRemoteCmdComplete) override;

    MONGO_MOD_PRIVATE void restartScheduledHeartbeats_forTest() override;

    void recordIfCWWCIsSetOnConfigServerOnStartup(OperationContext* opCtx) final;

    SplitPrepareSessionManager* getSplitPrepareSessionManager() override;

    void clearSyncSource() override;

    // ==================== Private API ===================
    // Called by AutoGetRstlForStepUpStepDown before taking RSTL when making stepdown transitions
    MONGO_MOD_PRIVATE void autoGetRstlEnterStepDown() final;

    // Called by AutoGetRstlForStepUpStepDown before releasing RSTL when making stepdown
    // transitions.  Also called in case of failure to acquire RSTL.  There will be one call to this
    // method for each call to autoGetRSTLEnterStepDown.
    MONGO_MOD_PRIVATE void autoGetRstlExitStepDown() final;

    // ================== Test support API ===================

    /**
     * If called after startReplication(), blocks until all asynchronous
     * activities associated with replication start-up complete.
     */
    MONGO_MOD_PRIVATE void waitForStartUpComplete_forTest();

    /**
     * Gets the replica set configuration in use by the node.
     */
    MONGO_MOD_PRIVATE ReplSetConfig getReplicaSetConfig_forTest();

    /**
     * Returns scheduled time of election timeout callback.
     * Returns Date_t() if callback is not scheduled.
     */
    MONGO_MOD_NEEDS_REPLACEMENT Date_t getElectionTimeout_forTest() const;

    /*
     * Return a randomized offset amount that is scaled in proportion to the size of the
     * _electionTimeoutPeriod.
     */
    MONGO_MOD_PRIVATE Milliseconds getRandomizedElectionOffset_forTest();

    /**
     * Returns the scheduled time of the priority takeover callback. If a priority
     * takeover has not been scheduled, returns boost::none.
     */
    MONGO_MOD_PRIVATE boost::optional<Date_t> getPriorityTakeover_forTest() const;

    /**
     * Returns the scheduled time of the catchup takeover callback. If a catchup
     * takeover has not been scheduled, returns boost::none.
     */
    MONGO_MOD_PRIVATE boost::optional<Date_t> getCatchupTakeover_forTest() const;

    /**
     * Returns the catchup takeover CallbackHandle.
     */
    MONGO_MOD_PRIVATE executor::TaskExecutor::CallbackHandle getCatchupTakeoverCbh_forTest() const;

    /**
     * Returns the cached horizon topology version from most recent SplitHorizonChange.
     */
    MONGO_MOD_PRIVATE int64_t getLastHorizonChange_forTest() const;

    /**
     * Simple wrappers around _setLastOptimeForMember to make it easier to test.
     */
    MONGO_MOD_PRIVATE Status setLastAppliedOptime_forTest(long long cfgVer,
                                                          long long memberId,
                                                          const OpTime& opTime,
                                                          Date_t wallTime = Date_t());
    MONGO_MOD_PRIVATE Status setLastWrittenOptime_forTest(long long cfgVer,
                                                          long long memberId,
                                                          const OpTime& opTime,
                                                          Date_t wallTime = Date_t());
    MONGO_MOD_PRIVATE Status setLastDurableOptime_forTest(long long cfgVer,
                                                          long long memberId,
                                                          const OpTime& opTime,
                                                          Date_t wallTime = Date_t());

    /**
     * Simple test wrappers that expose private methods.
     */
    MONGO_MOD_PRIVATE void handleHeartbeatResponse_forTest(BSONObj response,
                                                           int targetIndex,
                                                           Milliseconds ping = Milliseconds(100));

    /**
     * Non-blocking version of updateTerm.
     * Returns event handle that we can use to wait for the operation to complete.
     * When the operation is complete (waitForEvent() returns), 'updateResult' will be set
     * to a status telling if the term increased or a stepdown was triggered.
     */
    MONGO_MOD_PRIVATE executor::TaskExecutor::EventHandle updateTerm_forTest(
        long long term, TopologyCoordinator::UpdateTermResult* updateResult);

    /**
     * If called after ElectionState::start(), blocks until all asynchronous
     * activities associated with election complete.
     */
    MONGO_MOD_PRIVATE void waitForElectionFinish_forTest();

    /**
     * If called after ElectionState::start(), blocks until all asynchronous
     * activities associated with election dry run complete, including writing
     * last vote and scheduling the real election.
     */
    MONGO_MOD_PRIVATE void waitForElectionDryRunFinish_forTest();

    /**
     * Waits until a stepdown attempt has begun. Callers should ensure that the stepdown attempt
     * won't fully complete before this method is called, or this method may never return.
     */
    MONGO_MOD_PRIVATE void waitForStepDownAttempt_forTest();

    /**
     * Cancels all future processing work of the VoteRequester and sets the election state to
     * kCanceled.
     */
    MONGO_MOD_PRIVATE void cancelElection_forTest();


    /**
     * Returns a pointer to the topology coordinator used by this replication coordinator.
     */
    MONGO_MOD_PRIVATE TopologyCoordinator* getTopologyCoordinator_forTest();

    /**
     * Runs the repl set initiate internal function.
     */
    MONGO_MOD_PRIVATE Status runReplSetInitiate_forTest(const BSONObj& configObj,
                                                        BSONObjBuilder* resultObj);

    /**
     * Implementation of an interface used to synchronize changes to custom write concern tags in
     * the config and custom default write concern settings.
     * See base class fore more information.
     */
    class WriteConcernTagChangesImpl : public WriteConcernTagChanges {
    public:
        WriteConcernTagChangesImpl() = default;
        ~WriteConcernTagChangesImpl() override = default;

        bool reserveDefaultWriteConcernChange() override {
            stdx::lock_guard lock(_mutex);
            if (_configWriteConcernTagChanges > 0) {
                return false;
            }
            _defaultWriteConcernChanges++;
            return true;
        }

        void releaseDefaultWriteConcernChange() override {
            stdx::lock_guard lock(_mutex);
            invariant(_defaultWriteConcernChanges > 0);
            _defaultWriteConcernChanges--;
        }

        bool reserveConfigWriteConcernTagChange() override {
            stdx::lock_guard lock(_mutex);
            if (_defaultWriteConcernChanges > 0) {
                return false;
            }
            _configWriteConcernTagChanges++;
            return true;
        }

        void releaseConfigWriteConcernTagChange() override {
            stdx::lock_guard lock(_mutex);
            invariant(_configWriteConcernTagChanges > 0);
            _configWriteConcernTagChanges--;
        }

    private:
        //
        // All member variables are labeled with one of the following codes indicating the
        // synchronization rules for accessing them.
        //
        // (R)  Read-only in concurrent operation; no synchronization required.
        // (S)  Self-synchronizing; access in any way from any context.
        // (PS) Pointer is read-only in concurrent operation, item pointed to is self-synchronizing;
        //      Access in any context.
        // (M)  Reads and writes guarded by _mutex
        // (I)  Independently synchronized, see member variable comment.

        // The number of config write concern tag changes currently underway.
        size_t _configWriteConcernTagChanges{0};  // (M)

        // The number of default write concern changes currently underway.
        size_t _defaultWriteConcernChanges{0};  // (M)

        // Used to synchronize access to the above variables.
        stdx::mutex _mutex;  // (S)
    };

    /**
     * Returns a pointer to the WriteConcernTagChanges used by this instance.
     */
    WriteConcernTagChanges* getWriteConcernTagChanges() override;

    bool isRetryableWrite(OperationContext* opCtx) const override;

    boost::optional<UUID> getInitialSyncId(OperationContext* opCtx) override;

    void setConsistentDataAvailable(OperationContext* opCtx, bool isDataMajorityCommitted) override;
    bool isDataConsistent() const override;
    MONGO_MOD_PRIVATE void setConsistentDataAvailable_forTest();

    MONGO_MOD_PRIVATE ReplicationCoordinatorExternalState* getExternalState_forTest();
    MONGO_MOD_PRIVATE executor::TaskExecutor* getReplExecutor_forTest();

private:
    using CallbackFn = executor::TaskExecutor::CallbackFn;

    using CallbackHandle = executor::TaskExecutor::CallbackHandle;

    using EventHandle = executor::TaskExecutor::EventHandle;

    using ScheduleFn = std::function<StatusWith<executor::TaskExecutor::CallbackHandle>(
        const executor::TaskExecutor::CallbackFn& work)>;

    using SharedPromiseOfHelloResponse = SharedPromise<std::shared_ptr<const HelloResponse>>;

    /**
     * Configuration states for a replica set node.
     *
     * Transition diagram:
     *
     * PreStart ------------------> ReplicationDisabled
     *    |
     *    |
     *    v
     * StartingUp -------> Uninitialized <------> Initiating
     *         \                     ^               |
     *          -------              |               |
     *                 |             |               |
     *                 v             v               |
     * Reconfig <---> Steady <----> HBReconfig       |
     *                    ^                          /
     *                    |                         /
     *                     \                       /
     *                      -----------------------
     */
    enum ConfigState {
        kConfigPreStart,
        kConfigStartingUp,
        kConfigReplicationDisabled,
        kConfigUninitialized,
        kConfigSteady,
        kConfigInitiating,
        kConfigReconfiguring,
        kConfigHBReconfiguring
    };

    /**
     * Type describing actions to take after a change to the MemberState _memberState.
     */
    enum PostMemberStateUpdateAction {
        kActionNone,
        kActionSteppedDown,
        kActionRollbackOrRemoved,
        kActionFollowerModeStateChange,
        kActionStartSingleNodeElection
    };

    struct Waiter {
        Promise<void> promise;
        boost::optional<WriteConcernOptions> writeConcern;
        // A flag to mark this waiter abandoned which allows early clean-up for the waiter.
        AtomicWord<bool> givenUp{false};
        explicit Waiter(Promise<void> p, boost::optional<WriteConcernOptions> w = boost::none)
            : promise(std::move(p)), writeConcern(w) {}
    };

    using SharedWaiterHandle = std::shared_ptr<Waiter>;

    // This is a waiter list for things waiting on local opTimes only.
    class WaiterList {
    public:
        WaiterList() = delete;
        WaiterList(Counter64& waiterCountMetric);

        // Adds waiter into the list.
        void add(WithLock lk, const OpTime& opTime, SharedWaiterHandle waiter);
        // Adds a waiter into the list and returns the future of the waiter's promise.
        std::pair<SharedSemiFuture<void>, SharedWaiterHandle> add(WithLock lk,
                                                                  const OpTime& opTime);
        // Returns whether waiter is found and removed.
        bool remove(WithLock lk, const OpTime& opTime, SharedWaiterHandle waiter);
        // Signals all waiters whose opTime is <= the given opTime (if any) that satisfy the
        // condition in func.
        void setValueIf(
            WithLock lk,
            std::function<bool(WithLock, const OpTime&, const SharedWaiterHandle&)> func,
            boost::optional<OpTime> opTime = boost::none);
        // Signals all waiters from the list and fulfills promises with Error status.
        void setErrorAll(WithLock lk, Status status);

    private:
        // Waiters sorted by OpTime.
        std::multimap<OpTime, SharedWaiterHandle> _waiters;
        // We keep a separate count outside _waiters.size() in order to avoid having to
        // take a lock to read the metric.
        Counter64& _waiterCountMetric;
    };

    // This is a waiter list for things waiting on opTimes along with a WriteConcern.  It breaks
    // the waiters up by WriteConcern (using the hash and equal functors above) so that within a
    // sub-list, the waiters are satisfied in order.
    class WriteConcernWaiterList {
    public:
        WriteConcernWaiterList() = delete;
        WriteConcernWaiterList(Counter64& waiterCountMetric);

        // Adds waiter into the list.
        void add(WithLock lk, const OpTime& opTime, SharedWaiterHandle waiter);
        // Adds a waiter into the list and returns the future of the waiter's promise.
        std::pair<SharedSemiFuture<void>, SharedWaiterHandle> add(WithLock lk,
                                                                  const OpTime& opTime,
                                                                  WriteConcernOptions w);
        // Returns whether waiter is found and removed.
        bool remove(WithLock lk, const OpTime& opTime, SharedWaiterHandle waiter);

        // Signals all waiters whose opTime is <= the given opTime (if any) that satisfy the
        // condition in func.  The func must have the property that, for any given
        // WriteConcernOptions, there is some optime T such that func returns true for all
        // optimes <= T, and false for all optimes > T.
        void setValueIf(
            WithLock lk,
            std::function<bool(WithLock, const OpTime&, const WriteConcernOptions&)> func,
            boost::optional<OpTime> opTime = boost::none);
        // Signals all waiters from the list and fulfills promises with Error status.
        void setErrorAll(WithLock lk, Status status);

    private:
        // Waiters sorted by OpTime.
        stdx::unordered_map<WriteConcernOptions,
                            std::multimap<OpTime, SharedWaiterHandle, std::less<>>,
                            HashWriteConcernForReplication,
                            EqualWriteConcernForReplication>
            _waiters;
        Counter64& _waiterCountMetric;
    };

    enum class HeartbeatState { kScheduled = 0, kSent = 1 };
    struct HeartbeatHandleMetadata {
        HeartbeatState hbState;
        HostAndPort target;
    };

    // The state and logic of primary catchup.
    //
    // When start() is called, CatchupState will schedule the timeout callback. When we get
    // responses of the latest heartbeats from all nodes, the _targetOpTime is set.
    // The primary exits catchup mode when any of the following happens.
    //   1) My last applied optime reaches the target optime, if we've received a heartbeat from all
    //      nodes.
    //   2) Catchup timeout expires.
    //   3) Primary steps down.
    //   4) The primary has to roll back to catch up.
    //   5) The primary is too stale to catch up.
    //
    // On abort, the state resets the pointer to itself in ReplCoordImpl. In other words, the
    // life cycle of the state object aligns with the conceptual state.
    // In shutdown, the timeout callback will be canceled by the executor and the state is safe to
    // destroy.
    //
    // Any function of the state must be called while holding _mutex.
    class CatchupState {
    public:
        CatchupState(ReplicationCoordinatorImpl* repl) : _repl(repl) {}
        // start() can only be called once.
        void start(WithLock lk);
        // Reset the state itself to destruct the state.
        void abort(WithLock lk, PrimaryCatchUpConclusionReason reason);
        // Heartbeat calls this function to update the target optime.
        void signalHeartbeatUpdate(WithLock lk);
        // Increment the counter for the number of ops applied during catchup.
        void incrementNumCatchUpOps(WithLock lk, long numOps);

    private:
        ReplicationCoordinatorImpl* _repl;  // Not owned.
        // Callback handle used to cancel a scheduled catchup timeout callback.
        executor::TaskExecutor::CallbackHandle _timeoutCbh;
        // Target optime to reach after which we can exit catchup mode.
        OpTime _targetOpTime;
        // Handle to a Waiter that waits for the _targetOpTime.
        SharedWaiterHandle _waiter;
        // Counter for the number of ops applied during catchup.
        long _numCatchUpOps = 0;
    };

    class ElectionState {
    public:
        ElectionState(ReplicationCoordinatorImpl* repl)
            : _repl(repl),
              _topCoord(repl->_topCoord.get()),
              _replExecutor(repl->_replExecutor.get()) {}

        /**
         * Begins an attempt to elect this node.
         * Called after an incoming heartbeat changes this node's view of the set such that it
         * believes it can be elected PRIMARY.
         * For proper concurrency, start methods must be called while holding _mutex.
         *
         * For V1 (raft) style elections the election path is:
         *      _processDryRunResult() (may skip)
         *      _startRealElection()
         *      _writeLastVoteForMyElection()
         *      _requestVotesForRealElection()
         *      _onVoteRequestComplete()
         */
        void start(WithLock lk, StartElectionReasonEnum reason);

        // Returns the election finished event.
        executor::TaskExecutor::EventHandle getElectionFinishedEvent(WithLock);

        // Returns the election dry run finished event.
        executor::TaskExecutor::EventHandle getElectionDryRunFinishedEvent(WithLock);

        // Notifies the VoteRequester to cancel further processing. Sets the election state to
        // canceled.
        void cancel(WithLock lk);

    private:
        class LoseElectionGuardV1;
        class LoseElectionDryRunGuardV1;

        /**
         * Returns the election result from the VoteRequester.
         */
        VoteRequester::Result _getElectionResult(WithLock) const;

        /**
         * Starts the VoteRequester and requests votes from known members of the replica set.
         */
        StatusWith<executor::TaskExecutor::EventHandle> _startVoteRequester(
            WithLock,
            long long term,
            bool dryRun,
            OpTime lastWrittenOpTime,
            OpTime lastAppliedOpTime,
            int primaryIndex);

        /**
         * Starts VoteRequester to run the real election when last vote write has completed.
         */
        void _requestVotesForRealElection(WithLock lk,
                                          long long newTerm,
                                          StartElectionReasonEnum reason);

        /**
         * Callback called when the dryRun VoteRequester has completed; checks the results and
         * decides whether to conduct a proper election.
         * "originalTerm" was the term during which the dry run began, if the term has since
         * changed, do not run for election.
         */
        void _processDryRunResult(long long originalTerm, StartElectionReasonEnum reason);

        /**
         * Begins executing a real election. This is called either a successful dry run, or when the
         * dry run was skipped (which may be specified for a ReplSetStepUp).
         */
        void _startRealElection(WithLock lk,
                                long long originalTerm,
                                StartElectionReasonEnum reason);

        /**
         * Writes the last vote in persistent storage after completing dry run successfully.
         * This job will be scheduled to run in DB worker threads.
         */
        void _writeLastVoteForMyElection(LastVote lastVote,
                                         const executor::TaskExecutor::CallbackArgs& cbData,
                                         StartElectionReasonEnum reason);

        /**
         * Callback called when the VoteRequester has completed; checks the results and
         * decides whether to change state to primary and alert other nodes of our primary-ness.
         * "originalTerm" was the term during which the election began, if the term has since
         * changed, do not step up as primary.
         */
        void _onVoteRequestComplete(long long originalTerm, StartElectionReasonEnum reason);

        // Not owned.
        ReplicationCoordinatorImpl* _repl;
        // The VoteRequester used to start and gather results from the election voting process.
        std::unique_ptr<VoteRequester> _voteRequester;
        // Flag that indicates whether the election has been canceled.
        bool _isCanceled = false;
        // Event that the election code will signal when the in-progress election completes.
        executor::TaskExecutor::EventHandle _electionFinishedEvent;

        // Event that the election code will signal when the in-progress election dry run completes,
        // which includes writing the last vote and scheduling the real election.
        executor::TaskExecutor::EventHandle _electionDryRunFinishedEvent;

        // Pointer to the TopologyCoordinator owned by ReplicationCoordinator.
        TopologyCoordinator* _topCoord;

        // Pointer to the executor owned by ReplicationCoordinator.
        executor::TaskExecutor* _replExecutor;
    };

    // Inner class to manage the concurrency of _canAcceptNonLocalWrites and _canServeNonLocalReads.
    class ReadWriteAbility {
    public:
        ReadWriteAbility(bool canAcceptNonLocalWrites)
            : _canAcceptNonLocalWrites(canAcceptNonLocalWrites), _canServeNonLocalReads(0U) {}

        // Asserts ReplicationStateTransitionLock is held in mode X.
        void setCanAcceptNonLocalWrites(WithLock, OperationContext* opCtx, bool newVal);

        bool canAcceptNonLocalWrites(WithLock) const;
        bool canAcceptNonLocalWrites_UNSAFE() const;  // For early errors.

        // Asserts ReplicationStateTransitionLock is held in an intent or exclusive mode.
        bool canAcceptNonLocalWrites(OperationContext* opCtx) const;

        bool canServeNonLocalReads_UNSAFE() const;

        // Asserts ReplicationStateTransitionLock is held in an intent or exclusive mode.
        bool canServeNonLocalReads(OperationContext* opCtx) const;

        // Asserts ReplicationStateTransitionLock is held in mode X.
        void setCanServeNonLocalReads(OperationContext* opCtx, unsigned int newVal);

        void setCanServeNonLocalReads_UNSAFE(unsigned int newVal);

    private:
        // Flag that indicates whether writes to databases other than "local" are allowed.  Used to
        // answer canAcceptWritesForDatabase() and canAcceptWritesFor() questions. In order to read
        // it, must have either the RSTL or the replication coordinator mutex. To set it, must have
        // both the RSTL in mode X and the replication coordinator mutex.
        // Always true for standalone nodes.
        AtomicWord<bool> _canAcceptNonLocalWrites;

        // Flag that indicates whether reads from databases other than "local" are allowed. Unlike
        // _canAcceptNonLocalWrites, above, this question is about admission control on secondaries.
        // Accidentally providing the prior value for a limited period of time is acceptable, except
        // during rollback. In order to read it, must have the RSTL. To set it when transitioning
        // into RS_ROLLBACK, must have the RSTL in mode X. Otherwise, no lock or mutex is necessary
        // to set it.
        AtomicWord<unsigned> _canServeNonLocalReads;
    };

    void _resetMyLastOpTimes(WithLock lk);

    /**
     * Returns a new WriteConcernOptions based on "wc" but with UNSET syncMode reset to JOURNAL or
     * NONE based on our rsConfig.
     */
    WriteConcernOptions _populateUnsetWriteConcernOptionsSyncMode(WithLock lk,
                                                                  WriteConcernOptions wc);

    /**
     * Returns the _writeConcernMajorityJournalDefault of our current _rsConfig.
     */
    bool getWriteConcernMajorityShouldJournal(WithLock lk) const;

    /**
     * Returns the write concerns used by oplog commitment check and config replication check.
     */
    WriteConcernOptions _getOplogCommitmentWriteConcern(WithLock lk);
    WriteConcernOptions _getConfigReplicationWriteConcern();

    /**
     * Returns the OpTime of the current committed snapshot, if one exists.
     */
    OpTime _getCurrentCommittedSnapshotOpTime(WithLock lk) const;

    /**
     *  Verifies that ReadConcernArgs match node's readConcern.
     */
    Status _validateReadConcern(OperationContext* opCtx, const ReadConcernArgs& readConcern);

    /**
     * Helper to update our saved config, cancel any pending heartbeats, and kick off sending
     * new heartbeats based on the new config.
     *
     * Returns an action to be performed after unlocking _mutex, via
     * _performPostMemberStateUpdateAction.
     */
    PostMemberStateUpdateAction _setCurrentRSConfig(WithLock lk,
                                                    OperationContext* opCtx,
                                                    const ReplSetConfig& newConfig,
                                                    int myIndex);

    /**
     * Helper to wake waiters in _replicationWaiterList waiting for opTime <= the opTime passed in
     * (or all waiters if opTime passed in is boost::none) that are doneWaitingForReplication.
     */
    void _wakeReadyWaiters(WithLock lk, boost::optional<OpTime> opTime = boost::none);

    /**
     * Scheduled to cause the ReplicationCoordinator to reconsider any state that might
     * need to change as a result of time passing - for instance becoming PRIMARY when a single
     * node replica set member's stepDown period ends.
     */
    void _handleTimePassing(const executor::TaskExecutor::CallbackArgs& cbData);

    /**
     * Chooses a candidate for election handoff and sends a ReplSetStepUp command to it.
     */
    void _performElectionHandoff();

    /**
     * Helper method for awaitReplication to register a waiter in _replicationWaiterList with the
     * given opTime and writeConcern. Called while holding _mutex.
     */
    std::pair<SharedSemiFuture<void>, SharedWaiterHandle> _startWaitingForReplication(
        WithLock lock, const OpTime& opTime, const WriteConcernOptions& writeConcern);

    /**
     * Returns an object with all of the information this node knows about the replica set's
     * progress.
     */
    BSONObj _getReplicationProgress(WithLock wl) const;

    /**
     * Returns true if the given writeConcern is satisfied up to "optime" or is unsatisfiable.
     *
     * If the writeConcern is 'majority', also waits for _currentCommittedSnapshot to be newer than
     * minSnapshot.
     */
    bool _doneWaitingForReplication(WithLock lk,
                                    const OpTime& opTime,
                                    const WriteConcernOptions& writeConcern);

    /**
     *  Returns whether or not "members" list contains at least 'numNodes'.
     */
    bool _haveNumNodesSatisfiedCommitQuorum(WithLock lk,
                                            int numNodes,
                                            const std::vector<mongo::HostAndPort>& members) const;

    /**
     * Returns whether or not "members" list matches the tagPattern.
     */
    bool _haveTaggedNodesSatisfiedCommitQuorum(
        WithLock lk,
        const ReplSetTagPattern& tagPattern,
        const std::vector<mongo::HostAndPort>& members) const;

    Status _checkIfWriteConcernCanBeSatisfied(WithLock,
                                              const WriteConcernOptions& writeConcern) const;

    Status _checkIfCommitQuorumCanBeSatisfied(WithLock,
                                              const CommitQuorumOptions& commitQuorum) const;

    int _getMyId(WithLock lk) const;

    OpTime _getMyLastWrittenOpTime(WithLock) const;
    OpTimeAndWallTime _getMyLastWrittenOpTimeAndWallTime(WithLock) const;

    OpTime _getMyLastAppliedOpTime(WithLock) const;
    OpTimeAndWallTime _getMyLastAppliedOpTimeAndWallTime(WithLock) const;

    OpTime _getMyLastDurableOpTime(WithLock) const;
    OpTimeAndWallTime _getMyLastDurableOpTimeAndWallTime(WithLock) const;

    /**
     * Helper method for updating our tracking of the last optime applied by a given node.
     * This is only valid to call on replica sets.
     * "configVersion" will be populated with our config version if it and the configVersion
     * of "args" differ.
     *
     * If either written, applied or durable optime has changed, returns the later of the three
     * (even if that's not the one which changed).  Otherwise returns a null optime.
     */
    StatusWith<OpTime> _setLastOptimeForMember(WithLock lk,
                                               const UpdatePositionArgs::UpdateInfo& args);

    /**
     * Helper for processReplSetUpdatePosition, companion to _setLastOptimeForMember above.  Updates
     * replication coordinator state and notifies waiters after remote optime updates.  Must be
     * called within the same critical section as _setLastOptimeForMember.
     */
    void _updateStateAfterRemoteOpTimeUpdates(WithLock lk, const OpTime& maxRemoteOpTime);

    /**
     * This function will report our position externally (like upstream) if necessary.
     *
     * Takes in a unique lock, that must already be locked, on _mutex.
     *
     * Lock will be released after this method finishes.
     *
     * When prioritized is set to true, the reporter will try to schedule an updatePosition request
     * even there is already one in flight.
     */
    void _reportUpstream(stdx::unique_lock<ObservableMutex<stdx::mutex>> lock, bool prioritized);

    /**
     * Helpers to set the last written, applied and durable OpTime.
     */
    void _setMyLastWrittenOpTimeAndWallTime(WithLock lk,
                                            const OpTimeAndWallTime& opTime,
                                            bool isRollbackAllowed);
    void _setMyLastAppliedOpTimeAndWallTime(WithLock lk,
                                            const OpTimeAndWallTime& opTime,
                                            bool isRollbackAllowed);
    void _setMyLastDurableOpTimeAndWallTime(WithLock lk,
                                            const OpTimeAndWallTime& opTimeAndWallTime,
                                            bool isRollbackAllowed);
    // The return bool value means whether the corresponding timestamp is advanced in these
    // functions.
    bool _setMyLastAppliedOpTimeAndWallTimeForward(WithLock lk,
                                                   const OpTimeAndWallTime& opTimeAndWallTime);
    bool _setMyLastDurableOpTimeAndWallTimeForward(WithLock lk,
                                                   const OpTimeAndWallTime& opTimeAndWallTime);

    /**
     * Schedules a heartbeat using this node's "replSetName" to be sent to "target" at "when".
     */
    void _scheduleHeartbeatToTarget(WithLock lk,
                                    const HostAndPort& target,
                                    Date_t when,
                                    std::string replSetName);

    /**
     * Processes each heartbeat response using this node's "replSetName".
     *
     * Schedules additional heartbeats, triggers elections and step downs, etc.
     */
    void _handleHeartbeatResponse(const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData,
                                  const std::string& replSetName);

    rss::consensus::ReplicationStateTransitionGuard _killConflictingOperations(
        rss::consensus::IntentRegistry::InterruptionType interrupt, OperationContext* opCtx);

    void _trackHeartbeatHandle(WithLock,
                               const StatusWith<executor::TaskExecutor::CallbackHandle>& handle,
                               HeartbeatState hbState,
                               const HostAndPort& target);

    void _untrackHeartbeatHandle(WithLock, const executor::TaskExecutor::CallbackHandle& handle);

    /*
     * Return a randomized offset amount that is scaled in proportion to the size of the
     * _electionTimeoutPeriod. Used to add randomization to an election timeout.
     */
    Milliseconds _getRandomizedElectionOffset(WithLock lk);

    /*
     * Return the upper bound of the offset amount returned by _getRandomizedElectionOffset
     * This is actually off by one, that is, the election offset is in the half-open range
     * [0, electionOffsetUpperBound)
     */
    long long _getElectionOffsetUpperBound(WithLock lk);

    /**
     * Starts a heartbeat for each member in the current config.  Called while holding _mutex.
     */
    void _startHeartbeats(WithLock lk);

    /**
     * Cancels all heartbeats.  Called while holding replCoord _mutex.
     */
    void _cancelHeartbeats(WithLock);

    /**
     * Cancels all heartbeats that have been scheduled but not yet sent out, then reschedules them
     * at the current time immediately using this node's "replSetName". Called while holding
     * replCoord _mutex.
     */
    void _restartScheduledHeartbeats(WithLock lk, const std::string& replSetName);

    /**
     * Asynchronously sends a heartbeat to "target" using this node's "replSetName".
     *
     * Scheduled by _scheduleHeartbeatToTarget.
     */
    void _doMemberHeartbeat(executor::TaskExecutor::CallbackArgs cbData,
                            const HostAndPort& target,
                            const std::string& replSetName);


    MemberState _getMemberState(WithLock) const;

    /**
     * Helper method for setting this node to a specific follower mode.
     *
     * Note: The opCtx may be null, but must be non-null if the new state is RS_ROLLBACK.
     */
    Status _setFollowerMode(OperationContext* opCtx, const MemberState& newState);

    /**
     * Starts loading the replication configuration from local storage, and if it is valid,
     * schedules a callback (of _finishLoadLocalConfig) to set it as the current replica set
     * config (sets _rsConfig and _thisMembersConfigIndex).
     * Returns true if it finishes loading the local config, which most likely means there
     * was no local config at all or it was invalid in some way, and false if there was a valid
     * config detected but more work is needed to set it as the local config (which will be
     * handled by the callback to _finishLoadLocalConfig).
     *
     * Increments the rollback ID if the the server was shut down uncleanly.
     */
    bool _startLoadLocalConfig(OperationContext* opCtx,
                               StorageEngine::LastShutdownState lastShutdownState);

    /**
     * Callback that finishes the work started in _startLoadLocalConfig and sets _rsConfigState
     * to kConfigSteady, so that we can begin processing heartbeats and reconfigs.
     */
    void _finishLoadLocalConfig(const executor::TaskExecutor::CallbackArgs& cbData,
                                const ReplSetConfig& localConfig,
                                const StatusWith<OpTimeAndWallTime>& lastOpTimeAndWallTimeStatus,
                                const StatusWith<LastVote>& lastVoteStatus);

    /**
     * Start replicating data, and does an initial sync if needed first.
     */
    void _startDataReplication(OperationContext* opCtx);

    /**
     * Start initial sync.
     */
    void _startInitialSync(OperationContext* opCtx,
                           InitialSyncerInterface::OnCompletionFn onCompletionFn,
                           bool fallbackToLogical = false);


    /**
     * Function to be called on completion of initial sync.
     */
    void _initialSyncerCompletionFunction(const StatusWith<OpTimeAndWallTime>& opTimeStatus);

    /**
     * Function that executes a replSetInitiate command. This function should be run in an internal
     * thread so that it cannot be interrupted upon state transition.
     */
    Status _runReplSetInitiate(const BSONObj& configObj, BSONObjBuilder* resultObj);

    /**
     * Finishes the work of _runReplSetInitiate() in the event of a successful quorum check.
     */
    void _finishReplSetInitiate(OperationContext* opCtx,
                                const ReplSetConfig& newConfig,
                                int myIndex);

    /**
     * Finishes the work of processReplSetReconfig, in the event of
     * a successful quorum check.
     */
    void _finishReplSetReconfig(OperationContext* opCtx,
                                const ReplSetConfig& newConfig,
                                bool isForceReconfig,
                                int myIndex);

    template <typename T>
    static StatusOrStatusWith<T> _futureGetNoThrowWithDeadline(OperationContext* opCtx,
                                                               SharedSemiFuture<T>& f,
                                                               Date_t deadline,
                                                               ErrorCodes::Error error) {
        try {
            return opCtx->runWithDeadline(deadline, error, [&] { return f.getNoThrow(opCtx); });
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }


    /**
     * Changes _rsConfigState to newState, and notify any waiters.
     */
    void _setConfigState(WithLock, ConfigState newState);

    /**
     * Returns the string representation of the config state.
     */
    static std::string getConfigStateString(ConfigState state);

    /**
     * Returns true if the horizon mappings between the oldConfig and newConfig are different.
     */
    void _errorOnPromisesIfHorizonChanged(WithLock lk,
                                          OperationContext* opCtx,
                                          const ReplSetConfig& oldConfig,
                                          const ReplSetConfig& newConfig,
                                          int oldIndex,
                                          int newIndex);

    /**
     * Fulfills the promises that are waited on by awaitable hello requests. This increments the
     * server TopologyVersion.
     */
    void _fulfillTopologyChangePromise(WithLock);

    /**
     * Update _canAcceptNonLocalWrites based on _topCoord->canAcceptWrites().
     */
    void _updateWriteAbilityFromTopologyCoordinator(WithLock lk, OperationContext* opCtx);

    /**
     * Updates the cached value, _memberState, to match _topCoord's reported
     * member state, from getMemberState().
     *
     * Returns an enum indicating what action to take after releasing _mutex, if any.
     * Call performPostMemberStateUpdateAction on the return value after releasing
     * _mutex.
     */
    PostMemberStateUpdateAction _updateMemberStateFromTopologyCoordinator(WithLock lk);

    /**
     * Performs a post member-state update action.  Do not call while holding _mutex.
     */
    void _performPostMemberStateUpdateAction(PostMemberStateUpdateAction action);

    /**
     * Update state after winning an election.
     */
    void _postWonElectionUpdateMemberState(WithLock lk);

    /**
     * Helper to select appropriate sync source after transitioning from a follower state.
     */
    void _onFollowerModeStateChange();

    /**
     * Removes 'host' from the sync source denylist. If 'host' isn't found, it's simply
     * ignored and no error is thrown.
     *
     * Must be scheduled as a callback.
     */
    void _undenylistSyncSource(const executor::TaskExecutor::CallbackArgs& cbData,
                               const HostAndPort& host);

    /**
     * Schedules stepdown to run with the global exclusive lock.
     */
    executor::TaskExecutor::EventHandle _stepDownStart();

    /**
     * Completes a step-down of the current node.  Must be run with a global
     * shared or global exclusive lock.
     * Signals 'finishedEvent' on successful completion.
     */
    void _stepDownFinish(const executor::TaskExecutor::CallbackArgs& cbData,
                         const executor::TaskExecutor::EventHandle& finishedEvent);

    /**
     * Returns true if I am primary in the current configuration but not electable or removed in the
     * new config.
     */
    bool _shouldStepDownOnReconfig(WithLock,
                                   const ReplSetConfig& newConfig,
                                   StatusWith<int> myIndex);

    /**
     * Schedules a replica set config change.
     */
    void _scheduleHeartbeatReconfig(WithLock lk, const ReplSetConfig& newConfig);

    /**
     * Method to write a configuration transmitted via heartbeat message to stable storage.
     */
    void _heartbeatReconfigStore(const executor::TaskExecutor::CallbackArgs& cbd,
                                 const ReplSetConfig& newConfig);

    /**
     * Conclusion actions of a heartbeat-triggered reconfiguration.
     */
    void _heartbeatReconfigFinish(const executor::TaskExecutor::CallbackArgs& cbData,
                                  const ReplSetConfig& newConfig,
                                  StatusWith<int> myIndex);

    /**
     * Calculates the time (in millis) left in quiesce mode and converts the value to int64.
     */
    long long _calculateRemainingQuiesceTimeMillis() const;

    /**
     * Fills a HelloResponse with the appropriate replication related fields. horizonString
     * should be passed in if hasValidConfig is true.
     */
    std::shared_ptr<HelloResponse> _makeHelloResponse(
        const boost::optional<std::string>& horizonString, WithLock, bool hasValidConfig) const;

    /**
     * Creates a semi-future for HelloResponse. horizonString should be passed in if and only if
     * the server is a valid member of the config.
     */
    virtual SharedSemiFuture<SharedHelloResponse> _getHelloResponseFuture(
        WithLock,
        const SplitHorizon::Parameters& horizonParams,
        const boost::optional<std::string>& horizonString,
        boost::optional<TopologyVersion> clientTopologyVersion);

    /**
     * Returns the horizon string by parsing horizonParams if the node is a valid member of the
     * replica set. Otherwise, return boost::none.
     */
    boost::optional<std::string> _getHorizonString(
        WithLock, const SplitHorizon::Parameters& horizonParams) const;

    /**
     * Utility method that schedules or performs actions specified by a HeartbeatResponseAction
     * returned by a TopologyCoordinator::processHeartbeatResponse(V1) call with the given
     * value of "responseStatus".
     *
     * Requires "lock" to own _mutex, and returns the same unique_lock.
     */
    stdx::unique_lock<ObservableMutex<stdx::mutex>> _handleHeartbeatResponseAction(
        const HeartbeatResponseAction& action,
        const StatusWith<ReplSetHeartbeatResponse>& responseStatus,
        stdx::unique_lock<ObservableMutex<stdx::mutex>> lock);

    /**
     * Updates the last committed OpTime to be 'committedOpTime' if it is more recent than the
     * current last committed OpTime. We ignore 'committedOpTime' if it has a different term than
     * our lastApplied, unless 'fromSyncSource'=true, which guarantees we are on the same branch of
     * history as 'committedOpTime', so we update our commit point to min(committedOpTime,
     * lastApplied).
     * Also updates corresponding wall clock time.
     * The 'forInitiate' flag is used to force-advance our commit point during the execuction
     * of the replSetInitiate command.
     */
    void _advanceCommitPoint(WithLock lk,
                             const OpTimeAndWallTime& committedOpTimeAndWallTime,
                             bool fromSyncSource,
                             bool forInitiate = false);

    /**
     * Scan the memberData and determine the highest last written or last
     * durable optime present on a majority of servers; set _lastCommittedOpTime to this
     * new entry.
     *
     * Whether the last written or last durable op time is used depends on whether
     * the config getWriteConcernMajorityShouldJournal is set.
     */
    void _updateLastCommittedOpTimeAndWallTime(WithLock lk);

    /** Terms only increase, so if an incoming term is less than or equal to our
     * current term (_termShadow), there is no need to take the mutex and call _updateTerm.
     * Since _termShadow may be lagged, this may return true when the term does not need to be
     * updated, which is harmless because _updateTerm will do nothing in that case.
     */
    bool _needToUpdateTerm(long long term);

    /**
     * Callback that attempts to set the current term in topology coordinator and
     * relinquishes primary if the term actually changes and we are primary.
     * *updateTermResult will be the result of the update term attempt.
     * Returns the finish event if it does not finish in this function, for example,
     * due to stepdown, otherwise the returned EventHandle is invalid.
     */
    EventHandle _updateTerm(WithLock lk,
                            long long term,
                            TopologyCoordinator::UpdateTermResult* updateTermResult = nullptr);

    /**
     * Callback that processes the ReplSetMetadata returned from a command run against another
     * replica set member and so long as the config version in the metadata matches the replica set
     * config version this node currently has, updates the current term.
     *
     * This does NOT update this node's notion of the commit point.
     *
     * Returns the finish event which is invalid if the process has already finished.
     */
    EventHandle _processReplSetMetadata(WithLock lk, const rpc::ReplSetMetadata& replMetadata);

    /**
     * Blesses a snapshot to be used for new committed reads.
     *
     * Returns true if the value was updated to `newCommittedSnapshot`.
     */
    bool _updateCommittedSnapshot(WithLock lk, const OpTime& newCommittedSnapshot);

    /**
     * A helper method that returns the current stable optime based on the current commit point.
     */
    OpTime _recalculateStableOpTime(WithLock lk);

    /**
     * Calculates and sets the value of the 'stable' replication optime for the storage engine.
     */
    void _setStableTimestampForStorage(WithLock lk);

    /**
     * Clears the current committed snapshot.
     */
    void _clearCommittedSnapshot(WithLock);

    /**
     * Bottom half of _scheduleNextLivenessUpdate.
     * Must be called with _mutex held.
     * If reschedule is true, will recompute the liveness update even if a timeout is
     * already pending.
     */
    void _scheduleNextLivenessUpdate(WithLock, bool reschedule);

    /**
     * Callback which marks downed nodes as down, triggers a stepdown if a majority of nodes are no
     * longer visible, and reschedules itself.
     */
    void _handleLivenessTimeout(const executor::TaskExecutor::CallbackArgs& cbData);

    /**
     * If "updatedMemberId" is the current _earliestMemberId, calls _scheduleNextLivenessUpdate to
     * schedule a new one.
     * Returns immediately otherwise.
     */
    void _rescheduleLivenessUpdate(WithLock, int updatedMemberId);

    /**
     * Cancels all outstanding _priorityTakeover callbacks.
     */
    void _cancelPriorityTakeover(WithLock);

    /**
     * Cancels all outstanding _catchupTakeover callbacks.
     */
    void _cancelCatchupTakeover(WithLock);

    /**
     * Cancels the current _handleElectionTimeout callback and reschedules a new callback.
     * Returns immediately otherwise.
     */
    void _cancelAndRescheduleElectionTimeout(WithLock lk);

    /**
     * Callback which starts an election if this node is electable and using protocolVersion 1.
     */
    void _startElectSelfIfEligibleV1(StartElectionReasonEnum reason);
    void _startElectSelfIfEligibleV1(WithLock, StartElectionReasonEnum reason);

    /**
     * Schedules work to be run no sooner than 'when' and returns handle to callback.
     * If work cannot be scheduled due to shutdown, returns empty handle.
     * All other non-shutdown scheduling failures will abort the process.
     * Does not run 'work' if callback is canceled.
     */
    CallbackHandle _scheduleWorkAt(Date_t when, CallbackFn work);

    /**
     * Creates an event.
     * Returns invalid event handle if the executor is shutting down.
     * Otherwise aborts on non-shutdown error.
     */
    EventHandle _makeEvent();

    /**
     * Finish catch-up mode and start drain mode.
     */
    void _enterDrainMode(WithLock);

    /**
     * Waits for the config state to leave kConfigStartingUp, which indicates that start() has
     * finished.
     */
    void _waitForStartUpComplete();

    /**
     * Cancels the running election, if any, and returns an event that will be signaled when the
     * canceled election completes. If there is no running election, returns an invalid event
     * handle.
     */
    executor::TaskExecutor::EventHandle _cancelElectionIfNeeded(WithLock);

    /**
     * Waits until the lastApplied/lastWritten opTime is at least the 'targetOpTime'.
     */
    Status _waitUntilOpTime(OperationContext* opCtx,
                            OpTime targetOpTime,
                            boost::optional<Date_t> deadline = boost::none,
                            bool waitForLastApplied = true);

    /**
     * Waits until the optime of the current node is at least the opTime specified in 'readConcern'.
     * Supports local and majority readConcern.
     */
    // TODO: remove when SERVER-29729 is done
    Status _waitUntilOpTimeForReadDeprecated(OperationContext* opCtx,
                                             const ReadConcernArgs& readConcern);

    /**
     * Waits until the deadline or until the optime of the current node is at least the clusterTime
     * specified in 'readConcern'. Supports local and majority readConcern.
     * If maxTimeMS and deadline are both specified, it waits for min(maxTimeMS, deadline).
     */
    Status _waitUntilClusterTimeForRead(OperationContext* opCtx,
                                        const ReadConcernArgs& readConcern,
                                        boost::optional<Date_t> deadline);

    /**
     * Initializes a horizon name to promise mapping. Each awaitable hello request will block on
     * the promise mapped to by the horizon name determined from this map. This map should be
     * cleared and reinitialized after any reconfig that will change the SplitHorizon.
     */
    void _createHorizonTopologyChangePromiseMapping(WithLock);

    /**
     * Returns a pseudorandom number no less than 0 and less than limit (which must be positive).
     */
    int64_t _nextRandomInt64(WithLock, int64_t limit);

    /**
     * This is called by a primary when they become aware that a node has completed initial sync.
     * That primary initiates a reconfig to remove the 'newlyAdded' for that node, if it was set.
     */
    void _reconfigToRemoveNewlyAddedField(const executor::TaskExecutor::CallbackArgs& cbData,
                                          MemberId memberId,
                                          ConfigVersionAndTerm versionAndTerm);

    /**
     * Sets the implicit default write concern on startup.
     */
    void _setImplicitDefaultWriteConcern(OperationContext* opCtx, WithLock lk);

    /*
     * Calculates and returns the read preference for the node.
     */
    ReadPreference _getSyncSourceReadPreference(WithLock lk) const;

    /*
     * Performs the replica set reconfig procedure. Certain consensus safety checks are omitted when
     * either 'force' or 'skipSafetyChecks' are true.
     */
    Status _doReplSetReconfig(OperationContext* opCtx,
                              GetNewConfigFn getNewConfig,
                              bool force,
                              bool skipSafetyChecks);

    /**
     * This validation should be called on shard startup, it fasserts if the defaultWriteConcern
     * on the shard is set to w:1 and CWWC is not set.
     */
    void _validateDefaultWriteConcernOnShardStartup(WithLock lk) const;

    /**
     * Checks whether the node can currently accept replicated writes. This method is unsafe and
     * is for internal use only as its result is only accurate while holding the RSTL.
     */
    bool _canAcceptReplicatedWrites_UNSAFE(OperationContext* opCtx);

    /**
     * Checks whether the collection indicated by nsOrUUID is replicated.
     */
    bool _isCollectionReplicated(OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID);

    /**
     * Returns the latest configuration without acquiring `_mutex`. Internally, it reads the config
     * from a thread-local cache. The config is refreshed to the latest if stale.
     */
    const ReplSetConfig& _getReplSetConfig() const;

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access in any way from any context.
    // (PS) Pointer is read-only in concurrent operation, item pointed to is self-synchronizing;
    //      Access in any context.
    // (M)  Reads and writes guarded by _mutex
    // (I)  Independently synchronized, see member variable comment.

    // Protects member data of this ReplicationCoordinator.
    mutable ObservableMutex<stdx::mutex> _mutex;  // (S)

    // Handles to actively queued heartbeats.
    size_t _maxSeenHeartbeatQSize = 0;
    stdx::unordered_map<executor::TaskExecutor::CallbackHandle,
                        HeartbeatHandleMetadata,
                        absl::Hash<executor::TaskExecutor::CallbackHandle>>
        _heartbeatHandles;

    // When this node does not know itself to be a member of a config, it adds
    // every host that sends it a heartbeat request to this set, and also starts
    // sending heartbeat requests to that host.  This set is cleared whenever
    // a node discovers that it is a member of a config.
    stdx::unordered_set<HostAndPort> _seedList;  // (M)

    // Back pointer to the ServiceContext that has started the instance.
    ServiceContext* const _service;  // (S)

    // Parsed command line arguments related to replication.
    const ReplSettings _settings;  // (R)

    // Pointer to the TopologyCoordinator owned by this ReplicationCoordinator.
    std::unique_ptr<TopologyCoordinator> _topCoord;  // (M)

    // Executor that drives the topology coordinator.
    std::shared_ptr<executor::TaskExecutor> _replExecutor;  // (S)

    // Pointer to the ReplicationCoordinatorExternalState owned by this ReplicationCoordinator.
    std::unique_ptr<ReplicationCoordinatorExternalState> _externalState;  // (PS)

    // list of information about clients waiting on replication or lastDurable opTime.
    // Waiters in this list are checked and notified on remote nodes' opTime updates and self's
    // lastDurable opTime updates. We do not check this list on self's lastApplied opTime updates to
    // avoid checking all waiters in the list on every write.
    WriteConcernWaiterList _replicationWaiterList;  // (M)

    // list of information about clients waiting for a particular lastApplied opTime.
    // Waiters in this list are checked and notified on self's lastApplied opTime updates.
    WaiterList _lastAppliedOpTimeWaiterList;  // (M)

    // list of information about clients waiting for a particular lastWritten opTime.
    // Waiters in this list are checked and notified on self's lastWritten opTime updates.
    WaiterList _lastWrittenOpTimeWaiterList;  // (M)

    // Maps a horizon name to the promise waited on by awaitable hello requests when the node
    // has an initialized replica set config and is an active member of the replica set.
    StringMap<std::shared_ptr<SharedPromiseOfHelloResponse>>
        _horizonToTopologyChangePromiseMap;  // (M)

    // Maps a requested SNI to the promise waited on by awaitable hello requests when the node
    // has an unitialized replica set config or is removed. An empty SNI will map to a promise on
    // the default horizon.
    StringMap<std::shared_ptr<SharedPromiseOfHelloResponse>> _sniToValidConfigPromiseMap;  // (M)

    // Set to true when we are in the process of shutting down replication.
    bool _inShutdown;  // (M)

    // The term of the last election that resulted in this node becoming primary.  "Shadow" because
    // this follows the authoritative value in the topology coordinatory.
    AtomicWord<long long> _electionIdTermShadow;  // (S)

    // Used to signal threads waiting for changes to _memberState.
    stdx::condition_variable _memberStateChange;  // (M)

    // Current ReplicaSet state.
    MemberState _memberState;  // (M)

    ReplicationCoordinator::OplogSyncState _oplogSyncState = OplogSyncState::Running;  // (M)

    // Used to signal threads waiting for changes to _rsConfigState.
    stdx::condition_variable _rsConfigStateChange;  // (M)

    // Represents the configuration state of the coordinator, which controls how and when
    // _rsConfig may change.  See the state transition diagram in the type definition of
    // ConfigState for details.
    ConfigState _rsConfigState;  // (M)

    // An instance for getting a lease on the current ReplicaSet
    // configuration object, including the information about tag groups that is
    // used to satisfy write concern requests with named gle modes.
    mutable VersionedValue<ReplSetConfig, WriteRarelyRWMutex> _rsConfig;  // (S)

    // This member's index position in the current config.
    int _selfIndex;  // (M)

    // Whether we slept last time we attempted an election but possibly tied with other nodes.
    bool _sleptLastElection;  // (M)

    // Used to manage the concurrency around _canAcceptNonLocalWrites and _canServeNonLocalReads.
    std::unique_ptr<ReadWriteAbility> _readWriteAbility;  // (S)

    // ReplicationProcess used to hold information related to the replication and application of
    // operations from the sync source.
    ReplicationProcess* const _replicationProcess;  // (PS)

    // Storage interface used by initial syncer.
    StorageInterface* _storage;  // (PS)
    // InitialSyncer used for initial sync.
    std::shared_ptr<InitialSyncerInterface>
        _initialSyncer;  // (I) pointer set under mutex, copied by callers.

    // The non-null OpTime used for committed reads, if there is one.
    // When engaged, this must be <= _lastCommittedOpTime.
    boost::optional<OpTime> _currentCommittedSnapshot;  // (M)

    // Used to signal threads that are waiting for a new value of _currentCommittedSnapshot.
    stdx::condition_variable _currentCommittedSnapshotCond;  // (M)

    // Callback Handle used to cancel a scheduled LivenessTimeout callback.
    DelayableTimeoutCallback _handleLivenessTimeoutCallback;  // (S)

    // Used to manage scheduling and canceling election timeouts.
    DelayableTimeoutCallbackWithJitter _handleElectionTimeoutCallback;  // (M)

    // Callback Handle used to cancel a scheduled PriorityTakeover callback.
    executor::TaskExecutor::CallbackHandle _priorityTakeoverCbh;  // (M)

    // Priority takeover callback will not run before this time.
    // If this date is Date_t(), the callback is either unscheduled or canceled.
    // Used for testing only.
    Date_t _priorityTakeoverWhen;  // (M)

    // Callback Handle used to cancel a scheduled CatchupTakeover callback.
    executor::TaskExecutor::CallbackHandle _catchupTakeoverCbh;  // (M)

    // Catchup takeover callback will not run before this time.
    // If this date is Date_t(), the callback is either unscheduled or canceled.
    // Used for testing only.
    Date_t _catchupTakeoverWhen;  // (M)

    // Callback handle used by _waitForStartUpComplete() to block until configuration
    // is loaded and external state threads have been started (unless this node is an arbiter).
    CallbackHandle _finishLoadLocalConfigCbh;  // (M)

    // The id of the earliest member, for which the handleLivenessTimeout callback has been
    // scheduled.  We need this so that we don't needlessly cancel and reschedule the callback on
    // every liveness update.
    int _earliestMemberId = -1;  // (M)

    // Cached copy of the current config protocol version.
    AtomicWord<long long> _protVersion{1};  // (S)

    // Source of random numbers used in setting election timeouts, etc.
    PseudoRandom _random;  // (M)

    // The catchup state including all catchup logic. The presence of a non-null pointer indicates
    // that the node is currently in catchup mode.
    std::unique_ptr<CatchupState> _catchupState;  // (X)

    // The election state that includes logic to start and return information from the election
    // voting process.
    std::unique_ptr<ElectionState> _electionState;  // (M)

    // Atomic-synchronized copy of Topology Coordinator's _term, for use by the public getTerm()
    // function.
    // This variable must be written immediately after _term, and thus its value can lag.
    // Reading this value does not require the replication coordinator mutex to be locked.
    AtomicWord<long long> _termShadow;  // (S)

    // When we decide to step down due to hearing about a higher term, we remember the term we heard
    // here so we can update our term to match as part of finishing stepdown.
    boost::optional<long long> _pendingTermUpdateDuringStepDown;  // (M)

    AtomicWord<bool> _startedSteadyStateReplication{false};

    // If we're in stepdown code and therefore should claim we don't allow
    // writes.  This is a counter rather than a flag because there are scenarios where multiple
    // stepdowns are attempted at once.
    short _stepDownPending = 0;

    // If we're in terminal shutdown.  If true, we'll refuse to vote in elections.
    bool _inTerminalShutdown = false;  // (M)

    // If we're in quiesce mode.  If true, we'll respond to hello requests with ok:0.
    bool _inQuiesceMode = false;  // (M)

    // The deadline until which quiesce mode will last.
    Date_t _quiesceDeadline;  // (M)

    // The cached value of the 'counter' field in the server's TopologyVersion.
    AtomicWord<int64_t> _cachedTopologyVersionCounter;  // (S)

    // The cached value of the topology from the most recent SplitHorizonChange.
    int64_t _lastHorizonTopologyChange{-1};  // (M)

    // This should be set during sharding initialization except on config shard.
    boost::optional<bool> _wasCWWCSetOnConfigServerOnStartup;

    InitialSyncerInterface::OnCompletionFn _onCompletion;

    // Construct used to synchronize default write concern changes with config write concern
    // changes.
    WriteConcernTagChangesImpl _writeConcernTagChanges;

    // Pointer to the SplitPrepareSessionManager owned by this ReplicationCoordinator.
    SplitPrepareSessionManager _splitSessionManager;  // (S)

    // Whether data writes are being done on a consistent copy of the data. The value is false until
    // setConsistentDataAvailable is called - that's after replSetInitiate, after initial sync
    // completes, after storage recovers from a stable checkpoint, or after replication recovery
    // from an unstable checkpoint.
    AtomicWord<bool> _isDataConsistent{false};

    rss::consensus::IntentRegistry& _intentRegistry;
    /**
     * Manages tracking for whether this node is able to serve (non-stale) majority reads with
     * primary read preference.
     */
    class PrimaryMajorityReadsAvailability {
    public:
        PrimaryMajorityReadsAvailability() : _promise(nullptr) {}

        /**
         * Resets the state of this type to track a new step-up and term as primary.
         * It is an error to call this method but not follow it with a call to one of
         * `allowReads()`, `disallowReads()`, or `onBecomeNonPrimary()`.
         */
        void onBecomePrimary() {
            auto lock = _mutex.writeLock();
            invariant(!_promise);
            _promise = std::make_unique<SharedPromise<void>>();
        }

        /**
         * This method must be called when a node that was primary steps down. This will unblock
         * any reads that are currently waiting for the node to be able to serve primary majority
         * reads.
         */
        void onBecomeNonPrimary() {
            auto lock = _mutex.writeLock();
            invariant(_promise);
            // If we already completed the promise (either with success or error) then no reads
            // are waiting on it and we don't need to change it, as read preference validation
            // will reject primary read preference reads going forward.
            // However, if we have not completed the promise (this can happen if we stepped down
            // before we managed to create a waiter to complete it) then we need to explicitly
            // fail it here.
            if (!_promise->getFuture().isReady()) {
                _promise->setError({ErrorCodes::PrimarySteppedDown,
                                    "Primary stepped down while waiting for majority read "
                                    "availability)"});
            }
            _promise = nullptr;
        }

        /**
         * Mark that this node can now serve primary majority reads.
         */
        void allowReads() {
            auto lock = _mutex.readLock();
            invariant(_promise);
            _promise->emplaceValue();
        }

        /**
         * Mark that this node cannot serve primary majority reads. This indicates the node failed
         * to become primary. Any reads that were waiting for availability will be failed with the
         * provided status.
         */
        void disallowReads(Status status) {
            invariant(!status.isOK());
            auto lock = _mutex.readLock();
            invariant(_promise);
            _promise->setError(status);
        }

        /**
         * Waits until this node is able to serve primary majority reads.
         */
        Status waitForReadsAvailable(OperationContext* opCtx) const {
            SharedSemiFuture<void> future;
            {
                auto lock = _mutex.readLock();
                if (!_promise) {
                    return {ErrorCodes::NotPrimaryNoSecondaryOk,
                            "not primary and secondaryOk=false"};
                }
                future = _promise->getFuture();
            }
            return future.getNoThrow(opCtx);
        }


    private:
        // Synchronizes reads/writes of _promise.
        mutable WriteRarelyRWMutex _mutex;

        // A promise which is fulfilled once a new primary's first write in its new term has been
        // majority committed.
        // This is significant as once that entry is majority committed, we know the node has
        // majority committed all writes from earlier terms as well, and thus the node has an up-
        // to-date view of the commit point and will not serve stale reads.
        // This promise should always be completed (either with success or error) when a node is not
        // primary. When a node is stepping up and before it starts accepting reads with primary
        // read preference, it is reset with a fresh promise that will either be succeeded or failed
        // depending on whether we successfully majority commit the node's "new primary" oplog entry
        // written during step-up. In order to read this member and call any method on it, obtain a
        // read lock from _mutex. A write lock is only necessary when actually overwriting the value
        // of this member, as SharedPromise provides safety for concurrent calls to its methods.
        std::unique_ptr<SharedPromise<void>> _promise;
    };

    PrimaryMajorityReadsAvailability _primaryMajorityReadsAvailability;  // (S)
};

extern Counter64& replicationWaiterListMetric;
extern Counter64& opTimeWaiterListMetric;

}  // namespace repl
}  // namespace mongo
