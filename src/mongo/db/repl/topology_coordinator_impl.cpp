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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/topology_coordinator_impl.h"

#include <limits>

#include "mongo/db/audit.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/heartbeat_response_action.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/repl_set_declare_election_winner_args.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_set_html_summary.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

using std::vector;

const Seconds TopologyCoordinatorImpl::VoteLease::leaseTime = Seconds(30);

namespace {

template <typename T>
int indexOfIterator(const std::vector<T>& vec, typename std::vector<T>::const_iterator& it) {
    return static_cast<int>(it - vec.begin());
}

// Maximum number of retries for a failed heartbeat.
const int kMaxHeartbeatRetries = 2;

/**
 * Returns true if the only up heartbeats are auth errors.
 */
bool _hasOnlyAuthErrorUpHeartbeats(const std::vector<MemberHeartbeatData>& hbdata,
                                   const int selfIndex) {
    bool foundAuthError = false;
    for (std::vector<MemberHeartbeatData>::const_iterator it = hbdata.begin(); it != hbdata.end();
         ++it) {
        if (indexOfIterator(hbdata, it) == selfIndex) {
            continue;
        }

        if (it->up()) {
            return false;
        }

        if (it->hasAuthIssue()) {
            foundAuthError = true;
        }
    }

    return foundAuthError;
}

}  // namespace

void PingStats::start(Date_t now) {
    _lastHeartbeatStartDate = now;
    _numFailuresSinceLastStart = 0;
}

void PingStats::hit(Milliseconds millis) {
    _numFailuresSinceLastStart = std::numeric_limits<int>::max();
    ++count;

    value = value == UninitializedPing ? millis : Milliseconds((value * 4 + millis) / 5);
}

void PingStats::miss() {
    ++_numFailuresSinceLastStart;
}

TopologyCoordinatorImpl::TopologyCoordinatorImpl(Options options)
    : _role(Role::follower),
      _term(OpTime::kUninitializedTerm),
      _currentPrimaryIndex(-1),
      _forceSyncSourceIndex(-1),
      _options(std::move(options)),
      _selfIndex(-1),
      _stepDownPending(false),
      _maintenanceModeCalls(0),
      _followerMode(MemberState::RS_STARTUP2) {
    invariant(getMemberState() == MemberState::RS_STARTUP);
}

TopologyCoordinator::Role TopologyCoordinatorImpl::getRole() const {
    return _role;
}

void TopologyCoordinatorImpl::setForceSyncSourceIndex(int index) {
    invariant(_forceSyncSourceIndex < _rsConfig.getNumMembers());
    _forceSyncSourceIndex = index;
}

HostAndPort TopologyCoordinatorImpl::getSyncSourceAddress() const {
    return _syncSource;
}

HostAndPort TopologyCoordinatorImpl::chooseNewSyncSource(Date_t now,
                                                         const Timestamp& lastTimestampApplied) {
    // If we are primary, then we aren't syncing from anyone (else).
    if (_iAmPrimary()) {
        return HostAndPort();
    }

    // If we are not a member of the current replica set configuration, no sync source is valid.
    if (_selfIndex == -1) {
        LOG(2) << "Cannot sync from any members because we are not in the replica set config";
        return HostAndPort();
    }

    // if we have a target we've requested to sync from, use it
    if (_forceSyncSourceIndex != -1) {
        invariant(_forceSyncSourceIndex < _rsConfig.getNumMembers());
        _syncSource = _rsConfig.getMemberAt(_forceSyncSourceIndex).getHostAndPort();
        _forceSyncSourceIndex = -1;
        std::string msg(str::stream() << "syncing from: " << _syncSource.toString()
                                      << " by request");
        log() << msg << rsLog;
        setMyHeartbeatMessage(now, msg);
        return _syncSource;
    }

    // wait for 2N pings (not counting ourselves) before choosing a sync target
    int needMorePings = (_hbdata.size() - 1) * 2 - _getTotalPings();

    if (needMorePings > 0) {
        OCCASIONALLY log() << "waiting for " << needMorePings
                           << " pings from other members before syncing";
        _syncSource = HostAndPort();
        return _syncSource;
    }

    // If we are only allowed to sync from the primary, set that
    if (!_rsConfig.isChainingAllowed()) {
        if (_currentPrimaryIndex == -1) {
            LOG(1) << "Cannot select sync source because chaining is"
                      " not allowed and primary is unknown/down";
            _syncSource = HostAndPort();
            return _syncSource;
        } else if (_memberIsBlacklisted(*_currentPrimaryMember(), now)) {
            LOG(1) << "Cannot select sync source because chaining is"
                      "not allowed and primary is not currently accepting our updates";
            _syncSource = HostAndPort();
            return _syncSource;
        } else {
            _syncSource = _rsConfig.getMemberAt(_currentPrimaryIndex).getHostAndPort();
            std::string msg(str::stream() << "syncing from primary: " << _syncSource.toString());
            log() << msg << rsLog;
            setMyHeartbeatMessage(now, msg);
            return _syncSource;
        }
    }

    // find the member with the lowest ping time that is ahead of me

    // choose a time that will exclude no candidates by default, in case we don't see a primary
    OpTime oldestSyncOpTime;

    // Find primary's oplog time. Reject sync candidates that are more than
    // _options.maxSyncSourceLagSecs seconds behind.
    if (_currentPrimaryIndex != -1) {
        OpTime primaryOpTime = _hbdata.at(_currentPrimaryIndex).getAppliedOpTime();

        // Check if primaryOpTime is still close to 0 because we haven't received
        // our first heartbeat from a new primary yet.
        unsigned int maxLag =
            static_cast<unsigned int>(durationCount<Seconds>(_options.maxSyncSourceLagSecs));
        if (primaryOpTime.getSecs() >= maxLag) {
            oldestSyncOpTime =
                OpTime(Timestamp(primaryOpTime.getSecs() - maxLag, 0), primaryOpTime.getTerm());
        }
    }

    int closestIndex = -1;

    // Make two attempts, with less restrictive rules the second time.
    //
    // During the first attempt, we ignore those nodes that have a larger slave
    // delay, hidden nodes or non-voting, and nodes that are excessively behind.
    //
    // For the second attempt include those nodes, in case those are the only ones we can reach.
    //
    // This loop attempts to set 'closestIndex', to select a viable candidate.
    for (int attempts = 0; attempts < 2; ++attempts) {
        for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin();
             it != _hbdata.end();
             ++it) {
            const int itIndex = indexOfIterator(_hbdata, it);
            // Don't consider ourselves.
            if (itIndex == _selfIndex) {
                continue;
            }
            // Candidate must be up to be considered.
            if (!it->up()) {
                continue;
            }
            // Candidate must be PRIMARY or SECONDARY state to be considered.
            if (!it->getState().readable()) {
                continue;
            }

            const MemberConfig& itMemberConfig(_rsConfig.getMemberAt(itIndex));

            // Things to skip on the first attempt.
            if (attempts == 0) {
                // Candidate must be a voter if we are a voter.
                if (_selfConfig().isVoter() && !itMemberConfig.isVoter()) {
                    continue;
                }
                // Candidates must not be hidden.
                if (itMemberConfig.isHidden()) {
                    continue;
                }
                // Candidates cannot be excessively behind.
                if (it->getAppliedOpTime() < oldestSyncOpTime) {
                    continue;
                }
                // Candidate must not have a configured delay larger than ours.
                if (_selfConfig().getSlaveDelay() < itMemberConfig.getSlaveDelay()) {
                    continue;
                }
            }
            // Candidate must build indexes if we build indexes, to be considered.
            if (_selfConfig().shouldBuildIndexes()) {
                if (!itMemberConfig.shouldBuildIndexes()) {
                    continue;
                }
            }
            // only consider candidates that are ahead of where we are
            if (it->getAppliedOpTime().getTimestamp() <= lastTimestampApplied) {
                continue;
            }
            // Candidate cannot be more latent than anything we've already considered.
            if ((closestIndex != -1) &&
                (_getPing(itMemberConfig.getHostAndPort()) >
                 _getPing(_rsConfig.getMemberAt(closestIndex).getHostAndPort()))) {
                continue;
            }
            // Candidate cannot be blacklisted.
            if (_memberIsBlacklisted(itMemberConfig, now)) {
                continue;
            }
            // This candidate has passed all tests; set 'closestIndex'
            closestIndex = itIndex;
        }
        if (closestIndex != -1)
            break;  // no need for second attempt
    }

    if (closestIndex == -1) {
        // Did not find any members to sync from
        std::string msg("could not find member to sync from");
        // Only log when we had a valid sync source before
        if (!_syncSource.empty()) {
            log() << msg << rsLog;
        }
        setMyHeartbeatMessage(now, msg);

        _syncSource = HostAndPort();
        return _syncSource;
    }
    _syncSource = _rsConfig.getMemberAt(closestIndex).getHostAndPort();
    std::string msg(str::stream() << "syncing from: " << _syncSource.toString(), 0);
    log() << msg << rsLog;
    setMyHeartbeatMessage(now, msg);
    return _syncSource;
}

bool TopologyCoordinatorImpl::_memberIsBlacklisted(const MemberConfig& memberConfig,
                                                   Date_t now) const {
    std::map<HostAndPort, Date_t>::const_iterator blacklisted =
        _syncSourceBlacklist.find(memberConfig.getHostAndPort());
    if (blacklisted != _syncSourceBlacklist.end()) {
        if (blacklisted->second > now) {
            return true;
        }
    }
    return false;
}

void TopologyCoordinatorImpl::blacklistSyncSource(const HostAndPort& host, Date_t until) {
    LOG(2) << "blacklisting " << host << " until " << until.toString();
    _syncSourceBlacklist[host] = until;
}

void TopologyCoordinatorImpl::unblacklistSyncSource(const HostAndPort& host, Date_t now) {
    std::map<HostAndPort, Date_t>::iterator hostItr = _syncSourceBlacklist.find(host);
    if (hostItr != _syncSourceBlacklist.end() && now >= hostItr->second) {
        LOG(2) << "unblacklisting " << host;
        _syncSourceBlacklist.erase(hostItr);
    }
}

void TopologyCoordinatorImpl::clearSyncSourceBlacklist() {
    _syncSourceBlacklist.clear();
}

void TopologyCoordinatorImpl::prepareSyncFromResponse(const ReplicationExecutor::CallbackArgs& data,
                                                      const HostAndPort& target,
                                                      const OpTime& lastOpApplied,
                                                      BSONObjBuilder* response,
                                                      Status* result) {
    if (data.status == ErrorCodes::CallbackCanceled) {
        *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
        return;
    }

    response->append("syncFromRequested", target.toString());

    if (_selfIndex == -1) {
        *result = Status(ErrorCodes::NotSecondary, "Removed and uninitialized nodes do not sync");
        return;
    }

    const MemberConfig& selfConfig = _selfConfig();
    if (selfConfig.isArbiter()) {
        *result = Status(ErrorCodes::NotSecondary, "arbiters don't sync");
        return;
    }
    if (_selfIndex == _currentPrimaryIndex) {
        *result = Status(ErrorCodes::NotSecondary, "primaries don't sync");
        return;
    }

    ReplicaSetConfig::MemberIterator targetConfig = _rsConfig.membersEnd();
    int targetIndex = 0;
    for (ReplicaSetConfig::MemberIterator it = _rsConfig.membersBegin();
         it != _rsConfig.membersEnd();
         ++it) {
        if (it->getHostAndPort() == target) {
            targetConfig = it;
            break;
        }
        ++targetIndex;
    }
    if (targetConfig == _rsConfig.membersEnd()) {
        *result = Status(ErrorCodes::NodeNotFound,
                         str::stream() << "Could not find member \"" << target.toString()
                                       << "\" in replica set");
        return;
    }
    if (targetIndex == _selfIndex) {
        *result = Status(ErrorCodes::InvalidOptions, "I cannot sync from myself");
        return;
    }
    if (targetConfig->isArbiter()) {
        *result = Status(ErrorCodes::InvalidOptions,
                         str::stream() << "Cannot sync from \"" << target.toString()
                                       << "\" because it is an arbiter");
        return;
    }
    if (!targetConfig->shouldBuildIndexes() && selfConfig.shouldBuildIndexes()) {
        *result = Status(ErrorCodes::InvalidOptions,
                         str::stream() << "Cannot sync from \"" << target.toString()
                                       << "\" because it does not build indexes");
        return;
    }

    if (selfConfig.isVoter() && !targetConfig->isVoter()) {
        *result = Status(ErrorCodes::InvalidOptions,
                         str::stream() << "Cannot sync from \"" << target.toString()
                                       << "\" because it is not a voter");
        return;
    }

    const MemberHeartbeatData& hbdata = _hbdata.at(targetIndex);
    if (hbdata.hasAuthIssue()) {
        *result =
            Status(ErrorCodes::Unauthorized,
                   str::stream() << "not authorized to communicate with " << target.toString());
        return;
    }
    if (hbdata.getHealth() == 0) {
        *result =
            Status(ErrorCodes::HostUnreachable,
                   str::stream() << "I cannot reach the requested member: " << target.toString());
        return;
    }
    if (hbdata.getAppliedOpTime().getSecs() + 10 < lastOpApplied.getSecs()) {
        warning() << "attempting to sync from " << target << ", but its latest opTime is "
                  << hbdata.getAppliedOpTime().getSecs() << " and ours is "
                  << lastOpApplied.getSecs() << " so this may not work";
        response->append("warning",
                         str::stream() << "requested member \"" << target.toString()
                                       << "\" is more than 10 seconds behind us");
        // not returning bad Status, just warning
    }

    HostAndPort prevSyncSource = getSyncSourceAddress();
    if (!prevSyncSource.empty()) {
        response->append("prevSyncTarget", prevSyncSource.toString());
    }

    setForceSyncSourceIndex(targetIndex);
    *result = Status::OK();
}

void TopologyCoordinatorImpl::prepareFreshResponse(
    const ReplicationCoordinator::ReplSetFreshArgs& args,
    const Date_t now,
    const OpTime& lastOpApplied,
    BSONObjBuilder* response,
    Status* result) {
    if (_rsConfig.getProtocolVersion() != 0) {
        *result = Status(ErrorCodes::BadValue,
                         str::stream() << "replset: incompatible replset protocol version: "
                                       << _rsConfig.getProtocolVersion());
        return;
    }

    if (_selfIndex == -1) {
        *result = Status(ErrorCodes::ReplicaSetNotFound,
                         "Cannot participate in elections because not initialized");
        return;
    }

    if (args.setName != _rsConfig.getReplSetName()) {
        *result =
            Status(ErrorCodes::ReplicaSetNotFound,
                   str::stream() << "Wrong repl set name. Expected: " << _rsConfig.getReplSetName()
                                 << ", received: " << args.setName);
        return;
    }

    if (args.id == static_cast<unsigned>(_selfConfig().getId())) {
        *result = Status(ErrorCodes::BadValue,
                         str::stream() << "Received replSetFresh command from member with the "
                                          "same member ID as ourself: " << args.id);
        return;
    }

    bool weAreFresher = false;
    if (_rsConfig.getConfigVersion() > args.cfgver) {
        log() << "replSet member " << args.who << " is not yet aware its cfg version "
              << args.cfgver << " is stale";
        response->append("info", "config version stale");
        weAreFresher = true;
    }
    // check not only our own optime, but any other member we can reach
    else if (OpTime(args.opTime, _term) < _latestKnownOpTime(lastOpApplied)) {
        weAreFresher = true;
    }
    response->appendDate("opTime",
                         Date_t::fromMillisSinceEpoch(lastOpApplied.getTimestamp().asLL()));
    response->append("fresher", weAreFresher);

    std::string errmsg;
    bool doVeto = _shouldVetoMember(args, now, lastOpApplied, &errmsg);
    response->append("veto", doVeto);
    if (doVeto) {
        response->append("errmsg", errmsg);
    }
    *result = Status::OK();
}

bool TopologyCoordinatorImpl::_shouldVetoMember(
    const ReplicationCoordinator::ReplSetFreshArgs& args,
    const Date_t& now,
    const OpTime& lastOpApplied,
    std::string* errmsg) const {
    if (_rsConfig.getConfigVersion() < args.cfgver) {
        // We are stale; do not veto.
        return false;
    }

    const unsigned int memberID = args.id;
    const int hopefulIndex = _getMemberIndex(memberID);
    invariant(hopefulIndex != _selfIndex);
    const int highestPriorityIndex = _getHighestPriorityElectableIndex(now, lastOpApplied);

    if (hopefulIndex == -1) {
        *errmsg = str::stream() << "replSet couldn't find member with id " << memberID;
        return true;
    }

    if (_iAmPrimary() && lastOpApplied >= _hbdata.at(hopefulIndex).getAppliedOpTime()) {
        // hbinfo is not updated for ourself, so if we are primary we have to check the
        // primary's last optime separately
        *errmsg = str::stream() << "I am already primary, "
                                << _rsConfig.getMemberAt(hopefulIndex).getHostAndPort().toString()
                                << " can try again once I've stepped down";
        return true;
    }

    if (_currentPrimaryIndex != -1 && (hopefulIndex != _currentPrimaryIndex) &&
        (_hbdata.at(_currentPrimaryIndex).getAppliedOpTime() >=
         _hbdata.at(hopefulIndex).getAppliedOpTime())) {
        // other members might be aware of more up-to-date nodes
        *errmsg =
            str::stream() << _rsConfig.getMemberAt(hopefulIndex).getHostAndPort().toString()
                          << " is trying to elect itself but "
                          << _rsConfig.getMemberAt(_currentPrimaryIndex).getHostAndPort().toString()
                          << " is already primary and more up-to-date";
        return true;
    }

    if ((highestPriorityIndex != -1)) {
        const MemberConfig& hopefulMember = _rsConfig.getMemberAt(hopefulIndex);
        const MemberConfig& priorityMember = _rsConfig.getMemberAt(highestPriorityIndex);

        if (priorityMember.getPriority() > hopefulMember.getPriority()) {
            *errmsg = str::stream() << hopefulMember.getHostAndPort().toString()
                                    << " has lower priority of " << hopefulMember.getPriority()
                                    << " than " << priorityMember.getHostAndPort().toString()
                                    << " which has a priority of " << priorityMember.getPriority();
            return true;
        }
    }

    UnelectableReasonMask reason = _getUnelectableReason(hopefulIndex, lastOpApplied);
    reason &= ~RefusesToStand;
    if (reason) {
        *errmsg = str::stream() << "I don't think "
                                << _rsConfig.getMemberAt(hopefulIndex).getHostAndPort().toString()
                                << " is electable because the "
                                << _getUnelectableReasonString(reason);
        return true;
    }

    return false;
}

// produce a reply to a received electCmd
void TopologyCoordinatorImpl::prepareElectResponse(
    const ReplicationCoordinator::ReplSetElectArgs& args,
    const Date_t now,
    const OpTime& lastOpApplied,
    BSONObjBuilder* response,
    Status* result) {
    if (_rsConfig.getProtocolVersion() != 0) {
        *result = Status(ErrorCodes::BadValue,
                         str::stream() << "replset: incompatible replset protocol version: "
                                       << _rsConfig.getProtocolVersion());
        return;
    }
    if (_selfIndex == -1) {
        *result = Status(ErrorCodes::ReplicaSetNotFound,
                         "Cannot participate in election because not initialized");
        return;
    }

    const long long myver = _rsConfig.getConfigVersion();
    const int highestPriorityIndex = _getHighestPriorityElectableIndex(now, lastOpApplied);

    const MemberConfig* primary = _currentPrimaryMember();
    const MemberConfig* hopeful = _rsConfig.findMemberByID(args.whoid);
    const MemberConfig* highestPriority =
        highestPriorityIndex == -1 ? NULL : &_rsConfig.getMemberAt(highestPriorityIndex);

    int vote = 0;
    if (args.set != _rsConfig.getReplSetName()) {
        log() << "replSet error received an elect request for '" << args.set
              << "' but our set name is '" << _rsConfig.getReplSetName() << "'";
    } else if (myver < args.cfgver) {
        // we are stale.  don't vote
        log() << "replSetElect not voting because our config version is stale. Our version: "
              << myver << ", their version: " << args.cfgver;
    } else if (myver > args.cfgver) {
        // they are stale!
        log() << "replSetElect command received stale config version # during election. "
                 "Our version: " << myver << ", their version: " << args.cfgver;
        vote = -10000;
    } else if (!hopeful) {
        log() << "replSetElect couldn't find member with id " << args.whoid;
        vote = -10000;
    } else if (_iAmPrimary()) {
        log() << "I am already primary, " << hopeful->getHostAndPort().toString()
              << " can try again once I've stepped down";
        vote = -10000;
    } else if (primary) {
        log() << hopeful->getHostAndPort().toString() << " is trying to elect itself but "
              << primary->getHostAndPort().toString() << " is already primary";
        vote = -10000;
    } else if (highestPriority && highestPriority->getPriority() > hopeful->getPriority()) {
        // TODO(spencer): What if the lower-priority member is more up-to-date?
        log() << hopeful->getHostAndPort().toString() << " has lower priority than "
              << highestPriority->getHostAndPort().toString();
        vote = -10000;
    } else if (_voteLease.when + VoteLease::leaseTime >= now && _voteLease.whoId != args.whoid) {
        log() << "replSet voting no for " << hopeful->getHostAndPort().toString() << "; voted for "
              << _voteLease.whoHostAndPort.toString() << ' '
              << durationCount<Seconds>(now - _voteLease.when) << " secs ago";
    } else {
        _voteLease.when = now;
        _voteLease.whoId = args.whoid;
        _voteLease.whoHostAndPort = hopeful->getHostAndPort();
        vote = _selfConfig().getNumVotes();
        invariant(hopeful->getId() == args.whoid);
        if (vote > 0) {
            log() << "replSetElect voting yea for " << hopeful->getHostAndPort().toString() << " ("
                  << args.whoid << ')';
        }
    }

    response->append("vote", vote);
    response->append("round", args.round);
    *result = Status::OK();
}

// produce a reply to a heartbeat
Status TopologyCoordinatorImpl::prepareHeartbeatResponse(Date_t now,
                                                         const ReplSetHeartbeatArgs& args,
                                                         const std::string& ourSetName,
                                                         const OpTime& lastOpApplied,
                                                         const OpTime& lastOpDurable,
                                                         ReplSetHeartbeatResponse* response) {
    if (args.getProtocolVersion() != 1) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "replset: incompatible replset protocol version: "
                                    << args.getProtocolVersion());
    }

    // Verify that replica set names match
    const std::string rshb = args.getSetName();
    if (ourSetName != rshb) {
        log() << "replSet set names do not match, ours: " << ourSetName
              << "; remote node's: " << rshb;
        response->noteMismatched();
        return Status(ErrorCodes::InconsistentReplicaSetNames,
                      str::stream() << "Our set name of " << ourSetName << " does not match name "
                                    << rshb << " reported by remote node");
    }

    const MemberState myState = getMemberState();
    if (_selfIndex == -1) {
        if (myState.removed()) {
            return Status(ErrorCodes::InvalidReplicaSetConfig,
                          "Our replica set configuration is invalid or does not include us");
        }
    } else {
        invariant(_rsConfig.getReplSetName() == args.getSetName());
        if (args.getSenderId() == _selfConfig().getId()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Received heartbeat from member with the same "
                                           "member ID as ourself: " << args.getSenderId());
        }
    }

    // This is a replica set
    response->noteReplSet();

    response->setSetName(ourSetName);
    response->setState(myState.s);
    if (myState.primary()) {
        response->setElectionTime(_electionTime);
    }

    // Are we electable
    response->setElectable(!_getMyUnelectableReason(now, lastOpApplied));

    // Heartbeat status message
    response->setHbMsg(_getHbmsg(now));
    response->setTime(duration_cast<Seconds>(now - Date_t{}));
    response->setAppliedOpTime(lastOpApplied);
    response->setDurableOpTime(lastOpDurable);

    if (!_syncSource.empty()) {
        response->setSyncingTo(_syncSource);
    }

    if (!_rsConfig.isInitialized()) {
        response->setConfigVersion(-2);
        return Status::OK();
    }

    const long long v = _rsConfig.getConfigVersion();
    response->setConfigVersion(v);
    // Deliver new config if caller's version is older than ours
    if (v > args.getConfigVersion()) {
        response->setConfig(_rsConfig);
    }

    // Resolve the caller's id in our Member list
    int from = -1;
    if (v == args.getConfigVersion() && args.getSenderId() != -1) {
        from = _getMemberIndex(args.getSenderId());
    }
    if (from == -1) {
        // Can't find the member, so we leave out the stateDisagreement field
        return Status::OK();
    }
    invariant(from != _selfIndex);

    // if we thought that this node is down, let it know
    if (!_hbdata.at(from).up()) {
        response->noteStateDisagreement();
    }

    // note that we got a heartbeat from this node
    _hbdata.at(from).setLastHeartbeatRecv(now);
    return Status::OK();
}

Status TopologyCoordinatorImpl::prepareHeartbeatResponseV1(Date_t now,
                                                           const ReplSetHeartbeatArgsV1& args,
                                                           const std::string& ourSetName,
                                                           const OpTime& lastOpApplied,
                                                           const OpTime& lastOpDurable,
                                                           ReplSetHeartbeatResponse* response) {
    // Verify that replica set names match
    const std::string rshb = args.getSetName();
    if (ourSetName != rshb) {
        log() << "replSet set names do not match, ours: " << ourSetName
              << "; remote node's: " << rshb;
        return Status(ErrorCodes::InconsistentReplicaSetNames,
                      str::stream() << "Our set name of " << ourSetName << " does not match name "
                                    << rshb << " reported by remote node");
    }

    const MemberState myState = getMemberState();
    if (_selfIndex == -1) {
        if (myState.removed()) {
            return Status(ErrorCodes::InvalidReplicaSetConfig,
                          "Our replica set configuration is invalid or does not include us");
        }
    } else {
        if (args.getSenderId() == _selfConfig().getId()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Received heartbeat from member with the same "
                                           "member ID as ourself: " << args.getSenderId());
        }
    }

    response->setSetName(ourSetName);

    response->setState(myState.s);

    if (myState.primary()) {
        response->setElectionTime(_electionTime);
    }

    response->setAppliedOpTime(lastOpApplied);
    response->setDurableOpTime(lastOpDurable);

    if (_currentPrimaryIndex != -1) {
        response->setPrimaryId(_rsConfig.getMemberAt(_currentPrimaryIndex).getId());
    }

    response->setTerm(_term);

    if (!_syncSource.empty()) {
        response->setSyncingTo(_syncSource);
    }

    if (!_rsConfig.isInitialized()) {
        response->setConfigVersion(-2);
        return Status::OK();
    }

    const long long v = _rsConfig.getConfigVersion();
    response->setConfigVersion(v);
    // Deliver new config if caller's version is older than ours
    if (v > args.getConfigVersion()) {
        response->setConfig(_rsConfig);
    }

    // Resolve the caller's id in our Member list
    int from = -1;
    if (v == args.getConfigVersion() && args.getSenderId() != -1) {
        from = _getMemberIndex(args.getSenderId());
    }
    if (from == -1) {
        return Status::OK();
    }
    invariant(from != _selfIndex);

    // note that we got a heartbeat from this node
    _hbdata.at(from).setLastHeartbeatRecv(now);
    return Status::OK();
}

int TopologyCoordinatorImpl::_getMemberIndex(int id) const {
    int index = 0;
    for (ReplicaSetConfig::MemberIterator it = _rsConfig.membersBegin();
         it != _rsConfig.membersEnd();
         ++it, ++index) {
        if (it->getId() == id) {
            return index;
        }
    }
    return -1;
}

std::pair<ReplSetHeartbeatArgs, Milliseconds> TopologyCoordinatorImpl::prepareHeartbeatRequest(
    Date_t now, const std::string& ourSetName, const HostAndPort& target) {
    PingStats& hbStats = _pings[target];
    Milliseconds alreadyElapsed = now - hbStats.getLastHeartbeatStartDate();
    if (!_rsConfig.isInitialized() ||
        (hbStats.getNumFailuresSinceLastStart() > kMaxHeartbeatRetries) ||
        (alreadyElapsed >= _rsConfig.getHeartbeatTimeoutPeriodMillis())) {
        // This is either the first request ever for "target", or the heartbeat timeout has
        // passed, so we're starting a "new" heartbeat.
        hbStats.start(now);
        alreadyElapsed = Milliseconds(0);
    }
    ReplSetHeartbeatArgs hbArgs;
    hbArgs.setProtocolVersion(1);
    hbArgs.setCheckEmpty(false);
    if (_rsConfig.isInitialized()) {
        hbArgs.setSetName(_rsConfig.getReplSetName());
        hbArgs.setConfigVersion(_rsConfig.getConfigVersion());
        if (_selfIndex >= 0) {
            const MemberConfig& me = _selfConfig();
            hbArgs.setSenderHost(me.getHostAndPort());
            hbArgs.setSenderId(me.getId());
        }
    } else {
        hbArgs.setSetName(ourSetName);
        hbArgs.setConfigVersion(-2);
    }

    const Milliseconds timeoutPeriod(_rsConfig.isInitialized()
                                         ? _rsConfig.getHeartbeatTimeoutPeriodMillis()
                                         : ReplicaSetConfig::kDefaultHeartbeatTimeoutPeriod);
    const Milliseconds timeout = timeoutPeriod - alreadyElapsed;
    return std::make_pair(hbArgs, timeout);
}

std::pair<ReplSetHeartbeatArgsV1, Milliseconds> TopologyCoordinatorImpl::prepareHeartbeatRequestV1(
    Date_t now, const std::string& ourSetName, const HostAndPort& target) {
    PingStats& hbStats = _pings[target];
    Milliseconds alreadyElapsed(now.asInt64() - hbStats.getLastHeartbeatStartDate().asInt64());
    if (!_rsConfig.isInitialized() ||
        (hbStats.getNumFailuresSinceLastStart() > kMaxHeartbeatRetries) ||
        (alreadyElapsed >= _rsConfig.getHeartbeatTimeoutPeriodMillis())) {
        // This is either the first request ever for "target", or the heartbeat timeout has
        // passed, so we're starting a "new" heartbeat.
        hbStats.start(now);
        alreadyElapsed = Milliseconds(0);
    }
    ReplSetHeartbeatArgsV1 hbArgs;
    if (_rsConfig.isInitialized()) {
        hbArgs.setSetName(_rsConfig.getReplSetName());
        hbArgs.setConfigVersion(_rsConfig.getConfigVersion());
        if (_selfIndex >= 0) {
            const MemberConfig& me = _selfConfig();
            hbArgs.setSenderId(me.getId());
            hbArgs.setSenderHost(me.getHostAndPort());
        }
        hbArgs.setTerm(_term);
    } else {
        hbArgs.setSetName(ourSetName);
        // Config version -2 is for uninitialized config.
        hbArgs.setConfigVersion(-2);
        hbArgs.setTerm(OpTime::kInitialTerm);
    }

    const Milliseconds timeoutPeriod(_rsConfig.isInitialized()
                                         ? _rsConfig.getHeartbeatTimeoutPeriodMillis()
                                         : ReplicaSetConfig::kDefaultHeartbeatTimeoutPeriod);
    const Milliseconds timeout(timeoutPeriod - alreadyElapsed);
    return std::make_pair(hbArgs, timeout);
}

HeartbeatResponseAction TopologyCoordinatorImpl::processHeartbeatResponse(
    Date_t now,
    Milliseconds networkRoundTripTime,
    const HostAndPort& target,
    const StatusWith<ReplSetHeartbeatResponse>& hbResponse,
    const OpTime& myLastOpApplied) {
    const MemberState originalState = getMemberState();
    PingStats& hbStats = _pings[target];
    invariant(hbStats.getLastHeartbeatStartDate() != Date_t());
    const bool isUnauthorized = (hbResponse.getStatus().code() == ErrorCodes::Unauthorized) ||
        (hbResponse.getStatus().code() == ErrorCodes::AuthenticationFailed);
    if (!hbResponse.isOK()) {
        if (isUnauthorized) {
            hbStats.hit(networkRoundTripTime);
        } else {
            hbStats.miss();
        }
    } else {
        hbStats.hit(networkRoundTripTime);
        // Log diagnostics.
        if (hbResponse.getValue().isStateDisagreement()) {
            LOG(1) << target << " thinks that we are down because they cannot send us heartbeats.";
        }
    }

    // If a node is not PRIMARY and has no sync source,
    // we increase the heartbeat rate in order
    // to help it find a sync source more quickly,
    // which helps ensure the PRIMARY will continue to
    // see the majority of the cluster.
    //
    // Arbiters also use half the election timeout period for their heartbeat frequency
    Milliseconds heartbeatInterval;
    if (_rsConfig.getProtocolVersion() == 1 &&
        (getMemberState().arbiter() || (getSyncSourceAddress().empty() && !_iAmPrimary()))) {
        heartbeatInterval = _rsConfig.getElectionTimeoutPeriod() / 2;
    } else {
        heartbeatInterval = _rsConfig.getHeartbeatInterval();
    }

    const Milliseconds alreadyElapsed = now - hbStats.getLastHeartbeatStartDate();
    Date_t nextHeartbeatStartDate;
    // Determine next heartbeat start time.
    if (hbStats.getNumFailuresSinceLastStart() <= kMaxHeartbeatRetries &&
        alreadyElapsed < _rsConfig.getHeartbeatTimeoutPeriod()) {
        // There are still retries left, let's use one.
        nextHeartbeatStartDate = now;
    } else {
        nextHeartbeatStartDate = now + heartbeatInterval;
    }

    if (hbResponse.isOK() && hbResponse.getValue().hasConfig()) {
        const long long currentConfigVersion =
            _rsConfig.isInitialized() ? _rsConfig.getConfigVersion() : -2;
        const ReplicaSetConfig& newConfig = hbResponse.getValue().getConfig();
        if (newConfig.getConfigVersion() > currentConfigVersion) {
            HeartbeatResponseAction nextAction = HeartbeatResponseAction::makeReconfigAction();
            nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
            return nextAction;
        } else {
            // Could be we got the newer version before we got the response, or the
            // target erroneously sent us one, even through it isn't newer.
            if (newConfig.getConfigVersion() < currentConfigVersion) {
                LOG(1) << "Config version from heartbeat was older than ours.";
            } else {
                LOG(2) << "Config from heartbeat response was same as ours.";
            }
            if (logger::globalLogDomain()->shouldLog(MongoLogDefaultComponent_component,
                                                     ::mongo::LogstreamBuilder::severityCast(2))) {
                LogstreamBuilder lsb = log();
                if (_rsConfig.isInitialized()) {
                    lsb << "Current config: " << _rsConfig.toBSON() << "; ";
                }
                lsb << "Config in heartbeat: " << newConfig.toBSON();
            }
        }
    }

    // Check if the heartbeat target is in our config.  If it isn't, there's nothing left to do,
    // so return early.
    if (!_rsConfig.isInitialized()) {
        HeartbeatResponseAction nextAction = HeartbeatResponseAction::makeNoAction();
        nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
        return nextAction;
    }
    const int memberIndex = _rsConfig.findMemberIndexByHostAndPort(target);
    if (memberIndex == -1) {
        LOG(1) << "Could not find " << target << " in current config so ignoring --"
                                                 " current config: " << _rsConfig.toBSON();
        HeartbeatResponseAction nextAction = HeartbeatResponseAction::makeNoAction();
        nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
        return nextAction;
    }

    invariant(memberIndex != _selfIndex);

    MemberHeartbeatData& hbData = _hbdata.at(memberIndex);
    const MemberConfig member = _rsConfig.getMemberAt(memberIndex);
    if (!hbResponse.isOK()) {
        if (isUnauthorized) {
            LOG(1) << "setAuthIssue: heartbeat response failed due to authentication"
                      " issue for member _id:" << member.getId();
            hbData.setAuthIssue(now);
        } else if (hbStats.getNumFailuresSinceLastStart() > kMaxHeartbeatRetries ||
                   alreadyElapsed >= _rsConfig.getHeartbeatTimeoutPeriod()) {
            LOG(1) << "setDownValues: heartbeat response failed for member _id:" << member.getId()
                   << ", msg:  " << hbResponse.getStatus().reason();

            hbData.setDownValues(now, hbResponse.getStatus().reason());
        } else {
            LOG(3) << "Bad heartbeat response from " << target << "; trying again; Retries left: "
                   << (kMaxHeartbeatRetries - hbStats.getNumFailuresSinceLastStart()) << "; "
                   << alreadyElapsed << " have already elapsed";
        }
    } else {
        ReplSetHeartbeatResponse hbr = std::move(hbResponse.getValue());
        LOG(3) << "setUpValues: heartbeat response good for member _id:" << member.getId()
               << ", msg:  " << hbr.getHbMsg();
        hbData.setUpValues(now, member.getHostAndPort(), std::move(hbr));
    }

    HeartbeatResponseAction nextAction;
    if (_rsConfig.getProtocolVersion() == 0) {
        nextAction = _updatePrimaryFromHBData(memberIndex, originalState, now, myLastOpApplied);
    } else {
        nextAction = _updatePrimaryFromHBDataV1(memberIndex, originalState, now, myLastOpApplied);
    }

    nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
    return nextAction;
}

HeartbeatResponseAction TopologyCoordinatorImpl::setMemberAsDown(Date_t now,
                                                                 const int memberIndex,
                                                                 const OpTime& myLastOpApplied) {
    invariant(memberIndex != _selfIndex);
    invariant(memberIndex != -1);
    invariant(_currentPrimaryIndex == _selfIndex);
    MemberHeartbeatData& hbData = _hbdata.at(memberIndex);
    hbData.setDownValues(now, "no response within election timeout period");

    if (CannotSeeMajority & _getMyUnelectableReason(now, myLastOpApplied)) {
        if (_stepDownPending) {
            return HeartbeatResponseAction::makeNoAction();
        }
        _stepDownPending = true;
        log() << "can't see a majority of the set, relinquishing primary";
        return HeartbeatResponseAction::makeStepDownSelfAction(_selfIndex);
    }

    return HeartbeatResponseAction::makeNoAction();
}

HeartbeatResponseAction TopologyCoordinatorImpl::_updatePrimaryFromHBDataV1(
    int updatedConfigIndex,
    const MemberState& originalState,
    Date_t now,
    const OpTime& lastOpApplied) {
    //
    // Updates the local notion of which remote node, if any is primary.
    // Start the priority takeover process if we are eligible.
    //

    invariant(updatedConfigIndex != _selfIndex);

    // If we are missing from the config, do not participate in primary maintenance or election.
    if (_selfIndex == -1) {
        return HeartbeatResponseAction::makeNoAction();
    }
    // If we are the primary, there must be no other primary, otherwise its higher term would
    // have already made us step down.
    if (_currentPrimaryIndex == _selfIndex) {
        return HeartbeatResponseAction::makeNoAction();
    }

    // Scan the member list's heartbeat data for who is primary, and update _currentPrimaryIndex.
    int primaryIndex = -1;
    for (size_t i = 0; i < _hbdata.size(); i++) {
        const MemberHeartbeatData& member = _hbdata.at(i);
        if (member.getState().primary() && member.up()) {
            if (primaryIndex == -1 || _hbdata.at(primaryIndex).getTerm() < member.getTerm()) {
                primaryIndex = i;
            }
        }
    }
    _currentPrimaryIndex = primaryIndex;
    if (_currentPrimaryIndex == -1) {
        return HeartbeatResponseAction::makeNoAction();
    }

    // Clear last heartbeat message on ourselves.
    setMyHeartbeatMessage(now, "");

    // Priority takeover when the replset is stable.
    //
    // Take over the primary only if the remote primary is in the latest term I know.
    // This is done only when we get a heartbeat response from the primary.
    // Otherwise, there must be an outstanding election, which may succeed or not, but
    // the remote primary will become aware of that election eventually and step down.
    if (_hbdata.at(primaryIndex).getTerm() == _term && updatedConfigIndex == primaryIndex &&
        _rsConfig.getMemberAt(primaryIndex).getPriority() <
            _rsConfig.getMemberAt(_selfIndex).getPriority()) {
        LOG(4) << "I can take over the primary due to higher priority."
               << " Current primary index: " << primaryIndex << " in term "
               << _hbdata.at(primaryIndex).getTerm();

        return HeartbeatResponseAction::makePriorityTakeoverAction();
    }
    return HeartbeatResponseAction::makeNoAction();
}

HeartbeatResponseAction TopologyCoordinatorImpl::_updatePrimaryFromHBData(
    int updatedConfigIndex,
    const MemberState& originalState,
    Date_t now,
    const OpTime& lastOpApplied) {
    // This method has two interrelated responsibilities, performed in two phases.
    //
    // First, it updates the local notion of which remote node, if any is primary.  In the
    // process, it may request a remote primary to step down because there is a higher priority
    // node waiting, or because the local node thinks it is primary and that it has a more
    // recent electionTime.  It may instead decide that the local node should step down itself,
    // because a remote has a more recent election time.
    //
    // Second, if there is no remote primary, and the local node is not primary, it considers
    // whether or not to stand for election.
    invariant(updatedConfigIndex != _selfIndex);

    // We are missing from the config, so do not participate in primary maintenance or election.
    if (_selfIndex == -1) {
        return HeartbeatResponseAction::makeNoAction();
    }

    ////////////////////
    // Phase 1
    ////////////////////

    // If we believe the node whose data was just updated is primary, confirm that
    // the updated data supports that notion.  If not, erase our notion of who is primary.
    if (updatedConfigIndex == _currentPrimaryIndex) {
        const MemberHeartbeatData& updatedHBData = _hbdata.at(updatedConfigIndex);
        if (!updatedHBData.up() || !updatedHBData.getState().primary()) {
            _currentPrimaryIndex = -1;
        }
    }

    // If the current primary is not highest priority and up to date (within 10s),
    // have them/me stepdown.
    if (_currentPrimaryIndex != -1) {
        // check if we should ask the primary (possibly ourselves) to step down
        const int highestPriorityIndex = _getHighestPriorityElectableIndex(now, lastOpApplied);
        if (highestPriorityIndex != -1) {
            const MemberConfig& currentPrimaryMember = _rsConfig.getMemberAt(_currentPrimaryIndex);
            const MemberConfig& highestPriorityMember = _rsConfig.getMemberAt(highestPriorityIndex);
            const OpTime highestPriorityMemberOptime = highestPriorityIndex == _selfIndex
                ? lastOpApplied
                : _hbdata.at(highestPriorityIndex).getAppliedOpTime();

            if ((highestPriorityMember.getPriority() > currentPrimaryMember.getPriority()) &&
                _isOpTimeCloseEnoughToLatestToElect(highestPriorityMemberOptime, lastOpApplied)) {
                const OpTime latestOpTime = _latestKnownOpTime(lastOpApplied);

                if (_iAmPrimary()) {
                    if (_stepDownPending) {
                        return HeartbeatResponseAction::makeNoAction();
                    }
                    _stepDownPending = true;
                    log() << "Stepping down self (priority " << currentPrimaryMember.getPriority()
                          << ") because " << highestPriorityMember.getHostAndPort()
                          << " has higher priority " << highestPriorityMember.getPriority()
                          << " and is only "
                          << (latestOpTime.getSecs() - highestPriorityMemberOptime.getSecs())
                          << " seconds behind me";
                    const Date_t until =
                        now + VoteLease::leaseTime + _rsConfig.getHeartbeatInterval();
                    if (_electionSleepUntil < until) {
                        _electionSleepUntil = until;
                    }
                    return HeartbeatResponseAction::makeStepDownSelfAction(_selfIndex);
                } else if ((highestPriorityIndex == _selfIndex) && (_electionSleepUntil <= now)) {
                    // If this node is the highest priority node, and it is not in
                    // an inter-election sleep period, ask the current primary to step down.
                    // This is an optimization, because the remote primary will almost certainly
                    // notice this node's electability promptly, via its own heartbeat process.
                    log() << "Requesting that " << currentPrimaryMember.getHostAndPort()
                          << " (priority " << currentPrimaryMember.getPriority()
                          << ") step down because I have higher priority "
                          << highestPriorityMember.getPriority() << " and am only "
                          << (latestOpTime.getSecs() - highestPriorityMemberOptime.getSecs())
                          << " seconds behind it";
                    int primaryIndex = _currentPrimaryIndex;
                    _currentPrimaryIndex = -1;
                    return HeartbeatResponseAction::makeStepDownRemoteAction(primaryIndex);
                }
            }
        }
    }

    // Scan the member list's heartbeat data for who is primary, and update
    // _currentPrimaryIndex and _role, or request a remote to step down, as necessary.
    {
        int remotePrimaryIndex = -1;
        for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin();
             it != _hbdata.end();
             ++it) {
            const int itIndex = indexOfIterator(_hbdata, it);
            if (itIndex == _selfIndex) {
                continue;
            }

            if (it->getState().primary() && it->up()) {
                if (remotePrimaryIndex != -1) {
                    // two other nodes think they are primary (asynchronously polled)
                    // -- wait for things to settle down.
                    warning() << "two remote primaries (transiently)";
                    return HeartbeatResponseAction::makeNoAction();
                }
                remotePrimaryIndex = itIndex;
            }
        }

        if (remotePrimaryIndex != -1) {
            // If it's the same as last time, don't do anything further.
            if (_currentPrimaryIndex == remotePrimaryIndex) {
                return HeartbeatResponseAction::makeNoAction();
            }
            // Clear last heartbeat message on ourselves (why?)
            setMyHeartbeatMessage(now, "");

            // If we are also primary, this is a problem.  Determine who should step down.
            if (_iAmPrimary()) {
                Timestamp remoteElectionTime = _hbdata.at(remotePrimaryIndex).getElectionTime();
                log() << "another primary seen with election time " << remoteElectionTime
                      << " my election time is " << _electionTime;

                // Step down whomever has the older election time.
                if (remoteElectionTime > _electionTime) {
                    if (_stepDownPending) {
                        return HeartbeatResponseAction::makeNoAction();
                    }
                    _stepDownPending = true;
                    log() << "stepping down; another primary was elected more recently";
                    return HeartbeatResponseAction::makeStepDownSelfAction(_selfIndex);
                } else {
                    log() << "another PRIMARY detected and it should step down"
                             " since it was elected earlier than me";
                    return HeartbeatResponseAction::makeStepDownRemoteAction(remotePrimaryIndex);
                }
            }

            _currentPrimaryIndex = remotePrimaryIndex;
            return HeartbeatResponseAction::makeNoAction();
        }
    }

    ////////////////////
    // Phase 2
    ////////////////////

    // We do not believe any remote to be primary.

    // If we are primary, check if we can still see majority of the set;
    // stepdown if we can't.
    if (_iAmPrimary()) {
        if (CannotSeeMajority & _getMyUnelectableReason(now, lastOpApplied)) {
            if (_stepDownPending) {
                return HeartbeatResponseAction::makeNoAction();
            }
            _stepDownPending = true;
            log() << "can't see a majority of the set, relinquishing primary";
            return HeartbeatResponseAction::makeStepDownSelfAction(_selfIndex);
        }

        LOG(2) << "Choosing to remain primary";
        return HeartbeatResponseAction::makeNoAction();
    }

    fassert(18505, _currentPrimaryIndex == -1);

    const MemberState currentState = getMemberState();
    if (originalState.recovering() && currentState.secondary()) {
        // We just transitioned from RECOVERING to SECONDARY, this can only happen if we
        // received a heartbeat with an auth error when previously all the heartbeats we'd
        // received had auth errors.  In this case, don't return makeElectAction() because
        // that could cause the election to start before the ReplicationCoordinator has updated
        // its notion of the member state to SECONDARY.  Instead return noAction so that the
        // ReplicationCooridinator knows to update its tracking of the member state off of the
        // TopologyCoordinator, and leave starting the election until the next heartbeat comes
        // back.
        return HeartbeatResponseAction::makeNoAction();
    }

    // At this point, there is no primary anywhere.  Check to see if we should become a
    // candidate.
    if (!checkShouldStandForElection(now, lastOpApplied)) {
        return HeartbeatResponseAction::makeNoAction();
    }
    fassert(28816, becomeCandidateIfElectable(now, lastOpApplied));
    return HeartbeatResponseAction::makeElectAction();
}

bool TopologyCoordinatorImpl::checkShouldStandForElection(Date_t now,
                                                          const OpTime& lastOpApplied) const {
    if (_currentPrimaryIndex != -1) {
        return false;
    }
    invariant(_role != Role::leader);

    if (_role == Role::candidate) {
        LOG(2) << "Not standing for election again; already candidate";
        return false;
    }

    const UnelectableReasonMask unelectableReason = _getMyUnelectableReason(now, lastOpApplied);
    if (NotCloseEnoughToLatestOptime & unelectableReason) {
        LOG(2) << "Not standing for election because "
               << _getUnelectableReasonString(unelectableReason) << "; my last optime is "
               << lastOpApplied << " and the newest is " << _latestKnownOpTime(lastOpApplied);
        return false;
    }
    if (unelectableReason) {
        LOG(2) << "Not standing for election because "
               << _getUnelectableReasonString(unelectableReason);
        return false;
    }
    if (_electionSleepUntil > now) {
        if (_rsConfig.getProtocolVersion() == 1) {
            LOG(2) << "Not standing for election before "
                   << dateToISOStringLocal(_electionSleepUntil)
                   << " because I stood up or learned about a new term too recently";
        } else {
            LOG(2) << "Not standing for election before "
                   << dateToISOStringLocal(_electionSleepUntil) << " because I stood too recently";
        }
        return false;
    }
    // All checks passed. Start election proceedings.
    return true;
}

bool TopologyCoordinatorImpl::_aMajoritySeemsToBeUp() const {
    int vUp = 0;
    for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin(); it != _hbdata.end();
         ++it) {
        const int itIndex = indexOfIterator(_hbdata, it);
        if (itIndex == _selfIndex || it->up()) {
            vUp += _rsConfig.getMemberAt(itIndex).getNumVotes();
        }
    }

    return vUp * 2 > _rsConfig.getTotalVotingMembers();
}

bool TopologyCoordinatorImpl::_isOpTimeCloseEnoughToLatestToElect(
    const OpTime& otherOpTime, const OpTime& ourLastOpApplied) const {
    const OpTime latestKnownOpTime = _latestKnownOpTime(ourLastOpApplied);
    // Use addition instead of subtraction to avoid overflow.
    return otherOpTime.getSecs() + 10 >= (latestKnownOpTime.getSecs());
}

bool TopologyCoordinatorImpl::_iAmPrimary() const {
    if (_role == Role::leader) {
        invariant(_currentPrimaryIndex == _selfIndex);
        return true;
    }
    return false;
}

OpTime TopologyCoordinatorImpl::_latestKnownOpTime(const OpTime& ourLastOpApplied) const {
    OpTime latest = ourLastOpApplied;

    for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin(); it != _hbdata.end();
         ++it) {
        if (indexOfIterator(_hbdata, it) == _selfIndex) {
            continue;
        }

        // Ignore down members
        if (!it->up()) {
            continue;
        }
        // Ignore removed nodes (not in config, so not valid).
        if (it->getState().removed()) {
            continue;
        }

        OpTime optime = it->getAppliedOpTime();

        if (optime > latest) {
            latest = optime;
        }
    }

    return latest;
}

bool TopologyCoordinatorImpl::_isMemberHigherPriority(int memberOneIndex,
                                                      int memberTwoIndex) const {
    if (memberOneIndex == -1)
        return false;

    if (memberTwoIndex == -1)
        return true;

    return _rsConfig.getMemberAt(memberOneIndex).getPriority() >
        _rsConfig.getMemberAt(memberTwoIndex).getPriority();
}

int TopologyCoordinatorImpl::_getHighestPriorityElectableIndex(Date_t now,
                                                               const OpTime& lastOpApplied) const {
    int maxIndex = -1;
    for (int currentIndex = 0; currentIndex < _rsConfig.getNumMembers(); currentIndex++) {
        UnelectableReasonMask reason = currentIndex == _selfIndex
            ? _getMyUnelectableReason(now, lastOpApplied)
            : _getUnelectableReason(currentIndex, lastOpApplied);
        if (None == reason && _isMemberHigherPriority(currentIndex, maxIndex)) {
            maxIndex = currentIndex;
        }
    }

    return maxIndex;
}

void TopologyCoordinatorImpl::prepareForStepDown() {
    _stepDownPending = true;
}

void TopologyCoordinatorImpl::changeMemberState_forTest(const MemberState& newMemberState,
                                                        const Timestamp& electionTime) {
    invariant(_selfIndex != -1);
    if (newMemberState == getMemberState())
        return;
    switch (newMemberState.s) {
        case MemberState::RS_PRIMARY:
            _role = Role::candidate;
            processWinElection(OID(), electionTime);
            invariant(_role == Role::leader);
            break;
        case MemberState::RS_SECONDARY:
        case MemberState::RS_ROLLBACK:
        case MemberState::RS_RECOVERING:
        case MemberState::RS_STARTUP2:
            _role = Role::follower;
            _followerMode = newMemberState.s;
            if (_currentPrimaryIndex == _selfIndex) {
                _currentPrimaryIndex = -1;
                _stepDownPending = false;
            }
            break;
        case MemberState::RS_STARTUP:
            updateConfig(ReplicaSetConfig(), -1, Date_t(), OpTime());
            break;
        default:
            severe() << "Cannot switch to state " << newMemberState;
            invariant(false);
    }
    if (getMemberState() != newMemberState.s) {
        severe() << "Expected to enter state " << newMemberState << " but am now in "
                 << getMemberState();
        invariant(false);
    }
    log() << newMemberState;
}

void TopologyCoordinatorImpl::_setCurrentPrimaryForTest(int primaryIndex) {
    if (primaryIndex == _selfIndex) {
        changeMemberState_forTest(MemberState::RS_PRIMARY);
    } else {
        if (_iAmPrimary()) {
            changeMemberState_forTest(MemberState::RS_SECONDARY);
        }
        if (primaryIndex != -1) {
            ReplSetHeartbeatResponse hbResponse;
            hbResponse.setState(MemberState::RS_PRIMARY);
            hbResponse.setElectionTime(Timestamp());
            hbResponse.setAppliedOpTime(_hbdata.at(primaryIndex).getAppliedOpTime());
            hbResponse.setSyncingTo(HostAndPort());
            hbResponse.setHbMsg("");
            _hbdata.at(primaryIndex)
                .setUpValues(_hbdata.at(primaryIndex).getLastHeartbeat(),
                             _rsConfig.getMemberAt(primaryIndex).getHostAndPort(),
                             std::move(hbResponse));
        }
        _currentPrimaryIndex = primaryIndex;
    }
}

const MemberConfig* TopologyCoordinatorImpl::_currentPrimaryMember() const {
    if (_currentPrimaryIndex == -1)
        return NULL;

    return &(_rsConfig.getMemberAt(_currentPrimaryIndex));
}

void TopologyCoordinatorImpl::prepareStatusResponse(const ReplicationExecutor::CallbackArgs& data,
                                                    const ReplSetStatusArgs& rsStatusArgs,
                                                    BSONObjBuilder* response,
                                                    Status* result) {
    if (data.status == ErrorCodes::CallbackCanceled) {
        *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
        return;
    }

    // output for each member
    vector<BSONObj> membersOut;
    const MemberState myState = getMemberState();
    const Date_t now = rsStatusArgs.now;
    const OpTime& lastOpApplied = rsStatusArgs.lastOpApplied;

    if (_selfIndex == -1) {
        // We're REMOVED or have an invalid config
        response->append("state", static_cast<int>(myState.s));
        response->append("stateStr", myState.toString());
        response->append("uptime", rsStatusArgs.selfUptime);

        if (_rsConfig.getProtocolVersion() == 1) {
            BSONObjBuilder opTime(response->subobjStart("optime"));
            opTime.append("ts", lastOpApplied.getTimestamp());
            opTime.append("t", lastOpApplied.getTerm());
            opTime.done();
        } else {
            response->append("optime", lastOpApplied.getTimestamp());
        }

        response->appendDate("optimeDate",
                             Date_t::fromDurationSinceEpoch(Seconds(lastOpApplied.getSecs())));
        if (_maintenanceModeCalls) {
            response->append("maintenanceMode", _maintenanceModeCalls);
        }
        std::string s = _getHbmsg(now);
        if (!s.empty())
            response->append("infoMessage", s);
        *result = Status(ErrorCodes::InvalidReplicaSetConfig,
                         "Our replica set config is invalid or we are not a member of it");
        return;
    }

    for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin(); it != _hbdata.end();
         ++it) {
        const int itIndex = indexOfIterator(_hbdata, it);
        if (itIndex == _selfIndex) {
            // add self
            BSONObjBuilder bb;
            bb.append("_id", _selfConfig().getId());
            bb.append("name", _selfConfig().getHostAndPort().toString());
            bb.append("health", 1.0);
            bb.append("state", static_cast<int>(myState.s));
            bb.append("stateStr", myState.toString());
            bb.append("uptime", rsStatusArgs.selfUptime);
            if (!_selfConfig().isArbiter()) {
                if (_rsConfig.getProtocolVersion() == 1) {
                    BSONObjBuilder opTime(bb.subobjStart("optime"));
                    opTime.append("ts", lastOpApplied.getTimestamp());
                    opTime.append("t", lastOpApplied.getTerm());
                    opTime.done();
                } else {
                    bb.append("optime", lastOpApplied.getTimestamp());
                }

                bb.appendDate("optimeDate",
                              Date_t::fromDurationSinceEpoch(Seconds(lastOpApplied.getSecs())));
            }

            if (!_syncSource.empty() && !_iAmPrimary()) {
                bb.append("syncingTo", _syncSource.toString());
            }

            if (_maintenanceModeCalls) {
                bb.append("maintenanceMode", _maintenanceModeCalls);
            }

            std::string s = _getHbmsg(now);
            if (!s.empty())
                bb.append("infoMessage", s);

            if (myState.primary()) {
                bb.append("electionTime", _electionTime);
                bb.appendDate("electionDate",
                              Date_t::fromDurationSinceEpoch(Seconds(_electionTime.getSecs())));
            }
            bb.appendIntOrLL("configVersion", _rsConfig.getConfigVersion());
            bb.append("self", true);
            membersOut.push_back(bb.obj());
        } else {
            // add non-self member
            const MemberConfig& itConfig = _rsConfig.getMemberAt(itIndex);
            BSONObjBuilder bb;
            bb.append("_id", itConfig.getId());
            bb.append("name", itConfig.getHostAndPort().toString());
            double h = it->getHealth();
            bb.append("health", h);
            const MemberState state = it->getState();
            bb.append("state", static_cast<int>(state.s));
            if (h == 0) {
                // if we can't connect the state info is from the past
                // and could be confusing to show
                bb.append("stateStr", "(not reachable/healthy)");
            } else {
                bb.append("stateStr", it->getState().toString());
            }

            const unsigned int uptime = static_cast<unsigned int>((
                it->getUpSince() != Date_t() ? durationCount<Seconds>(now - it->getUpSince()) : 0));
            bb.append("uptime", uptime);
            if (!itConfig.isArbiter()) {
                if (_rsConfig.getProtocolVersion() == 1) {
                    BSONObjBuilder opTime(bb.subobjStart("optime"));
                    opTime.append("ts", it->getAppliedOpTime().getTimestamp());
                    opTime.append("t", it->getAppliedOpTime().getTerm());
                    opTime.done();
                } else {
                    bb.append("optime", it->getAppliedOpTime().getTimestamp());
                }

                bb.appendDate(
                    "optimeDate",
                    Date_t::fromDurationSinceEpoch(Seconds(it->getAppliedOpTime().getSecs())));
            }
            bb.appendDate("lastHeartbeat", it->getLastHeartbeat());
            bb.appendDate("lastHeartbeatRecv", it->getLastHeartbeatRecv());
            Milliseconds ping = _getPing(itConfig.getHostAndPort());
            if (ping != UninitializedPing) {
                bb.append("pingMs", durationCount<Milliseconds>(ping));
                std::string s = it->getLastHeartbeatMsg();
                if (!s.empty())
                    bb.append("lastHeartbeatMessage", s);
            }
            if (it->hasAuthIssue()) {
                bb.append("authenticated", false);
            }
            const HostAndPort& syncSource = it->getSyncSource();
            if (!syncSource.empty() && !state.primary()) {
                bb.append("syncingTo", syncSource.toString());
            }

            if (state == MemberState::RS_PRIMARY) {
                bb.append("electionTime", it->getElectionTime());
                bb.appendDate(
                    "electionDate",
                    Date_t::fromDurationSinceEpoch(Seconds(it->getElectionTime().getSecs())));
            }
            bb.appendIntOrLL("configVersion", it->getConfigVersion());
            membersOut.push_back(bb.obj());
        }
    }

    // sort members bson
    sort(membersOut.begin(), membersOut.end());

    response->append("set", _rsConfig.isInitialized() ? _rsConfig.getReplSetName() : "");
    response->append("date", now);
    response->append("myState", myState.s);
    response->append("term", _term);

    // Add sync source info
    if (!_syncSource.empty() && !myState.primary() && !myState.removed()) {
        response->append("syncingTo", _syncSource.toString());
    }

    if (_rsConfig.isConfigServer()) {
        response->append("configsvr", true);
    }

    response->append("heartbeatIntervalMillis",
                     durationCount<Milliseconds>(_rsConfig.getHeartbeatInterval()));

    BSONObjBuilder optimes;
    rsStatusArgs.lastCommittedOpTime.append(&optimes, "lastCommittedOpTime");
    if (!rsStatusArgs.readConcernMajorityOpTime.isNull()) {
        rsStatusArgs.readConcernMajorityOpTime.append(&optimes, "readConcernMajorityOpTime");
    }
    response->append("OpTimes", optimes.obj());

    response->append("members", membersOut);
    *result = Status::OK();
}

void TopologyCoordinatorImpl::fillIsMasterForReplSet(IsMasterResponse* response) {
    const MemberState myState = getMemberState();
    if (!_rsConfig.isInitialized()) {
        response->markAsNoConfig();
        return;
    }

    for (ReplicaSetConfig::MemberIterator it = _rsConfig.membersBegin();
         it != _rsConfig.membersEnd();
         ++it) {
        if (it->isHidden() || it->getSlaveDelay() > Seconds{0}) {
            continue;
        }

        if (it->isElectable()) {
            response->addHost(it->getHostAndPort());
        } else if (it->isArbiter()) {
            response->addArbiter(it->getHostAndPort());
        } else {
            response->addPassive(it->getHostAndPort());
        }
    }

    response->setReplSetName(_rsConfig.getReplSetName());
    if (myState.removed()) {
        response->markAsNoConfig();
        return;
    }

    response->setReplSetVersion(_rsConfig.getConfigVersion());
    response->setIsMaster(myState.primary());
    response->setIsSecondary(myState.secondary());

    const MemberConfig* curPrimary = _currentPrimaryMember();
    if (curPrimary) {
        response->setPrimary(curPrimary->getHostAndPort());
    }

    const MemberConfig& selfConfig = _rsConfig.getMemberAt(_selfIndex);
    if (selfConfig.isArbiter()) {
        response->setIsArbiterOnly(true);
    } else if (selfConfig.getPriority() == 0) {
        response->setIsPassive(true);
    }
    if (selfConfig.getSlaveDelay() > Seconds(0)) {
        response->setSlaveDelay(selfConfig.getSlaveDelay());
    }
    if (selfConfig.isHidden()) {
        response->setIsHidden(true);
    }
    if (!selfConfig.shouldBuildIndexes()) {
        response->setShouldBuildIndexes(false);
    }
    const ReplicaSetTagConfig tagConfig = _rsConfig.getTagConfig();
    if (selfConfig.hasTags(tagConfig)) {
        for (MemberConfig::TagIterator tag = selfConfig.tagsBegin(); tag != selfConfig.tagsEnd();
             ++tag) {
            std::string tagKey = tagConfig.getTagKey(*tag);
            if (tagKey[0] == '$') {
                // Filter out internal tags
                continue;
            }
            response->addTag(tagKey, tagConfig.getTagValue(*tag));
        }
    }
    response->setMe(selfConfig.getHostAndPort());
    if (_iAmPrimary()) {
        response->setElectionId(_electionId);
    }
}

void TopologyCoordinatorImpl::prepareFreezeResponse(Date_t now,
                                                    int secs,
                                                    BSONObjBuilder* response) {
    if (secs == 0) {
        _stepDownUntil = now;
        log() << "'unfreezing'";
        response->append("info", "unfreezing");

        if (_followerMode == MemberState::RS_SECONDARY && _rsConfig.getNumMembers() == 1 &&
            _selfIndex == 0 && _rsConfig.getMemberAt(_selfIndex).isElectable()) {
            // If we are a one-node replica set, we're the one member,
            // we're electable, and we are currently in followerMode SECONDARY,
            // we must transition to candidate now that our stepdown period
            // is no longer active, in leiu of heartbeats.
            _role = Role::candidate;
        }
    } else {
        if (secs == 1)
            response->append("warning", "you really want to freeze for only 1 second?");

        if (!_iAmPrimary()) {
            _stepDownUntil = std::max(_stepDownUntil, now + Seconds(secs));
            log() << "'freezing' for " << secs << " seconds";
        } else {
            log() << "received freeze command but we are primary";
        }
    }
}

bool TopologyCoordinatorImpl::becomeCandidateIfStepdownPeriodOverAndSingleNodeSet(Date_t now) {
    if (_stepDownUntil > now) {
        return false;
    }

    if (_followerMode == MemberState::RS_SECONDARY && _rsConfig.getNumMembers() == 1 &&
        _selfIndex == 0 && _rsConfig.getMemberAt(_selfIndex).isElectable()) {
        // If the new config describes a one-node replica set, we're the one member,
        // we're electable, and we are currently in followerMode SECONDARY,
        // we must transition to candidate, in leiu of heartbeats.
        _role = Role::candidate;
        return true;
    }
    return false;
}

void TopologyCoordinatorImpl::setElectionSleepUntil(Date_t newTime) {
    if (_electionSleepUntil < newTime) {
        _electionSleepUntil = newTime;
    }
}

Timestamp TopologyCoordinatorImpl::getElectionTime() const {
    return _electionTime;
}

OID TopologyCoordinatorImpl::getElectionId() const {
    return _electionId;
}

int TopologyCoordinatorImpl::getCurrentPrimaryIndex() const {
    return _currentPrimaryIndex;
}

Date_t TopologyCoordinatorImpl::getStepDownTime() const {
    return _stepDownUntil;
}

void TopologyCoordinatorImpl::_updateHeartbeatDataForReconfig(const ReplicaSetConfig& newConfig,
                                                              int selfIndex,
                                                              Date_t now) {
    std::vector<MemberHeartbeatData> oldHeartbeats;
    _hbdata.swap(oldHeartbeats);

    int index = 0;
    for (ReplicaSetConfig::MemberIterator it = newConfig.membersBegin();
         it != newConfig.membersEnd();
         ++it, ++index) {
        const MemberConfig& newMemberConfig = *it;
        // TODO: C++11: use emplace_back()
        if (index == selfIndex) {
            // Insert placeholder for ourself, though we will never consult it.
            _hbdata.push_back(MemberHeartbeatData());
        } else {
            MemberHeartbeatData newHeartbeatData;
            for (int oldIndex = 0; oldIndex < _rsConfig.getNumMembers(); ++oldIndex) {
                const MemberConfig& oldMemberConfig = _rsConfig.getMemberAt(oldIndex);
                if (oldMemberConfig.getId() == newMemberConfig.getId() &&
                    oldMemberConfig.getHostAndPort() == newMemberConfig.getHostAndPort()) {
                    // This member existed in the old config with the same member ID and
                    // HostAndPort, so copy its heartbeat data over.
                    newHeartbeatData = oldHeartbeats[oldIndex];
                    break;
                }
            }
            _hbdata.push_back(newHeartbeatData);
        }
    }
}

// This function installs a new config object and recreates MemberHeartbeatData objects
// that reflect the new config.
void TopologyCoordinatorImpl::updateConfig(const ReplicaSetConfig& newConfig,
                                           int selfIndex,
                                           Date_t now,
                                           const OpTime& lastOpApplied) {
    invariant(_role != Role::candidate);
    invariant(selfIndex < newConfig.getNumMembers());

    // Reset term on startup and upgrade/downgrade of protocol version.
    if (!_rsConfig.isInitialized() ||
        _rsConfig.getProtocolVersion() != newConfig.getProtocolVersion()) {
        if (newConfig.getProtocolVersion() == 1) {
            _term = OpTime::kInitialTerm;
        } else {
            invariant(newConfig.getProtocolVersion() == 0);
            _term = OpTime::kUninitializedTerm;
        }
        LOG(1) << "Updated term in topology coordinator to " << _term << " due to new config";
    }

    _updateHeartbeatDataForReconfig(newConfig, selfIndex, now);
    _stepDownPending = false;
    _rsConfig = newConfig;
    _selfIndex = selfIndex;
    _forceSyncSourceIndex = -1;

    if (_role == Role::leader) {
        if (_selfIndex == -1) {
            log() << "Could not remain primary because no longer a member of the replica set";
        } else if (!_selfConfig().isElectable()) {
            log() << " Could not remain primary because no longer electable";
        } else {
            // Don't stepdown if you don't have to.
            _currentPrimaryIndex = _selfIndex;
            return;
        }
        _role = Role::follower;
    }

    // By this point we know we are in Role::follower
    _currentPrimaryIndex = -1;  // force secondaries to re-detect who the primary is

    if (_followerMode == MemberState::RS_SECONDARY && _rsConfig.getNumMembers() == 1 &&
        _selfIndex == 0 && _rsConfig.getMemberAt(_selfIndex).isElectable()) {
        // If the new config describes a one-node replica set, we're the one member,
        // we're electable, and we are currently in followerMode SECONDARY,
        // we must transition to candidate, in leiu of heartbeats.
        _role = Role::candidate;
    }
}
std::string TopologyCoordinatorImpl::_getHbmsg(Date_t now) const {
    // ignore messages over 2 minutes old
    if ((now - _hbmsgTime) > Seconds{120}) {
        return "";
    }
    return _hbmsg;
}

void TopologyCoordinatorImpl::setMyHeartbeatMessage(const Date_t now, const std::string& message) {
    _hbmsgTime = now;
    _hbmsg = message;
}

const MemberConfig& TopologyCoordinatorImpl::_selfConfig() const {
    return _rsConfig.getMemberAt(_selfIndex);
}

TopologyCoordinatorImpl::UnelectableReasonMask TopologyCoordinatorImpl::_getUnelectableReason(
    int index, const OpTime& lastOpApplied) const {
    invariant(index != _selfIndex);
    const MemberConfig& memberConfig = _rsConfig.getMemberAt(index);
    const MemberHeartbeatData& hbData = _hbdata.at(index);
    UnelectableReasonMask result = None;
    if (memberConfig.isArbiter()) {
        result |= ArbiterIAm;
    }
    if (memberConfig.getPriority() <= 0) {
        result |= NoPriority;
    }
    if (hbData.getState() != MemberState::RS_SECONDARY) {
        result |= NotSecondary;
    }
    if (_rsConfig.getProtocolVersion() == 0 &&
        !_isOpTimeCloseEnoughToLatestToElect(hbData.getAppliedOpTime(), lastOpApplied)) {
        result |= NotCloseEnoughToLatestOptime;
    }
    if (hbData.up() && hbData.isUnelectable()) {
        result |= RefusesToStand;
    }
    invariant(result || memberConfig.isElectable());
    return result;
}

TopologyCoordinatorImpl::UnelectableReasonMask TopologyCoordinatorImpl::_getMyUnelectableReason(
    const Date_t now, const OpTime& lastApplied) const {
    UnelectableReasonMask result = None;
    if (lastApplied.isNull()) {
        result |= NoData;
    }
    if (!_aMajoritySeemsToBeUp()) {
        result |= CannotSeeMajority;
    }
    if (_selfIndex == -1) {
        result |= NotInitialized;
        return result;
    }
    if (_selfConfig().isArbiter()) {
        result |= ArbiterIAm;
    }
    if (_selfConfig().getPriority() <= 0) {
        result |= NoPriority;
    }
    if (_stepDownUntil > now) {
        result |= StepDownPeriodActive;
    }

    // Cannot be electable unless secondary or already primary
    if (!getMemberState().secondary() && !_iAmPrimary()) {
        result |= NotSecondary;
    }

    // Election rules only for protocol version 0.
    if (_rsConfig.getProtocolVersion() == 0) {
        if (_voteLease.whoId != -1 &&
            _voteLease.whoId != _rsConfig.getMemberAt(_selfIndex).getId() &&
            _voteLease.when + VoteLease::leaseTime >= now) {
            result |= VotedTooRecently;
        }
        if (!_isOpTimeCloseEnoughToLatestToElect(lastApplied, lastApplied)) {
            result |= NotCloseEnoughToLatestOptime;
        }
    }
    return result;
}

std::string TopologyCoordinatorImpl::_getUnelectableReasonString(
    const UnelectableReasonMask ur) const {
    invariant(ur);
    str::stream ss;
    bool hasWrittenToStream = false;
    if (ur & NoData) {
        ss << "node has no applied oplog entries";
        hasWrittenToStream = true;
    }
    if (ur & VotedTooRecently) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "I recently voted for " << _voteLease.whoHostAndPort.toString();
    }
    if (ur & CannotSeeMajority) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "I cannot see a majority";
    }
    if (ur & ArbiterIAm) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "member is an arbiter";
    }
    if (ur & NoPriority) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "member has zero priority";
    }
    if (ur & StepDownPeriodActive) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "I am still waiting for stepdown period to end at "
           << dateToISOStringLocal(_stepDownUntil);
    }
    if (ur & NotSecondary) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "member is not currently a secondary";
    }
    if (ur & NotCloseEnoughToLatestOptime) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "member is more than 10 seconds behind the most up-to-date member";
    }
    if (ur & NotInitialized) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "node is not a member of a valid replica set configuration";
    }
    if (ur & RefusesToStand) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "most recent heartbeat indicates node will not stand for election";
    }
    if (!hasWrittenToStream) {
        severe() << "Invalid UnelectableReasonMask value 0x" << integerToHex(ur);
        fassertFailed(26011);
    }
    ss << " (mask 0x" << integerToHex(ur) << ")";
    return ss;
}

Milliseconds TopologyCoordinatorImpl::_getPing(const HostAndPort& host) {
    return _pings[host].getMillis();
}

void TopologyCoordinatorImpl::_setElectionTime(const Timestamp& newElectionTime) {
    _electionTime = newElectionTime;
}

int TopologyCoordinatorImpl::_getTotalPings() {
    PingMap::iterator it = _pings.begin();
    PingMap::iterator end = _pings.end();
    int totalPings = 0;
    while (it != end) {
        totalPings += it->second.getCount();
        it++;
    }
    return totalPings;
}

std::vector<HostAndPort> TopologyCoordinatorImpl::getMaybeUpHostAndPorts() const {
    std::vector<HostAndPort> upHosts;
    for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin(); it != _hbdata.end();
         ++it) {
        const int itIndex = indexOfIterator(_hbdata, it);
        if (itIndex == _selfIndex) {
            continue;  // skip ourselves
        }
        if (!it->maybeUp()) {
            continue;  // skip DOWN nodes
        }

        upHosts.push_back(_rsConfig.getMemberAt(itIndex).getHostAndPort());
    }
    return upHosts;
}

bool TopologyCoordinatorImpl::voteForMyself(Date_t now) {
    if (_role != Role::candidate) {
        return false;
    }
    int selfId = _selfConfig().getId();
    if ((_voteLease.when + VoteLease::leaseTime >= now) && (_voteLease.whoId != selfId)) {
        log() << "not voting yea for " << selfId << " voted for "
              << _voteLease.whoHostAndPort.toString() << ' '
              << durationCount<Seconds>(now - _voteLease.when) << " secs ago";
        return false;
    }
    _voteLease.when = now;
    _voteLease.whoId = selfId;
    _voteLease.whoHostAndPort = _selfConfig().getHostAndPort();
    return true;
}

MemberState TopologyCoordinatorImpl::getMemberState() const {
    if (_selfIndex == -1) {
        if (_rsConfig.isInitialized()) {
            return MemberState::RS_REMOVED;
        }
        return MemberState::RS_STARTUP;
    }

    if (_rsConfig.isConfigServer()) {
        if (_options.configServerMode == CatalogManager::ConfigServerMode::NONE) {
            return MemberState::RS_REMOVED;
        }
        if (_options.configServerMode == CatalogManager::ConfigServerMode::CSRS) {
            invariant(_storageEngineSupportsReadCommitted != ReadCommittedSupport::kUnknown);
            if (_storageEngineSupportsReadCommitted == ReadCommittedSupport::kNo) {
                return MemberState::RS_REMOVED;
            }
        }
    } else {
        if (_options.configServerMode != CatalogManager::ConfigServerMode::NONE) {
            return MemberState::RS_REMOVED;
        }
    }

    if (_role == Role::leader) {
        invariant(_currentPrimaryIndex == _selfIndex);
        return MemberState::RS_PRIMARY;
    }
    const MemberConfig& myConfig = _selfConfig();
    if (myConfig.isArbiter()) {
        return MemberState::RS_ARBITER;
    }
    if (((_maintenanceModeCalls > 0) || (_hasOnlyAuthErrorUpHeartbeats(_hbdata, _selfIndex))) &&
        (_followerMode == MemberState::RS_SECONDARY)) {
        return MemberState::RS_RECOVERING;
    }
    return _followerMode;
}

void TopologyCoordinatorImpl::setElectionInfo(OID electionId, Timestamp electionOpTime) {
    invariant(_role == Role::leader);
    _electionTime = electionOpTime;
    _electionId = electionId;
}

void TopologyCoordinatorImpl::processWinElection(OID electionId, Timestamp electionOpTime) {
    invariant(_role == Role::candidate);
    _role = Role::leader;
    setElectionInfo(electionId, electionOpTime);
    _currentPrimaryIndex = _selfIndex;
    _syncSource = HostAndPort();
    _forceSyncSourceIndex = -1;
}

void TopologyCoordinatorImpl::processLoseElection() {
    invariant(_role == Role::candidate);
    const HostAndPort syncSourceAddress = getSyncSourceAddress();
    _electionTime = Timestamp(0, 0);
    _electionId = OID();
    _role = Role::follower;

    // Clear voteLease time, if we voted for ourselves in this election.
    // This will allow us to vote for others.
    if (_voteLease.whoId == _selfConfig().getId()) {
        _voteLease.when = Date_t();
    }
}

bool TopologyCoordinatorImpl::stepDown(Date_t until, bool force, const OpTime& lastOpApplied) {
    bool canStepDown = force;
    for (int i = 0; !canStepDown && i < _rsConfig.getNumMembers(); ++i) {
        if (i == _selfIndex) {
            continue;
        }
        UnelectableReasonMask reason = _getUnelectableReason(i, lastOpApplied);
        if (!reason && _hbdata.at(i).getAppliedOpTime() >= lastOpApplied) {
            canStepDown = true;
        }
    }

    if (!canStepDown) {
        return false;
    }
    _stepDownUntil = until;
    _stepDownSelfAndReplaceWith(-1);
    return true;
}

void TopologyCoordinatorImpl::setFollowerMode(MemberState::MS newMode) {
    invariant(_role == Role::follower);
    switch (newMode) {
        case MemberState::RS_RECOVERING:
        case MemberState::RS_ROLLBACK:
        case MemberState::RS_SECONDARY:
        case MemberState::RS_STARTUP2:
            _followerMode = newMode;
            break;
        default:
            invariant(false);
    }

    if (_followerMode != MemberState::RS_SECONDARY) {
        return;
    }

    // When a single node replica set transitions to SECONDARY, we must check if we should
    // be a candidate here.  This is necessary because a single node replica set has no
    // heartbeats that would normally change the role to candidate.

    if (_rsConfig.getNumMembers() == 1 && _selfIndex == 0 &&
        _rsConfig.getMemberAt(_selfIndex).isElectable()) {
        _role = Role::candidate;
    }
}

bool TopologyCoordinatorImpl::stepDownIfPending() {
    if (!_stepDownPending) {
        return false;
    }

    int remotePrimaryIndex = -1;
    for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin(); it != _hbdata.end();
         ++it) {
        const int itIndex = indexOfIterator(_hbdata, it);
        if (itIndex == _selfIndex) {
            continue;
        }

        if (it->getState().primary() && it->up()) {
            if (remotePrimaryIndex != -1) {
                // two other nodes think they are primary (asynchronously polled)
                // -- wait for things to settle down.
                remotePrimaryIndex = -1;
                warning() << "two remote primaries (transiently)";
                break;
            }
            remotePrimaryIndex = itIndex;
        }
    }
    _stepDownSelfAndReplaceWith(remotePrimaryIndex);
    return true;
}

void TopologyCoordinatorImpl::_stepDownSelfAndReplaceWith(int newPrimary) {
    invariant(_role == Role::leader);
    invariant(_selfIndex != -1);
    invariant(_selfIndex != newPrimary);
    invariant(_selfIndex == _currentPrimaryIndex);
    _currentPrimaryIndex = newPrimary;
    _role = Role::follower;
    _stepDownPending = false;
}

void TopologyCoordinatorImpl::adjustMaintenanceCountBy(int inc) {
    invariant(_role == Role::follower);
    _maintenanceModeCalls += inc;
    invariant(_maintenanceModeCalls >= 0);
}

int TopologyCoordinatorImpl::getMaintenanceCount() const {
    return _maintenanceModeCalls;
}

TopologyCoordinator::UpdateTermResult TopologyCoordinatorImpl::updateTerm(long long term,
                                                                          Date_t now) {
    if (term <= _term) {
        return TopologyCoordinator::UpdateTermResult::kAlreadyUpToDate;
    }
    // Don't run election if we just stood up or learned about a new term.
    _electionSleepUntil = now + _rsConfig.getElectionTimeoutPeriod();

    // Don't update the term just yet if we are going to step down, as we don't want to report
    // that we are primary in the new term.
    if (_iAmPrimary()) {
        return TopologyCoordinator::UpdateTermResult::kTriggerStepDown;
    }
    _term = term;
    return TopologyCoordinator::UpdateTermResult::kUpdatedTerm;
}


long long TopologyCoordinatorImpl::getTerm() {
    return _term;
}

bool TopologyCoordinatorImpl::shouldChangeSyncSource(const HostAndPort& currentSource,
                                                     const OpTime& myLastOpTime,
                                                     const OpTime& syncSourceLastOpTime,
                                                     bool syncSourceHasSyncSource,
                                                     Date_t now) const {
    // Methodology:
    // If there exists a viable sync source member other than currentSource, whose oplog has
    // reached an optime greater than _options.maxSyncSourceLagSecs later than currentSource's,
    // return true.
    // If the currentSource has the same replication progress as we do and has no source for further
    // progress, return true.

    // If the user requested a sync source change, return true.
    if (_forceSyncSourceIndex != -1) {
        return true;
    }

    const int currentSourceIndex = _rsConfig.findMemberIndexByHostAndPort(currentSource);
    if (currentSourceIndex == -1) {
        return true;
    }
    invariant(currentSourceIndex != _selfIndex);

    OpTime currentSourceOpTime =
        std::max(syncSourceLastOpTime, _hbdata.at(currentSourceIndex).getAppliedOpTime());

    if (currentSourceOpTime.isNull()) {
        // Haven't received a heartbeat from the sync source yet, so can't tell if we should
        // change.
        return false;
    }

    if (_rsConfig.getProtocolVersion() == 1 && !syncSourceHasSyncSource &&
        currentSourceOpTime <= myLastOpTime &&
        _hbdata.at(currentSourceIndex).getState() != MemberState::RS_PRIMARY) {
        return true;
    }

    unsigned int currentSecs = currentSourceOpTime.getSecs();
    unsigned int goalSecs = currentSecs + durationCount<Seconds>(_options.maxSyncSourceLagSecs);

    for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin(); it != _hbdata.end();
         ++it) {
        const int itIndex = indexOfIterator(_hbdata, it);
        const MemberConfig& candidateConfig = _rsConfig.getMemberAt(itIndex);
        if (it->up() && (candidateConfig.isVoter() || !_selfConfig().isVoter()) &&
            (candidateConfig.shouldBuildIndexes() || !_selfConfig().shouldBuildIndexes()) &&
            it->getState().readable() && !_memberIsBlacklisted(candidateConfig, now) &&
            goalSecs < it->getAppliedOpTime().getSecs()) {
            log() << "re-evaluating sync source because our current sync source's most recent "
                  << "OpTime is " << currentSourceOpTime.toString() << " which is more than "
                  << _options.maxSyncSourceLagSecs << " behind member "
                  << candidateConfig.getHostAndPort().toString() << " whose most recent OpTime is "
                  << it->getAppliedOpTime().toString();
            invariant(itIndex != _selfIndex);
            return true;
        }
    }
    return false;
}

void TopologyCoordinatorImpl::prepareReplResponseMetadata(rpc::ReplSetMetadata* metadata,
                                                          const OpTime& lastVisibleOpTime,
                                                          const OpTime& lastCommittedOpTime) const {
    *metadata =
        rpc::ReplSetMetadata(_term,
                             lastCommittedOpTime,
                             lastVisibleOpTime,
                             _rsConfig.getConfigVersion(),
                             _rsConfig.getReplicaSetId(),
                             _currentPrimaryIndex,
                             _rsConfig.findMemberIndexByHostAndPort(getSyncSourceAddress()));
}

void TopologyCoordinatorImpl::summarizeAsHtml(ReplSetHtmlSummary* output) {
    output->setConfig(_rsConfig);
    output->setHBData(_hbdata);
    output->setSelfIndex(_selfIndex);
    output->setPrimaryIndex(_currentPrimaryIndex);
    output->setSelfState(getMemberState());
    output->setSelfHeartbeatMessage(_hbmsg);
}

void TopologyCoordinatorImpl::processReplSetRequestVotes(const ReplSetRequestVotesArgs& args,
                                                         ReplSetRequestVotesResponse* response,
                                                         const OpTime& lastAppliedOpTime) {
    response->setTerm(_term);

    if (args.getTerm() < _term) {
        response->setVoteGranted(false);
        response->setReason("candidate's term is lower than mine");
    } else if (args.getConfigVersion() != _rsConfig.getConfigVersion()) {
        response->setVoteGranted(false);
        response->setReason("candidate's config version differs from mine");
    } else if (args.getSetName() != _rsConfig.getReplSetName()) {
        response->setVoteGranted(false);
        response->setReason("candidate's set name differs from mine");
    } else if (args.getLastCommittedOp() < lastAppliedOpTime) {
        response->setVoteGranted(false);
        response->setReason("candidate's data is staler than mine");
    } else if (!args.isADryRun() && _lastVote.getTerm() == args.getTerm()) {
        response->setVoteGranted(false);
        response->setReason("already voted for another candidate this term");
    } else {
        if (!args.isADryRun()) {
            _lastVote.setTerm(args.getTerm());
            _lastVote.setCandidateIndex(args.getCandidateIndex());
        }
        response->setVoteGranted(true);
    }
}

Status TopologyCoordinatorImpl::processReplSetDeclareElectionWinner(
    const ReplSetDeclareElectionWinnerArgs& args, long long* responseTerm) {
    *responseTerm = _term;
    if (args.getReplSetName() != _rsConfig.getReplSetName()) {
        return {ErrorCodes::BadValue, "replSet name does not match"};
    } else if (args.getTerm() < _term) {
        return {ErrorCodes::BadValue, "term has already passed"};
    } else if (args.getTerm() == _term && _currentPrimaryIndex > -1 &&
               args.getWinnerId() != _rsConfig.getMemberAt(_currentPrimaryIndex).getId()) {
        return {ErrorCodes::BadValue, "term already has a primary"};
    }

    _currentPrimaryIndex = _rsConfig.findMemberIndexByConfigId(args.getWinnerId());
    return Status::OK();
}

void TopologyCoordinatorImpl::loadLastVote(const LastVote& lastVote) {
    _lastVote = lastVote;
}

void TopologyCoordinatorImpl::voteForMyselfV1() {
    _lastVote.setTerm(_term);
    _lastVote.setCandidateIndex(_selfIndex);
}

void TopologyCoordinatorImpl::setPrimaryIndex(long long primaryIndex) {
    _currentPrimaryIndex = primaryIndex;
}

bool TopologyCoordinatorImpl::becomeCandidateIfElectable(const Date_t now,
                                                         const OpTime& lastOpApplied) {
    if (_role == Role::leader) {
        LOG(2) << "Not standing for election again; already primary";
        return false;
    }

    if (_role == Role::candidate) {
        LOG(2) << "Not standing for election again; already candidate";
        return false;
    }

    const UnelectableReasonMask unelectableReason = _getMyUnelectableReason(now, lastOpApplied);
    if (unelectableReason) {
        LOG(2) << "Not standing for election because "
               << _getUnelectableReasonString(unelectableReason);
        return false;
    }

    // All checks passed, become a candidate and start election proceedings.
    _role = Role::candidate;

    return true;
}

void TopologyCoordinatorImpl::setStorageEngineSupportsReadCommitted(bool supported) {
    _storageEngineSupportsReadCommitted =
        supported ? ReadCommittedSupport::kYes : ReadCommittedSupport::kNo;
}

}  // namespace repl
}  // namespace mongo
