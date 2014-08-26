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
#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_coordinator.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/util/concurrency/list.h"
#include "mongo/util/time_support.h"

namespace mongo {

    class OperationContext;

namespace repl {

    /**
     * Represents a latency measurement for each replica set member based on heartbeat requests.
     * The measurement is an average weighted 80% to the old value, and 20% to the new value.
     */
    class PingStats {
    public:
        PingStats();

        void hit(int millis);

        unsigned int getCount() {
            return count;
        }

        unsigned int getMillis() {
            return value;
        }
    private:
        unsigned int count;
        unsigned int value;
    };

    class TopologyCoordinatorImpl : public TopologyCoordinator {
    public:
        explicit TopologyCoordinatorImpl(Seconds maxSyncSourceLagSecs);

        // TODO(spencer): Can this be made private?
        virtual void setForceSyncSourceIndex(int index);

        // Looks up syncSource's address and returns it, for use by the Applier
        virtual HostAndPort getSyncSourceAddress() const;
        // Chooses and sets a new sync source, based on our current knowledge of the world
        virtual void chooseNewSyncSource(Date_t now, const OpTime& lastOpApplied);
        // Does not choose a member as a sync source until time given; 
        // call this when we have reason to believe it's a bad choice
        virtual void blacklistSyncSource(const HostAndPort& host, Date_t until);

        // Adds function pointer to callback list; calls function when config changes
        // Applier needs to know when things like chainingAllowed or slaveDelay change. 
        // ReplCoord needs to know when things like the tag sets change.
        virtual void registerConfigChangeCallback(const ConfigChangeCallbackFn& fn);
        // ReplCoord needs to know the state to implement certain public functions
        virtual void registerStateChangeCallback(const StateChangeCallbackFn& fn);
        
        // Applier calls this to notify that it's now safe to transition from SECONDARY to PRIMARY
        virtual void signalDrainComplete();

        // produces a reply to a replSetSyncFrom command
        virtual void prepareSyncFromResponse(const ReplicationExecutor::CallbackData& data,
                                             const HostAndPort& target,
                                             const OpTime& lastOpApplied,
                                             BSONObjBuilder* response,
                                             Status* result);

        virtual void prepareFreshResponse(const ReplicationExecutor::CallbackData& data,
                                          const ReplicationCoordinator::ReplSetFreshArgs& args,
                                          const OpTime& lastOpApplied,
                                          BSONObjBuilder* response,
                                          Status* result);

        // produces a reply to a received electCmd
        virtual void prepareElectResponse(const ReplicationExecutor::CallbackData& data,
                                          const ReplicationCoordinator::ReplSetElectArgs& args,
                                          const Date_t now,
                                          BSONObjBuilder* response);

        // produces a reply to a heartbeat
        virtual void prepareHeartbeatResponse(const ReplicationExecutor::CallbackData& data,
                                              Date_t now,
                                              const ReplSetHeartbeatArgs& args,
                                              const std::string& ourSetName,
                                              const OpTime& lastOpApplied,
                                              ReplSetHeartbeatResponse* response,
                                              Status* result);

        // updates internal state with heartbeat response
        ReplSetHeartbeatResponse::HeartbeatResultAction 
            updateHeartbeatData(Date_t now,
                                const MemberHeartbeatData& newInfo,
                                int id,
                                const OpTime& lastOpApplied);

        // produces a reply to a status request
        virtual void prepareStatusResponse(const ReplicationExecutor::CallbackData& data,
                                           Date_t now,
                                           unsigned uptime,
                                           const OpTime& lastOpApplied,
                                           BSONObjBuilder* response,
                                           Status* result);

        // produces a reply to a freeze request
        virtual void prepareFreezeResponse(const ReplicationExecutor::CallbackData& data,
                                           Date_t now,
                                           int secs,
                                           BSONObjBuilder* response,
                                           Status* result);

        // transitions PRIMARY to SECONDARY; caller must already be holding an appropriate dblock
        virtual void relinquishPrimary(OperationContext* txn);

        // updates internal config with new config (already validated)
        virtual void updateConfig(const ReplicaSetConfig& newConfig,
                                  int selfIndex,
                                  Date_t now,
                                  const OpTime& lastOpApplied);

        // Record a ping in millis based on the round-trip time of the heartbeat for the member
        virtual void recordPing(const HostAndPort& host, const Milliseconds elapsedMillis);

        // Changes _memberState to newMemberState, then calls all registered callbacks
        // for state changes.
        // NOTE: The only reason this method is public instead of private is for testing.  Do not
        // call this method from outside of TopologyCoordinatorImpl or a unit test.
        void _changeMemberState(const MemberState& newMemberState);

        // Sets "_electionTime" to "newElectionTime".
        // NOTE: The only reason this method exists is for testing.  Do not call this method from
        //       outside of TopologyCoordinatorImpl or a unit test.
        void _setElectionTime(const OpTime& newElectionTime);

        // Sets _currentPrimaryIndex to the given index.  Should only be used in unit tests!
        // TODO(spencer): Remove this once we can easily call for an election in unit tests to
        // set the current primary.
        void _setCurrentPrimaryForTest(int primaryIndex);

    private:

        enum UnelectableReason {
            None,
            CannotSeeMajority,
            NotCloseEnoughToLatestOptime,
            ArbiterIAm,
            NotSecondary,
            NoPriority,
            StepDownPeriodActive
        };

        // Returns the number of heartbeat pings which have occurred.
        virtual int _getTotalPings();

        // Returns the current "ping" value for the given member by their address
        virtual int _getPing(const HostAndPort& host);

        // Determines if we will veto the member specified by "memberID", given that the last op
        // we have applied locally is "lastOpApplied".
        // If we veto, the errmsg will be filled in with a reason
        bool _shouldVetoMember(unsigned int memberID,
                               const OpTime& lastOpApplied,
                               std::string* errmsg) const;

        // Returns the index of the member with the matching id, or -1 if none match.
        int _getMemberIndex(int id) const; 

        // Sees if a majority number of votes are held by members who are currently "up"
        bool _aMajoritySeemsToBeUp() const;

        // Is optime close enough to the latest known optime to qualify for an election
        bool _isOpTimeCloseEnoughToLatestToElect(const OpTime lastApplied) const;

        // Returns reason why "self" member is unelectable
        UnelectableReason _getMyUnelectableReason(const Date_t now,
                                                  const OpTime lastOpApplied) const;

        // Returns reason why memberIndex is unelectable
        UnelectableReason _getUnelectableReason(int memberIndex) const;

        // Returns the nice text of why the node is unelectable
        std::string _getUnelectableReasonString(UnelectableReason ur) const;

        // Return true if we are currently primary
        bool _iAmPrimary() const;

        // Returns the total number of votes in the current config
        int _totalVotes() const;

        // Scans through all members that are 'up' and return the latest known optime
        OpTime _latestKnownOpTime() const;

        // Begins election proceedings
        void _electSelf(Date_t now);

        // Scans the electable set and returns the highest priority member index
        int _getHighestPriorityElectableIndex() const;

        // Returns true if "one" member is higher priority than "two" member
        bool _isMemberHigherPriority(int memberOneIndex, int memberTwoIndex) const;

        // Helper shortcut to self config
        const MemberConfig& _selfConfig() const;

        // Returns NULL if there is no primary, or the MemberConfig* for the current primary
        const MemberConfig* _currentPrimaryMember() const;

        // Our current state (PRIMARY, SECONDARY, etc)
        MemberState _memberState;
        
        // This is a unique id that is generated and set each time we transition to PRIMARY, as the
        // result of an election.
        OID _electionId;
        // The time at which the current PRIMARY was elected.
        OpTime _electionTime;

        // the member we currently believe is primary, if one exists
        int _currentPrimaryIndex;
        // the member we are currently syncing from
        // -1 if no sync source (we are primary, or we cannot connect to anyone yet)
        int _syncSourceIndex; 
        // These members are not chosen as sync sources for a period of time, due to connection
        // issues with them
        std::map<HostAndPort, Date_t> _syncSourceBlacklist;
        // The next sync source to be chosen, requested via a replSetSyncFrom command
        int _forceSyncSourceIndex;
        // How far this node must fall behind before considering switching sync sources
        Seconds _maxSyncSourceLagSecs;

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

        int _selfIndex; // this node's index in _members and _currentConfig

        ReplicaSetConfig _currentConfig; // The current config, including a vector of MemberConfigs
        // heartbeat data for each member.  It is guaranteed that this vector will be maintained
        // in the same order as the MemberConfigs in _currentConfig, therefore the member config
        // index can be used to index into this vector as well.
        std::vector<MemberHeartbeatData> _hbdata;

        // Time when stepDown command expires
        Date_t _stepDownUntil;

        // Block syncing -- in case we fail auth when heartbeating other nodes
        bool _blockSync;

        // The number of calls we have had to enter maintenance mode
        int _maintenanceModeCalls;


        // Functions to call when a reconfig is finished.  We pass the new config object.
        std::vector<ConfigChangeCallbackFn> _configChangeCallbacks;

        // Functions to call when a state change happens.  We pass the new state.
        std::vector<StateChangeCallbackFn> _stateChangeCallbacks;

        typedef std::map<HostAndPort, PingStats> PingMap;
        // Ping stats for each member by HostAndPort;
        PingMap _pings;

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

        // Last vote info from the election
        struct LastVote {
            LastVote() : when(0), whoID(0xffffffff) { }
            Date_t when;
            unsigned whoID;
            HostAndPort whoHostAndPort;
        } _lastVote;


    };

} // namespace repl
} // namespace mongo
