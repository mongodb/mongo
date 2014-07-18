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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/topology_coordinator_impl.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/new_member.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/rs_sync.h" // maxSyncSourceLagSecs
#include "mongo/util/log.h"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kReplication);

namespace repl {

    TopologyCoordinatorImpl::TopologyCoordinatorImpl() :
        _startupStatus(PRESTART),
        _currentPrimaryIndex(-1),
        _syncSourceIndex(-1),
        _busyWithElectSelf(false),
        _selfIndex(0),
        _blockSync(false),
        _maintenanceModeCalls(0)
    {
    }

    void TopologyCoordinatorImpl::setLastApplied(const OpTime& optime) {
        _lastApplied = optime;
    }

    void TopologyCoordinatorImpl::setCommitOkayThrough(const OpTime& optime) {
        _commitOkayThrough = optime;
    }

    void TopologyCoordinatorImpl::setLastReceived(const OpTime& optime) {
        _lastReceived = optime;
    }

    HostAndPort TopologyCoordinatorImpl::getSyncSourceAddress() const {
        invariant(_syncSourceIndex >= 0);
        return _members[_syncSourceIndex].hap();
    }

    void TopologyCoordinatorImpl::chooseNewSyncSource(Date_t now) {
        // if we have a target we've requested to sync from, use it
/*
  This should be a HostAndPort.
*/
        // XXX Eric
/*
        if (_forceSyncTarget) {
            Member* target = _forceSyncTarget;
            _forceSyncTarget = 0;
            sethbmsg( str::stream() << "syncing to: " << target->fullName() << " by request", 0);
            return target;
        }
*/

        // wait for 2N pings before choosing a sync target
        int needMorePings = _members.size()*2 - HeartbeatInfo::numPings;

        if (needMorePings > 0) {
            OCCASIONALLY log() << "waiting for " << needMorePings 
                               << " pings from other members before syncing";
            return;
        }

        // If we are only allowed to sync from the primary, set that
        if (!_currentConfig.isChainingAllowed()) {
            // Sets NULL if we cannot reach the primary
            _syncSourceIndex = _currentPrimaryIndex;
        }

        // find the member with the lowest ping time that has more data than me

        // Find primary's oplog time. Reject sync candidates that are more than
        // maxSyncSourceLagSecs seconds behind.
        OpTime primaryOpTime;
        if (_currentPrimaryIndex != -1)
            primaryOpTime = _members[_currentPrimaryIndex].hbinfo().opTime;
        else
            // choose a time that will exclude no candidates, since we don't see a primary
            primaryOpTime = OpTime(maxSyncSourceLagSecs, 0);

        if (primaryOpTime.getSecs() < static_cast<unsigned int>(maxSyncSourceLagSecs)) {
            // erh - I think this means there was just a new election
            // and we don't yet know the new primary's optime
            primaryOpTime = OpTime(maxSyncSourceLagSecs, 0);
        }

        OpTime oldestSyncOpTime(primaryOpTime.getSecs() - maxSyncSourceLagSecs, 0);

        int closestIndex = -1;

        // Make two attempts.  The first attempt, we ignore those nodes with
        // slave delay higher than our own.  The second attempt includes such
        // nodes, in case those are the only ones we can reach.
        // This loop attempts to set 'closestIndex'.
        for (int attempts = 0; attempts < 2; ++attempts) {
            for (std::vector<NewMember>::const_iterator it = _members.begin(); 
                 it != _members.end(); 
                 ++it) {
                // Don't consider ourselves.
                if (it->configIndex() == _selfIndex) {
                    continue;
                }
                // Candidate must be up to be considered.
                if (!it->hbinfo().up()) {
                    continue;
                }
                // Candidate must be PRIMARY or SECONDARY state to be considered.
                if (!it->hbinfo().hbstate.readable()) {
                    continue;
                }
                // Candidate must build indexes if we build indexes, to be considered.
                if (_selfConfig().shouldBuildIndexes()) {
                    if (!_currentConfig.getMemberAt(it->configIndex()).shouldBuildIndexes()) {
                        continue;
                    }
                }

                if (it->hbinfo().hbstate == MemberState::RS_SECONDARY) {
                    // only consider secondaries that are ahead of where we are
                    if (it->hbinfo().opTime <= _lastApplied)
                        continue;
                    // omit secondaries that are excessively behind, on the first attempt at least.
                    if (attempts == 0 &&
                        it->hbinfo().opTime < oldestSyncOpTime)
                        continue;
                }

                // omit nodes that are more latent than anything we've already considered
                if ((closestIndex != -1) &&
                    (it->hbinfo().ping > _members[closestIndex].hbinfo().ping))
                    continue;

                if (attempts == 0 &&
                    (_selfConfig().getSlaveDelay() < 
                     _currentConfig.getMemberAt(it->configIndex()).getSlaveDelay()
                     || _currentConfig.getMemberAt(it->configIndex()).isHidden())) {
                    continue; // skip this one in the first attempt
                }

                std::map<HostAndPort,Date_t>::iterator vetoed = 
                    _syncSourceBlacklist.find(it->hap());
                if (vetoed != _syncSourceBlacklist.end()) {
                    // Do some veto housekeeping

                    // if this was on the veto list, check if it was vetoed in the last "while".
                    // if it was, skip.
                    if (vetoed->second >= now) {
                        if (now % 5 == 0) {
                            log() << "replSet not trying to sync from " << (*vetoed).first
                                  << ", it is vetoed for " << ((*vetoed).second - now) 
                                  << " more seconds" << rsLog;
                        }
                        continue;
                    }
                    _syncSourceBlacklist.erase(vetoed);
                    // fall through, this is a valid candidate now
                }
                // This candidate has passed all tests; set 'closestIndex'
                closestIndex = it->configIndex();
            }
            if (closestIndex != -1) break; // no need for second attempt
        }

        if (closestIndex == -1) {
            return;
        }

        sethbmsg( str::stream() << "syncing to: " << _members[closestIndex].fullName(), 0);
        _syncSourceIndex = closestIndex;
    }
    
    void TopologyCoordinatorImpl::blacklistSyncSource(const HostAndPort& host, Date_t until) {
        _syncSourceBlacklist[host] = until;
    }

    void TopologyCoordinatorImpl::registerConfigChangeCallback(const ConfigChangeCallbackFn& fn) {
        _configChangeCallbacks.push_back(fn);
    }

    void TopologyCoordinatorImpl::registerStateChangeCallback(const StateChangeCallbackFn& fn) {
        _stateChangeCallbacks.push_back(fn);
    }
        
    // Applier calls this to notify that it's now safe to transition from SECONDARY to PRIMARY
    void TopologyCoordinatorImpl::signalDrainComplete()  {
        // TODO
        
    }

    void TopologyCoordinatorImpl::relinquishPrimary(OperationContext* txn) {
        LOG(2) << "replSet attempting to relinquish" << endl;
        invariant(txn->lockState()->isWriteLocked());
        if (_memberState != MemberState::RS_PRIMARY) {
            // Already relinquished?
            log() << "replSet warning attempted to relinquish but not primary";
            return;
        }
        
        log() << "replSet relinquishing primary state" << rsLog;
        _changeMemberState(MemberState::RS_SECONDARY);

        // close sockets that were talking to us so they don't blithly send many writes that
        // will fail with "not master" (of course client could check result code, but in
        // case they are not)
        log() << "replSet closing client sockets after relinquishing primary" << rsLog;
        //MessagingPort::closeAllSockets(ScopedConn::keepOpen);
        // XXX Eric: what to do here?
    }

    // election entry point
    void TopologyCoordinatorImpl::_electSelf(Date_t now) {
        verify( !_selfConfig().isArbiter() );
        verify( _selfConfig().getSlaveDelay() == Seconds(0) );
        try {
            // XXX Eric
            //            _electSelf(now);
        }
        catch (RetryAfterSleepException&) {
            throw;
        }
        catch (VoteException& ) { // Eric: XXX
            log() << "replSet not trying to elect self as responded yea to someone else recently" 
                  << rsLog;
        }
        catch (const DBException& e) {
            log() << "replSet warning caught unexpected exception in electSelf() " << e.toString() 
                  << rsLog;
        }
        catch (...) {
            log() << "replSet warning caught unexpected exception in electSelf()" << rsLog;
        }
    }

    // Produce a reply to a RAFT-style RequestVote RPC; this is MongoDB ReplSetFresh command
    // The caller should validate that the message is for the correct set, and has the required data
    void TopologyCoordinatorImpl::prepareRequestVoteResponse(const Date_t now,
                                                             const BSONObj& cmdObj,
                                                             std::string& errmsg,
                                                             BSONObjBuilder& result) {

        string who = cmdObj["who"].String();
        int cfgver = cmdObj["cfgver"].Int();
        OpTime opTime(cmdObj["opTime"].Date());

        bool weAreFresher = false;
        if( _currentConfig.getConfigVersion() > cfgver ) {
            log() << "replSet member " << who << " is not yet aware its cfg version "
                  << cfgver << " is stale" << rsLog;
            result.append("info", "config version stale");
            weAreFresher = true;
        }
        // check not only our own optime, but any other member we can reach
        else if( opTime < _commitOkayThrough ||
                 opTime < _latestKnownOpTime())  {
            weAreFresher = true;
        }
        result.appendDate("opTime", _lastApplied.asDate());
        result.append("fresher", weAreFresher);

        bool doVeto = _shouldVeto(cmdObj, errmsg);
        result.append("veto",doVeto);
        if (doVeto) {
            result.append("errmsg", errmsg);
        }
    }

    bool TopologyCoordinatorImpl::_shouldVeto(const BSONObj& cmdObj, string& errmsg) const {
        // don't veto older versions
        if (cmdObj["id"].eoo()) {
            // they won't be looking for the veto field
            return false;
        }

        const int id = cmdObj["id"].Int();
        const int hopefulIndex = _getMemberIndex(id);
        const int highestPriorityIndex = _getHighestPriorityElectableIndex();

        if (hopefulIndex == -1) {
            errmsg = str::stream() << "replSet couldn't find member with id " << id;
            return true;
        }

        if ((_currentPrimaryIndex != -1) && 
            (_commitOkayThrough >= _members[hopefulIndex].hbinfo().opTime)) {
            // hbinfo is not updated, so we have to check the primary's last optime separately
            errmsg = str::stream() << "I am already primary, " << 
                _members[hopefulIndex].fullName() << " can try again once I've stepped down";
            return true;
        }

        if (_currentPrimaryIndex != -1 &&
                (_members[hopefulIndex].hbinfo().id() != 
                 _members[_currentPrimaryIndex].hbinfo().id()) &&
                (_members[_currentPrimaryIndex].hbinfo().opTime >= 
                 _members[hopefulIndex].hbinfo().opTime)) {
            // other members might be aware of more up-to-date nodes
            errmsg = str::stream() << _members[hopefulIndex].fullName() <<
                " is trying to elect itself but " << _members[_currentPrimaryIndex].fullName() <<
                " is already primary and more up-to-date";
            return true;
        }

        if ((highestPriorityIndex != -1) &&
            _currentConfig.getMemberAt(highestPriorityIndex).getPriority() > 
            _currentConfig.getMemberAt(hopefulIndex).getPriority()) {
            errmsg = str::stream() << _members[hopefulIndex].fullName() << 
                " has lower priority than " << _members[highestPriorityIndex].fullName();
            return true;
        }

        if (!_electableSet.count(id)) {
            errmsg = str::stream() << "I don't think " << _members[hopefulIndex].fullName() <<
                " is electable";
            return true;
        }

        return false;
    }

    namespace {
        const size_t LeaseTime = 3;
    } // namespace

    // produce a reply to a received electCmd
    void TopologyCoordinatorImpl::prepareElectCmdResponse(const Date_t now,
                                                          const BSONObj& cmdObj,
                                                          BSONObjBuilder& result) {

        //TODO: after eric's checkin, add executer stuff and error if cancelled
        DEV log() << "replSet received elect msg " << cmdObj.toString() << rsLog;
        else LOG(2) << "replSet received elect msg " << cmdObj.toString() << rsLog;

        std::string setName = cmdObj["setName"].String();
        int whoid = cmdObj["whoid"].Int();
        long long cfgver = cmdObj["cfgver"].Int();
        OID round = cmdObj["round"].OID();
        long long myver = _currentConfig.getConfigVersion();

        const int hopefulIndex = _getMemberIndex(whoid);
        const int highestPriorityIndex = _getHighestPriorityElectableIndex();

        int vote = 0;
        if ( setName != _currentConfig.getReplSetName() ) {
            log() << "replSet error received an elect request for '" << setName
                  << "' but our setName name is '" << 
                _currentConfig.getReplSetName() << "'" << rsLog;
        }
        else if ( myver < cfgver ) {
            // we are stale.  don't vote
        }
        else if ( myver > cfgver ) {
            // they are stale!
            log() << "replSet electCmdReceived info got stale version # during election" << rsLog;
            vote = -10000;
        }
        else if ( hopefulIndex == -1 ) {
            log() << "replSet electCmdReceived couldn't find member with id " << whoid << rsLog;
            vote = -10000;
        }
        else if ( _currentPrimaryIndex != -1 && _memberState == MemberState::RS_PRIMARY ) {
            log() << "I am already primary, " << _members[hopefulIndex].fullName()
                  << " can try again once I've stepped down" << rsLog;
            vote = -10000;
        }
        else if (_currentPrimaryIndex != -1) {
            log() << _members[hopefulIndex].fullName() << " is trying to elect itself but " <<
                _members[_currentPrimaryIndex].fullName() << " is already primary" << rsLog;
            vote = -10000;
        }
        else if ((highestPriorityIndex != -1) &&
                 _currentConfig.getMemberAt(highestPriorityIndex).getPriority() > 
                 _currentConfig.getMemberAt(hopefulIndex).getPriority()) {
            log() << _members[hopefulIndex].fullName() << " has lower priority than "
                  << _members[highestPriorityIndex].fullName();
            vote = -10000;
        }
        else {
            try {
                if (_lastVote.when + LeaseTime >= now && static_cast<int>(_lastVote.who) != whoid) {
                    LOG(1) << "replSet not voting yea for " << whoid
                           << " voted for " << _lastVote.who << ' ' << now-_lastVote.when
                           << " secs ago" << rsLog;
                    //TODO: remove exception, and change control flow?
                    throw VoteException();
                }
                _lastVote.when = now;
                _lastVote.who = whoid;
                vote = _selfConfig().isVoter() ? 1 : 0;
                invariant( _members[hopefulIndex].hbinfo().id() == whoid );
                log() << "replSet info voting yea for " << _members[hopefulIndex].fullName()
                      << " (" << whoid << ')' << rsLog;
            }
            catch(VoteException&) {
                log() << "replSet voting no for " << _members[hopefulIndex].fullName()
                      << " already voted for another" << rsLog;
            }
        }

        result.append("vote", vote);
        result.append("round", round);
    }

    // produce a reply to a heartbeat
    void TopologyCoordinatorImpl::prepareHeartbeatResponse(
            const ReplicationExecutor::CallbackData& data,
            Date_t now,
            const BSONObj& cmdObj,
            BSONObjBuilder* resultObj,
            Status* result) {
        if (data.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
            return;
        }

        if( cmdObj["pv"].Int() != 1 ) {
            *result = Status(ErrorCodes::BadValue, "incompatible replset protocol version");
            return;
        }

        // Verify that replica set names match
        std::string rshb = std::string(cmdObj.getStringField("replSetHeartbeat"));
        const ReplSettings& replSettings = getGlobalReplicationCoordinator()->getSettings();
        if (replSettings.ourSetName() != rshb) {
            *result = Status(ErrorCodes::BadValue, "repl set names do not match");
            log() << "replSet set names do not match, our cmdline: " << replSettings.replSet
                  << rsLog;
            log() << "replSet rshb: " << rshb << rsLog;
            resultObj->append("mismatch", true);
            return;
        }

        // This is a replica set
        resultObj->append("rs", true);

/*
        if( cmdObj["checkEmpty"].trueValue() ) {
            // Eric: XXX takes read lock; only used for initial sync heartbeat
            resultObj->append("hasData", replHasDatabases());
        }
*/

        // Verify that the config's replset name matches
        if (_currentConfig.getReplSetName() != cmdObj.getStringField("replSetHeartbeat")) {
            *result = Status(ErrorCodes::BadValue, "repl set names do not match (2)");
            resultObj->append("mismatch", true);
            return; 
        }
        resultObj->append("set", _currentConfig.getReplSetName());

        resultObj->append("state", _memberState.s);
        if (_memberState == MemberState::RS_PRIMARY) {
            resultObj->appendDate("electionTime", _electionTime.asDate());
        }

        // Are we electable
        resultObj->append("e",
                          _electableSet.find(_selfConfig().getId()) != 
                          _electableSet.end());
        // Heartbeat status message
        resultObj->append("hbmsg", _getHbmsg());
        resultObj->append("time", now);
        resultObj->appendDate("opTime", _lastApplied.asDate());

        if (_syncSourceIndex != -1) {
            resultObj->append("syncingTo", _members[_syncSourceIndex].fullName());
        }

        long long v = _currentConfig.getConfigVersion();
        resultObj->append("v", v);
        // Deliver new config if caller's version is older than ours
        if( v > cmdObj["v"].Long() )
            *resultObj << "config" << _currentConfig.asBson();

        // Resolve the caller's id in our Member list
        int from = -1;
        if (cmdObj.hasField("fromId")) {
            if (v == cmdObj["v"].Int()) {
                from = _getMemberIndex(cmdObj["fromId"].Int());
            }
        }
        if (from == -1) {
            // Can't find the member, so we leave out the stateDisagreement field
            *result = Status::OK();
            return;
        }

        // if we thought that this node is down, let it know
        if (!_members[from].hbinfo().up()) {
            resultObj->append("stateDisagreement", true);
        }

        // note that we got a heartbeat from this node
        _members[from].get_hbinfo().lastHeartbeatRecv = now;
        *result = Status::OK();
    }


    int TopologyCoordinatorImpl::_getMemberIndex(int id) const {
        int index = 0;
        for (ReplicaSetConfig::MemberIterator it = _currentConfig.membersBegin();
             it != _currentConfig.membersEnd();
             ++it, ++index) {
            if (it->getId() == id) {
                return index;
            }
        }
        return -1;
    }

    // update internal state with heartbeat response, and run topology checks
    HeartbeatResultAction TopologyCoordinatorImpl::updateHeartbeatInfo(
                                                        Date_t now,
                                                        const HeartbeatInfo& newInfo) {
        // Fill in the new heartbeat data for the appropriate member
        for (std::vector<NewMember>::iterator it = _members.begin(); 
             it != _members.end(); 
             ++it) {
            if (it->hbinfo().id() == newInfo.id()) {
                it->get_hbinfo().updateFromLastPoll(newInfo);
                break;
            }
        }

        // Don't bother to make any changes if we are an election candidate
        if (_busyWithElectSelf) return None;

        // ex-checkelectableset begins here
        unsigned int latestOp = _latestKnownOpTime().getSecs();
        
        // make sure the electable set is up-to-date
        if (_aMajoritySeemsToBeUp()
            && !_selfConfig().isArbiter()    // not an arbiter
            && (_selfConfig().getPriority() > 0)  // not priority 0
            && (_stepDownUntil <= now)              // stepDown timer has expired
            && (_memberState == MemberState::RS_SECONDARY)
            // we are within 10 seconds of primary
            && (latestOp == 0 || _lastApplied.getSecs() >= latestOp - 10)) {
            _electableSet.insert(_selfConfig().getId());
        }
        else {
            _electableSet.erase(_selfConfig().getId());
        }

        // check if we should ask the primary (possibly ourselves) to step down
        int highestPriorityIndex = _getHighestPriorityElectableIndex();
        
        if (_currentPrimaryIndex != -1) {
            if ((highestPriorityIndex != -1) &&
                (_currentConfig.getMemberAt(highestPriorityIndex).getPriority() > 
                 _currentConfig.getMemberAt(_currentPrimaryIndex).getPriority()) &&
                // if we're stepping down to allow another member to become primary, we
                // better have another member (latestOp), and it should be up-to-date
                (latestOp != 0) && 
                _members[highestPriorityIndex].hbinfo().opTime.getSecs() >= latestOp - 10) {
                log() << "stepping down " << 
                    _members[_currentPrimaryIndex].fullName() << " (priority " <<
                    _currentConfig.getMemberAt(_currentPrimaryIndex).getPriority() << "), " << 
                    _members[highestPriorityIndex].fullName() << " is priority " << 
                    _currentConfig.getMemberAt(highestPriorityIndex).getPriority() << " and " <<
                    (latestOp - _members[highestPriorityIndex].hbinfo().opTime.getSecs()) << 
                    " seconds behind";

                // Are we primary?
                if (isSelf(_members[_currentPrimaryIndex].hap())) {
                    // replSetStepDown tries to acquire the same lock
                    // msgCheckNewState takes, so we can't call replSetStepDown on
                    // ourselves.
                    // XXX Eric: schedule relinquish
                    //rs->relinquish();
                }
                else {
                    // We are not primary.  Step down the remote node.
                    BSONObj cmd = BSON( "replSetStepDown" << 1 );
/*                ScopedConn conn(primary->fullName());
                  BSONObj result;
                  // XXX Eric: schedule stepdown command

                  try {
                  if (!conn.runCommand("admin", cmd, result, 0)) {
                  log() << "stepping down " << primary->fullName()
                  << " failed: " << result << endl;
                  }
                  }
                  catch (DBException &e) {
                  log() << "stepping down " << primary->fullName() << " threw exception: "
                  << e.toString() << endl;
                  }
                  
*/
                    return StepDown;
                }
            }
        }


        // ex-checkauth begins here
        {
            int down = 0, authIssue = 0, total = 0;

            for (std::vector<NewMember>::const_iterator it = _members.begin(); 
                 it != _members.end(); 
                 ++it) {
                total++;

                // all authIssue servers will also be not up
                if (!it->hbinfo().up()) {
                    down++;
                    if (it->hbinfo().authIssue) {
                        authIssue++;
                    }
                }
            }

            // if all nodes are down or failed auth AND at least one failed
            // auth, go into recovering.  If all nodes are down, stay a
            // secondary.
            if (authIssue > 0 && down == total) {
                log() << "replset error could not reach/authenticate against any members";

                if (_currentPrimaryIndex == _selfIndex) {
                    log() << "auth problems, relinquishing primary" << rsLog;
                    // XXX Eric: schedule relinquish
                    //rs->relinquish();

                    return StepDown;
                }

                _blockSync = true;
                // syncing is how we get into SECONDARY state, so we'll be stuck in
                // RECOVERING until we unblock
                _changeMemberState(MemberState::RS_RECOVERING);
            }
            else {
                _blockSync = false;
            }
        }

        // If a remote is primary, check that it is still up.
        if (_currentPrimaryIndex != -1 && _currentPrimaryIndex != _selfIndex) {
            if (!_members[_currentPrimaryIndex].hbinfo().up() || 
                !_members[_currentPrimaryIndex].hbinfo().hbstate.primary()) {
                _currentPrimaryIndex = -1;
            }
        }

        // Scan the member list's heartbeat data for who is primary, and update ourselves if it's
        // not what currentPrimary is.
        {
            int remotePrimaryIndex = -1;
            for (std::vector<NewMember>::const_iterator it = _members.begin(); 
                 it != _members.end(); 
                 ++it) {
                if (it->configIndex() == _selfIndex) {
                    continue;
                }

                if( it->hbinfo().hbstate.primary() && it->hbinfo().up() ) {
                    if (remotePrimaryIndex != -1) {
                        /* two other nodes think they are primary (asynchronously polled) -- wait for things to settle down. */
                        log() << "replSet info two primaries (transiently)" << rsLog;
                        return None;
                    }
                    remotePrimaryIndex = it->configIndex();
                }
            }

            if (remotePrimaryIndex != -1) {
                // If it's the same as last time, don't do anything further.
                if (_currentPrimaryIndex == remotePrimaryIndex) {
                    return None;
                }
                // Clear last heartbeat message on ourselves (why?)
                _sethbmsg("");

                // insanity: this is what actually puts arbiters into ARBITER state
                if (_selfConfig().isArbiter()) {
                    _changeMemberState(MemberState::RS_ARBITER);
                    return None;
                }

                // If we are also primary, this is a problem.  Determine who should step down.
                if (_memberState == MemberState::RS_PRIMARY) {
                    OpTime remoteElectionTime = _members[remotePrimaryIndex].hbinfo().electionTime;
                    log() << "replset: another primary seen with election time " 
                          << remoteElectionTime; 
                    // Step down whoever has the older election time.
                    if (remoteElectionTime > _electionTime) {
                        log() << "stepping down; another primary was elected more recently";
                        // XXX Eric: schedule a relinquish
                        //rs->relinquish();
                        // after completion, set currentprimary to remotePrimaryIndex.
                        return StepDown;
                    }
                    else {
                        // else, stick around
                        log() << "another PRIMARY detected but it should step down"
                            " since it was elected earlier than me";
                        return None;
                    }
                }

                _currentPrimaryIndex = remotePrimaryIndex;
                return None;
            }
            /* didn't find anyone who is currently primary */
        }

        // If we are primary, check if we can still see majority of the set;
        // stepdown if we can't.
        if (_currentPrimaryIndex != -1) {
            /* we must be primary */
            fassert(18505, _currentPrimaryIndex == _selfIndex);

            if (_shouldRelinquish()) {
                log() << "can't see a majority of the set, relinquishing primary" << rsLog;
                // XXX Eric: schedule a relinquish
                //rs->relinquish();
                return StepDown;

            }

            return None;
        }

        // At this point, there is no primary anywhere.  Check to see if we should become an
        // election candidate.

        // If we can't elect ourselves due to config, can't become a candidate.
        if (!_selfConfig().isArbiter()       // not an arbiter
            && (_selfConfig().getPriority() > 0)  // not priority 0
            && (_stepDownUntil <= now)              // stepDown timer has expired
            && (_memberState == MemberState::RS_SECONDARY)) {
            OCCASIONALLY log() << "replSet I don't see a primary and I can't elect myself";
            return None;
        }

        // If we can't see a majority, can't become a candidate.
        if (!_aMajoritySeemsToBeUp()) {
            static Date_t last;
            static int n = 0;
            int ll = 0;
            if( ++n > 5 ) ll++;
            if( last + 60 > now ) ll++;
            LOG(ll) << "replSet can't see a majority, will not try to elect self" << rsLog;
            last = now;
            return None;
        }

        // If we can't elect ourselves due to the current electable set;
        // we are in the set if we are within 10 seconds of the latest known op (via heartbeats)
        if (!(_electableSet.find(_selfConfig().getId()) != _electableSet.end())) {
            // we are too far behind to become primary
            return None;
        }

        // All checks passed, become a candidate and start election proceedings.

        // don't try to do further elections & such while we are already working on one.
        _busyWithElectSelf = true; 
        return StartElection;

    // XXX: schedule an election
/*
        try {
            rs->elect.electSelf();
        }
        catch(RetryAfterSleepException&) {
            // we want to process new inbounds before trying this again.  so we just put a checkNewstate in the queue for eval later. 
            requeue();
        }
        catch(...) {
            log() << "replSet error unexpected assertion in rs manager" << rsLog;
        }
        
    }

*/
        _busyWithElectSelf = false;
        return None;
    }

    bool TopologyCoordinatorImpl::_shouldRelinquish() const {
        return !_aMajoritySeemsToBeUp();
    }

    bool TopologyCoordinatorImpl::_aMajoritySeemsToBeUp() const {
        int vUp = 0;
        for (std::vector<NewMember>::const_iterator it = _members.begin(); 
             it != _members.end(); 
             ++it) {
            if (it->hbinfo().up()) {
                vUp += _currentConfig.getMemberAt(it->configIndex()).isVoter() ? 1 : 0;
            }
        }

        return vUp * 2 > _totalVotes();
    }

    int TopologyCoordinatorImpl::_totalVotes() const {
        static int complain = 0;
        int vTot = 0;
        for (ReplicaSetConfig::MemberIterator it = _currentConfig.membersBegin();
             it != _currentConfig.membersEnd();
             ++it) {
            vTot += it->isVoter() ? 1 : 0;
        }
        if( vTot % 2 == 0 && vTot && complain++ == 0 )
            log() << "replSet warning: even number of voting members in replica set config - "
                     "add an arbiter or set votes to 0 on one of the existing members" << rsLog;
        return vTot;
    }

    OpTime TopologyCoordinatorImpl::_latestKnownOpTime() const {
        OpTime latest(0,0);

        for (std::vector<NewMember>::const_iterator it = _members.begin(); 
             it != _members.end(); 
             ++it) {

            if (!it->hbinfo().up()) {
                continue;
            }

            if (it->hbinfo().opTime > latest) {
                latest = it->hbinfo().opTime;
            }
        }

        return latest;
    }

    int TopologyCoordinatorImpl::_getHighestPriorityElectableIndex() const {
        int maxIndex = -1;
        std::set<unsigned int>::iterator it = _electableSet.begin();
        while (it != _electableSet.end()) {
            int candidateIndex = _getMemberIndex(*it);
            if (candidateIndex == -1) {
                log() << "couldn't find member: " << *it << endl;
                it++;
                continue;
            }
            if ((maxIndex == -1) || 
                _currentConfig.getMemberAt(maxIndex).getPriority() < 
                _currentConfig.getMemberAt(candidateIndex).getPriority()) {
                maxIndex = candidateIndex;
            }
            it++;
        }

        return maxIndex;
    }

    void TopologyCoordinatorImpl::_changeMemberState(const MemberState& newMemberState) {
        if (_memberState == newMemberState) {
            // nothing to do
            return;
        }
        _memberState = newMemberState;
        log() << "replSet " << _memberState.toString() << rsLog;

        for (std::vector<StateChangeCallbackFn>::const_iterator it = _stateChangeCallbacks.begin();
             it != _stateChangeCallbacks.end(); ++it) {
            (*it)(_memberState);
        }
    }

    void TopologyCoordinatorImpl::prepareStatusResponse(Date_t now,
                                                        const BSONObj& cmdObj,
                                                        BSONObjBuilder& result,
                                                        unsigned uptime) {
        // output for each member
        vector<BSONObj> membersOut;
        MemberState myState = _memberState;

        for (std::vector<NewMember>::const_iterator it = _members.begin(); 
             it != _members.end(); 
             ++it) {
            if (it->configIndex() == _selfIndex) {
                // add self
                BSONObjBuilder bb;
                bb.append("_id", _selfConfig().getId());
                bb.append("name", _members[_selfIndex].fullName());
                bb.append("health", 1.0);
                bb.append("state", static_cast<int>(myState.s));
                bb.append("stateStr", myState.toString());
                bb.append("uptime", uptime);
                if (!_selfConfig().isArbiter()) {
                    bb.appendTimestamp("optime", _lastApplied.asDate());
                    bb.appendDate("optimeDate", _lastApplied.getSecs() * 1000LL);
                }

                if (_maintenanceModeCalls) {
                    bb.append("maintenanceMode", _maintenanceModeCalls);
                }

                std::string s = _getHbmsg();
                if( !s.empty() )
                    bb.append("infoMessage", s);

                if (myState == MemberState::RS_PRIMARY) {
                    bb.append("electionTime", _electionTime);
                    bb.appendDate("electionDate", _electionTime.asDate());
                }
                bb.append("self", true);
                membersOut.push_back(bb.obj());
            }
            else {
                // add non-self member
                BSONObjBuilder bb;
                bb.append("_id", it->hbinfo().id());
                bb.append("name", it->fullName());
                double h = it->hbinfo().health;
                bb.append("health", h);
                bb.append("state", static_cast<int>(it->hbinfo().hbstate.s));
                if( h == 0 ) {
                    // if we can't connect the state info is from the past
                    // and could be confusing to show
                    bb.append("stateStr", "(not reachable/healthy)");
                }
                else {
                    bb.append("stateStr", it->hbinfo().hbstate.toString());
                }
                bb.append("uptime",
                          (unsigned) (it->hbinfo().upSince ? (now-it->hbinfo().upSince) : 0));
                if (!_currentConfig.getMemberAt(it->configIndex()).isArbiter()) {
                    bb.appendTimestamp("optime", it->hbinfo().opTime.asDate());
                    bb.appendDate("optimeDate", it->hbinfo().opTime.getSecs() * 1000LL);
                }
                bb.appendTimeT("lastHeartbeat", it->hbinfo().lastHeartbeat);
                bb.appendTimeT("lastHeartbeatRecv", it->hbinfo().lastHeartbeatRecv);
                bb.append("pingMs", it->hbinfo().ping);
                std::string s = it->hbinfo().lastHeartbeatMsg;
                if( !s.empty() )
                    bb.append("lastHeartbeatMessage", s);

                if (it->hbinfo().authIssue) {
                    bb.append("authenticated", false);
                }

                std::string syncingTo = it->hbinfo().syncingTo;
                if (!syncingTo.empty()) {
                    bb.append("syncingTo", syncingTo);
                }

                if (it->hbinfo().hbstate == MemberState::RS_PRIMARY) {
                    bb.appendTimestamp("electionTime", it->hbinfo().electionTime.asDate());
                    bb.appendDate("electionDate", it->hbinfo().electionTime.getSecs() * 1000LL);
                }
                membersOut.push_back(bb.obj());
            }
        }

        // sort members bson
        sort(membersOut.begin(), membersOut.end());

        result.append("set", _currentConfig.getReplSetName());
        result.appendTimeT("date", now);
        result.append("myState", myState.s);

        // Add sync source info
        if ((_syncSourceIndex != -1) && 
            (myState != MemberState::RS_PRIMARY) &&
            (myState != MemberState::RS_SHUNNED) ) {
            result.append("syncingTo", _members[_syncSourceIndex].fullName());
        }

        result.append("members", membersOut);
        /* TODO: decide where this lands
        if( replSetBlind )
            result.append("blind",true); // to avoid confusion if set...
                                         // normally never set except for testing.
        */
    }
    void TopologyCoordinatorImpl::prepareFreezeResponse(Date_t now,
                                                        const BSONObj& cmdObj,
                                                        BSONObjBuilder& result) {
        int secs = (int) cmdObj.firstElement().numberInt();

        if (secs == 0) {
            _stepDownUntil = now;
            log() << "replSet info 'unfreezing'" << rsLog;
            result.append("info","unfreezing");
        }
        else {
            if( secs == 1 )
                result.append("warning", "you really want to freeze for only 1 second?");

            if (_memberState != MemberState::RS_PRIMARY) {
                _stepDownUntil = now + secs;
                log() << "replSet info 'freezing' for " << secs << " seconds" << rsLog;
            }
            else {
                log() << "replSet info received freeze command but we are primary" << rsLog;
            }
        }
    }

    // This function installs a new config object and recreates NewMember objects that reflect
    // the new config.
    void TopologyCoordinatorImpl::updateConfig(const ReplicaSetConfig& newConfig, int selfIndex) {
        invariant(selfIndex < newConfig.getNumMembers());
        _currentConfig = newConfig;        

        _members.clear();
        _selfIndex = selfIndex;

        int index = 0;
        for (ReplicaSetConfig::MemberIterator it = _currentConfig.membersBegin();
             it != _currentConfig.membersEnd();
             ++it, ++index) {
            // C++11: use emplace_back()
            _members.push_back(NewMember(it->getHostAndPort(), it->getId(), index));
        }

        // call registered callbacks for config changes
        for (std::vector<ConfigChangeCallbackFn>::const_iterator it = 
                 _configChangeCallbacks.begin();
             it != _configChangeCallbacks.end(); ++it) {
            (*it)(_currentConfig);
        }

    }

    // TODO:Better story for heartbeat message handling.
    void TopologyCoordinatorImpl::_sethbmsg(const std::string& s, int logLevel) {
        static time_t lastLogged;
        _hbmsgTime = time(0);

        if (s == _hbmsg) {
            // unchanged
            if (_hbmsgTime - lastLogged < 60)
                return;
        }

        unsigned sz = s.size();
        if (sz >= 256)
            memcpy(_hbmsg, s.c_str(), 255);
        else {
            _hbmsg[sz] = 0;
            memcpy(_hbmsg, s.c_str(), sz);
        }
        if (!s.empty()) {
            lastLogged = _hbmsgTime;
            LOG(logLevel) << "replSet " << s << rsLog;
        }
    }
    
    const MemberConfig& TopologyCoordinatorImpl::_selfConfig() {
        return _currentConfig.getMemberAt(_selfIndex);
    }

} // namespace repl
} // namespace mongo
