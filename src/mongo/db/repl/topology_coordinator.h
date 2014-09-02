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

    struct MemberState;
    class ReplicaSetConfig;
    class ReplSetHeartbeatArgs;
    class TagSubgroup;

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

        /**
         * Description of actions taken in response to a heartbeat.
         *
         * This includes when to schedule the next heartbeat to a target, and any other actions to
         * take, such as scheduling an election or stepping down as primary.
         */
        class HeartbeatResponseAction {
        public:
            /**
             * Actions taken based on heartbeat responses
             */
            enum Action {
                NoAction,
                Reconfig,
                StartElection,
                StepDownSelf,
                StepDownRemotePrimary
            };

            /**
             * Makes a new action representing doing nothing.
             */
            static HeartbeatResponseAction makeNoAction();

            /**
             * Makes a new action representing the instruction to reconfigure the current node.
             */
            static HeartbeatResponseAction makeReconfigAction();

            /**
             * Makes a new action telling the current node to attempt to elect itself primary.
             */
            static HeartbeatResponseAction makeElectAction();

            /**
             * Makes a new action telling the current node to step down as primary.
             *
             * It is an error to call this with primaryIndex != the index of the current node.
             */
            static HeartbeatResponseAction makeStepDownSelfAction(int primaryIndex);

            /**
             * Makes a new action telling the current node to ask the specified remote node to step
             * down as primary.
             *
             * It is an error to call this with primaryIndex == the index of the current node.
             */
            static HeartbeatResponseAction makeStepDownRemoteAction(int primaryIndex);

            /**
             * Construct an action with unspecified action and a next heartbeat start date in the
             * past.
             */
            HeartbeatResponseAction();

            /**
             * Sets the date at which the next heartbeat should be scheduled.
             */
            void setNextHeartbeatStartDate(Date_t when);

            /**
             * Gets the action type of this action.
             */
            Action getAction() const { return _action; }

            /**
             * Gets the time at which the next heartbeat should be scheduled.  If the
             * time is not in the future, the next heartbeat should be scheduled immediately.
             */
            Date_t getNextHeartbeatStartDate() const { return _nextHeartbeatStartDate; }

            /**
             * If getAction() returns StepDownSelf or StepDownPrimary, this is the index
             * in the current replica set config of the node that ought to step down.
             */
            int getPrimaryConfigIndex() const { return _primaryIndex; }

        private:
            Action _action;
            int _primaryIndex;
            Date_t _nextHeartbeatStartDate;
        };

        virtual ~TopologyCoordinator();

        // The index into the config used when we next choose a sync source
        virtual void setForceSyncSourceIndex(int index) = 0;

        // Looks up _syncSource's address and returns it, for use by the Applier
        virtual HostAndPort getSyncSourceAddress() const = 0;
        // Chooses and sets a new sync source, based on our current knowledge of the world.
        virtual void chooseNewSyncSource(Date_t now, const OpTime& lastOpApplied) = 0;
        // Do not choose a member as a sync source until time given; 
        // call this when we have reason to believe it's a bad choice
        virtual void blacklistSyncSource(const HostAndPort& host, Date_t until) = 0;

        // Add function pointer to callback list; call function when config changes
        // Applier needs to know when things like chainingAllowed or slaveDelay change. 
        // ReplCoord needs to know when things like the tag sets change.
        typedef stdx::function<void (const ReplicaSetConfig& config, int myIndex)>
                ConfigChangeCallbackFn;
        virtual void registerConfigChangeCallback(const ConfigChangeCallbackFn& fn) = 0;
        // ReplCoord needs to know the state to implement certain public functions
        typedef stdx::function<void (const MemberState& newMemberState)> StateChangeCallbackFn;
        virtual void registerStateChangeCallback(const StateChangeCallbackFn& fn) = 0;

        // Applier calls this to notify that it's now safe to transition from SECONDARY to PRIMARY
        virtual void signalDrainComplete() = 0;

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
                                          BSONObjBuilder* response) = 0;

        // produce a reply to a heartbeat
        virtual void prepareHeartbeatResponse(const ReplicationExecutor::CallbackData& data,
                                              Date_t now,
                                              const ReplSetHeartbeatArgs& args,
                                              const std::string& ourSetName,
                                              const OpTime& lastOpApplied,
                                              ReplSetHeartbeatResponse* response,
                                              Status* result) = 0;

        /**
         * Prepares a heartbeat request appropriate for sending to "target", assuming the
         * current time is "now".  "ourSetName" is used as the name for our replica set if
         * the topology coordinator does not have a valid configuration installed.
         *
         * The returned pair contains proper arguments for a replSetHeartbeat command, and
         * an amount of time to wait for the response.
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
         */
        virtual HeartbeatResponseAction processHeartbeatResponse(
                Date_t now,
                Milliseconds networkRoundTripTime,
                const HostAndPort& target,
                const StatusWith<ReplSetHeartbeatResponse>& hbResponse,
                OpTime myLastOpApplied) = 0;

        // produce a reply to a status request
        virtual void prepareStatusResponse(const ReplicationExecutor::CallbackData& data,
                                           Date_t now,
                                           unsigned uptime,
                                           const OpTime& lastOpApplied,
                                           BSONObjBuilder* response,
                                           Status* result) = 0;

        // produce a reply to a freeze request
        virtual void prepareFreezeResponse(const ReplicationExecutor::CallbackData& data,
                                           Date_t now,
                                           int secs,
                                           BSONObjBuilder* response,
                                           Status* result) = 0;

        // transition PRIMARY to SECONDARY; caller must already be holding an appropriate dblock
        virtual void relinquishPrimary(OperationContext* txn) = 0;

        // Updates the topology coordinator's notion of the new configuration.
        virtual void updateConfig(const ReplicaSetConfig& newConfig,
                                  int selfIndex,
                                  Date_t now,
                                  const OpTime& lastOpApplied) = 0;

        // Record a "ping" based on the round-trip time of the heartbeat for the member
        virtual void recordPing(const HostAndPort& host, const Milliseconds elapsedMillis) = 0;

        // Retrieves a vector of HostAndPorts containing only nodes that are not DOWN
        // and are not ourselves.
        virtual std::vector<HostAndPort> getMaybeUpHostAndPorts() const = 0;

        // If we can vote for ourselves, updates the lastVote tracker and returns true.
        // If we cannot vote for ourselves (because we already voted too recently), returns false.
        virtual bool voteForMyself(Date_t now) = 0;

        // Sets _stepDownTime to newTime.
        virtual void setStepDownTime(Date_t newTime) = 0;

    protected:
        TopologyCoordinator() {}
    };
} // namespace repl
} // namespace mongo
