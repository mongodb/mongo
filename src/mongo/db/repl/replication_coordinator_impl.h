/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

class ElectCmdRunner;
class FreshnessChecker;
class HeartbeatResponseAction;
class LastVote;
class OplogReader;
class ReplicationProcess;
class ReplSetRequestVotesArgs;
class ReplSetConfig;
class SyncSourceFeedback;
class StorageInterface;
class TopologyCoordinator;
class VoteRequester;

class ReplicationCoordinatorImpl : public ReplicationCoordinator {
    MONGO_DISALLOW_COPYING(ReplicationCoordinatorImpl);

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

    virtual void shutdown(OperationContext* opCtx) override;

    virtual const ReplSettings& getSettings() const override;

    virtual Mode getReplicationMode() const override;

    virtual MemberState getMemberState() const override;

    virtual Status waitForMemberState(MemberState expectedState, Milliseconds timeout) override;

    virtual bool isInPrimaryOrSecondaryState() const override;

    virtual Seconds getSlaveDelaySecs() const override;

    virtual void clearSyncSourceBlacklist() override;

    virtual ReplicationCoordinator::StatusAndDuration awaitReplication(
        OperationContext* opCtx, const OpTime& opTime, const WriteConcernOptions& writeConcern);

    virtual Status stepDown(OperationContext* opCtx,
                            bool force,
                            const Milliseconds& waitTime,
                            const Milliseconds& stepdownTime);

    virtual bool isMasterForReportingPurposes();

    virtual bool canAcceptWritesForDatabase(OperationContext* opCtx, StringData dbName);
    virtual bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx, StringData dbName);

    bool canAcceptWritesFor(OperationContext* opCtx, const NamespaceString& ns) override;
    bool canAcceptWritesFor_UNSAFE(OperationContext* opCtx, const NamespaceString& ns) override;

    virtual Status checkIfWriteConcernCanBeSatisfied(const WriteConcernOptions& writeConcern) const;

    virtual Status checkCanServeReadsFor(OperationContext* opCtx,
                                         const NamespaceString& ns,
                                         bool slaveOk);
    virtual Status checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                const NamespaceString& ns,
                                                bool slaveOk);

    virtual bool shouldRelaxIndexConstraints(OperationContext* opCtx, const NamespaceString& ns);

    virtual void setMyLastAppliedOpTime(const OpTime& opTime);
    virtual void setMyLastDurableOpTime(const OpTime& opTime);

    virtual void setMyLastAppliedOpTimeForward(const OpTime& opTime, DataConsistency consistency);
    virtual void setMyLastDurableOpTimeForward(const OpTime& opTime);

    virtual void resetMyLastOpTimes();

    virtual void setMyHeartbeatMessage(const std::string& msg);

    virtual OpTime getMyLastAppliedOpTime() const override;
    virtual OpTime getMyLastDurableOpTime() const override;

    virtual Status waitUntilOpTimeForReadUntil(OperationContext* opCtx,
                                               const ReadConcernArgs& readConcern,
                                               boost::optional<Date_t> deadline) override;

    virtual Status waitUntilOpTimeForRead(OperationContext* opCtx,
                                          const ReadConcernArgs& readConcern) override;

    virtual OID getElectionId() override;

    virtual int getMyId() const override;

    virtual Status setFollowerMode(const MemberState& newState) override;

    virtual ApplierState getApplierState() override;

    virtual void signalDrainComplete(OperationContext* opCtx,
                                     long long termWhenBufferIsEmpty) override;

    virtual Status waitForDrainFinish(Milliseconds timeout) override;

    virtual void signalUpstreamUpdater() override;

    virtual Status resyncData(OperationContext* opCtx, bool waitUntilCompleted) override;

    virtual StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const override;

    virtual Status processReplSetGetStatus(BSONObjBuilder* result,
                                           ReplSetGetStatusResponseStyle responseStyle) override;

    virtual void fillIsMasterForReplSet(IsMasterResponse* result) override;

    virtual void appendSlaveInfoData(BSONObjBuilder* result) override;

    virtual ReplSetConfig getConfig() const override;

    virtual void processReplSetGetConfig(BSONObjBuilder* result) override;

    virtual void processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) override;

    virtual void advanceCommitPoint(const OpTime& committedOpTime) override;

    virtual void cancelAndRescheduleElectionTimeout() override;

    virtual Status setMaintenanceMode(bool activate) override;

    virtual bool getMaintenanceMode() override;

    virtual Status processReplSetSyncFrom(OperationContext* opCtx,
                                          const HostAndPort& target,
                                          BSONObjBuilder* resultObj) override;

    virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj) override;

    virtual Status processHeartbeat(const ReplSetHeartbeatArgs& args,
                                    ReplSetHeartbeatResponse* response) override;

    virtual Status processReplSetReconfig(OperationContext* opCtx,
                                          const ReplSetReconfigArgs& args,
                                          BSONObjBuilder* resultObj) override;

    virtual Status processReplSetInitiate(OperationContext* opCtx,
                                          const BSONObj& configObj,
                                          BSONObjBuilder* resultObj) override;

    virtual Status processReplSetFresh(const ReplSetFreshArgs& args,
                                       BSONObjBuilder* resultObj) override;

    virtual Status processReplSetElect(const ReplSetElectArgs& args,
                                       BSONObjBuilder* response) override;

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

    virtual Status processReplSetRequestVotes(OperationContext* opCtx,
                                              const ReplSetRequestVotesArgs& args,
                                              ReplSetRequestVotesResponse* response) override;

    virtual void prepareReplMetadata(const BSONObj& metadataRequestObj,
                                     const OpTime& lastOpTimeFromClient,
                                     BSONObjBuilder* builder) const override;

    virtual Status processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                      ReplSetHeartbeatResponse* response) override;

    virtual bool isV1ElectionProtocol() const override;

    virtual bool getWriteConcernMajorityShouldJournal() override;

    virtual void summarizeAsHtml(ReplSetHtmlSummary* s) override;

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

    virtual void waitUntilSnapshotCommitted(OperationContext* opCtx,
                                            const Timestamp& untilSnapshot) override;

    virtual void appendDiagnosticBSON(BSONObjBuilder*) override;

    virtual void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    virtual size_t getNumUncommittedSnapshots() override;

    virtual WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(
        WriteConcernOptions wc) override;

    virtual ReplSettings::IndexPrefetchConfig getIndexPrefetchConfig() const override;
    virtual void setIndexPrefetchConfig(const ReplSettings::IndexPrefetchConfig cfg) override;

    virtual Status stepUpIfEligible() override;

    virtual Status abortCatchupIfNeeded() override;

    void signalDropPendingCollectionsRemovedFromStorage() final;

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
     * Simple wrappers around _setLastOptime_inlock to make it easier to test.
     */
    Status setLastAppliedOptime_forTest(long long cfgVer, long long memberId, const OpTime& opTime);
    Status setLastDurableOptime_forTest(long long cfgVer, long long memberId, const OpTime& opTime);

    /**
     * Simple test wrappers that expose private methods.
     */
    boost::optional<OpTime> calculateStableOpTime_forTest(const std::set<OpTime>& candidates,
                                                          const OpTime& commitPoint);
    void cleanupStableOpTimeCandidates_forTest(std::set<OpTime>* candidates, OpTime stableOpTime);
    std::set<OpTime> getStableOpTimeCandidates_forTest();
    boost::optional<OpTime> getStableOpTime_forTest();

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
     * Waits until a stepdown command has begun. Callers should ensure that the stepdown attempt
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
        kActionCloseAllConnections,  // Also indicates that we should clear sharding state.
        kActionFollowerModeStateChange,
        kActionWinElection,
        kActionStartSingleNodeElection
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
        void abort_inlock();
        // Heartbeat calls this function to update the target optime.
        void signalHeartbeatUpdate_inlock();

    private:
        ReplicationCoordinatorImpl* _repl;  // Not owned.
        // Callback handle used to cancel a scheduled catchup timeout callback.
        executor::TaskExecutor::CallbackHandle _timeoutCbh;
        // Handle to a Waiter that contains the current target optime to reach after which
        // we can exit catchup mode.
        std::unique_ptr<CallbackWaiter> _waiter;
    };

    void _resetMyLastOpTimes_inlock();

    /**
     * Returns the _writeConcernMajorityJournalDefault of our current _rsConfig.
     */
    bool getWriteConcernMajorityShouldJournal_inlock() const;

    /**
     * Returns the OpTime of the current committed snapshot, if one exists.
     */
    OpTime _getCurrentCommittedSnapshotOpTime_inlock() const;

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
    PostMemberStateUpdateAction _setCurrentRSConfig_inlock(OperationContext* opCtx,
                                                           const ReplSetConfig& newConfig,
                                                           int myIndex);

    /**
     * Helper to wake waiters in _replicationWaiterList that are doneWaitingForReplication.
     */
    void _wakeReadyWaiters_inlock();

    /**
     * Scheduled to cause the ReplicationCoordinator to reconsider any state that might
     * need to change as a result of time passing - for instance becoming PRIMARY when a single
     * node replica set member's stepDown period ends.
     */
    void _handleTimePassing(const executor::TaskExecutor::CallbackArgs& cbData);

    /**
     * Helper method for _awaitReplication that takes an already locked unique_lock, but leaves
     * operation timing to the caller.
     */
    Status _awaitReplication_inlock(stdx::unique_lock<stdx::mutex>* lock,
                                    OperationContext* opCtx,
                                    const OpTime& opTime,
                                    const WriteConcernOptions& writeConcern);

    /**
     * Returns true if the given writeConcern is satisfied up to "optime" or is unsatisfiable.
     *
     * If the writeConcern is 'majority', also waits for _currentCommittedSnapshot to be newer than
     * minSnapshot.
     */
    bool _doneWaitingForReplication_inlock(const OpTime& opTime,
                                           const WriteConcernOptions& writeConcern);

    Status _checkIfWriteConcernCanBeSatisfied_inlock(const WriteConcernOptions& writeConcern) const;

    /**
     * Wakes up threads in the process of handling a stepdown request based on whether the
     * TopologyCoordinator now believes enough secondaries are caught up for the stepdown request to
     * complete.
     */
    void _signalStepDownWaiterIfReady_inlock();

    bool _canAcceptWritesFor_inlock(const NamespaceString& ns);

    int _getMyId_inlock() const;

    OpTime _getMyLastAppliedOpTime_inlock() const;
    OpTime _getMyLastDurableOpTime_inlock() const;

    /**
     * Helper method for updating our tracking of the last optime applied by a given node.
     * This is only valid to call on replica sets.
     * "configVersion" will be populated with our config version if it and the configVersion
     * of "args" differ.
     */
    Status _setLastOptime_inlock(const UpdatePositionArgs::UpdateInfo& args,
                                 long long* configVersion);

    /**
     * This function will report our position externally (like upstream) if necessary.
     *
     * Takes in a unique lock, that must already be locked, on _mutex.
     *
     * Lock will be released after this method finishes.
     */
    void _reportUpstream_inlock(stdx::unique_lock<stdx::mutex> lock);

    /**
     * Helpers to set the last applied and durable OpTime.
     */
    void _setMyLastAppliedOpTime_inlock(const OpTime& opTime,
                                        bool isRollbackAllowed,
                                        DataConsistency consistency);
    void _setMyLastDurableOpTime_inlock(const OpTime& opTime, bool isRollbackAllowed);

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
                                const StatusWith<OpTime>& lastOpTimeStatus,
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
    void _finishReplSetReconfig(const executor::TaskExecutor::CallbackArgs& cbData,
                                const ReplSetConfig& newConfig,
                                bool isForceReconfig,
                                int myIndex,
                                const executor::TaskExecutor::EventHandle& finishedEvent);

    /**
     * Changes _rsConfigState to newState, and notify any waiters.
     */
    void _setConfigState_inlock(ConfigState newState);

    /**
     * Updates the cached value, _memberState, to match _topCoord's reported
     * member state, from getMemberState().
     *
     * Returns an enum indicating what action to take after releasing _mutex, if any.
     * Call performPostMemberStateUpdateAction on the return value after releasing
     * _mutex.
     *
     * Note: opCtx may be null as currently not all paths thread an OperationContext all the way
     * down, but it must be non-null for any calls that change _canAcceptNonLocalWrites.
     */
    PostMemberStateUpdateAction _updateMemberStateFromTopologyCoordinator_inlock(
        OperationContext* opCtx);

    /**
     * Performs a post member-state update action.  Do not call while holding _mutex.
     */
    void _performPostMemberStateUpdateAction(PostMemberStateUpdateAction action);

    /**
     * Begins an attempt to elect this node.
     * Called after an incoming heartbeat changes this node's view of the set such that it
     * believes it can be elected PRIMARY.
     * For proper concurrency, start methods must be called while holding _mutex.
     *
     * For old style elections the election path is:
     *      _startElectSelf_inlock()
     *      _onFreshnessCheckComplete()
     *      _onElectCmdRunnerComplete()
     * For V1 (raft) style elections the election path is:
     *      _startElectSelfV1() or _startElectSelfV1_inlock()
     *      _onDryRunComplete()
     *      _writeLastVoteForMyElection()
     *      _startVoteRequester_inlock()
     *      _onVoteRequestComplete()
     */
    void _startElectSelf_inlock();
    void _startElectSelfV1_inlock(TopologyCoordinator::StartElectionReason reason);
    void _startElectSelfV1(TopologyCoordinator::StartElectionReason reason);

    /**
     * Callback called when the FreshnessChecker has completed; checks the results and
     * decides whether to continue election proceedings.
     **/
    void _onFreshnessCheckComplete();

    /**
     * Callback called when the ElectCmdRunner has completed; checks the results and
     * decides whether to complete the election and change state to primary.
     **/
    void _onElectCmdRunnerComplete();

    /**
     * Callback called when the dryRun VoteRequester has completed; checks the results and
     * decides whether to conduct a proper election.
     * "originalTerm" was the term during which the dry run began, if the term has since
     * changed, do not run for election.
     */
    void _onDryRunComplete(long long originalTerm);

    /**
     * Writes the last vote in persistent storage after completing dry run successfully.
     * This job will be scheduled to run in DB worker threads.
     */
    void _writeLastVoteForMyElection(LastVote lastVote,
                                     const executor::TaskExecutor::CallbackArgs& cbData);

    /**
     * Starts VoteRequester to run the real election when last vote write has completed.
     */
    void _startVoteRequester_inlock(long long newTerm);

    /**
     * Callback called when the VoteRequester has completed; checks the results and
     * decides whether to change state to primary and alert other nodes of our primary-ness.
     * "originalTerm" was the term during which the election began, if the term has since
     * changed, do not step up as primary.
     */
    void _onVoteRequestComplete(long long originalTerm);

    /**
     * Callback called after a random delay, to prevent repeated election ties.
     */
    void _recoverFromElectionTie(const executor::TaskExecutor::CallbackArgs& cbData);

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
     * Completes a step-down of the current node.  Must be run with a global
     * shared or global exclusive lock.
     * Signals 'finishedEvent' on successful completion.
     */
    void _stepDownFinish(const executor::TaskExecutor::CallbackArgs& cbData,
                         const executor::TaskExecutor::EventHandle& finishedEvent);

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
    stdx::unique_lock<stdx::mutex> _handleHeartbeatResponseAction_inlock(
        const HeartbeatResponseAction& action,
        const StatusWith<ReplSetHeartbeatResponse>& responseStatus,
        stdx::unique_lock<stdx::mutex> lock);

    /**
     * Updates the last committed OpTime to be "committedOpTime" if it is more recent than the
     * current last committed OpTime.
     */
    void _advanceCommitPoint_inlock(const OpTime& committedOpTime);

    /**
     * Scan the memberData and determine the highest last applied or last
     * durable optime present on a majority of servers; set _lastCommittedOpTime to this
     * new entry.  Wake any threads waiting for replication that now have their
     * write concern satisfied.
     *
     * Whether the last applied or last durable op time is used depends on whether
     * the config getWriteConcernMajorityShouldJournal is set.
     */
    void _updateLastCommittedOpTime_inlock();

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
    bool _updateCommittedSnapshot_inlock(const OpTime& newCommittedSnapshot);

    /**
     * A helper method that returns the current stable optime based on the current commit point and
     * set of stable optime candidates.
     */
    boost::optional<OpTime> _getStableOpTime_inlock();

    /**
     * Calculates the 'stable' replication optime given a set of optime candidates and the
     * current commit point. The stable optime is the greatest optime in 'candidates' that is
     * also less than or equal to 'commitPoint'.
     */
    boost::optional<OpTime> _calculateStableOpTime_inlock(const std::set<OpTime>& candidates,
                                                          const OpTime& commitPoint);

    /**
     * Removes any optimes from the optime set 'candidates' that are less than
     * 'stableOpTime'.
     */
    void _cleanupStableOpTimeCandidates(std::set<OpTime>* candidates, OpTime stableOpTime);

    /**
     * Calculates and sets the value of the 'stable' replication optime for the storage engine.
     * See ReplicationCoordinatorImpl::_calculateStableOpTime for a definition of 'stable', in
     * this context.
     */
    void _setStableTimestampForStorage_inlock();

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
    void _startElectSelfIfEligibleV1(TopologyCoordinator::StartElectionReason reason);

    /**
     * Resets the term of last vote to 0 to prevent any node from voting for term 0.
     */
    void _resetElectionInfoOnProtocolVersionUpgrade(OperationContext* opCtx,
                                                    const ReplSetConfig& oldConfig,
                                                    const ReplSetConfig& newConfig);

    /**
     * Schedules work to be run no sooner than 'when' and returns handle to callback.
     * If work cannot be scheduled due to shutdown, returns empty handle.
     * All other non-shutdown scheduling failures will abort the process.
     * Does not run 'work' if callback is canceled.
     */
    CallbackHandle _scheduleWorkAt(Date_t when, const CallbackFn& work);

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
    // (GM) Readable under any global intent lock or _mutex.  Must hold both the global lock in
    //      exclusive mode (MODE_X) and hold _mutex to write.
    // (I)  Independently synchronized, see member variable comment.

    // Protects member data of this ReplicationCoordinator.
    mutable stdx::mutex _mutex;  // (S)

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

    // Condition to signal when new heartbeat data comes in.
    stdx::condition_variable _stepDownWaiters;  // (M)

    // State for conducting an election of this node.
    // the presence of a non-null _freshnessChecker pointer indicates that an election is
    // currently in progress. When using the V1 protocol, a non-null _voteRequester pointer
    // indicates this instead.
    // Only one election is allowed at a time.
    std::unique_ptr<FreshnessChecker> _freshnessChecker;  // (M)

    std::unique_ptr<ElectCmdRunner> _electCmdRunner;  // (M)

    std::unique_ptr<VoteRequester> _voteRequester;  // (M)

    // Event that the election code will signal when the in-progress election completes.
    // Unspecified value when _freshnessChecker is NULL.
    executor::TaskExecutor::EventHandle _electionFinishedEvent;  // (M)

    // Event that the election code will signal when the in-progress election dry run completes,
    // which includes writing the last vote and scheduling the real election.
    executor::TaskExecutor::EventHandle _electionDryRunFinishedEvent;  // (M)

    // Whether we slept last time we attempted an election but possibly tied with other nodes.
    bool _sleptLastElection;  // (M)

    // Flag that indicates whether writes to databases other than "local" are allowed.  Used to
    // answer canAcceptWritesForDatabase() and canAcceptWritesFor() questions.
    // Always true for standalone nodes.
    bool _canAcceptNonLocalWrites;  // (GM)

    // Flag that indicates whether reads from databases other than "local" are allowed.  Unlike
    // _canAcceptNonLocalWrites, above, this question is about admission control on secondaries,
    // and we do not require that its observers be strongly synchronized.  Accidentally
    // providing the prior value for a limited period of time is acceptable.  Also unlike
    // _canAcceptNonLocalWrites, its value is only meaningful on replica set secondaries.
    AtomicUInt32 _canServeNonLocalReads;  // (S)

    // ReplicationProcess used to hold information related to the replication and application of
    // operations from the sync source.
    ReplicationProcess* const _replicationProcess;  // (PS)

    // Storage interface used by initial syncer.
    StorageInterface* _storage;  // (PS)
    // InitialSyncer used for initial sync.
    std::shared_ptr<InitialSyncer>
        _initialSyncer;  // (I) pointer set under mutex, copied by callers.

    // Hands out the next snapshot name.
    AtomicUInt64 _snapshotNameGenerator;  // (S)

    // The OpTimes and SnapshotNames for all snapshots newer than the current commit point, kept in
    // sorted order. Any time this is changed, you must also update _uncommitedSnapshotsSize.
    std::deque<OpTime> _uncommittedSnapshots;  // (M)

    // A cache of the size of _uncommittedSnaphots that can be read without any locking.
    // May only be written to while holding _mutex.
    AtomicUInt64 _uncommittedSnapshotsSize;  // (I)

    // The non-null OpTime and SnapshotName of the current snapshot used for committed reads, if
    // there is one.
    // When engaged, this must be <= _lastCommittedOpTime and < _uncommittedSnapshots.front().
    boost::optional<OpTime> _currentCommittedSnapshot;  // (M)

    // A set of optimes that are used for computing the replication system's current 'stable'
    // optime. Every time a node's applied optime is updated, it will be added to this set.
    // Optimes that are older than the current stable optime should get removed from this set.
    // This set should also be cleared if a rollback occurs.
    std::set<OpTime> _stableOpTimeCandidates;  // (M)

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
    AtomicInt64 _protVersion{1};  // (S)

    // Source of random numbers used in setting election timeouts, etc.
    PseudoRandom _random;  // (M)

    // This setting affects the Applier prefetcher behavior.
    mutable stdx::mutex _indexPrefetchMutex;
    ReplSettings::IndexPrefetchConfig _indexPrefetchConfig =
        ReplSettings::IndexPrefetchConfig::PREFETCH_ALL;  // (I)

    // The catchup state including all catchup logic. The presence of a non-null pointer indicates
    // that the node is currently in catchup mode.
    std::unique_ptr<CatchupState> _catchupState;  // (X)

    // Atomic-synchronized copy of Topology Coordinator's _term, for use by the public getTerm()
    // function.
    // This variable must be written immediately after _term, and thus its value can lag.
    // Reading this value does not require the replication coordinator mutex to be locked.
    AtomicInt64 _termShadow;  // (S)

    // When we decide to step down due to hearing about a higher term, we remember the term we heard
    // here so we can update our term to match as part of finishing stepdown.
    boost::optional<long long> _pendingTermUpdateDuringStepDown;  // (M)
};

}  // namespace repl
}  // namespace mongo
