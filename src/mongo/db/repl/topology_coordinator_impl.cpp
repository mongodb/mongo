/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/db/repl/topology_coordinator_impl.h"

#include "mongo/db/repl/member.h"
#include "mongo/db/repl/rs_sync.h" // maxSyncSourceLagSecs

namespace mongo {
namespace replset {

    TopologyCoordinatorImpl::TopologyCoordinatorImpl() :
        _majorityNumber(0) {
    };

    void TopologyCoordinatorImpl::setLastApplied(const OpTime& optime) {
        _lastApplied = optime;
    }

    void TopologyCoordinatorImpl::setCommitOkayThrough(const OpTime& optime) {
        _commitOkayThrough = optime;
    }

    void TopologyCoordinatorImpl::setLastReceived(const OpTime& optime) {
        _lastReceived = optime;
    }

    int TopologyCoordinatorImpl::getSelfSlaveDelay() const {
        invariant(_currentConfig.self);
        return _currentConfig.self->slaveDelay;
    }

    bool TopologyCoordinatorImpl::getChainingAllowedFlag() const {
        return _currentConfig.chainingAllowed;
    }    

    int TopologyCoordinatorImpl::getMajorityNumber() const {
        return _majorityNumber;
    };
    
    MemberState TopologyCoordinatorImpl::getMemberState() const {
        // TODO
        return MemberState();
    }

    void TopologyCoordinatorImpl::_calculateMajorityNumber() {
        int total = _currentConfig.members.size();
        int nonArbiters = total;
        int strictMajority = total/2+1;

        for (std::vector<MemberConfig>::iterator it = _currentConfig.members.begin(); 
             it < _currentConfig.members.end(); 
             it++) {
            if ((*it).arbiterOnly) {
                nonArbiters--;
            }
        }

        // majority should be all "normal" members if we have something like 4
        // arbiters & 3 normal members
        _majorityNumber = (strictMajority > nonArbiters) ? nonArbiters : strictMajority;
 
    }

    HostAndPort TopologyCoordinatorImpl::getSyncSourceAddress() const {
        return _syncSource->h();
    }

    void TopologyCoordinatorImpl::chooseNewSyncSource() {
//    const Member* ReplSetImpl::getMemberToSyncTo() {
//        lock lk(this);

        // if we have a target we've requested to sync from, use it

/*
  This should be a HostAndPort.
*/
// TODO
/*
        if (_forceSyncTarget) {
            Member* target = _forceSyncTarget;
            _forceSyncTarget = 0;
            sethbmsg( str::stream() << "syncing to: " << target->fullName() << " by request", 0);
            return target;
        }
*/

        // wait for 2N pings before choosing a sync target
        int needMorePings = _currentConfig.members.size()*2 - HeartbeatInfo::numPings;

        if (needMorePings > 0) {
            OCCASIONALLY log() << "waiting for " << needMorePings 
                               << " pings from other members before syncing";
            return;
        }

        // If we are only allowed to sync from the primary, set that
        if (!_currentConfig.chainingAllowed) {
            // Sets NULL if we cannot reach the primary
            _syncSource = _currentPrimary;
        }

        // find the member with the lowest ping time that has more data than me

        // Find primary's oplog time. Reject sync candidates that are more than
        // maxSyncSourceLagSecs seconds behind.
        OpTime primaryOpTime;
        if (_currentPrimary)
            primaryOpTime = _currentPrimary->hbinfo().opTime;
        else
            // choose a time that will exclude no candidates, since we don't see a primary
            primaryOpTime = OpTime(maxSyncSourceLagSecs, 0);

        if (primaryOpTime.getSecs() < static_cast<unsigned int>(maxSyncSourceLagSecs)) {
            // erh - I think this means there was just a new election
            // and we don't yet know the new primary's optime
            primaryOpTime = OpTime(maxSyncSourceLagSecs, 0);
        }

        OpTime oldestSyncOpTime(primaryOpTime.getSecs() - maxSyncSourceLagSecs, 0);

        Member *closest = 0;
        time_t now = 0;

        // Make two attempts.  The first attempt, we ignore those nodes with
        // slave delay higher than our own.  The second attempt includes such
        // nodes, in case those are the only ones we can reach.
        // This loop attempts to set 'closest'.
        for (int attempts = 0; attempts < 2; ++attempts) {
            for (Member *m = _otherMembers.head(); m; m = m->next()) {
                if (!m->syncable())
                    continue;

                if (m->state() == MemberState::RS_SECONDARY) {
                    // only consider secondaries that are ahead of where we are
                    if (m->hbinfo().opTime <= _lastApplied)
                        continue;
                    // omit secondaries that are excessively behind, on the first attempt at least.
                    if (attempts == 0 &&
                        m->hbinfo().opTime < oldestSyncOpTime)
                        continue;
                }

                // omit nodes that are more latent than anything we've already considered
                if (closest &&
                    (m->hbinfo().ping > closest->hbinfo().ping))
                    continue;

                if (attempts == 0 &&
                    (_currentConfig.self->slaveDelay < m->config().slaveDelay 
                     || m->config().hidden)) {
                    continue; // skip this one in the first attempt
                }

                map<string,time_t>::iterator vetoed = _syncSourceBlacklist.find(m->fullName());
                if (vetoed != _syncSourceBlacklist.end()) {
                    // Do some veto housekeeping
                    if (now == 0) {
                        now = time(0);
                    }

                    // if this was on the veto list, check if it was vetoed in the last "while".
                    // if it was, skip.
                    if (vetoed->second >= now) {
                        if (time(0) % 5 == 0) {
                            log() << "replSet not trying to sync from " << (*vetoed).first
                                  << ", it is vetoed for " << ((*vetoed).second - now) 
                                  << " more seconds" << rsLog;
                        }
                        continue;
                    }
                    _syncSourceBlacklist.erase(vetoed);
                    // fall through, this is a valid candidate now
                }
                // This candidate has passed all tests; set 'closest'
                closest = m;
            }
            if (closest) break; // no need for second attempt
        }

        if (!closest) {
            return;
        }

        sethbmsg( str::stream() << "syncing to: " << closest->fullName(), 0);
        _syncSource = closest;
    }
    
    void TopologyCoordinatorImpl::blacklistSyncSource(Member* member) {
        // TODO
        
    }

    void TopologyCoordinatorImpl::registerConfigChangeCallback(Callback_t) {
        // TODO
        
    }
        
    // Applier calls this to notify that it's now safe to transition from SECONDARY to PRIMARY
    void TopologyCoordinatorImpl::signalDrainComplete()  {
        // TODO
        
    }

    // election entry point
    void TopologyCoordinatorImpl::electSelf() {
        // TODO
        
    }

    // produce a reply to a RAFT-style RequestVote RPC; this is MongoDB ReplSetFresh command
    bool TopologyCoordinatorImpl::prepareRequestVoteResponse(const BSONObj& cmdObj, 
                                                         std::string& errmsg, 
                                                         BSONObjBuilder& result) {
        // TODO
        return false;
    }

    // produce a reply to a recevied electCmd
    void TopologyCoordinatorImpl::prepareElectCmdResponse(const BSONObj& cmdObj, 
                                                      BSONObjBuilder& result) {
        // TODO
        
    }

    // produce a reply to a heartbeat
    bool TopologyCoordinatorImpl::prepareHeartbeatResponse(const BSONObj& cmdObj, 
                                                       std::string& errmsg, 
                                                       BSONObjBuilder& result) {
        // TODO
        return false;
    }

    // update internal state with heartbeat response
    void TopologyCoordinatorImpl::updateHeartbeatInfo(const HeartbeatInfo& newInfo) {
        // TODO
        
    }


} // namespace replset
} // namespace mongo
