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

    class MemberHeartbeatData;
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

        virtual ~TopologyCoordinator() {}
        
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

        // update internal state with heartbeat response corresponding to 'id'
        virtual ReplSetHeartbeatResponse::HeartbeatResultAction 
            updateHeartbeatData(Date_t now,
                                const MemberHeartbeatData& newInfo,
                                int id,
                                const OpTime& lastOpApplied) = 0;

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

    protected:
        TopologyCoordinator() {}
    };
} // namespace repl
} // namespace mongo
