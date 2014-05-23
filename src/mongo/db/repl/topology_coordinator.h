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
#include "mongo/util/net/hostandport.h"

namespace mongo {

    class OpTime;

namespace repl {

    class HeartbeatInfo;
    class Member;
    struct MemberState;

    typedef int Callback_t; // TBD


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
        
        // The optime of the last op actually applied to the data
        virtual void setLastApplied(const OpTime& optime) = 0;
        // The optime of the last op marked as committed by the leader
        virtual void setCommitOkayThrough(const OpTime& optime) = 0;
        // The optime of the last op received over the network from the sync source
        virtual void setLastReceived(const OpTime& optime) = 0;

        // The amount of time this node delays applying ops when acting as a secondary
        virtual int getSelfSlaveDelay() const = 0;
        // Flag determining if chaining secondaries is allowed
        virtual bool getChainingAllowedFlag() const = 0;

        // For use with w:majority write concern
        virtual int getMajorityNumber() const = 0;

        // ReplCoord needs to know the state to implement certain public functions
        virtual MemberState getMemberState() const = 0;

        // Looks up _syncSource's address and returns it, for use by the Applier
        virtual HostAndPort getSyncSourceAddress() const = 0;
        // Chooses and sets a new sync source, based on our current knowledge of the world
        virtual void chooseNewSyncSource() = 0; // this is basically getMemberToSyncTo()
        // Do not choose a member as a sync source for a while; 
        // call this when we have reason to believe it's a bad choice (do we need this?)
        // (currently handled by _veto in rs_initialsync)
        virtual void blacklistSyncSource(Member* member) = 0;

        // Add function pointer to callback list; call function when config changes
        // Applier needs to know when things like chainingAllowed or slaveDelay change. 
        // ReplCoord needs to know when things like the tag sets change.
        virtual void registerConfigChangeCallback(Callback_t) = 0;

        // Applier calls this to notify that it's now safe to transition from SECONDARY to PRIMARY
        virtual void signalDrainComplete() = 0;

        // election entry point
        virtual void electSelf() = 0;

        // produce a reply to a RAFT-style RequestVote RPC
        virtual bool prepareRequestVoteResponse(const BSONObj& cmdObj, 
                                                std::string& errmsg, 
                                                BSONObjBuilder& result) = 0; 

        // produce a reply to a received electCmd
        virtual void prepareElectCmdResponse(const BSONObj& cmdObj, BSONObjBuilder& result) = 0;

        // produce a reply to a heartbeat
        virtual bool prepareHeartbeatResponse(const BSONObj& cmdObj, 
                                              std::string& errmsg, 
                                              BSONObjBuilder& result) = 0;

        // update internal state with heartbeat response
        virtual void updateHeartbeatInfo(const HeartbeatInfo& newInfo) = 0;
    protected:
        TopologyCoordinator() {}
    };
} // namespace repl
} // namespace mongo
