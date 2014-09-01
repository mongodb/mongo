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
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_coordinator.h"
#include "mongo/db/repl/repl_coordinator_external_state.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/platform/random.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

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

        virtual ReplicationCoordinator::StatusAndDuration awaitReplicationOfLastOp(
                const OperationContext* txn,
                const WriteConcernOptions& writeConcern);

        virtual Status stepDown(OperationContext* txn,
                                bool force,
                                const Milliseconds& waitTime,
                                const Milliseconds& stepdownTime);

        virtual Status stepDownAndWaitForSecondary(OperationContext* txn,
                                                   const Milliseconds& initialWaitTime,
                                                   const Milliseconds& stepdownTime,
                                                   const Milliseconds& postStepdownWaitTime);

        virtual bool isMasterForReportingPurposes();

        virtual bool canAcceptWritesForDatabase(const StringData& dbName);

        virtual Status checkIfWriteConcernCanBeSatisfied(
                const WriteConcernOptions& writeConcern) const;

        virtual Status canServeReadsFor(OperationContext* txn,
                                        const NamespaceString& ns,
                                        bool slaveOk);

        virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx);

        virtual Status setLastOptime(OperationContext* txn, const OID& rid, const OpTime& ts);

        virtual OID getElectionId();

        virtual OID getMyRID(OperationContext* txn);

        virtual void prepareReplSetUpdatePositionCommand(OperationContext* txn,
                                                         BSONObjBuilder* cmdBuilder);

        virtual void prepareReplSetUpdatePositionCommandHandshakes(
                OperationContext* txn,
                std::vector<BSONObj>* handshakes);

        virtual Status processReplSetGetStatus(BSONObjBuilder* result);

        virtual void processReplSetGetConfig(BSONObjBuilder* result);

        virtual bool setMaintenanceMode(OperationContext* txn, bool activate);

        virtual Status processReplSetMaintenance(OperationContext* txn,
                                                 bool activate,
                                                 BSONObjBuilder* resultObj);

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

        virtual void waitUpToOneSecondForOptimeChange(const OpTime& ot);

        virtual bool buildsIndexes();

        virtual std::vector<BSONObj> getHostsWrittenTo(const OpTime& op);

        virtual BSONObj getGetLastErrorDefault();

        virtual Status checkReplEnabledForCommand(BSONObjBuilder* result);

        virtual bool isReplEnabled() const;

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

        // ================== Test support API ===================

        /**
         * If called after startReplication(), blocks until all asynchronous
         * activities associated with replication start-up complete.
         */
        void waitForStartUpComplete();

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

        /*
         * Returns the OpTime of the last applied operation on this node.
         */
        OpTime _getLastOpApplied();
        OpTime _getLastOpApplied_inlock();

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

        // Handles to actively queued heartbeats.
        // Only accessed serially in ReplicationExecutor callbacks, which makes it safe to access
        // outside of _mutex.
        HeartbeatHandles _heartbeatHandles;

        // Parsed command line arguments related to replication.  Set once at startup and then
        // never modified again, which makes it safe to read outside of _mutex.
        // TODO(spencer): Currently this actually is not true, there is global mutable state
        // in ReplSettings, but we should be able to get rid of that after the legacy repl
        // coordinator is gone. At that point we can make this const.
        ReplSettings _settings;

        // Our RID, used to identify us to our sync source when sending replication progress
        // updates upstream.  Set once at startup and then never modified again, which makes it
        // safe to read outside of _mutex.
        // TODO(spencer): put behind _mutex
        OID _myRID;

        // Pointer to the TopologyCoordinator owned by this ReplicationCoordinator.
        boost::scoped_ptr<TopologyCoordinator> _topCoord;

        // Executor that drives the topology coordinator.
        ReplicationExecutor _replExecutor;

        // Pointer to the ReplicationCoordinatorExternalState owned by this ReplicationCoordinator.
        boost::scoped_ptr<ReplicationCoordinatorExternalState> _externalState;

        // Thread that _syncSourceFeedback runs in to send replSetUpdatePosition commands upstream.
        boost::scoped_ptr<boost::thread> _syncSourceFeedbackThread;

        // Thread that drives actions in the topology coordinator
        boost::scoped_ptr<boost::thread> _topCoordDriverThread;

        // Protects member data of this ReplicationCoordinator.
        mutable boost::mutex _mutex;

        /// ============= All members below this line are guarded by _mutex ==================== ///

        // Rollback ID. Used to check if a rollback happened during some interval of time
        // TODO: ideally this should only change on rollbacks NOT on mongod restarts also.
        int _rbid;

        // list of information about clients waiting on replication.  Does *not* own the
        // WaiterInfos.
        std::vector<WaiterInfo*> _replicationWaiterList;

        // Set to true when we are in the process of shutting down replication.
        bool _inShutdown;

        // Election ID of the last election that resulted in this node becoming primary.
        OID _electionID;

        // Maps nodes in this replication group to information known about it such as its
        // replication progress and its ID in the replica set config.
        SlaveInfoMap _slaveInfoMap;

        // Current ReplicaSet state.
        MemberState _currentState;

        // Used to signal threads waiting for changes to _rsConfigState.
        boost::condition_variable _rsConfigStateChange;

        // Represents the configuration state of the coordinator, which controls how and when
        // _rsConfig may change.  See the state transition diagram in the type definition of
        // ConfigState for details.
        ConfigState _rsConfigState;

        // The current ReplicaSet configuration object, including the information about tag groups
        // that is used to satisfy write concern requests with named gle modes.
        ReplicaSetConfig _rsConfig;

        // This member's index position in the current config.
        int _thisMembersConfigIndex;

        // PRNG; seeded at class construction time.
        PseudoRandom _random;

    };

} // namespace repl
} // namespace mongo
