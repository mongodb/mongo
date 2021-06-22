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

#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/net/hostandport.h"

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

class HeartbeatResponseAction;
class LastVote;
class ReplicationProcess;
class ReplSetRequestVotesArgs;
class ReplSetConfig;
class SyncSourceFeedback;
class StorageInterface;
class TopologyCoordinator;
class VoteRequester;

class ReplicationCoordinatorImpl : public ReplicationCoordinator {
    ReplicationCoordinatorImpl(const ReplicationCoordinatorImpl&) = delete;
    ReplicationCoordinatorImpl& operator=(const ReplicationCoordinatorImpl&) = delete;

public:
    ReplicationCoordinatorImpl(ServiceContext* serviceContext,
                               const ReplSettings& settings,
                               std::unique_ptr<ReplicationCoordinatorExternalState> externalState,
                               std::unique_ptr<executor::TaskExecutor> executor,
                               std::unique_ptr<TopologyCoordinator> topoCoord,
                               ReplicationProcess* replicationProcess,
                               StorageInterface* storage,
                               int64_t prngSeed);

    virtual ~ReplicationCoordinatorImpl();

    // ================== Members of public ReplicationCoordinator API ===================

    virtual void startup(OperationContext* opCtx) override;

    virtual void enterTerminalShutdown() override;

    virtual void shutdown(OperationContext* opCtx) override;

    virtual const ReplSettings& getSettings() const override;

    virtual Mode getReplicationMode() const override;

    virtual MemberState getMemberState() const override;

    virtual std::vector<MemberData> getMemberData() const override;

    virtual bool canAcceptNonLocalWrites() const override;

    virtual Status waitForMemberState(MemberState expectedState, Milliseconds timeout) override;

    virtual bool isInPrimaryOrSecondaryState(OperationContext* opCtx) const override;

    virtual bool isInPrimaryOrSecondaryState_UNSAFE() const override;

    virtual Seconds getSlaveDelaySecs() const override;

    virtual void clearSyncSourceBlacklist() override;

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

    virtual Status checkIfCommitQuorumCanBeSatisfied(
        const CommitQuorumOptions& commitQuorum) const override;

    virtual StatusWith<bool> checkIfCommitQuorumIsSatisfied(
        const CommitQuorumOptions& commitQuorum,
        const std::vector<HostAndPort>& commitReadyMembers) const override;

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

    virtual OpTime getMyLastAppliedOpTime() const override;
    virtual OpTimeAndWallTime getMyLastAppliedOpTimeAndWallTime() const override;

    virtual OpTime getMyLastDurableOpTime() const override;
    virtual OpTimeAndWallTime getMyLastDurableOpTimeAndWallTime() const override;

    virtual Status waitUntilOpTimeForReadUntil(OperationContext* opCtx,
                                               const ReadConcernArgs& readConcern,
                                               boost::optional<Date_t> deadline) override;

    virtual Status waitUntilOpTimeForRead(OperationContext* opCtx,
                                          const ReadConcernArgs& readConcern) override;
    Status awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) override;
    virtual OID getElectionId() override;

    virtual int getMyId() const override;

    virtual HostAndPort getMyHostAndPort() const override;

    virtual Status setFollowerMode(const MemberState& newState) override;

    virtual Status setFollowerModeStrict(OperationContext* opCtx,
                                         const MemberState& newState) override;

    virtual ApplierState getApplierState() override;

    virtual void signalDrainComplete(OperationContext* opCtx,
                                     long long termWhenBufferIsEmpty) override;

    virtual Status waitForDrainFinish(Milliseconds timeout) override;

    virtual void signalUpstreamUpdater() override;

    virtual Status resyncData(OperationContext* opCtx, bool waitUntilCompleted) override;

    virtual StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const override;

    virtual Status processReplSetGetStatus(BSONObjBuilder* result,
                                           ReplSetGetStatusResponseStyle responseStyle) override;

    virtual void fillIsMasterForReplSet(IsMasterResponse* result,
                                        const SplitHorizon::Parameters& horizonParams) override;

    virtual void appendSlaveInfoData(BSONObjBuilder* result) override;

    virtual ReplSetConfig getConfig() const override;

    virtual void processReplSetGetConfig(BSONObjBuilder* result) override;

    virtual void processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) override;

    virtual void advanceCommitPoint(const OpTimeAndWallTime& committedOpTimeAndWallTime,
                                    bool fromSyncSource) override;

    virtual void cancelAndRescheduleElectionTimeout() override;

    virtual Status setMaintenanceMode(OperationContext* opCtx, bool activate) override;

    virtual bool getMaintenanceMode() override;

    virtual Status processReplSetSyncFrom(OperationContext* opCtx,
                                          const HostAndPort& target,
                                          BSONObjBuilder* resultObj) override;

    virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj) override;

    virtual Status processReplSetReconfig(OperationContext* opCtx,
                                          const ReplSetReconfigArgs& args,
                                          BSONObjBuilder* resultObj) override;

    virtual Status processReplSetInitiate(OperationContext* opCtx,
                                          const BSONObj& configObj,
                                          BSONObjBuilder* resultObj) override;

    virtual Status processReplSetUpdatePosition(const UpdatePositionArgs& updates,
                                                long long* configVersion) override;

    virtual bool buildsIndexes() override;

    virtual std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op,
                                                       bool durablyWritten) override;

    virtual std::vector<HostAndPort> getOtherNodesInReplSet() const override;

    virtual WriteConcernOptions getGetLastErrorDefault() override;

    virtual Status checkReplEnabledForCommand(BSONObjBuilder* result) override;

    virtual bool isReplEnabled() const override;

    virtual HostAndPort chooseNewSyncSource(const OpTime& lastOpTimeFetched) override;

    virtual void blacklistSyncSource(const HostAndPort& host, Date_t until) override;

    virtual void resetLastOpTimesFromOplog(OperationContext* opCtx,
                                           DataConsistency consistency) override;

    virtual bool shouldChangeSyncSource(
        const HostAndPort& currentSource,
        const rpc::ReplSetMetadata& replMetadata,
        boost::optional<rpc::OplogQueryMetadata> oqMetadata) override;

    virtual OpTime getLastCommittedOpTime() const override;
    virtual OpTimeAndWallTime getLastCommittedOpTimeAndWallTime() const override;

    virtual Status processReplSetRequestVotes(OperationContext* opCtx,
                                              const ReplSetRequestVotesArgs& args,
                                              ReplSetRequestVotesResponse* response) override;

    virtual void prepareReplMetadata(const BSONObj& metadataRequestObj,
                                     const OpTime& lastOpTimeFromClient,
                                     BSONObjBuilder* builder) const override;

    virtual Status processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                      ReplSetHeartbeatResponse* response) override;

    virtual bool getWriteConcernMajorityShouldJournal() override;

    virtual void dropAllSnapshots() override;
    /**
     * Get current term from topology coordinator
     */
    virtual long long getTerm() override;

    // Returns the ServiceContext where this instance runs.
    virtual ServiceContext* getServiceContext() override {
        return _service;
    }

    virtual Status updateTerm(OperationContext* opCtx, long long term) override;

    virtual OpTime getCurrentCommittedSnapshotOpTime() const override;

    virtual OpTimeAndWallTime getCurrentCommittedSnapshotOpTimeAndWallTime() const override;

    virtual void waitUntilSnapshotCommitted(OperationContext* opCtx,
                                            const Timestamp& untilSnapshot) override;

    virtual void appendDiagnosticBSON(BSONObjBuilder*) override;

    virtual void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    virtual size_t getNumUncommittedSnapshots() override;

    virtual void createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) override;

    virtual WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(
        WriteConcernOptions wc) override;

    virtual Status stepUpIfEligible(bool skipDryRun) override;

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

    // ================== Test support API ===================

    /**
     * If called after startReplication(), blocks until all asynchronous
     * activities associated with replication start-up complete.
     */
    void waitForStartUpComplete_forTest();

    /**
     * Gets the replica set configuration in use by the node.
     */
    ReplSetConfig getReplicaSetConfig_forTest();

    /**
     * Returns scheduled time of election timeout callback.
     * Returns Date_t() if callback is not scheduled.
     */
    Date_t getElectionTimeout_forTest() const;

    /*
     * Return a randomized offset amount that is scaled in proportion to the size of the
     * _electionTimeoutPeriod.
     */
    Milliseconds getRandomizedElectionOffset_forTest();

    /**
     * Returns the scheduled time of the priority takeover callback. If a priority
     * takeover has not been scheduled, returns boost::none.
     */
    boost::optional<Date_t> getPriorityTakeover_forTest() const;

    /**
     * Returns the scheduled time of the catchup takeover callback. If a catchup
     * takeover has not been scheduled, returns boost::none.
     */
    boost::optional<Date_t> getCatchupTakeover_forTest() const;

    /**
     * Returns the catchup takeover CallbackHandle.
     */
    executor::TaskExecutor::CallbackHandle getCatchupTakeoverCbh_forTest() const;

    /**
     * Simple wrappers around _setLastOptime to make it easier to test.
     */
    Status setLastAppliedOptime_forTest(long long cfgVer,
                                        long long memberId,
                                        const OpTime& opTime,
                                        Date_t wallTime = Date_t());
    Status setLastDurableOptime_forTest(long long cfgVer,
                                        long long memberId,
                                        const OpTime& opTime,
                                        Date_t wallTime = Date_t());

    /**
     * Simple test wrappers that expose private methods.
     */
    boost::optional<OpTimeAndWallTime> chooseStableOpTimeFromCandidates_forTest(
        const std::set<OpTimeAndWallTime>& candidates,
        const OpTimeAndWallTime& maximumStableOpTime);
    void cleanupStableOpTimeCandidates_forTest(std::set<OpTimeAndWallTime>* candidates,
                                               OpTimeAndWallTime stableOpTime);
    std::set<OpTimeAndWallTime> getStableOpTimeCandidates_forTest();

    /**
     * Non-blocking version of updateTerm.
     * Returns event handle that we can use to wait for the operation to complete.
     * When the operation is complete (waitForEvent() returns), 'updateResult' will be set
     * to a status telling if the term increased or a stepdown was triggered.
     */
    executor::TaskExecutor::EventHandle updateTerm_forTest(
        long long term, TopologyCoordinator::UpdateTermResult* updateResult);

    /**
     * If called after _startElectSelfV1(), blocks until all asynchronous
     * activities associated with election complete.
     */
    void waitForElectionFinish_forTest();

    /**
     * If called after _startElectSelfV1(), blocks until all asynchronous
     * activities associated with election dry run complete, including writing
     * last vote and scheduling the real election.
     */
    void waitForElectionDryRunFinish_forTest();

    /**
     * Waits until a stepdown attempt has begun. Callers should ensure that the stepdown attempt
     * won't fully complete before this method is called, or this method may never return.
     */
    void waitForStepDownAttempt_forTest();

private:
    using CallbackFn = executor::TaskExecutor::CallbackFn;

    using CallbackHandle = executor::TaskExecutor::CallbackHandle;

    using EventHandle = executor::TaskExecutor::EventHandle;

    using ScheduleFn = stdx::function<StatusWith<executor::TaskExecutor::CallbackHandle>(
        const executor::TaskExecutor::CallbackFn& work)>;

    class LoseElectionGuardV1;
    class LoseElectionDryRunGuardV1;

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

    // This object acquires RSTL in X mode to perform state transition to (step up)/from (step down)
    // primary. In order to acquire RSTL, it also starts "RstlKillOpthread" which kills conflicting
    // operations (user/system) and aborts stashed running transactions.
    class AutoGetRstlForStepUpStepDown {
    public:
        AutoGetRstlForStepUpStepDown(
            ReplicationCoordinatorImpl* repl,
            OperationContext* opCtx,
            ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
            Date_t deadline = Date_t::max());

        // Disallows copying.
        AutoGetRstlForStepUpStepDown(const AutoGetRstlForStepUpStepDown&) = delete;
        AutoGetRstlForStepUpStepDown& operator=(const AutoGetRstlForStepUpStepDown&) = delete;

        /*
         * Releases RSTL lock.
         */
        void rstlRelease();

        /*
         * Reacquires RSTL lock.
         */
        void rstlReacquire();

        /*
         * Returns _userOpsKilled value.
         */
        size_t getUserOpsKilled() const;

        /*
         * Increments _userOpsKilled by val.
         */
        void incrementUserOpsKilled(size_t val = 1);

        /*
         * Returns _userOpsRunning value.
         */
        size_t getUserOpsRunning() const;

        /*
         * Increments _userOpsRunning by val.
         */
        void incrementUserOpsRunning(size_t val = 1);

        /*
         * Returns the step up/step down opCtx.
         */
        const OperationContext* getOpCtx() const;

    private:
        /**
         * It will spawn a new thread killOpThread to kill operations that conflict with state
         * transitions (step up and step down).
         */
        void _startKillOpThread();

        /**
         * On state transition, we need to kill all write operations and all transactional
         * operations, so that unprepared and prepared transactions can release or yield their
         * locks. The required ordering between step up/step down steps are:
         * 1) Enqueue RSTL in X mode.
         * 2) Kill all conflicting operations.
         *       - Write operation that takes global lock in IX and X mode.
         *       - Read operations that takes global lock in S mode.
         *       - Operations(read/write) that are blocked on prepare conflict.
         * 3) Abort unprepared transactions.
         * 4) Repeat step 2) and 3) until the step up/step down thread can acquire RSTL.
         * 5) Yield locks of all prepared transactions. This applies only to step down as on
         * secondary we currently yield locks for prepared transactions.
         *
         * Since prepared transactions don't hold RSTL, step 1) to step 3) make sure all
         * running transactions that may hold RSTL finish, get killed or yield their locks,
         * so that we can acquire RSTL at step 4). Holding the locks of prepared transactions
         * until step 5) guarantees if any conflict operations (e.g. DDL operations) failed
         * to be killed for any reason, we will get a deadlock instead of a silent data corruption.
         *
         * Loops continuously to kill all conflicting operations. And, aborts all stashed (inactive)
         * transactions.
         * Terminates once killSignaled is set true.
         */
        void _killOpThreadFn();

        /*
         * Signals killOpThread to stop killing operations.
         */
        void _stopAndWaitForKillOpThread();

        ReplicationCoordinatorImpl* const _replCord;  // not owned.
        // step up/step down opCtx.
        OperationContext* const _opCtx;  // not owned.
        // This field is optional because we need to start killOpThread to kill operations after
        // RSTL enqueue.
        boost::optional<ReplicationStateTransitionLockGuard> _rstlLock;
        // Thread that will run killOpThreadFn().
        std::unique_ptr<stdx::thread> _killOpThread;
        // Tracks number of operations killed on step up / step down.
        size_t _userOpsKilled = 0;
        // Tracks number of operations left running on step up / step down.
        size_t _userOpsRunning = 0;
        // Protects killSignaled and stopKillingOps cond. variable.
        Mutex _mutex = MONGO_MAKE_LATCH("AutoGetRstlForStepUpStepDown::_mutex");
        // Signals thread about the change of killSignaled value.
        stdx::condition_variable _stopKillingOps;
        // Once this is set to true, the killOpThreadFn method will terminate.
        bool _killSignaled = false;
        // The state transition that is in progress. Should never be set to rollback within this
        // class.
        ReplicationCoordinator::OpsKillingStateTransitionEnum _stateTransition;
    };

    // Abstract struct that holds information about clients waiting for replication.
    // Subclasses need to define how to notify them.
    struct Waiter {
        Waiter(OpTime _opTime, const WriteConcernOptions* _writeConcern);
        virtual ~Waiter() = default;

        BSONObj toBSON() const;
        std::string toString() const;
        // Controls whether or not this Waiter should stay on the WaiterList upon notification.
        virtual bool runs_once() const = 0;

        // It is invalid to call notify_inlock() unless holding ReplicationCoordinatorImpl::_mutex.
        virtual void notify_inlock() = 0;

        const OpTime opTime;
        const WriteConcernOptions* writeConcern = nullptr;
    };

    // When ThreadWaiter gets notified, it will signal the conditional variable.
    //
    // This is used when a thread wants to block inline until the opTime is reached with the given
    // writeConcern.
    struct ThreadWaiter : public Waiter {
        ThreadWaiter(OpTime _opTime,
                     const WriteConcernOptions* _writeConcern,
                     stdx::condition_variable* _condVar);
        void notify_inlock() override;
        bool runs_once() const override {
            return false;
        }

        // This predicate is an extremely simple way to never miss a notification. When the
        // ThreadWater::notify_inlock() is called, notifyCount is incremented. As should be obvious,
        // this predicate should only be called "inlock" as well. The ThreadWaiter itself goes away
        // with SERVER-43135 so this function and notifyCount as a concept is a v4.2 only measure.
        auto makePredicate() {
            return [this, startingNotifyCount = notifyCount]() -> bool {
                return notifyCount != startingNotifyCount;
            };
        }

        uint64_t notifyCount = 0;
        stdx::condition_variable* condVar = nullptr;
    };

    // When the waiter is notified, finishCallback will be called while holding replCoord _mutex
    // since WaiterLists are protected by _mutex.
    //
    // This is used when we want to run a callback when the opTime is reached.
    struct CallbackWaiter : public Waiter {
        using FinishFunc = stdx::function<void()>;

        CallbackWaiter(OpTime _opTime, FinishFunc _finishCallback);
        void notify_inlock() override;
        bool runs_once() const override {
            return true;
        }

        // The callback that will be called when this waiter is notified.
        FinishFunc finishCallback = nullptr;
    };

    class WaiterGuard;

    class WaiterList {
    public:
        using WaiterType = Waiter*;

        // Adds waiter into the list.
        void add_inlock(WaiterType waiter);
        // Returns whether waiter is found and removed.
        bool remove_inlock(WaiterType waiter);
        // Signals all waiters that satisfy the condition.
        void signalIf_inlock(stdx::function<bool(WaiterType)> fun);
        // Signals all waiters from the list.
        void signalAll_inlock();

    private:
        std::vector<WaiterType> _list;
    };

    typedef std::vector<executor::TaskExecutor::CallbackHandle> HeartbeatHandles;

    // The state and logic of primary catchup.
    //
    // When start() is called, CatchupState will schedule the timeout callback. When we get
    // responses of the latest heartbeats from all nodes, the target time (opTime of _waiter) is
    // set.
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
        void start_inlock();
        // Reset the state itself to destruct the state.
        void abort_inlock(PrimaryCatchUpConclusionReason reason);
        // Heartbeat calls this function to update the target optime.
        void signalHeartbeatUpdate_inlock();
        // Increment the counter for the number of ops applied during catchup.
        void incrementNumCatchUpOps_inlock(long numOps);

    private:
        ReplicationCoordinatorImpl* _repl;  // Not owned.
        // Callback handle used to cancel a scheduled catchup timeout callback.
        executor::TaskExecutor::CallbackHandle _timeoutCbh;
        // Handle to a Waiter that contains the current target optime to reach after which
        // we can exit catchup mode.
        std::unique_ptr<CallbackWaiter> _waiter;
        // Counter for the number of ops applied during catchup.
        long _numCatchUpOps = 0;
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
    bool getWriteConcernMajorityShouldJournal_inlock() const;

    /**
     * Returns the OpTime of the current committed snapshot, if one exists.
     */
    OpTime _getCurrentCommittedSnapshotOpTime_inlock() const;

    /**
     * Returns the OpTime and corresponding wall clock time of the current committed snapshot, if
     * one exists.
     */
    OpTimeAndWallTime _getCurrentCommittedSnapshotOpTimeAndWallTime_inlock() const;

    /**
     * Returns the OpTime of the current committed snapshot converted to LogicalTime.
     */
    LogicalTime _getCurrentCommittedLogicalTime_inlock() const;

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
     * Helper to wake waiters in _replicationWaiterList that are doneWaitingForReplication.
     */
    void _wakeReadyWaiters(WithLock lk);

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
     * Helper method for _awaitReplication that takes an already locked unique_lock, but leaves
     * operation timing to the caller.
     */
    Status _awaitReplication_inlock(stdx::unique_lock<Latch>* lock,
                                    OperationContext* opCtx,
                                    const OpTime& opTime,
                                    const WriteConcernOptions& writeConcern);

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
    bool _doneWaitingForReplication_inlock(const OpTime& opTime,
                                           const WriteConcernOptions& writeConcern);

    Status _checkIfWriteConcernCanBeSatisfied_inlock(const WriteConcernOptions& writeConcern) const;

    Status _checkIfCommitQuorumCanBeSatisfied(WithLock,
                                              const CommitQuorumOptions& commitQuorum) const;

    bool _canAcceptWritesFor_inlock(const NamespaceString& ns);

    int _getMyId_inlock() const;

    OpTime _getMyLastAppliedOpTime_inlock() const;
    OpTimeAndWallTime _getMyLastAppliedOpTimeAndWallTime_inlock() const;

    OpTime _getMyLastDurableOpTime_inlock() const;
    OpTimeAndWallTime _getMyLastDurableOpTimeAndWallTime_inlock() const;

    /**
     * Helper method for updating our tracking of the last optime applied by a given node.
     * This is only valid to call on replica sets.
     * "configVersion" will be populated with our config version if it and the configVersion
     * of "args" differ.
     */
    Status _setLastOptime(WithLock lk,
                          const UpdatePositionArgs::UpdateInfo& args,
                          long long* configVersion);

    /**
     * This function will report our position externally (like upstream) if necessary.
     *
     * Takes in a unique lock, that must already be locked, on _mutex.
     *
     * Lock will be released after this method finishes.
     */
    void _reportUpstream_inlock(stdx::unique_lock<Latch> lock);

    /**
     * Helpers to set the last applied and durable OpTime.
     */
    void _setMyLastAppliedOpTimeAndWallTime(WithLock lk,
                                            const OpTimeAndWallTime& opTime,
                                            bool isRollbackAllowed,
                                            DataConsistency consistency);
    void _setMyLastDurableOpTimeAndWallTime(WithLock lk,
                                            const OpTimeAndWallTime& opTimeAndWallTime,
                                            bool isRollbackAllowed);

    /**
     * Schedules a heartbeat to be sent to "target" at "when". "targetIndex" is the index
     * into the replica set config members array that corresponds to the "target", or -1 if
     * "target" is not in _rsConfig.
     */
    void _scheduleHeartbeatToTarget_inlock(const HostAndPort& target, int targetIndex, Date_t when);

    /**
     * Processes each heartbeat response.
     *
     * Schedules additional heartbeats, triggers elections and step downs, etc.
     */
    void _handleHeartbeatResponse(const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData,
                                  int targetIndex);

    void _trackHeartbeatHandle_inlock(
        const StatusWith<executor::TaskExecutor::CallbackHandle>& handle);

    void _untrackHeartbeatHandle_inlock(const executor::TaskExecutor::CallbackHandle& handle);

    /*
     * Return a randomized offset amount that is scaled in proportion to the size of the
     * _electionTimeoutPeriod. Used to add randomization to an election timeout.
     */
    Milliseconds _getRandomizedElectionOffset_inlock();

    /**
     * Starts a heartbeat for each member in the current config.  Called while holding _mutex.
     */
    void _startHeartbeats_inlock();

    /**
     * Cancels all heartbeats.  Called while holding replCoord _mutex.
     */
    void _cancelHeartbeats_inlock();

    /**
     * Cancels all heartbeats, then starts a heartbeat for each member in the current config.
     * Called while holding replCoord _mutex.
     */
    void _restartHeartbeats_inlock();

    /**
     * Asynchronously sends a heartbeat to "target". "targetIndex" is the index
     * into the replica set config members array that corresponds to the "target", or -1 if
     * we don't have a valid replica set config.
     *
     * Scheduled by _scheduleHeartbeatToTarget_inlock.
     */
    void _doMemberHeartbeat(executor::TaskExecutor::CallbackArgs cbData,
                            const HostAndPort& target,
                            int targetIndex);


    MemberState _getMemberState_inlock() const;

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
     */
    bool _startLoadLocalConfig(OperationContext* opCtx);

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
    void _startDataReplication(OperationContext* opCtx,
                               stdx::function<void()> startCompleted = nullptr);

    /**
     * Stops replicating data by stopping the applier, fetcher and such.
     */
    void _stopDataReplication(OperationContext* opCtx);

    /**
     * Finishes the work of processReplSetInitiate() in the event of a successful quorum check.
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

    /**
     * Changes _rsConfigState to newState, and notify any waiters.
     */
    void _setConfigState_inlock(ConfigState newState);

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
     * Begins an attempt to elect this node.
     * Called after an incoming heartbeat changes this node's view of the set such that it
     * believes it can be elected PRIMARY.
     * For proper concurrency, start methods must be called while holding _mutex.
     *
     * For V1 (raft) style elections the election path is:
     *      _startElectSelfV1() or _startElectSelfV1_inlock()
     *      _processDryRunResult() (may skip)
     *      _startRealElection_inlock()
     *      _writeLastVoteForMyElection()
     *      _startVoteRequester_inlock()
     *      _onVoteRequestComplete()
     */
    void _startElectSelfV1_inlock(StartElectionReasonEnum reason);
    void _startElectSelfV1(StartElectionReasonEnum reason);

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
    void _startRealElection_inlock(long long originalTerm, StartElectionReasonEnum reason);

    /**
     * Writes the last vote in persistent storage after completing dry run successfully.
     * This job will be scheduled to run in DB worker threads.
     */
    void _writeLastVoteForMyElection(LastVote lastVote,
                                     const executor::TaskExecutor::CallbackArgs& cbData,
                                     StartElectionReasonEnum reason);

    /**
     * Starts VoteRequester to run the real election when last vote write has completed.
     */
    void _startVoteRequester_inlock(long long newTerm, StartElectionReasonEnum reason);

    /**
     * Callback called when the VoteRequester has completed; checks the results and
     * decides whether to change state to primary and alert other nodes of our primary-ness.
     * "originalTerm" was the term during which the election began, if the term has since
     * changed, do not step up as primary.
     */
    void _onVoteRequestComplete(long long originalTerm, StartElectionReasonEnum reason);

    /**
     * Removes 'host' from the sync source blacklist. If 'host' isn't found, it's simply
     * ignored and no error is thrown.
     *
     * Must be scheduled as a callback.
     */
    void _unblacklistSyncSource(const executor::TaskExecutor::CallbackArgs& cbData,
                                const HostAndPort& host);

    /**
     * Schedules a request that the given host step down; logs any errors.
     */
    void _requestRemotePrimaryStepdown(const HostAndPort& target);

    /**
     * Schedules stepdown to run with the global exclusive lock.
     */
    executor::TaskExecutor::EventHandle _stepDownStart();

    /**
     * kill all conflicting operations that are blocked either on prepare conflict or have taken
     * global lock not in MODE_IS. The conflicting operations can be either user or system
     * operations marked as killable.
     */
    void _killConflictingOpsOnStepUpAndStepDown(AutoGetRstlForStepUpStepDown* arsc,
                                                ErrorCodes::Error reason);

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
    void _scheduleHeartbeatReconfig_inlock(const ReplSetConfig& newConfig);

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
     * Utility method that schedules or performs actions specified by a HeartbeatResponseAction
     * returned by a TopologyCoordinator::processHeartbeatResponse(V1) call with the given
     * value of "responseStatus".
     *
     * Requires "lock" to own _mutex, and returns the same unique_lock.
     */
    stdx::unique_lock<Latch> _handleHeartbeatResponseAction_inlock(
        const HeartbeatResponseAction& action,
        const StatusWith<ReplSetHeartbeatResponse>& responseStatus,
        stdx::unique_lock<Latch> lock);

    /**
     * Updates the last committed OpTime to be 'committedOpTime' if it is more recent than the
     * current last committed OpTime. We ignore 'committedOpTime' if it has a different term than
     * our lastApplied, unless 'fromSyncSource'=true, which guarantees we are on the same branch of
     * history as 'committedOpTime', so we update our commit point to min(committedOpTime,
     * lastApplied).
     * Also updates corresponding wall clock time.
     */
    void _advanceCommitPoint(WithLock lk,
                             const OpTimeAndWallTime& committedOpTimeAndWallTime,
                             bool fromSyncSource);

    /**
     * Scan the memberData and determine the highest last applied or last
     * durable optime present on a majority of servers; set _lastCommittedOpTime to this
     * new entry.  Wake any threads waiting for replication that now have their
     * write concern satisfied.
     *
     * Whether the last applied or last durable op time is used depends on whether
     * the config getWriteConcernMajorityShouldJournal is set.
     */
    void _updateLastCommittedOpTimeAndWallTime(WithLock lk);

    /**
     * Callback that attempts to set the current term in topology coordinator and
     * relinquishes primary if the term actually changes and we are primary.
     * *updateTermResult will be the result of the update term attempt.
     * Returns the finish event if it does not finish in this function, for example,
     * due to stepdown, otherwise the returned EventHandle is invalid.
     */
    EventHandle _updateTerm_inlock(
        long long term, TopologyCoordinator::UpdateTermResult* updateTermResult = nullptr);

    /**
     * Callback that processes the ReplSetMetadata returned from a command run against another
     * replica set member and so long as the config version in the metadata matches the replica set
     * config version this node currently has, updates the current term.
     *
     * This does NOT update this node's notion of the commit point.
     *
     * Returns the finish event which is invalid if the process has already finished.
     */
    EventHandle _processReplSetMetadata_inlock(const rpc::ReplSetMetadata& replMetadata);

    /**
     * Prepares a metadata object for ReplSetMetadata.
     */
    void _prepareReplSetMetadata_inlock(const OpTime& lastOpTimeFromClient,
                                        BSONObjBuilder* builder) const;

    /**
     * Prepares a metadata object for OplogQueryMetadata.
     */
    void _prepareOplogQueryMetadata_inlock(int rbid, BSONObjBuilder* builder) const;

    /**
     * Blesses a snapshot to be used for new committed reads.
     *
     * Returns true if the value was updated to `newCommittedSnapshot`.
     */
    bool _updateCommittedSnapshot(WithLock lk, const OpTimeAndWallTime& newCommittedSnapshot);

    /**
     * A helper method that returns the current stable optime based on the current commit point and
     * set of stable optime candidates.
     */
    boost::optional<OpTimeAndWallTime> _recalculateStableOpTime(WithLock lk);

    /**
     * Calculates the 'stable' replication optime given a set of optime candidates and a maximum
     * stable optime. The stable optime is the greatest optime in 'candidates' that is also less
     * than or equal to 'maximumStableOpTime' and other criteria.
     *
     * Returns boost::none if there is no satisfactory candidate.
     */
    boost::optional<OpTimeAndWallTime> _chooseStableOpTimeFromCandidates(
        WithLock lk,
        const std::set<OpTimeAndWallTime>& candidates,
        OpTimeAndWallTime maximumStableOpTime);

    /**
     * Removes any optimes from the optime set 'candidates' that are less than
     * 'stableOpTime'.
     */
    void _cleanupStableOpTimeCandidates(std::set<OpTimeAndWallTime>* candidates,
                                        OpTimeAndWallTime stableOpTime);

    /**
     * Calculates and sets the value of the 'stable' replication optime for the storage engine.  See
     * ReplicationCoordinatorImpl::_chooseStableOpTimeFromCandidates for a definition of 'stable',
     * in this context.
     */
    void _setStableTimestampForStorage(WithLock lk);

    /**
     * Drops all snapshots and clears the "committed" snapshot.
     */
    void _dropAllSnapshots_inlock();

    /**
     * Bottom half of _scheduleNextLivenessUpdate.
     * Must be called with _mutex held.
     */
    void _scheduleNextLivenessUpdate_inlock();

    /**
     * Callback which marks downed nodes as down, triggers a stepdown if a majority of nodes are no
     * longer visible, and reschedules itself.
     */
    void _handleLivenessTimeout(const executor::TaskExecutor::CallbackArgs& cbData);

    /**
     * If "updatedMemberId" is the current _earliestMemberId, cancels the current
     * _handleLivenessTimeout callback and calls _scheduleNextLivenessUpdate to schedule a new one.
     * Returns immediately otherwise.
     */
    void _cancelAndRescheduleLivenessUpdate_inlock(int updatedMemberId);

    /**
     * Cancels all outstanding _priorityTakeover callbacks.
     */
    void _cancelPriorityTakeover_inlock();

    /**
     * Cancels all outstanding _catchupTakeover callbacks.
     */
    void _cancelCatchupTakeover_inlock();

    /**
     * Cancels the current _handleElectionTimeout callback and reschedules a new callback.
     * Returns immediately otherwise.
     */
    void _cancelAndRescheduleElectionTimeout_inlock();

    /**
     * Callback which starts an election if this node is electable and using protocolVersion 1.
     */
    void _startElectSelfIfEligibleV1(StartElectionReasonEnum reason);

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
     * Wrap a function into executor callback.
     * If the callback is cancelled, the given function won't run.
     */
    executor::TaskExecutor::CallbackFn _wrapAsCallbackFn(const stdx::function<void()>& work);

    /**
     * Finish catch-up mode and start drain mode.
     */
    void _enterDrainMode_inlock();

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
    executor::TaskExecutor::EventHandle _cancelElectionIfNeeded_inlock();

    /**
     * Waits until the optime of the current node is at least the 'opTime'.
     */
    Status _waitUntilOpTime(OperationContext* opCtx,
                            bool isMajorityReadConcern,
                            OpTime opTime,
                            boost::optional<Date_t> deadline = boost::none);

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
     * Returns a pseudorandom number no less than 0 and less than limit (which must be positive).
     */
    int64_t _nextRandomInt64_inlock(int64_t limit);

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
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ReplicationCoordinatorImpl::_mutex");  // (S)

    // Handles to actively queued heartbeats.
    HeartbeatHandles _heartbeatHandles;  // (M)

    // When this node does not know itself to be a member of a config, it adds
    // every host that sends it a heartbeat request to this set, and also starts
    // sending heartbeat requests to that host.  This set is cleared whenever
    // a node discovers that it is a member of a config.
    stdx::unordered_set<HostAndPort> _seedList;  // (M)

    // Back pointer to the ServiceContext that has started the instance.
    ServiceContext* const _service;  // (S)

    // Parsed command line arguments related to replication.
    const ReplSettings _settings;  // (R)

    // Mode of replication specified by _settings.
    const Mode _replMode;  // (R)

    // Pointer to the TopologyCoordinator owned by this ReplicationCoordinator.
    std::unique_ptr<TopologyCoordinator> _topCoord;  // (M)

    // Executor that drives the topology coordinator.
    std::unique_ptr<executor::TaskExecutor> _replExecutor;  // (S)

    // Pointer to the ReplicationCoordinatorExternalState owned by this ReplicationCoordinator.
    std::unique_ptr<ReplicationCoordinatorExternalState> _externalState;  // (PS)

    // list of information about clients waiting on replication.  Does *not* own the WaiterInfos.
    WaiterList _replicationWaiterList;  // (M)

    // list of information about clients waiting for a particular opTime.
    // Does *not* own the WaiterInfos.
    WaiterList _opTimeWaiterList;  // (M)

    // Waiter waiting on w:majority write availability.
    std::unique_ptr<CallbackWaiter> _wMajorityWriteAvailabilityWaiter;  // (M)

    // Set to true when we are in the process of shutting down replication.
    bool _inShutdown;  // (M)

    // Election ID of the last election that resulted in this node becoming primary.
    OID _electionId;  // (M)

    // Used to signal threads waiting for changes to _memberState.
    stdx::condition_variable _memberStateChange;  // (M)

    // Current ReplicaSet state.
    MemberState _memberState;  // (M)

    // Used to signal threads waiting for changes to _memberState.
    stdx::condition_variable _drainFinishedCond;  // (M)

    ReplicationCoordinator::ApplierState _applierState = ApplierState::Running;  // (M)

    // Used to signal threads waiting for changes to _rsConfigState.
    stdx::condition_variable _rsConfigStateChange;  // (M)

    // Represents the configuration state of the coordinator, which controls how and when
    // _rsConfig may change.  See the state transition diagram in the type definition of
    // ConfigState for details.
    ConfigState _rsConfigState;  // (M)

    // The current ReplicaSet configuration object, including the information about tag groups
    // that is used to satisfy write concern requests with named gle modes.
    ReplSetConfig _rsConfig;  // (M)

    // This member's index position in the current config.
    int _selfIndex;  // (M)

    std::unique_ptr<VoteRequester> _voteRequester;  // (M)

    // Event that the election code will signal when the in-progress election completes.
    executor::TaskExecutor::EventHandle _electionFinishedEvent;  // (M)

    // Event that the election code will signal when the in-progress election dry run completes,
    // which includes writing the last vote and scheduling the real election.
    executor::TaskExecutor::EventHandle _electionDryRunFinishedEvent;  // (M)

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
    std::shared_ptr<InitialSyncer>
        _initialSyncer;  // (I) pointer set under mutex, copied by callers.

    // Hands out the next snapshot name.
    AtomicWord<unsigned long long> _snapshotNameGenerator;  // (S)

    // The OpTimes and SnapshotNames for all snapshots newer than the current commit point, kept in
    // sorted order. Any time this is changed, you must also update _uncommitedSnapshotsSize.
    std::deque<OpTime> _uncommittedSnapshots;  // (M)

    // A cache of the size of _uncommittedSnaphots that can be read without any locking.
    // May only be written to while holding _mutex.
    AtomicWord<unsigned long long> _uncommittedSnapshotsSize;  // (I)

    // The non-null OpTimeAndWallTime and SnapshotName of the current snapshot used for committed
    // reads, if there is one.
    // When engaged, this must be <= _lastCommittedOpTime and < _uncommittedSnapshots.front().
    boost::optional<OpTimeAndWallTime> _currentCommittedSnapshot;  // (M)

    // A set of optimes that are used for computing the replication system's current 'stable'
    // optime. Every time a node's applied optime is updated, it will be added to this set.
    // Optimes that are older than the current stable optime should get removed from this set.
    // This set should also be cleared if a rollback occurs.
    std::set<OpTimeAndWallTime> _stableOpTimeCandidates;  // (M)

    // A flag that enables/disables advancement of the stable timestamp for storage.
    bool _shouldSetStableTimestamp = true;  // (M)

    // Used to signal threads that are waiting for new committed snapshots.
    stdx::condition_variable _currentCommittedSnapshotCond;  // (M)

    // Callback Handle used to cancel a scheduled LivenessTimeout callback.
    executor::TaskExecutor::CallbackHandle _handleLivenessTimeoutCbh;  // (M)

    // Callback Handle used to cancel a scheduled ElectionTimeout callback.
    executor::TaskExecutor::CallbackHandle _handleElectionTimeoutCbh;  // (M)

    // Election timeout callback will not run before this time.
    // If this date is Date_t(), the callback is either unscheduled or canceled.
    // Used for testing only.
    Date_t _handleElectionTimeoutWhen;  // (M)

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

    // Atomic-synchronized copy of Topology Coordinator's _term, for use by the public getTerm()
    // function.
    // This variable must be written immediately after _term, and thus its value can lag.
    // Reading this value does not require the replication coordinator mutex to be locked.
    AtomicWord<long long> _termShadow;  // (S)

    // When we decide to step down due to hearing about a higher term, we remember the term we heard
    // here so we can update our term to match as part of finishing stepdown.
    boost::optional<long long> _pendingTermUpdateDuringStepDown;  // (M)

    // Whether data replication is active.
    bool _startedSteadyStateReplication = false;  // (M)

    // If we're waiting to get the RSTL at stepdown and therefore should claim we don't allow
    // writes.  This is a counter rather than a flag because there are scenarios where multiple
    // stepdowns are attempted at once.
    short _waitingForRSTLAtStepDown = 0;

    // If we're in terminal shutdown.  If true, we'll refuse to vote in elections.
    bool _inTerminalShutdown = false;  // (M)
};

}  // namespace repl
}  // namespace mongo
