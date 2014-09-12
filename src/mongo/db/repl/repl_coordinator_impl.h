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

#include <boost/scoped_ptr.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/optime.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_coordinator.h"
#include "mongo/db/repl/repl_coordinator_external_state.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

    class Timer;

namespace repl {

    class OplogReader;
    class SyncSourceFeedback;
    class TopologyCoordinator;

    class ReplicationCoordinatorImpl : public ReplicationCoordinator,
                                       public KillOpListenerInterface {
        MONGO_DISALLOW_COPYING(ReplicationCoordinatorImpl);

    public:

        // Takes ownership of the "externalState", "topCoord" and "network" objects.
        ReplicationCoordinatorImpl(const ReplSettings& settings,
                                   ReplicationCoordinatorExternalState* externalState,
                                   ReplicationExecutor::NetworkInterface* network,
                                   TopologyCoordinator* topoCoord,
                                   int64_t prngSeed);
        virtual ~ReplicationCoordinatorImpl();

        // ================== Members of public ReplicationCoordinator API ===================

        virtual void startReplication(OperationContext* txn);

        virtual void shutdown();

        virtual ReplSettings& getSettings();

        virtual Mode getReplicationMode() const;

        virtual MemberState getCurrentMemberState() const;

        /*
         * Implementation of the KillOpListenerInterface interrupt method so that we can wake up
         * threads blocked in awaitReplication() when a killOp command comes in.
         */
        virtual void interrupt(unsigned opId);

        /*
         * Implementation of the KillOpListenerInterface interruptAll method so that we can wake up
         * threads blocked in awaitReplication() when we kill all operations.
         */
        virtual void interruptAll();

        virtual ReplicationCoordinator::StatusAndDuration awaitReplication(
                const OperationContext* txn,
                const OpTime& ts,
                const WriteConcernOptions& writeConcern);

        virtual ReplicationCoordinator::StatusAndDuration awaitReplicationOfLastOpForClient(
                const OperationContext* txn,
                const WriteConcernOptions& writeConcern);

        virtual ReplicationCoordinator::StatusAndDuration awaitReplicationOfLastOpApplied(
                const OperationContext* txn,
                const WriteConcernOptions& writeConcern);

        virtual Status stepDown(OperationContext* txn,
                                bool force,
                                const Milliseconds& waitTime,
                                const Milliseconds& stepdownTime);

        virtual bool isMasterForReportingPurposes();

        virtual bool canAcceptWritesForDatabase(const StringData& dbName);

        virtual Status checkIfWriteConcernCanBeSatisfied(
                const WriteConcernOptions& writeConcern) const;

        virtual Status canServeReadsFor(OperationContext* txn,
                                        const NamespaceString& ns,
                                        bool slaveOk);

        virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx);

        virtual Status setLastOptime(OperationContext* txn, const OID& rid, const OpTime& ts);

        virtual Status setMyLastOptime(OperationContext* txn, const OpTime& ts);

        virtual OpTime getMyLastOptime() const;

        virtual OID getElectionId();

        virtual OID getMyRID() const;

        virtual void setFollowerMode(const MemberState& newState);

        virtual void prepareReplSetUpdatePositionCommand(OperationContext* txn,
                                                         BSONObjBuilder* cmdBuilder);

        virtual void prepareReplSetUpdatePositionCommandHandshakes(
                OperationContext* txn,
                std::vector<BSONObj>* handshakes);

        virtual Status processReplSetGetStatus(BSONObjBuilder* result);

        virtual void processReplSetGetConfig(BSONObjBuilder* result);

        virtual Status setMaintenanceMode(OperationContext* txn, bool activate);

        virtual Status processReplSetSyncFrom(const HostAndPort& target,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj);

        virtual Status processHeartbeat(const ReplSetHeartbeatArgs& args,
                                        ReplSetHeartbeatResponse* response);

        virtual Status processReplSetReconfig(OperationContext* txn,
                                              const ReplSetReconfigArgs& args,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetInitiate(OperationContext* txn,
                                              const BSONObj& configObj,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetGetRBID(BSONObjBuilder* resultObj);

        virtual void incrementRollbackID();

        virtual Status processReplSetFresh(const ReplSetFreshArgs& args,
                                           BSONObjBuilder* resultObj);

        virtual Status processReplSetElect(const ReplSetElectArgs& args,
                                           BSONObjBuilder* resultObj);

        virtual Status processReplSetUpdatePosition(OperationContext* txn,
                                                    const UpdatePositionArgs& updates);

        virtual Status processHandshake(const OperationContext* txn,
                                        const HandshakeArgs& handshake);

        virtual bool buildsIndexes();

        virtual std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op);

        virtual BSONObj getGetLastErrorDefault();

        virtual Status checkReplEnabledForCommand(BSONObjBuilder* result);

        virtual bool isReplEnabled() const;

        virtual void connectOplogReader(OperationContext* txn,
                                        BackgroundSync* bgsync,
                                        OplogReader* r);

        
        // ================== Members of replication code internal API ===================

        // This is a temporary hack to set the replset config to the config detected by the
        // legacy coordinator.
        // TODO(spencer): Remove this once this class can load its own config
        void forceCurrentRSConfigHack(const BSONObj& config, int myIndex);

        /**
         * Does a heartbeat for a member of the replica set.
         * Should be started during (re)configuration or in the heartbeat callback only.
         */
        void doMemberHeartbeat(ReplicationExecutor::CallbackData cbData,
                               const HostAndPort& hap);

        /**
         * Cancels all heartbeats.
         *
         * This is only called during the callback when there is a new config.
         * At this time no new heartbeats can be scheduled due to the serialization
         * of calls via the executor.
         */
        void cancelHeartbeats();

        /**
         * Chooses a sync source.
         * A wrapper that schedules _chooseNewSyncSource() through the Replication Executor and
         * waits for its completion.
         */
        HostAndPort chooseNewSyncSource();

        /**
         * Blacklists 'host' until 'until'.
         * A wrapper that schedules _blacklistSyncSource() through the Replication Executor and
         * waits for its completion.
         */
        void blacklistSyncSource(const HostAndPort& host, Date_t until);


        // ================== Test support API ===================

        /**
         * If called after startReplication(), blocks until all asynchronous
         * activities associated with replication start-up complete.
         */
        void waitForStartUpComplete();

        /**
         * Used by testing code to run election proceedings, in leiu of a better
         * method to acheive this.
         */
        void testElection();

        /**
         * Used to set the current member state of this node.
         * Should only be used in unit tests.
         */
        void _setCurrentMemberState_forTest(const MemberState& newState);

    private:

        /**
         * Configuration states for a replica set node.
         *
         * Transition diagram:
         *
         * ReplicationDisabled   +----------> HBReconfig
         *    ^                  |                     \
         *    |                  v                      |
         * StartingUp -> Uninitialized <-> Initiating   |
         *          \                    /              |
         *           \        __________/               /
         *            v      v                         /
         *             Steady <-----------------------
         *               ^
         *               |
         *               v
         *             Reconfig
         */
        enum ConfigState {
            kConfigStartingUp,
            kConfigReplicationDisabled,
            kConfigUninitialized,
            kConfigSteady,
            kConfigInitiating,
            kConfigReconfiguring,
            kConfigHBReconfiguring
        };

        // Struct that holds information about clients waiting for replication.
        struct WaiterInfo;

        // Struct that holds information about nodes in this replication group, mainly used for
        // tracking replication progress for write concern satisfaction.
        struct SlaveInfo {
            OpTime opTime; // Our last known OpTime that this slave has replicated to.
            HostAndPort hostAndPort; // Client address of the slave.
            int memberID; // ID of the node in the replica set config, or -1 if we're not a replSet.
            SlaveInfo() : memberID(-1) {}
        };

        // Map of node RIDs to their SlaveInfo.
        typedef unordered_map<OID, SlaveInfo, OID::Hasher> SlaveInfoMap;

        typedef std::vector<ReplicationExecutor::CallbackHandle> HeartbeatHandles;

        /**
         * Helpers to update our saved config, cancel any pending heartbeats, and kick off sending
         * new heartbeats based on the new config.  Must *only* be called from within the
         * ReplicationExecutor context.
         */
        void _setCurrentRSConfig(
                const ReplicationExecutor::CallbackData& cbData,
                const ReplicaSetConfig& newConfig,
                int myIndex);
        void _setCurrentRSConfig_inlock(
                const ReplicaSetConfig& newConfig,
                int myIndex);

        /**
         * Helper method for setting/unsetting maintenance mode.  Scheduled by setMaintenanceMode()
         * to run in a global write lock in the replication executor thread.
         */
        void _setMaintenanceMode_helper(const ReplicationExecutor::CallbackData& cbData,
                                        bool activate,
                                        Status* result);

        /**
         * Bottom half of _setCurrentMemberState_forTest.
         */
        void _setCurrentMemberState_forTestFinish(const ReplicationExecutor::CallbackData& cbData,
                                                  const MemberState& newState);

        /*
         * Returns the OpTime of the last applied operation on this node.
         */
        OpTime _getLastOpApplied();
        OpTime _getLastOpApplied_inlock();

        /**
         * Helper method for _awaitReplication that takes an already locked unique_lock and a
         * Timer for timing the operation which has been counting since before the lock was
         * acquired.
         */
        ReplicationCoordinator::StatusAndDuration _awaitReplication_inlock(
                const Timer* timer,
                boost::unique_lock<boost::mutex>* lock,
                const OperationContext* txn,
                const OpTime& ts,
                const WriteConcernOptions& writeConcern);

        /*
         * Returns true if the given writeConcern is satisfied up to "optime" or is unsatisfiable.
         */
        bool _doneWaitingForReplication_inlock(const OpTime& opTime,
                                               const WriteConcernOptions& writeConcern);

        /**
         * Helper for _doneWaitingForReplication_inlock that takes an integer write concern.
         */
        bool _doneWaitingForReplication_numNodes_inlock(const OpTime& opTime, int numNodes);

        /**
         * Helper for _doneWaitingForReplication_inlock that takes a tag pattern representing a
         * named write concern mode.
         */
        bool _doneWaitingForReplication_gleMode_inlock(const OpTime& opTime,
                                                       const ReplicaSetTagPattern& tagPattern);

        Status _checkIfWriteConcernCanBeSatisfied_inlock(
                const WriteConcernOptions& writeConcern) const;

        OID _getMyRID_inlock() const;

        /**
         * Bottom half of setFollowerMode.
         */
        void _setFollowerModeFinish(const ReplicationExecutor::CallbackData& cbData,
                                    const MemberState& newState);

        /**
         * Helper method for setLastOptime and setMyLastOptime that takes in a unique lock on
         * _mutex.  The passed in lock must already be locked.  It is unknown what state the lock
         * will be in after this method finishes.
         */
        Status _setLastOptime_inlock(boost::unique_lock<boost::mutex>* lock,
                                     const OID& rid,
                                     const OpTime& ts);

        /**
         * Processes each heartbeat response.
         * Also responsible for scheduling additional heartbeats within the timeout if they error,
         * and on success.
         */
        void _handleHeartbeatResponse(const ReplicationExecutor::RemoteCommandCallbackData& cbData,
                                      const HostAndPort& hap,
                                      Date_t firstCallDate,
                                      int retriesLeft);

        void _trackHeartbeatHandle(const ReplicationExecutor::CallbackHandle& handle);

        void _untrackHeartbeatHandle(const ReplicationExecutor::CallbackHandle& handle);

        /**
         * Starts a heartbeat for each member in the current config
         */
        void _startHeartbeats();

        MemberState _getCurrentMemberState_inlock() const;

        /**
         * Returns the current replication mode. This method requires the caller to be holding
         * "_mutex" to be called safely.
         */
        Mode _getReplicationMode_inlock() const;

        /**
         * Starts loading the replication configuration from local storage, and if it is valid,
         * schedules a callback (of _finishLoadLocalConfig) to set it as the current replica set
         * config (sets _rsConfig and _thisMembersConfigIndex).
         * Returns true if it finishes loading the local config, which most likely means there
         * was no local config at all or it was invalid in some way, and false if there was a valid
         * config detected but more work is needed to set it as the local config (which will be
         * handled by the callback to _finishLoadLocalConfig).
         */
        bool _startLoadLocalConfig(OperationContext* txn);

        /**
         * Callback that finishes the work started in _startLoadLocalConfig and sets
         * _isStartupComplete to true, so that we can begin processing heartbeats and reconfigs.
         */
        void _finishLoadLocalConfig(const ReplicationExecutor::CallbackData& cbData,
                                    const ReplicaSetConfig& localConfig);

        /**
         * Helper method that does most of the work of _finishLoadLocalConfig, minus setting
         * _isStartupComplete to true.
         */
        void _finishLoadLocalConfig_helper(const ReplicationExecutor::CallbackData& cbData,
                                           const ReplicaSetConfig& localConfig);

        /**
         * Callback that finishes the work of processReplSetInitiate() inside the replication
         * executor context, in the event of a successful quorum check.
         */
        void _finishReplSetInitiate(
                const ReplicationExecutor::CallbackData& cbData,
                const ReplicaSetConfig& newConfig,
                int myIndex);

        /**
         * Changes _rsConfigState to newState, and notify any waiters.
         */
        void _setConfigState_inlock(ConfigState newState);

        /**
         * Begins an attempt to elect this node.
         * Called after an incoming heartbeat changes this node's view of the set such that it
         * believes it can be elected PRIMARY.
         * For proper concurrency, must be called via a ReplicationExecutor callback.
         * finishEvh is an event that is signaled when election is done, regardless of success.
         **/
        void _startElectSelf(const ReplicationExecutor::CallbackData& cbData,
                             const ReplicationExecutor::EventHandle& finishEvh);

        /**
         * Callback called when the FreshnessChecker has completed; checks the results and
         * decides whether to continue election proceedings.
         * finishEvh is an event that is signaled when election is complete.
         **/
        void _onFreshnessCheckComplete(const ReplicationExecutor::CallbackData& cbData,
                                       const ReplicationExecutor::EventHandle& finishEvh);

        /**
         * Callback called when the ElectCmdRunner has completed; checks the results and
         * decides whether to complete the election and change state to primary.
         * finishEvh is an event that is signaled when election is complete.
         **/
        void _onElectCmdRunnerComplete(const ReplicationExecutor::CallbackData& cbData,
                                       const ReplicationExecutor::EventHandle& finishEvh);

        /**
         * Chooses a new sync source.  Must be scheduled as a callback.
         * 
         * Calls into the Topology Coordinator, which uses its current view of the set to choose
         * the most appropriate sync source.
         */
        void _chooseNewSyncSource(const ReplicationExecutor::CallbackData& cbData,
                                  HostAndPort* newSyncSource);

        /**
         * Adds 'host' to the sync source blacklist until 'until'. A blacklisted source cannot
         * be chosen as a sync source.
         *
         * Must be scheduled as a callback.
         */
        void _blacklistSyncSource(const ReplicationExecutor::CallbackData& cbData,
                                  const HostAndPort& host,
                                  Date_t until);


        //
        // All member variables are labeled with one of the following codes indicating the
        // synchronization rules for accessing them.
        //
        // (R)  Read-only in concurrent operation; no synchronization required.
        // (S)  Self-synchronizing; access in any way from any context.
        // (PS) Pointer is read-only in concurrent operation, item pointed to is self-synchronizing;
        //      Access in any context.
        // (M)  Reads and writes guarded by _mutex
        // (X)  Reads and writes must be performed in a callback in _replExecutor
        // (MX) Must hold _mutex and be in a callback in _replExecutor to write; must either hold
        //      _mutex or be in a callback in _replExecutor to read.
        // (I)  Independently synchronized, see member variable comment.

        // Protects member data of this ReplicationCoordinator.
        mutable boost::mutex _mutex;                                                      // (S)

        // Handles to actively queued heartbeats.
        HeartbeatHandles _heartbeatHandles;                                               // (X)

        // Parsed command line arguments related to replication.
        // TODO(spencer): Currently there is global mutable state
        // in ReplSettings, but we should be able to get rid of that after the legacy repl
        // coordinator is gone. At that point we can make this const.
        ReplSettings _settings;                                                           // (R)

        // Pointer to the TopologyCoordinator owned by this ReplicationCoordinator.
        boost::scoped_ptr<TopologyCoordinator> _topCoord;                                 // (X)

        // Executor that drives the topology coordinator.
        ReplicationExecutor _replExecutor;                                                // (S)

        // Pointer to the ReplicationCoordinatorExternalState owned by this ReplicationCoordinator.
        boost::scoped_ptr<ReplicationCoordinatorExternalState> _externalState;            // (PS)

        // Thread that _syncSourceFeedback runs in to send replSetUpdatePosition commands upstream.
        // Set in startReplication() and thereafter accessed in shutdown.
        boost::scoped_ptr<boost::thread> _syncSourceFeedbackThread;                       // (I)

        // Thread that drives actions in the topology coordinator
        // Set in startReplication() and thereafter accessed in shutdown.
        boost::scoped_ptr<boost::thread> _topCoordDriverThread;                           // (I)

        // Our RID, used to identify us to our sync source when sending replication progress
        // updates upstream.  Set once in startReplication() and then never modified again.
        OID _myRID;                                                                       // (M)

        // Rollback ID. Used to check if a rollback happened during some interval of time
        // TODO: ideally this should only change on rollbacks NOT on mongod restarts also.
        int _rbid;                                                                        // (M)

        // list of information about clients waiting on replication.  Does *not* own the
        // WaiterInfos.
        std::vector<WaiterInfo*> _replicationWaiterList;                                  // (M)

        // Set to true when we are in the process of shutting down replication.
        bool _inShutdown;                                                                 // (M)

        // Election ID of the last election that resulted in this node becoming primary.
        OID _electionID;                                                                  // (M)

        // Maps nodes in this replication group to information known about it such as its
        // replication progress and its ID in the replica set config.
        SlaveInfoMap _slaveInfoMap;                                                       // (M)

        // Current ReplicaSet state.
        MemberState _currentState;                                                        // (M)

        // Used to signal threads waiting for changes to _rsConfigState.
        boost::condition_variable _rsConfigStateChange;                                   // (M)

        // Represents the configuration state of the coordinator, which controls how and when
        // _rsConfig may change.  See the state transition diagram in the type definition of
        // ConfigState for details.
        ConfigState _rsConfigState;                                                       // (M)

        // The current ReplicaSet configuration object, including the information about tag groups
        // that is used to satisfy write concern requests with named gle modes.
        ReplicaSetConfig _rsConfig;                                                       // (MX)

        // This member's index position in the current config.
        int _thisMembersConfigIndex;                                                      // (MX)

        // Used for conducting an election of this node;
        // the presence of a non-null _freshnessChecker pointer indicates that an election is
        // currently in progress.  Only one election is allowed at once.
        boost::scoped_ptr<FreshnessChecker> _freshnessChecker;                            // (X)
        boost::scoped_ptr<ElectCmdRunner> _electCmdRunner;                                // (X)

        // Whether we slept last time we attempted an election but possibly tied with other nodes.
        bool _sleptLastElection;                                                          // (X)
    };

} // namespace repl
} // namespace mongo
