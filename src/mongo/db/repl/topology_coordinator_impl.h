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
#include <vector>

#include "mongo/bson/optime.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/util/concurrency/list.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

    class TopologyCoordinatorImpl : public TopologyCoordinator {
    public:

        TopologyCoordinatorImpl();
        virtual ~TopologyCoordinatorImpl() {};
        
        virtual void setLastApplied(const OpTime& optime);
        virtual void setCommitOkayThrough(const OpTime& optime);
        virtual void setLastReceived(const OpTime& optime);

        // Looks up _syncSource's address and returns it, for use by the Applier
        virtual HostAndPort getSyncSourceAddress() const;
        // Chooses and sets a new sync source, based on our current knowledge of the world
        virtual void chooseNewSyncSource(Date_t now); // this is basically getMemberToSyncTo()
        // Do not choose a member as a sync source until time given; 
        // call this when we have reason to believe it's a bad choice
        virtual void blacklistSyncSource(const HostAndPort& host, Date_t until);

        // Add function pointer to callback list; call function when config changes
        // Applier needs to know when things like chainingAllowed or slaveDelay change. 
        // ReplCoord needs to know when things like the tag sets change.
        virtual void registerConfigChangeCallback(const ConfigChangeCallbackFn& fn);
        // ReplCoord needs to know the state to implement certain public functions
        virtual void registerStateChangeCallback(const StateChangeCallbackFn& fn);
        
        // Applier calls this to notify that it's now safe to transition from SECONDARY to PRIMARY
        virtual void signalDrainComplete();

        // produce a reply to a RAFT-style RequestVote RPC; this is MongoDB ReplSetFresh command
        virtual bool prepareRequestVoteResponse(const BSONObj& cmdObj, 
                                                std::string& errmsg, 
                                                BSONObjBuilder& result);

        // produce a reply to a received electCmd
        virtual void prepareElectCmdResponse(const BSONObj& cmdObj, BSONObjBuilder& result);

        // produce a reply to a heartbeat
        virtual void prepareHeartbeatResponse(ReplicationExecutor* executor,
                                              const Status& inStatus,
                                              Date_t now,
                                              const BSONObj& cmdObj, 
                                              BSONObjBuilder* resultObj,
                                              Status* result);

        // update internal state with heartbeat response
        virtual void updateHeartbeatInfo(Date_t now, const HeartbeatInfo& newInfo);

        // transition PRIMARY to SECONDARY; caller must already be holding an appropriate dblock
        virtual void relinquishPrimary();

    private:

        // Logic to determine if we should step down as primary
        bool _shouldRelinquish() const;

        // See if a majority number of votes are held by members who are currently "up"
        bool _aMajoritySeemsToBeUp() const;

        // Return the total number of votes in the current config
        int _totalVotes() const;

        // Scan through all members that are 'up' and return the latest known optime
        OpTime _latestKnownOpTime() const;

        // Begin election proceedings
        void _electSelf(Date_t now);

        // Scans the electable set and returns the highest priority member
        const Member* _getHighestPriorityElectable() const;

        // Change _memberState, if state is different from _memberState.
        // Call all registered callbacks for state changes.
        void _changeMemberState(const MemberState& newMemberState);

        OpTime _lastApplied;  // the last op that the applier has actually written to the data
        OpTime _commitOkayThrough; // the primary's latest op that won't get rolled back
        OpTime _lastReceived; // the last op we have received from our sync source

        // Our current state (PRIMARY, SECONDARY, etc)
        MemberState _memberState;
        
        enum StartupStatus {
            PRESTART=0, LOADINGCONFIG=1, BADCONFIG=2, EMPTYCONFIG=3,
            EMPTYUNREACHABLE=4, STARTED=5, SOON=6
        }  _startupStatus;

        // This is a unique id that is generated and set each time we transition to PRIMARY, as the
        // result of an election.
        OID _electionId;
        // PRIMARY server's time when the election to primary occurred
        OpTime _electionTime;

        // set of electable members' _ids
        // For implementation of priorities
        std::set<unsigned int> _electableSet;

        // the member we currently believe is primary, if one exists
        const Member *_currentPrimary;
        // the member we are currently syncing from
        // NULL if no sync source (we are primary, or we cannot connect to anyone yet)
        const Member* _syncSource; 
        // These members are not chosen as sync sources for a period of time, due to connection
        // issues with them
        std::map<HostAndPort, Date_t> _syncSourceBlacklist;

        // insanity follows

        // "heartbeat message"
        // sent in requestHeartbeat respond in field "hbm"
        char _hbmsg[256]; // we change this unlocked, thus not a std::string
        Date_t _hbmsgTime; // when it was logged
        void _sethbmsg(const std::string& s, int logLevel = 0);
        // heartbeat msg to send to others; descriptive diagnostic info
        std::string _getHbmsg() const {
            if ( time(0)-_hbmsgTime > 120 ) return "";
            return _hbmsg;
        }

        // Flag to prevent re-entering election code
        bool _busyWithElectSelf;

        ReplicaSetConfig _currentConfig;

        // Time when stepDown command expires
        Date_t _stepDownUntil;

        // Block syncing -- in case we fail auth when heartbeating other nodes
        bool _blockSync;


        // Functions to call when a reconfig is finished.  We pass the new config object.
        std::vector<ConfigChangeCallbackFn> _configChangeCallbacks;

        // Functions to call when a state change happens.  We pass the new state.
        std::vector<StateChangeCallbackFn> _stateChangeCallbacks;

        Member* _self;
        List1<Member> _otherMembers; // all members of the set EXCEPT _self.

        Member* _getMutableMember(unsigned int id);
        const Member* _getConstMember(unsigned int id) const;

        // do these need settors?  the current code has no way to change these values.
        struct HeartbeatOptions {
        HeartbeatOptions() :  heartbeatSleepMillis(2000), 
                heartbeatTimeoutMillis(10000),
                heartbeatConnRetries(2) 
            { }
            
            unsigned heartbeatSleepMillis;
            unsigned heartbeatTimeoutMillis;
            unsigned heartbeatConnRetries ;

            void check() {
                uassert(17490, "bad replset heartbeat option", heartbeatSleepMillis >= 10);
                uassert(17491, "bad replset heartbeat option", heartbeatTimeoutMillis >= 10);
            }

            bool operator==(const HeartbeatOptions& r) const {
                return (heartbeatSleepMillis==r.heartbeatSleepMillis && 
                        heartbeatTimeoutMillis==r.heartbeatTimeoutMillis &&
                        heartbeatConnRetries==r.heartbeatConnRetries);
            }
        } _heartbeatOptions;


    };

} // namespace repl
} // namespace mongo
