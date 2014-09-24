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

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/repl_coordinator.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

    class OperationContext;
    class OpTime;

namespace repl {

    class HeartbeatResponseAction;
    class ReplSetHeartbeatArgs;
    class ReplicaSetConfig;
    class TagSubgroup;
    struct MemberState;

    /**
     * Replication Topology Coordinator interface.
     *
     * This object is responsible for managing the topology of the cluster.
     * Tasks include consensus and leader election, chaining, and configuration management.
     * Methods of this class should be non-blocking.
     */
    class TopologyCoordinator {
        MONGO_DISALLOW_COPYING(TopologyCoordinator);
    public:
        class Role;

        virtual ~TopologyCoordinator();

        ////////////////////////////////////////////////////////////
        //
        // State inspection methods.
        //
        ////////////////////////////////////////////////////////////

        /**
         * Gets the role of this member in the replication protocol.
         */
        virtual Role getRole() const = 0;

        /**
         * Gets the MemberState of this member in the replica set.
         */
        virtual MemberState getMemberState() const = 0;

        /**
         * Returns the address of the current sync source, or an empty HostAndPort if there is no
         * current sync source.
         */
        virtual HostAndPort getSyncSourceAddress() const = 0;

        /**
         * Retrieves a vector of HostAndPorts containing all nodes that are neither DOWN nor
         * ourself.
         */
        virtual std::vector<HostAndPort> getMaybeUpHostAndPorts() const = 0;

        /**
         * Gets the earliest time the current node will stand for election.
         */
        virtual Date_t getStepDownTime() const = 0;

        /**
         * Gets the current value of the maintenance mode counter.
         */
        virtual int getMaintenanceCount() const = 0;

        ////////////////////////////////////////////////////////////
        //
        // Basic state manipulation methods.
        //
        ////////////////////////////////////////////////////////////

        /**
         * Sets the index into the config used when we next choose a sync source
         */
        virtual void setForceSyncSourceIndex(int index) = 0;

        /**
         * Chooses and sets a new sync source, based on our current knowledge of the world.
         */
        virtual HostAndPort chooseNewSyncSource(Date_t now, const OpTime& lastOpApplied) = 0;

        /**
         * Suppresses selecting "host" as sync source until "until".
         */
        virtual void blacklistSyncSource(const HostAndPort& host, Date_t until) = 0;

        /**
         * Determines if a new sync source should be chosen, if a better candidate sync source is
         * available.  If the current sync source's last optime is more than _maxSyncSourceLagSecs
         * behind any syncable source, this function returns true.
         */
        virtual bool shouldChangeSyncSource(const HostAndPort& currentSource) const = 0;

        /**
         * Sets the earliest time the current node will stand for election to "newTime".
         *
         * Does not affect the node's state or the process of any elections in flight.
         */
        virtual void setStepDownTime(Date_t newTime) = 0;

        /**
         * Sets the reported mode of this node to one of RS_SECONDARY, RS_STARTUP2, RS_ROLLBACK or
         * RS_RECOVERING, when getRole() == Role::follower.  This is the interface by which the
         * applier changes the reported member state of the current node, and enables or suppresses
         * electability of the current node.  All modes but RS_SECONDARY indicate an unelectable
         * follower state (one that cannot transition to candidate).
         */
        virtual void setFollowerMode(MemberState::MS newMode) = 0;

        /**
         * Adjusts the maintenance mode count by "inc".
         *
         * It is an error to call this method if getRole() does not return Role::follower.
         * It is an error to allow the maintenance count to go negative.
         */
        virtual void adjustMaintenanceCountBy(int inc) = 0;

        ////////////////////////////////////////////////////////////
        //
        // Methods that prepare responses to command requests.
        //
        ////////////////////////////////////////////////////////////

        // produces a reply to a replSetSyncFrom command
        virtual void prepareSyncFromResponse(const ReplicationExecutor::CallbackData& data,
                                             const HostAndPort& target,
                                             const OpTime& lastOpApplied,
                                             BSONObjBuilder* response,
                                             Status* result) = 0;

        // produce a reply to a replSetFresh command
        virtual void prepareFreshResponse(const ReplicationExecutor::CallbackData& data,
                                          const ReplicationCoordinator::ReplSetFreshArgs& args,
                                          const OpTime& lastOpApplied,
                                          BSONObjBuilder* response,
                                          Status* result) = 0;

        // produce a reply to a received electCmd
        virtual void prepareElectResponse(const ReplicationExecutor::CallbackData& data,
                                          const ReplicationCoordinator::ReplSetElectArgs& args,
                                          const Date_t now,
                                          BSONObjBuilder* response,
                                          Status* result) = 0;

        // produce a reply to a heartbeat
        virtual void prepareHeartbeatResponse(const ReplicationExecutor::CallbackData& data,
                                              Date_t now,
                                              const ReplSetHeartbeatArgs& args,
                                              const std::string& ourSetName,
                                              const OpTime& lastOpApplied,
                                              ReplSetHeartbeatResponse* response,
                                              Status* result) = 0;

        // produce a reply to a status request
        virtual void prepareStatusResponse(const ReplicationExecutor::CallbackData& data,
                                           Date_t now,
                                           unsigned uptime,
                                           const OpTime& lastOpApplied,
                                           BSONObjBuilder* response,
                                           Status* result) = 0;

        // produce a reply to an ismaster request.  It is only valid to call this if we are a
        // replset.
        virtual void fillIsMasterForReplSet(IsMasterResponse* response) = 0;

        // produce a reply to a freeze request
        virtual void prepareFreezeResponse(const ReplicationExecutor::CallbackData& data,
                                           Date_t now,
                                           int secs,
                                           BSONObjBuilder* response,
                                           Status* result) = 0;

        ////////////////////////////////////////////////////////////
        //
        // Methods for sending and receiving heartbeats,
        // reconfiguring and handling the results of standing for
        // election.
        //
        ////////////////////////////////////////////////////////////

        /**
         * Updates the topology coordinator's notion of the replica set configuration.
         *
         * "newConfig" is the new configuration, and "selfIndex" is the index of this
         * node's configuration information in "newConfig", or "selfIndex" is -1 to
         * indicate that this node is not a member of "newConfig".
         *
         * newConfig.isInitialized() should be true, though implementations may accept
         * configurations where this is not true, for testing purposes.
         */
        virtual void updateConfig(const ReplicaSetConfig& newConfig,
                                  int selfIndex,
                                  Date_t now,
                                  const OpTime& lastOpApplied) = 0;

        /**
         * Prepares a heartbeat request appropriate for sending to "target", assuming the
         * current time is "now".  "ourSetName" is used as the name for our replica set if
         * the topology coordinator does not have a valid configuration installed.
         *
         * The returned pair contains proper arguments for a replSetHeartbeat command, and
         * an amount of time to wait for the response.
         *
         * This call should be paired (with intervening network communication) with a call to
         * processHeartbeatResponse for the same "target".
         */
        virtual std::pair<ReplSetHeartbeatArgs, Milliseconds> prepareHeartbeatRequest(
                Date_t now,
                const std::string& ourSetName,
                const HostAndPort& target) = 0;

        /**
         * Processes a heartbeat response from "target" that arrived around "now", having
         * spent "networkRoundTripTime" millis on the network.
         *
         * Updates internal topology coordinator state, and returns instructions about what action
         * to take next.
         *
         * If the next action indicates StartElection, the topology coordinator has transitioned to
         * the "candidate" role, and will remain there until processWinElection or
         * processLoseElection are called.
         *
         * If the next action indicates "StepDownSelf", the topology coordinator has transitioned
         * to the "follower" role from "leader", and the caller should take any necessary actions
         * to become a follower.
         *
         * If the next action indicates "StepDownRemotePrimary", the caller should take steps to
         * cause the specified remote host to step down from primary to secondary.
         *
         * If the next action indicates "Reconfig", the caller should verify the configuration in
         * hbResponse is acceptable, perform any other reconfiguration actions it must, and call
         * updateConfig with the new configuration and the appropriate value for "selfIndex".  It
         * must also wrap up any outstanding elections (by calling processLoseElection or
         * processWinElection) before calling updateConfig.
         *
         * This call should be paired (with intervening network communication) with a call to
         * prepareHeartbeatRequest for the same "target".
         */
        virtual HeartbeatResponseAction processHeartbeatResponse(
                Date_t now,
                Milliseconds networkRoundTripTime,
                const HostAndPort& target,
                const StatusWith<ReplSetHeartbeatResponse>& hbResponse,
                OpTime myLastOpApplied) = 0;

        /**
         * If getRole() == Role::candidate and this node has not voted too recently, updates the
         * lastVote tracker and returns true.  Otherwise, returns false.
         */
        virtual bool voteForMyself(Date_t now) = 0;

        /**
         * Performs state updates associated with winning an election.
         *
         * It is an error to call this if the topology coordinator is not in candidate mode.
         *
         * Exactly one of either processWinElection or processLoseElection must be called if
         * processHeartbeatResponse returns StartElection, to exit candidate mode.
         */
        virtual void processWinElection(
                Date_t now,
                OID electionId,
                OpTime myLastOpApplied,
                OpTime electionOpTime) = 0;

        /**
         * Performs state updates associated with losing an election.
         *
         * It is an error to call this if the topology coordinator is not in candidate mode.
         *
         * Exactly one of either processWinElection or processLoseElection must be called if
         * processHeartbeatResponse returns StartElection, to exit candidate mode.
         */
        virtual void processLoseElection(Date_t now, OpTime myLastOpApplied) = 0;

        /**
         * Changes the coordinator from the leader role to the follower role.
         */
        virtual void stepDown() = 0;

        ////////////////////////////////////////////////////////////
        //
        // Testing interface
        //
        ////////////////////////////////////////////////////////////
        virtual void changeMemberState_forTest(const MemberState& newState,
                                               OpTime electionTime = OpTime(0,0)) = 0;

    protected:
        TopologyCoordinator() {}
    };

    /**
     * Type that denotes the role of a node in the replication protocol.
     *
     * The role is distinct from MemberState, in that it only deals with the
     * roles a node plays in the basic protocol -- leader, follower and candidate.
     * The mapping between MemberState and Role is complex -- several MemberStates
     * map to the follower role, and MemberState::RS_SECONDARY maps to either
     * follower or candidate roles, e.g.
     */
    class TopologyCoordinator::Role {
    public:
        /**
         * Constant indicating leader role.
         */
        static const Role leader;

        /**
         * Constant indicating follower role.
         */
        static const Role follower;

        /**
         * Constant indicating candidate role
         */
        static const Role candidate;

        Role() {}

        bool operator==(Role other) const { return _value == other._value; }
        bool operator!=(Role other) const { return _value != other._value; }

        std::string toString() const;

    private:
        explicit Role(int value);

        int _value;
    };

} // namespace repl
} // namespace mongo
