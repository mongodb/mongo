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
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

    class OperationContext;
    class OpTime;

namespace repl {

    class HeartbeatInfo;
    class Member;
    struct MemberState;
    class TagSubgroup;

    /**
     * Actions taken based on heartbeat responses
     */
    enum HeartbeatResultAction {
        StepDown,
        StartElection,
        None
    };

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

        // TODO(spencer): Move this and ReplicaSetConfig out of TopologyCoordinator
        class MemberConfig {
        public:
        MemberConfig() :
            _id(-1),
                votes(1),
                priority(1.0),
                arbiterOnly(false),
                slaveDelay(0),
                hidden(false),
                buildIndexes(true) { }
            int _id;              /* ordinal */
            unsigned votes;       /* how many votes this node gets. default 1. */
            HostAndPort h;
            double priority;      /* 0 means can never be primary */
            bool arbiterOnly;
            int slaveDelay;       /* seconds.  */
            bool hidden;          /* if set, don't advertise to drivers in isMaster. */
                                  /* for non-primaries (priority 0) */
            bool buildIndexes;    /* if false, do not create any non-_id indexes */
            std::map<std::string,std::string> tags;     /* tagging for data center, rack, etc. */
        private:
            std::set<TagSubgroup*> _groups; // the subgroups this member belongs to
        };

        struct ReplicaSetConfig {
            // TODO(spencer): make sure it is safe to copy this
            std::vector<MemberConfig> members;
            std::string replSetName;
            int version;
            MemberConfig* self;

            /**
             * If replication can be chained. If chaining is disallowed, it can still be explicitly
             * enabled via the replSetSyncFrom command, but it will not happen automatically.
             */
            bool chainingAllowed;

            // Number of nodes needed for w:majority writes
            int majorityNumber;

            BSONObj asBson() const;

            // Calculate majority number based on current config and store in majorityNumber;
            // done as part of reconfig.
            void calculateMajorityNumber();
        };

        virtual ~TopologyCoordinator() {}
        
        // The optime of the last op actually applied to the data
        virtual void setLastApplied(const OpTime& optime) = 0;
        // The optime of the last op marked as committed by the leader
        virtual void setCommitOkayThrough(const OpTime& optime) = 0;
        // The optime of the last op received over the network from the sync source
        virtual void setLastReceived(const OpTime& optime) = 0;

        // Looks up _syncSource's address and returns it, for use by the Applier
        virtual HostAndPort getSyncSourceAddress() const = 0;
        // Chooses and sets a new sync source, based on our current knowledge of the world
        virtual void chooseNewSyncSource(Date_t now) = 0; // this is basically getMemberToSyncTo()
        // Do not choose a member as a sync source until time given; 
        // call this when we have reason to believe it's a bad choice
        virtual void blacklistSyncSource(const HostAndPort& host, Date_t until) = 0;

        // Add function pointer to callback list; call function when config changes
        // Applier needs to know when things like chainingAllowed or slaveDelay change. 
        // ReplCoord needs to know when things like the tag sets change.
        typedef stdx::function<void (const ReplicaSetConfig& config)> ConfigChangeCallbackFn;
        virtual void registerConfigChangeCallback(const ConfigChangeCallbackFn& fn) = 0;
        // ReplCoord needs to know the state to implement certain public functions
        typedef stdx::function<void (const MemberState& newMemberState)> StateChangeCallbackFn;
        virtual void registerStateChangeCallback(const StateChangeCallbackFn& fn) = 0;

        // Applier calls this to notify that it's now safe to transition from SECONDARY to PRIMARY
        virtual void signalDrainComplete() = 0;

        // produce a reply to a RAFT-style RequestVote RPC
        virtual void prepareRequestVoteResponse(const Date_t now,
                                                const BSONObj& cmdObj,
                                                std::string& errmsg, 
                                                BSONObjBuilder& result) = 0; 

        // produce a reply to a received electCmd
        virtual void prepareElectCmdResponse(const Date_t now,
                                             const BSONObj& cmdObj,
                                             BSONObjBuilder& result) = 0;

        // produce a reply to a heartbeat
        virtual void prepareHeartbeatResponse(const ReplicationExecutor::CallbackData& data,
                                              Date_t now,
                                              const BSONObj& cmdObj, 
                                              BSONObjBuilder* resultObj,
                                              Status* result) = 0;

        // update internal state with heartbeat response
        virtual HeartbeatResultAction updateHeartbeatInfo(Date_t now,
                                                          const HeartbeatInfo& newInfo) = 0;

        // produce a reply to a status request
        virtual void prepareStatusResponse(Date_t now,
                                           const BSONObj& cmdObj,
                                           BSONObjBuilder& result,
                                           unsigned uptime) = 0;

        // produce a reply to a freeze request
        virtual void prepareFreezeResponse(Date_t now,
                                           const BSONObj& cmdObj,
                                           BSONObjBuilder& result) = 0;

        // transition PRIMARY to SECONDARY; caller must already be holding an appropriate dblock
        virtual void relinquishPrimary(OperationContext* txn) = 0;

        // called with new config; notifies all on change
        virtual void updateConfig(const ReplicaSetConfig newConfig, const int selfId) = 0;

    protected:
        TopologyCoordinator() {}
    };
} // namespace repl
} // namespace mongo
