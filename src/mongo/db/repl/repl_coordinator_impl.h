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
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/optime.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_coordinator.h"
#include "mongo/db/repl/repl_coordinator_external_state.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

    class ReplicationCoordinatorImpl : public ReplicationCoordinator {
        MONGO_DISALLOW_COPYING(ReplicationCoordinatorImpl);

    public:

        // Takes ownership of the passed in ReplicationCoordinatorExternalState.
        ReplicationCoordinatorImpl(const ReplSettings& settings,
                                   ReplicationCoordinatorExternalState* externalState);
        virtual ~ReplicationCoordinatorImpl();

        // ================== Members of public ReplicationCoordinator API ===================

        virtual void startReplication(TopologyCoordinator* topCoord,
                                      ReplicationExecutor::NetworkInterface* network);

        virtual void shutdown();

        virtual bool isShutdownOkay() const;

        virtual ReplSettings& getSettings();

        virtual Mode getReplicationMode() const;

        virtual MemberState getCurrentMemberState() const;

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

        virtual bool canAcceptWritesForDatabase(const StringData& database);

        virtual Status checkIfWriteConcernCanBeSatisfied(
                const WriteConcernOptions& writeConcern) const;

        virtual Status canServeReadsFor(const NamespaceString& ns, bool slaveOk);

        virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx);

        virtual Status setLastOptime(OperationContext* txn, const OID& rid, const OpTime& ts);

        virtual OID getElectionId();

        virtual OID getMyRID(OperationContext* txn);

        virtual void prepareReplSetUpdatePositionCommand(OperationContext* txn,
                                                         BSONObjBuilder* cmdBuilder);

        virtual void prepareReplSetUpdatePositionCommandHandshakes(
                OperationContext* txn,
                std::vector<BSONObj>* handshakes);

        virtual void processReplSetGetStatus(BSONObjBuilder* result);

        virtual void processReplSetGetConfig(BSONObjBuilder* result);

        virtual bool setMaintenanceMode(OperationContext* txn, bool activate);

        virtual Status processReplSetMaintenance(OperationContext* txn,
                                                 bool activate,
                                                 BSONObjBuilder* resultObj);

        virtual Status processReplSetSyncFrom(const std::string& target,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj);

        virtual Status processHeartbeat(const ReplSetHeartbeatArgs& args,
                                        BSONObjBuilder* resultObj);

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
                                                    const BSONArray& updates,
                                                    BSONObjBuilder* resultObj);

        virtual Status processReplSetUpdatePositionHandshake(const OperationContext* txn,
                                                             const BSONObj& handshake,
                                                             BSONObjBuilder* resultObj);

        virtual Status processHandshake(const OperationContext* txn,
                                        const OID& remoteID,
                                        const BSONObj& handshake);

        virtual void waitUpToOneSecondForOptimeChange(const OpTime& ot);

        virtual bool buildsIndexes();

        virtual std::vector<BSONObj> getHostsWrittenTo(const OpTime& op);

        virtual BSONObj getGetLastErrorDefault();

        virtual Status checkReplEnabledForCommand(BSONObjBuilder* result);

        // ================== Members of replication code internal API ===================

        // Called by the TopologyCoordinator whenever this node's replica set state transitions.
        void setCurrentMemberState(const MemberState& newState);

        // Called by the TopologyCoordinator whenever the replica set configuration is updated
        void setCurrentReplicaSetConfig(const ReplicaSetConfig& newConfig, int myIndex);

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

    private:

        // Struct that holds information about clients waiting for replication.
        struct WaiterInfo;

        /*
         * Returns true if the given writeConcern is satisfied up to "optime".
         */
        bool _opReplicatedEnough_inlock(const OpTime& opTime,
                                        const WriteConcernOptions& writeConcern);

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
         * Start a heartbeat for each member in the current config
         */
        void _startHeartbeats();

        // Handles to actively queued heartbeats
        typedef std::vector<ReplicationExecutor::CallbackHandle> HeartbeatHandles;
        HeartbeatHandles _heartbeatHandles;

        // Protects all member data of this ReplicationCoordinator.
        mutable boost::mutex _mutex;

        // list of information about clients waiting on replication.  Does *not* own the
        // WaiterInfos.
        std::vector<WaiterInfo*> _replicationWaiterList;

        // Set to true when we are in the process of shutting down replication.
        bool _inShutdown;

        // Parsed command line arguments related to replication.
        ReplSettings _settings;

        // Our RID, used to identify us to our sync source when sending replication progress
        // updates upstream.  Set once at startup and then never modified again.
        OID _myRID;

        // Pointer to the TopologyCoordinator owned by this ReplicationCoordinator.
        boost::scoped_ptr<TopologyCoordinator> _topCoord;

        // Pointer to the ReplicationExecutor owned by this ReplicationCoordinator.
        boost::scoped_ptr<ReplicationExecutor> _replExecutor;

        // Pointer to the ReplicationCoordinatorExternalState owned by this ReplicationCoordinator.
        boost::scoped_ptr<ReplicationCoordinatorExternalState> _externalState;

        // Thread that drives actions in the topology coordinator
        boost::scoped_ptr<boost::thread> _topCoordDriverThread;

        struct SlaveInfo {
            OpTime opTime; // Our last known OpTime that this slave has replicated to.
            HostAndPort hostAndPort; // Client address of the slave.
            int memberID; // ID of the node in the replica set config, or -1 if we're not a replSet.
            SlaveInfo() : memberID(-1) {}
        };
        // Maps nodes in this replication group to information known about it such as its
        // replication progress and its ID in the replica set config.
        typedef unordered_map<OID, SlaveInfo, OID::Hasher> SlaveInfoMap;
        SlaveInfoMap _slaveInfoMap;

        // Current ReplicaSet state.
        MemberState _currentState;

        // The current ReplicaSet configuration object, including the information about tag groups
        // that is used to satisfy write concern requests with named gle modes.
        ReplicaSetConfig _rsConfig;

        // This member's index position in the current config.
        int _thisMembersConfigIndex;
    };

} // namespace repl
} // namespace mongo
