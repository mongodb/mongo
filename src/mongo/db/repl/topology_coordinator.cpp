/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#define LOGV2_FOR_ELECTION(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                             \
        ID, DLEVEL, {logv2::LogComponent::kReplicationElection}, MESSAGE, ##__VA_ARGS__)
#define LOGV2_FOR_HEARTBEATS(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                               \
        ID, DLEVEL, {logv2::LogComponent::kReplicationHeartbeats}, MESSAGE, ##__VA_ARGS__)

#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/topology_coordinator_gen.h"

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <limits>
#include <string>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/heartbeat_response_action.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hex.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(forceSyncSourceCandidate);
MONGO_FAIL_POINT_DEFINE(voteNoInElection);
MONGO_FAIL_POINT_DEFINE(voteYesInDryRunButNoInRealElection);

// If this fail point is enabled, TopologyCoordinator::shouldChangeSyncSource() will ignore
// the option TopologyCoordinator::Options::maxSyncSourceLagSecs. The sync source will not be
// re-evaluated if it lags behind another node by more than 'maxSyncSourceLagSecs' seconds.
MONGO_FAIL_POINT_DEFINE(disableMaxSyncSourceLagSecs);

constexpr Milliseconds TopologyCoordinator::PingStats::UninitializedPingTime;

// Tracks the number of times we decide to change sync sources in order to sync from a significantly
// closer node.
CounterMetric numSyncSourceChangesDueToSignificantlyCloserNode(
    "repl.syncSource.numSyncSourceChangesDueToSignificantlyCloserNode");

using namespace fmt::literals;

std::string TopologyCoordinator::roleToString(TopologyCoordinator::Role role) {
    switch (role) {
        case TopologyCoordinator::Role::kLeader:
            return "leader";
        case TopologyCoordinator::Role::kFollower:
            return "follower";
        case TopologyCoordinator::Role::kCandidate:
            return "candidate";
    }
    MONGO_UNREACHABLE;
}

TopologyCoordinator::~TopologyCoordinator() {}

std::ostream& operator<<(std::ostream& os, TopologyCoordinator::Role role) {
    return os << TopologyCoordinator::roleToString(role);
}

std::ostream& operator<<(std::ostream& os,
                         TopologyCoordinator::PrepareFreezeResponseResult result) {
    switch (result) {
        case TopologyCoordinator::PrepareFreezeResponseResult::kNoAction:
            return os << "no action";
        case TopologyCoordinator::PrepareFreezeResponseResult::kSingleNodeSelfElect:
            return os << "single node self elect";
    }
    MONGO_UNREACHABLE;
}

namespace {
template <typename T>
int indexOfIterator(const std::vector<T>& vec, typename std::vector<T>::const_iterator& it) {
    return static_cast<int>(it - vec.begin());
}

/**
 * Returns true if the only up heartbeats are auth errors.
 */
bool _hasOnlyAuthErrorUpHeartbeats(const std::vector<MemberData>& hbdata, const int selfIndex) {
    bool foundAuthError = false;
    for (std::vector<MemberData>::const_iterator it = hbdata.begin(); it != hbdata.end(); ++it) {
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

void appendOpTime(BSONObjBuilder* bob, const char* elemName, const OpTime& opTime) {
    opTime.append(bob, elemName);
}
}  // namespace

void TopologyCoordinator::PingStats::start(Date_t now) {
    _lastHeartbeatStartDate = now;
    _numFailuresSinceLastStart = 0;
    _state = HeartbeatState::TRYING;
}

void TopologyCoordinator::PingStats::hit(Milliseconds millis) {
    _state = HeartbeatState::SUCCEEDED;
    ++hitCount;

    averagePingTimeMs = averagePingTimeMs == UninitializedPingTime
        ? millis
        : Milliseconds((averagePingTimeMs * 4 + millis) / 5);
}

void TopologyCoordinator::PingStats::set_forTest(Milliseconds millis) {
    _state = HeartbeatState::SUCCEEDED;

    averagePingTimeMs = millis;
}

void TopologyCoordinator::PingStats::miss() {
    ++_numFailuresSinceLastStart;
    // Transition to 'FAILED' state if this was our last retry.
    if (_numFailuresSinceLastStart > kMaxHeartbeatRetries) {
        _state = PingStats::HeartbeatState::FAILED;
    }
}

bool TopologyCoordinator::RecentSyncSourceChanges::changedTooOftenRecently(Date_t now) {
    size_t maxSize = maxNumSyncSourceChangesPerHour.load();

    // Return false if we have fewer than maxNumSyncSourceChangesPerHour entries.
    if (_recentChanges.empty() || _recentChanges.size() < maxSize) {
        return false;
    }

    // Remove additional entries in case maxNumSyncSourceChangesPerHour was changed.
    while (_recentChanges.size() > maxSize) {
        _recentChanges.pop();
    }

    // Return whether all entries in the queue happened within the last hour by checking the oldest
    // entry.
    auto hourBefore = now - Hours(1);
    return _recentChanges.front() > hourBefore;
}

void TopologyCoordinator::RecentSyncSourceChanges::addNewEntry(Date_t now) {
    // Remove additional entries if the queue already has maxNumSyncSourceChangerPerHour entries.
    while (_recentChanges.size() >= static_cast<size_t>(maxNumSyncSourceChangesPerHour.load())) {
        _recentChanges.pop();
    }
    _recentChanges.push(now);
    return;
}

std::queue<Date_t> TopologyCoordinator::RecentSyncSourceChanges::getChanges_forTest() {
    return _recentChanges;
}

TopologyCoordinator::TopologyCoordinator(Options options)
    : _role(Role::kFollower),
      _topologyVersion(instanceId, 0),
      _term(OpTime::kUninitializedTerm),
      _currentPrimaryIndex(-1),
      _forceSyncSourceIndex(-1),
      _replSetSyncFromSet(false),
      _options(std::move(options)),
      _selfIndex(-1),
      _maintenanceModeCalls(0),
      _followerMode(MemberState::RS_STARTUP2) {
    invariant(getMemberState() == MemberState::RS_STARTUP);
    // Need an entry for self in the memberHearbeatData.
    _memberData.emplace_back();
    _memberData.back().setIsSelf(true);
}

TopologyCoordinator::Role TopologyCoordinator::getRole() const {
    return _role;
}

TopologyVersion TopologyCoordinator::getTopologyVersion() const {
    return _topologyVersion;
}

void TopologyCoordinator::setForceSyncSourceIndex(int index) {
    invariant(_forceSyncSourceIndex < _rsConfig.getNumMembers());
    _forceSyncSourceIndex = index;
}

HostAndPort TopologyCoordinator::getSyncSourceAddress() const {
    return _syncSource;
}

void TopologyCoordinator::_clearSyncSource() {
    _syncSource = HostAndPort();
    _replSetSyncFromSet = false;
}

void TopologyCoordinator::_setSyncSource(HostAndPort newSyncSource,
                                         Date_t now,
                                         bool fromReplSetSyncFrom) {
    _syncSource = newSyncSource;
    _replSetSyncFromSet = fromReplSetSyncFrom;

    // If we chose another node rather than clearing the sync source, update the recent sync source
    // changes.
    if (!_syncSource.empty()) {
        _recentSyncSourceChanges.addNewEntry(now);
    }
}

HostAndPort TopologyCoordinator::chooseNewSyncSource(Date_t now,
                                                     const OpTime& lastOpTimeFetched,
                                                     ReadPreference readPreference) {
    // Check to see if we should choose a sync source because the 'replSetSyncFrom' command was set.
    auto maybeSyncSource = _chooseSyncSourceReplSetSyncFrom(now);
    if (maybeSyncSource) {
        // If we have a forced sync source via 'replSetSyncFrom', set the _replSetSyncFromSet flag
        // to true.
        _setSyncSource(*maybeSyncSource, now, true /* fromReplSetSyncFrom */);
        return _syncSource;
    }

    // Check to make sure we can choose a sync source, and choose one if the
    // 'forceSyncSourceCandidate' failpoint is set.
    maybeSyncSource = _chooseSyncSourceInitialChecks(now);
    if (maybeSyncSource) {
        _setSyncSource(*maybeSyncSource, now);
        return _syncSource;
    }

    // If we are only allowed to sync from the primary, use it as the sync source if possible.
    if (readPreference == ReadPreference::PrimaryOnly) {
        _setSyncSource(_choosePrimaryAsSyncSource(now, lastOpTimeFetched), now);
        if (_syncSource.empty()) {
            LOGV2_DEBUG(3873104,
                        1,
                        "Cannot select a sync source because the primary is not a valid sync "
                        "source and the sync source read preference is 'primary'.");
        }
        return _syncSource;
    } else if (readPreference == ReadPreference::PrimaryPreferred) {
        // If we prefer the primary, try it first.
        _setSyncSource(_choosePrimaryAsSyncSource(now, lastOpTimeFetched), now);
        if (!_syncSource.empty()) {
            return _syncSource;
        }
    }
    _setSyncSource(_chooseNearbySyncSource(now, lastOpTimeFetched, readPreference), now);
    return _syncSource;
}

HostAndPort TopologyCoordinator::_chooseNearbySyncSource(Date_t now,
                                                         const OpTime& lastOpTimeFetched,
                                                         ReadPreference readPreference) {
    // We should have handled PrimaryOnly before calling this.
    invariant(readPreference != ReadPreference::PrimaryOnly);

    // find the member with the lowest ping time that is ahead of me

    int closestIndex = -1;

    // Make two attempts, with less restrictive rules the second time.
    //
    // During the first attempt, we ignore those nodes that have a larger secondary
    // delay, hidden nodes or non-voting, and nodes that are excessively behind.
    //
    // For the second attempt include those nodes, in case those are the only ones we can reach.
    //
    // This loop attempts to set 'closestIndex', to select a viable candidate.
    for (int attempts = 0; attempts < 2; ++attempts) {
        for (size_t candidateIndex = 0; candidateIndex < _memberData.size(); candidateIndex++) {
            if (!_isEligibleSyncSource(candidateIndex,
                                       now,
                                       lastOpTimeFetched,
                                       readPreference,
                                       attempts == 0 /* firstAttempt */,
                                       true /* shouldCheckStaleness */)) {
                // Node is not a viable sync source candidate.
                continue;
            }

            // Set 'closestIndex' if this node is the first viable candidate we have encountered.
            if (closestIndex == -1) {
                closestIndex = candidateIndex;
                continue;
            }

            const auto syncSourceCandidate = _rsConfig.getMemberAt(candidateIndex).getHostAndPort();
            const auto closestNode = _rsConfig.getMemberAt(closestIndex).getHostAndPort();

            // Do not update 'closestIndex' if the candidate is not the closest node we've seen.
            auto syncSourceCandidatePing = _getPing(syncSourceCandidate);
            auto closestPing = _getPing(closestNode);
            if (syncSourceCandidatePing > closestPing) {
                LOGV2_DEBUG(3873114,
                            2,
                            "Cannot select sync source with higher latency than the best "
                            "candidate",
                            "syncSourceCandidate"_attr = syncSourceCandidate,
                            "syncSourceCandidatePing"_attr = syncSourceCandidatePing,
                            "closestNode"_attr = closestNode,
                            "closestPing"_attr = closestPing);
                continue;
            }
            closestIndex = candidateIndex;
        }
        if (closestIndex != -1)
            break;  // no need for second attempt
    }

    if (closestIndex == -1) {
        // Did not find any members to sync from
        // Only log when we had a valid sync source before
        static constexpr char message[] = "Could not find member to sync from";
        if (!_syncSource.empty()) {
            LOGV2(21798, message);
        }
        setMyHeartbeatMessage(now, message);

        return HostAndPort();
    }

    auto syncSource = _rsConfig.getMemberAt(closestIndex).getHostAndPort();
    LOGV2(21799, "Sync source candidate chosen", "syncSource"_attr = syncSource);
    std::string msg(str::stream() << "syncing from: " << syncSource.toString(), 0);
    setMyHeartbeatMessage(now, msg);
    return syncSource;
}

OpTime TopologyCoordinator::_getOldestSyncOpTime() const {
    OpTime oldestSyncOpTime = OpTime();

    // Find primary's oplog time. We will reject sync candidates that are more than
    // _options.maxSyncSourceLagSecs seconds behind this optime.
    if (_currentPrimaryIndex != -1) {
        OpTime primaryOpTime = _memberData.at(_currentPrimaryIndex).getHeartbeatAppliedOpTime();

        // Check if primaryOpTime is still close to 0 because we haven't received
        // our first heartbeat from a new primary yet.
        auto maxLag =
            static_cast<unsigned int>(durationCount<Seconds>(_options.maxSyncSourceLagSecs));
        if (primaryOpTime.getSecs() >= maxLag) {
            oldestSyncOpTime =
                OpTime(Timestamp(primaryOpTime.getSecs() - maxLag, 0), primaryOpTime.getTerm());
        }
    }
    return oldestSyncOpTime;
}

bool TopologyCoordinator::_isEligibleSyncSource(int candidateIndex,
                                                Date_t now,
                                                const OpTime& lastOpTimeFetched,
                                                ReadPreference readPreference,
                                                const bool firstAttempt,
                                                const bool shouldCheckStaleness) const {
    // Don't consider ourselves.
    if (candidateIndex == _selfIndex) {
        return false;
    }

    const MemberConfig& memberConfig(_rsConfig.getMemberAt(candidateIndex));
    const auto syncSourceCandidate = memberConfig.getHostAndPort();
    const auto memberData = _memberData[candidateIndex];

    // Candidate must be up to be considered.
    if (!memberData.up()) {
        LOGV2_DEBUG(3873106,
                    2,
                    "Cannot select sync source because it is not up",
                    "syncSourceCandidate"_attr = syncSourceCandidate);
        return false;
    }
    // Candidate must be PRIMARY or SECONDARY state to be considered.
    if (!memberData.getState().readable()) {
        LOGV2_DEBUG(3873107,
                    2,
                    "Cannot select sync source because it is not readable",
                    "syncSourceCandidate"_attr = syncSourceCandidate);
        return false;
    }

    // Disallow the primary for first or all attempts depending on the readPreference.
    if (readPreference == ReadPreference::SecondaryOnly ||
        (readPreference == ReadPreference::SecondaryPreferred && firstAttempt)) {
        if (memberData.getState().primary()) {
            LOGV2_DEBUG(3873101,
                        2,
                        "Cannot select sync source because it is a primary and we are "
                        "looking for a secondary",
                        "syncSourceCandidate"_attr = syncSourceCandidate);
            return false;
        }
    }

    // On the first attempt, we skip candidates that do not match these criteria.
    if (firstAttempt) {
        // Candidate must be a voter if we are a voter.
        if (_selfConfig().isVoter() && !memberConfig.isVoter()) {
            LOGV2_DEBUG(3873108,
                        2,
                        "Cannot select sync source because we are a voter and it is not",
                        "syncSourceCandidate"_attr = syncSourceCandidate);
            return false;
        }
        // Candidates must not be hidden.
        if (memberConfig.isHidden()) {
            LOGV2_DEBUG(3873109,
                        2,
                        "Cannot select sync source because it is hidden",
                        "syncSourceCandidate"_attr = syncSourceCandidate);
            return false;
        }
        // Candidates cannot be excessively behind, if we are checking for staleness.
        if (shouldCheckStaleness) {
            const auto oldestSyncOpTime = _getOldestSyncOpTime();
            if (memberData.getHeartbeatAppliedOpTime() < oldestSyncOpTime) {
                LOGV2_DEBUG(3873110,
                            2,
                            "Cannot select sync source because it is too far behind",
                            "syncSourceCandidate"_attr = syncSourceCandidate,
                            "syncSourceCandidateOpTime"_attr =
                                memberData.getHeartbeatAppliedOpTime(),
                            "oldestAcceptableOpTime"_attr = oldestSyncOpTime);
                return false;
            }
        }
        // Candidate must not have a configured delay larger than ours.
        if (_selfConfig().getSecondaryDelay() < memberConfig.getSecondaryDelay()) {
            LOGV2_DEBUG(3873111,
                        2,
                        "Cannot select sync source with larger secondaryDelaySecs than ours",
                        "syncSourceCandidate"_attr = syncSourceCandidate,
                        "syncSourceCandidateSecondaryDelaySecs"_attr =
                            memberConfig.getSecondaryDelay(),
                        "secondaryDelaySecs"_attr = _selfConfig().getSecondaryDelay());
            return false;
        }
    }
    // Candidate must build indexes if we build indexes, to be considered.
    if (_selfConfig().shouldBuildIndexes()) {
        if (!memberConfig.shouldBuildIndexes()) {
            LOGV2_DEBUG(3873112,
                        2,
                        "Cannot select sync source which does not build indexes when we do",
                        "syncSourceCandidate"_attr = syncSourceCandidate);
            return false;
        }
    }
    // Only select a candidate that is ahead of me, if we are checking for staleness.
    if (shouldCheckStaleness && memberData.getHeartbeatAppliedOpTime() <= lastOpTimeFetched) {
        LOGV2_DEBUG(3873113,
                    1,
                    "Cannot select sync source which is not ahead of me",
                    "syncSourceCandidate"_attr = syncSourceCandidate,
                    "syncSourceCandidateLastAppliedOpTime"_attr =
                        memberData.getHeartbeatAppliedOpTime().toBSON(),
                    "lastOpTimeFetched"_attr = lastOpTimeFetched.toBSON());
        return false;
    }
    // Candidate cannot be denylisted.
    if (_memberIsDenylisted(memberConfig, now)) {
        LOGV2_DEBUG(3873115,
                    1,
                    "Cannot select sync source which is denylisted",
                    "syncSourceCandidate"_attr = syncSourceCandidate);
        return false;
    }
    // This candidate has passed all tests.
    return true;
}

boost::optional<HostAndPort> TopologyCoordinator::_chooseSyncSourceReplSetSyncFrom(Date_t now) {
    if (_selfIndex == -1) {
        return boost::none;
    }

    if (_forceSyncSourceIndex == -1) {
        return boost::none;
    }

    // If we have a target we've requested to sync from, use it.
    invariant(_forceSyncSourceIndex < _rsConfig.getNumMembers());
    auto syncSource = _rsConfig.getMemberAt(_forceSyncSourceIndex).getHostAndPort();
    _forceSyncSourceIndex = -1;
    LOGV2(21782, "Choosing sync source candidate by request", "syncSource"_attr = syncSource);
    std::string msg(str::stream() << "syncing from: " << syncSource.toString() << " by request");
    setMyHeartbeatMessage(now, msg);
    return syncSource;
}

boost::optional<HostAndPort> TopologyCoordinator::_chooseSyncSourceInitialChecks(Date_t now) {
    // If we are not a member of the current replica set configuration, no sync source is valid.
    if (_selfIndex == -1) {
        LOGV2_DEBUG(
            21778, 1, "Cannot sync from any members because we are not in the replica set config");
        return HostAndPort();
    }

    if (auto sfp = forceSyncSourceCandidate.scoped(); MONGO_unlikely(sfp.isActive())) {
        const auto& data = sfp.getData();
        const auto hostAndPortElem = data["hostAndPort"];
        if (!hostAndPortElem) {
            LOGV2_FATAL(50835,
                        "'forceSyncSoureCandidate' parameter set with invalid host and port: "
                        "{failpointData}",
                        "'forceSyncSoureCandidate' parameter set with invalid host and port",
                        "failpointData"_attr = data);
        }

        const auto hostAndPort = HostAndPort(hostAndPortElem.checkAndGetStringData());
        const int syncSourceIndex = _rsConfig.findMemberIndexByHostAndPort(hostAndPort);
        if (syncSourceIndex < 0) {
            LOGV2(3873118,
                  "'forceSyncSourceCandidate' failed due to host and port not in "
                  "replica set config.",
                  "syncSourceCandidate"_attr = hostAndPort.toString());
            fassertFailed(50836);
        }

        if (_memberIsDenylisted(_rsConfig.getMemberAt(syncSourceIndex), now)) {
            LOGV2(3873119,
                  "Cannot select a sync source because forced candidate is denylisted.",
                  "syncSourceCandidate"_attr = hostAndPort.toString());
            return HostAndPort();
        }

        auto syncSource = _rsConfig.getMemberAt(syncSourceIndex).getHostAndPort();
        LOGV2(21781,
              "Choosing sync source candidate due to 'forceSyncSourceCandidate' parameter",
              "syncSource"_attr = syncSource);
        std::string msg(str::stream() << "syncing from: " << syncSource.toString()
                                      << " by 'forceSyncSourceCandidate' parameter");
        setMyHeartbeatMessage(now, msg);
        return syncSource;
    }

    // wait for 2N pings (not counting ourselves) before choosing a sync target
    int numPingsNeeded = (_memberData.size() - 1) * 2 - pingsInConfig;

    if (numPingsNeeded > 0) {
        static Occasionally sampler;
        if (sampler.tick()) {
            LOGV2(21783,
                  "waiting for {pingsNeeded} pings from other members before syncing",
                  "Waiting for pings from other members before syncing",
                  "pingsNeeded"_attr = numPingsNeeded);
        }
        return HostAndPort();
    }
    return boost::none;
}

HostAndPort TopologyCoordinator::_choosePrimaryAsSyncSource(Date_t now,
                                                            const OpTime& lastOpTimeFetched) {
    LOGV2_DEBUG(5676400,
                2,
                "Attempting to choose current primary as sync source",
                "currentPrimaryIndex"_attr = _currentPrimaryIndex);
    if (_currentPrimaryIndex == -1) {
        LOGV2_DEBUG(21784,
                    1,
                    "Cannot select the primary as sync source because"
                    " the primary is unknown/down.");
        return HostAndPort();
    } else if (_memberIsDenylisted(*getCurrentPrimaryMember(), now)) {
        LOGV2_DEBUG(
            3873116,
            1,
            "Cannot select the primary as sync source because the primary member is denylisted",
            "primary"_attr = getCurrentPrimaryMember()->getHostAndPort());
        return HostAndPort();
    } else if (_currentPrimaryIndex == _selfIndex) {
        LOGV2_DEBUG(
            21786, 1, "Cannot select the primary as sync source because this node is primary.");
        return HostAndPort();
    } else if (_memberData.at(_currentPrimaryIndex).getLastAppliedOpTime() < lastOpTimeFetched) {
        LOGV2_DEBUG(4615639,
                    1,
                    "Cannot select the primary as sync source because the primary "
                    "is behind this node.",
                    "primary"_attr = getCurrentPrimaryMember()->getHostAndPort(),
                    "primaryOpTime"_attr =
                        _memberData.at(_currentPrimaryIndex).getLastAppliedOpTime(),
                    "lastFetchedOpTime"_attr = lastOpTimeFetched);
        return HostAndPort();
    } else {
        auto syncSource = getCurrentPrimaryMember()->getHostAndPort();
        LOGV2(3873117, "Choosing primary as sync source", "primary"_attr = syncSource);
        std::string msg(str::stream() << "syncing from primary: " << syncSource.toString());
        setMyHeartbeatMessage(now, msg);
        return syncSource;
    }
}

bool TopologyCoordinator::_memberIsDenylisted(const MemberConfig& memberConfig, Date_t now) const {
    std::map<HostAndPort, Date_t>::const_iterator denylisted =
        _syncSourceDenylist.find(memberConfig.getHostAndPort());
    if (denylisted != _syncSourceDenylist.end()) {
        if (denylisted->second > now) {
            return true;
        }
    }
    return false;
}

void TopologyCoordinator::denylistSyncSource(const HostAndPort& host, Date_t until) {
    LOGV2_DEBUG(21800,
                2,
                "denylisting {syncSource} until {until}",
                "Denylisting sync source",
                "syncSource"_attr = host,
                "until"_attr = until.toString());
    _syncSourceDenylist[host] = until;
}

void TopologyCoordinator::undenylistSyncSource(const HostAndPort& host, Date_t now) {
    std::map<HostAndPort, Date_t>::iterator hostItr = _syncSourceDenylist.find(host);
    if (hostItr != _syncSourceDenylist.end() && now >= hostItr->second) {
        LOGV2_DEBUG(21801,
                    2,
                    "undenylisting {syncSource}",
                    "Undenylisting sync source",
                    "syncSource"_attr = host);
        _syncSourceDenylist.erase(hostItr);
    }
}

void TopologyCoordinator::clearSyncSourceDenylist() {
    _syncSourceDenylist.clear();
}

void TopologyCoordinator::prepareSyncFromResponse(const HostAndPort& target,
                                                  BSONObjBuilder* response,
                                                  Status* result) {
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

    ReplSetConfig::MemberIterator targetConfig = _rsConfig.membersEnd();
    int targetIndex = 0;
    for (ReplSetConfig::MemberIterator it = _rsConfig.membersBegin(); it != _rsConfig.membersEnd();
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

    const MemberData& hbdata = _memberData.at(targetIndex);
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
    const OpTime lastOpApplied = getMyLastAppliedOpTime();
    if (hbdata.getHeartbeatAppliedOpTime().getSecs() + 10 < lastOpApplied.getSecs()) {
        LOGV2_WARNING(
            21837,
            "attempting to sync from {syncSource}, but its latest opTime is "
            "{syncSourceHeartbeatAppliedOpTime} and ours is "
            "{lastOpApplied} so this may not work",
            "Attempting to sync from sync source, but it is more than 10 seconds behind us",
            "syncSource"_attr = target,
            "syncSourceHeartbeatAppliedOpTime"_attr = hbdata.getHeartbeatAppliedOpTime().getSecs(),
            "lastOpApplied"_attr = lastOpApplied.getSecs());
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

// produce a reply to a heartbeat, and return whether the remote node's config has changed.
StatusWith<bool> TopologyCoordinator::prepareHeartbeatResponseV1(
    Date_t now,
    const ReplSetHeartbeatArgsV1& args,
    StringData ourSetName,
    ReplSetHeartbeatResponse* response) {
    // Verify that replica set names match
    const std::string rshb = args.getSetName();
    if (ourSetName != rshb) {
        LOGV2(21802,
              "replSet set names do not match, ours: {ourSetName}; remote node's: "
              "{remoteNodeSetName}",
              "replSet set names do not match",
              "ourSetName"_attr = ourSetName,
              "remoteNodeSetName"_attr = rshb);
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
        if (args.getSenderId() == _selfConfig().getId().getData()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Received heartbeat from member with the same "
                                           "member ID as ourself: "
                                        << args.getSenderId());
        }
    }

    response->setSetName(ourSetName);

    response->setState(myState.s);

    if (myState.primary()) {
        response->setElectionTime(_electionTime);
    }

    OpTimeAndWallTime lastOpApplied;
    OpTimeAndWallTime lastOpDurable;

    // We include null times for lastApplied and lastDurable if we are in STARTUP_2, as we do not
    // want to report replication progress and be part of write majorities while in initial sync.
    if (!myState.startup2()) {
        lastOpApplied = getMyLastAppliedOpTimeAndWallTime();
        lastOpDurable = getMyLastDurableOpTimeAndWallTime();
    }

    response->setAppliedOpTimeAndWallTime(lastOpApplied);
    response->setDurableOpTimeAndWallTime(lastOpDurable);

    if (_currentPrimaryIndex != -1) {
        response->setPrimaryId(_rsConfig.getMemberAt(_currentPrimaryIndex).getId().getData());
    }

    response->setTerm(_term);

    if (!_syncSource.empty()) {
        response->setSyncingTo(_syncSource);
    }

    if (!_rsConfig.isInitialized()) {
        response->setConfigVersion(-2);
        return false;
    }

    response->setElectable(
        !_getMyUnelectableReason(now, StartElectionReasonEnum::kElectionTimeout));

    const long long v = _rsConfig.getConfigVersion();
    const long long t = _rsConfig.getConfigTerm();
    response->setConfigVersion(v);
    response->setConfigTerm(t);
    // Deliver new config if caller's config is older than ours
    if (_rsConfig.getConfigVersionAndTerm() > args.getConfigVersionAndTerm()) {
        response->setConfig(_rsConfig);
    }

    // Resolve the caller's id in our Member list
    int from = -1;
    if (v == args.getConfigVersion() && args.getSenderId() != -1) {
        from = _getMemberIndex(args.getSenderId());
    }
    if (from == -1) {
        return false;
    }
    invariant(from != _selfIndex);

    auto& fromNodeData = _memberData.at(from);
    // note that we got a heartbeat from this node
    fromNodeData.setLastHeartbeatRecv(now);
    // Update liveness for sending node.
    fromNodeData.updateLiveness(now);
    return fromNodeData.getConfigVersionAndTerm() < args.getConfigVersionAndTerm();
}

int TopologyCoordinator::_getMemberIndex(int id) const {
    int index = 0;
    for (ReplSetConfig::MemberIterator it = _rsConfig.membersBegin(); it != _rsConfig.membersEnd();
         ++it, ++index) {
        if (it->getId() == MemberId(id)) {
            return index;
        }
    }
    return -1;
}

std::pair<ReplSetHeartbeatArgsV1, Milliseconds> TopologyCoordinator::prepareHeartbeatRequestV1(
    Date_t now, StringData ourSetName, const HostAndPort& target) {
    PingStats& hbStats = _pings[target];
    Milliseconds alreadyElapsed(now.asInt64() - hbStats.getLastHeartbeatStartDate().asInt64());
    if ((!_rsConfig.isInitialized()) || !hbStats.trying() ||
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
        if (_rsConfig.getConfigTerm() != OpTime::kUninitializedTerm) {
            hbArgs.setConfigTerm(_rsConfig.getConfigTerm());
        }
        if (_currentPrimaryIndex >= 0) {
            // Send primary member id if one exists.
            hbArgs.setPrimaryId(_memberData.at(_currentPrimaryIndex).getMemberId().getData());
        }
        if (_selfIndex >= 0) {
            const MemberConfig& me = _selfConfig();
            hbArgs.setSenderId(me.getId().getData());
            hbArgs.setSenderHost(me.getHostAndPort());
        }
        hbArgs.setTerm(_term);
    } else {
        hbArgs.setSetName(ourSetName);
        // Config version -2 is for uninitialized config.
        hbArgs.setConfigVersion(-2);
        hbArgs.setTerm(OpTime::kInitialTerm);
    }
    hbArgs.setHeartbeatVersion(1);

    const Milliseconds timeoutPeriod(
        _rsConfig.isInitialized() ? _rsConfig.getHeartbeatTimeoutPeriodMillis()
                                  : Milliseconds{ReplSetConfig::kDefaultHeartbeatTimeoutPeriod});
    const Milliseconds timeout(timeoutPeriod - alreadyElapsed);
    return std::make_pair(hbArgs, timeout);
}

bool isUnrecoverableHeartbeatFailure(Status status) {
    return status.code() == ErrorCodes::InconsistentReplicaSetNames;
}

HeartbeatResponseAction TopologyCoordinator::processHeartbeatResponse(
    Date_t now,
    Milliseconds networkRoundTripTime,
    const HostAndPort& target,
    const StatusWith<ReplSetHeartbeatResponse>& hbResponse) {
    PingStats& hbStats = _pings[target];
    invariant(hbStats.getLastHeartbeatStartDate() != Date_t());
    const bool isUnauthorized = (hbResponse.getStatus().code() == ErrorCodes::Unauthorized) ||
        (hbResponse.getStatus().code() == ErrorCodes::AuthenticationFailed);

    // Replication of auth changes can cause temporary auth failures.
    if (hbResponse.isOK() || isUnauthorized) {
        hbStats.hit(networkRoundTripTime);
    } else {
        hbStats.miss();
    }

    // If a node is not PRIMARY and has no sync source, we increase the heartbeat rate in order
    // to help it find a sync source more quickly, which helps ensure the PRIMARY will continue to
    // see the majority of the cluster.
    //
    // Arbiters also decrease their heartbeat interval to at most half the election timeout period.
    Milliseconds heartbeatInterval = _rsConfig.getHeartbeatInterval();
    if (getMemberState().arbiter()) {
        heartbeatInterval =
            std::min(_rsConfig.getElectionTimeoutPeriod() / 2, _rsConfig.getHeartbeatInterval());
    } else if (getSyncSourceAddress().empty() && !_iAmPrimary()) {
        heartbeatInterval = std::min(_rsConfig.getElectionTimeoutPeriod() / 2,
                                     _rsConfig.getHeartbeatInterval() / 4);
    }

    const Milliseconds alreadyElapsed = now - hbStats.getLastHeartbeatStartDate();
    Date_t nextHeartbeatStartDate;
    // Determine the next heartbeat start time. If a heartbeat has not succeeded or failed, and we
    // have not used up the timeout period, we should retry.
    if (hbStats.trying() && (alreadyElapsed < _rsConfig.getHeartbeatTimeoutPeriod())) {
        // There are still retries left, let's use one.
        nextHeartbeatStartDate = now;
    } else {
        nextHeartbeatStartDate = now + heartbeatInterval;
    }

    if (hbStats.failed()) {
        LOGV2_FOR_HEARTBEATS(
            23974,
            0,
            "Heartbeat to {target} failed after {maxHeartbeatRetries} retries, response "
            "status: {error}",
            "Heartbeat failed after max retries",
            "target"_attr = target,
            "maxHeartbeatRetries"_attr = kMaxHeartbeatRetries,
            "error"_attr = hbResponse.getStatus());
    }

    HeartbeatResponseAction nextAction = HeartbeatResponseAction::makeNoAction();
    nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);

    if (hbResponse.isOK() && hbResponse.getValue().hasConfig()) {
        // -2 is for uninitialized config.
        const ConfigVersionAndTerm currentConfigVersionAndTerm = _rsConfig.isInitialized()
            ? _rsConfig.getConfigVersionAndTerm()
            : ConfigVersionAndTerm(-2, OpTime::kUninitializedTerm);
        const ReplSetConfig& newConfig = hbResponse.getValue().getConfig();
        if (newConfig.getConfigVersionAndTerm() > currentConfigVersionAndTerm) {
            nextAction = HeartbeatResponseAction::makeReconfigAction();
            nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);

            // Only continue processing heartbeat in primary state. In other states it is not
            // safe to continue processing heartbeat and should start reconfig right away.
            // e.g. if this node was removed from replSet, _selfIndex is -1, and a following
            // check on _selfIndex will keep retrying heartbeat to fetch new config, preventing
            // the new config to be installed.
            if (_role != Role::kLeader) {
                return nextAction;
            }

            // Continue processing heartbeat responses even if we decide to install a new config.
        } else {
            // Could be we got the newer version before we got the response, or the
            // target erroneously sent us one, even though it isn't newer.
            if (newConfig.getConfigVersionAndTerm() < currentConfigVersionAndTerm) {
                LOGV2_DEBUG(21803, 1, "Config version from heartbeat was older than ours");
            } else {
                LOGV2_DEBUG(21804, 2, "Config from heartbeat response was same as ours");
            }
            if (_rsConfig.isInitialized()) {
                LOGV2_DEBUG(
                    4615641,
                    2,
                    "Current config: {currentConfig}; Config in heartbeat: {heartbeatConfig}",
                    "Heartbeat config",
                    "currentConfig"_attr = _rsConfig.toBSON(),
                    "heartbeatConfig"_attr = newConfig.toBSON());
            } else {
                LOGV2_DEBUG(4615647,
                            2,
                            "Config in heartbeat: {heartbeatConfig}",
                            "Heartbeat config",
                            "heartbeatConfig"_attr = newConfig.toBSON());
            }
        }
    }

    // Check if the heartbeat target is in our config.  If it isn't, there's nothing left to do,
    // so return early.
    if (!_rsConfig.isInitialized()) {
        return nextAction;
    }
    // This server is not in the config, either because it was removed or a DNS error finding self.
    if (_selfIndex == -1) {
        LOGV2(3564900,
              "Could not find self in current config, retrying DNS resolution of members",
              "target"_attr = target,
              "currentConfig"_attr = _rsConfig.toBSON());
        nextAction = HeartbeatResponseAction::makeRetryReconfigAction();
        nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
        return nextAction;
    }
    const int memberIndex = _rsConfig.findMemberIndexByHostAndPort(target);
    if (memberIndex == -1) {
        LOGV2_DEBUG(21806,
                    1,
                    "Could not find {target} in current config so ignoring --"
                    " current config: {currentConfig}",
                    "Could not find target in current config so ignoring",
                    "target"_attr = target,
                    "currentConfig"_attr = _rsConfig.toBSON());
        return nextAction;
    }

    invariant(memberIndex != _selfIndex);

    MemberData& hbData = _memberData.at(memberIndex);
    const MemberConfig member = _rsConfig.getMemberAt(memberIndex);
    bool advancedOpTimeOrUpdatedConfig = false;
    bool becameElectable = false;
    bool changedMemberState = false;
    if (!hbResponse.isOK()) {
        if (isUnauthorized) {
            hbData.setAuthIssue(now);
        }
        // If the heartbeat has failed i.e. used up all retries, then we mark the target node as
        // down.
        else if (hbStats.failed() || (alreadyElapsed >= _rsConfig.getHeartbeatTimeoutPeriod()) ||
                 isUnrecoverableHeartbeatFailure(hbResponse.getStatus())) {
            hbData.setDownValues(now, hbResponse.getStatus().reason());
        } else {
            LOGV2_DEBUG(21807,
                        3,
                        "Bad heartbeat response from {target}; trying again; Retries left: "
                        "{retriesLeft}; {retriesElapsed} have already elapsed",
                        "Bad heartbeat response; trying again",
                        "target"_attr = target,
                        "retriesLeft"_attr = (hbStats.retriesLeft()),
                        "retriesElapsed"_attr = alreadyElapsed);
        }
    } else {
        ReplSetHeartbeatResponse hbr = std::move(hbResponse.getValue());
        LOGV2_DEBUG(21808,
                    3,
                    "setUpValues: heartbeat response good for member _id:{memberId}",
                    "setUpValues: heartbeat response good",
                    "memberId"_attr = member.getId());
        pingsInConfig++;
        auto wasUnelectable = hbData.isUnelectable();
        auto hbChanges = hbData.setUpValues(now, std::move(hbr));
        advancedOpTimeOrUpdatedConfig =
            hbChanges.getOpTimeAdvanced() || hbChanges.getConfigChanged();
        changedMemberState = hbChanges.getMemberStateChanged();
        becameElectable = wasUnelectable && !hbData.isUnelectable();
    }

    _updatePrimaryFromHBDataV1(now);

    // If we've decided to install a newer config, we don't need to consider takeovers.
    if (nextAction.getAction() == HeartbeatResponseAction::Reconfig) {
        return nextAction;
    }

    nextAction = _shouldTakeOverPrimary(memberIndex);

    nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
    nextAction.setAdvancedOpTimeOrUpdatedConfig(advancedOpTimeOrUpdatedConfig);
    nextAction.setBecameElectable(becameElectable);
    nextAction.setChangedMemberState(changedMemberState);
    return nextAction;
}

bool TopologyCoordinator::haveNumNodesReachedOpTime(const OpTime& targetOpTime,
                                                    int numNodes,
                                                    bool durablyWritten) {
    // Replication progress that is for some reason ahead of us should not allow us to
    // satisfy a write concern if we aren't caught up ourselves.
    OpTime myOpTime = durablyWritten ? getMyLastDurableOpTime() : getMyLastAppliedOpTime();
    if (myOpTime < targetOpTime) {
        return false;
    }

    // Invariants that we only wait for an OpTime in the term that this node is currently writing
    // to. In other words, we do not support waiting for an OpTime written by a previous primary
    // because comparing members' lastApplied/lastDurable alone is not sufficient to tell if the
    // OpTime has been replicated.
    invariant(targetOpTime.getTerm() == getMyLastAppliedOpTime().getTerm());

    for (auto&& memberData : _memberData) {
        const auto isArbiter = _rsConfig.getMemberAt(memberData.getConfigIndex()).isArbiter();

        // We do not count arbiters towards the write concern.
        if (isArbiter) {
            continue;
        }

        const OpTime& memberOpTime =
            durablyWritten ? memberData.getLastDurableOpTime() : memberData.getLastAppliedOpTime();

        // In addition to checking if a member has a greater/equal timestamp field we also need to
        // make sure that the memberOpTime is in the same term as the OpTime we wait for. If a
        // member's OpTime has a higher term, it indicates that this node will be stepping down. And
        // thus we do not know if the target OpTime in our previous term has been replicated to the
        // member because the memberOpTime in a higher term could correspond to an operation in a
        // divergent branch of history regardless of its timestamp.
        if (memberOpTime.getTerm() == targetOpTime.getTerm() &&
            memberOpTime.getTimestamp() >= targetOpTime.getTimestamp()) {
            --numNodes;
        }

        if (numNodes <= 0) {
            return true;
        }
    }
    return false;
}

bool TopologyCoordinator::haveTaggedNodesReachedOpTime(const OpTime& opTime,
                                                       const ReplSetTagPattern& tagPattern,
                                                       bool durablyWritten) {
    auto pred = makeOpTimePredicate(opTime, durablyWritten);
    return haveTaggedNodesSatisfiedCondition(pred, tagPattern);
}

TopologyCoordinator::MemberPredicate TopologyCoordinator::makeOpTimePredicate(const OpTime& opTime,
                                                                              bool durablyWritten) {
    // Invariants that we only wait for an OpTime in the term that this node is currently writing
    // to. In other words, we do not support waiting for an OpTime written by a previous primary
    // because comparing members' lastApplied/lastDurable alone is not sufficient to tell if the
    // OpTime has been replicated.
    invariant(opTime.getTerm() == getMyLastAppliedOpTime().getTerm());

    return [=](const MemberData& memberData) {
        auto memberOpTime =
            durablyWritten ? memberData.getLastDurableOpTime() : memberData.getLastAppliedOpTime();

        // In addition to checking if a member has a greater/equal timestamp field we also need to
        // make sure that the memberOpTime is in the same term as the OpTime we wait for. If a
        // member's OpTime has a higher term, it indicates that this node will be stepping down. And
        // thus we do not know if the target OpTime in our previous term has been replicated to the
        // member because the memberOpTime in a higher term could correspond to an operation in a
        // divergent branch of history regardless of its timestamp.
        return memberOpTime.getTerm() == opTime.getTerm() &&
            memberOpTime.getTimestamp() >= opTime.getTimestamp();
    };
}

TopologyCoordinator::MemberPredicate TopologyCoordinator::makeConfigPredicate() {
    return [&](const MemberData& memberData) {
        return memberData.getConfigVersionAndTerm() == _rsConfig.getConfigVersionAndTerm();
    };
}

bool TopologyCoordinator::haveTaggedNodesSatisfiedCondition(
    std::function<bool(const MemberData&)> pred, const ReplSetTagPattern& tagPattern) {
    ReplSetTagMatch matcher(tagPattern);

    for (auto&& memberData : _memberData) {
        if (pred(memberData)) {
            // This node has satisfied the predicate, now we need to check if it is a part
            // of the tagPattern.
            int memberIndex = memberData.getConfigIndex();
            invariant(memberIndex >= 0);
            const MemberConfig& memberConfig = _rsConfig.getMemberAt(memberIndex);
            for (auto&& it = memberConfig.tagsBegin(); it != memberConfig.tagsEnd(); ++it) {
                if (matcher.update(*it)) {
                    return true;
                }
            }
        }
    }
    return false;
}

HeartbeatResponseAction TopologyCoordinator::checkMemberTimeouts(Date_t now) {
    bool stepdown = false;
    for (int memberIndex = 0; memberIndex < static_cast<int>(_memberData.size()); memberIndex++) {
        auto& memberData = _memberData[memberIndex];
        if (!memberData.isSelf() && !memberData.lastUpdateStale() &&
            now - memberData.getLastUpdate() >= _rsConfig.getElectionTimeoutPeriod()) {
            memberData.markLastUpdateStale();
            if (_iAmPrimary()) {
                stepdown = stepdown || setMemberAsDown(now, memberIndex);
            }
        }
    }
    if (stepdown) {
        LOGV2(21809, "Can't see a majority of the set, relinquishing primary");
        return HeartbeatResponseAction::makeStepDownSelfAction(_selfIndex);
    }
    return HeartbeatResponseAction::makeNoAction();
}

std::vector<HostAndPort> TopologyCoordinator::getHostsWrittenTo(const OpTime& op,
                                                                bool durablyWritten) {
    std::vector<HostAndPort> hosts;
    for (const auto& memberData : _memberData) {
        if (durablyWritten) {
            if (memberData.getLastDurableOpTime() < op) {
                continue;
            }
        } else if (memberData.getLastAppliedOpTime() < op) {
            continue;
        }

        hosts.push_back(memberData.getHostAndPort());
    }
    return hosts;
}

bool TopologyCoordinator::setMemberAsDown(Date_t now, const int memberIndex) {
    invariant(memberIndex != _selfIndex);
    invariant(memberIndex != -1);
    invariant(_currentPrimaryIndex == _selfIndex);
    MemberData& hbData = _memberData.at(memberIndex);
    hbData.setDownValues(now, "no response within election timeout period");

    if (CannotSeeMajority &
        _getMyUnelectableReason(now, StartElectionReasonEnum::kElectionTimeout)) {
        return true;
    }

    return false;
}

std::pair<MemberId, Date_t> TopologyCoordinator::getStalestLiveMember() const {
    Date_t earliestDate = Date_t::max();
    MemberId earliestMemberId;
    for (const auto& memberData : _memberData) {
        if (memberData.isSelf()) {
            continue;
        }
        if (memberData.lastUpdateStale()) {
            // Already stale.
            continue;
        }
        LOGV2_DEBUG(21810,
                    3,
                    "memberData lastupdate is: {memberDataLastUpdate}",
                    "memberData last update",
                    "memberDataLastUpdate"_attr = memberData.getLastUpdate());
        if (earliestDate > memberData.getLastUpdate()) {
            earliestDate = memberData.getLastUpdate();
            earliestMemberId = memberData.getMemberId();
        }
    }
    LOGV2_DEBUG(21811,
                3,
                "stalest member {earliestMemberId} date: {earliestDate}",
                "Stalest member",
                "earliestMemberId"_attr = earliestMemberId,
                "earliestDate"_attr = earliestDate);
    return std::make_pair(earliestMemberId, earliestDate);
}

void TopologyCoordinator::resetMemberTimeouts(Date_t now,
                                              const stdx::unordered_set<HostAndPort>& member_set) {
    for (auto&& memberData : _memberData) {
        if (member_set.count(memberData.getHostAndPort()))
            memberData.updateLiveness(now);
    }
}

OpTime TopologyCoordinator::getMyLastAppliedOpTime() const {
    return _selfMemberData().getLastAppliedOpTime();
}

OpTimeAndWallTime TopologyCoordinator::getMyLastAppliedOpTimeAndWallTime() const {
    return {_selfMemberData().getLastAppliedOpTime(), _selfMemberData().getLastAppliedWallTime()};
}

void TopologyCoordinator::setMyLastAppliedOpTimeAndWallTime(OpTimeAndWallTime opTimeAndWallTime,
                                                            Date_t now,
                                                            bool isRollbackAllowed) {
    auto opTime = opTimeAndWallTime.opTime;
    auto& myMemberData = _selfMemberData();
    auto myLastAppliedOpTime = myMemberData.getLastAppliedOpTime();

    if (!(isRollbackAllowed || opTime == myLastAppliedOpTime)) {
        invariant(opTime > myLastAppliedOpTime);
        // In pv1, oplog entries are ordered by non-decreasing term and strictly increasing
        // timestamp. So, in pv1, its not possible for us to get opTime with higher term and
        // timestamp lesser than or equal to our current lastAppliedOptime.
        invariant(opTime.getTerm() == OpTime::kUninitializedTerm ||
                  myLastAppliedOpTime.getTerm() == OpTime::kUninitializedTerm ||
                  opTime.getTimestamp() > myLastAppliedOpTime.getTimestamp());
    }

    myMemberData.setLastAppliedOpTimeAndWallTime(opTimeAndWallTime, now);
}

OpTime TopologyCoordinator::getMyLastDurableOpTime() const {
    return _selfMemberData().getLastDurableOpTime();
}

OpTimeAndWallTime TopologyCoordinator::getMyLastDurableOpTimeAndWallTime() const {
    return {_selfMemberData().getLastDurableOpTime(), _selfMemberData().getLastDurableWallTime()};
}

void TopologyCoordinator::setMyLastDurableOpTimeAndWallTime(OpTimeAndWallTime opTimeAndWallTime,
                                                            Date_t now,
                                                            bool isRollbackAllowed) {
    auto opTime = opTimeAndWallTime.opTime;
    auto& myMemberData = _selfMemberData();
    invariant(isRollbackAllowed || opTime >= myMemberData.getLastDurableOpTime());
    myMemberData.setLastDurableOpTimeAndWallTime(opTimeAndWallTime, now);
}

StatusWith<bool> TopologyCoordinator::setLastOptimeForMember(
    const UpdatePositionArgs::UpdateInfo& args, Date_t now) {
    if (_selfIndex == -1) {
        // Ignore updates when we're in state REMOVED.
        return Status(ErrorCodes::NotPrimaryOrSecondary,
                      "Received replSetUpdatePosition command but we are in state REMOVED");
    }
    invariant(_rsConfig.isInitialized());  // Can only use setLastOptimeForMember in replSet mode.

    MemberId memberId;
    try {
        memberId = MemberId(args.memberId);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    if (memberId == _rsConfig.getMemberAt(_selfIndex).getId()) {
        // Do not let remote nodes tell us what our optime is.
        return false;
    }

    LOGV2_DEBUG(21812,
                2,
                "received notification that node with memberID {memberId} in config with version "
                "{configVersion} has reached optime: {appliedOpTime} and is durable through: "
                "{durableOpTime}",
                "Received replSetUpdatePosition",
                "memberId"_attr = memberId,
                "configVersion"_attr = args.cfgver,
                "appliedOpTime"_attr = args.appliedOpTime,
                "durableOpTime"_attr = args.durableOpTime);

    auto* memberData = _findMemberDataByMemberId(memberId.getData());

    // If we are applying a splitConfig for a shard split, we may still be receiving updates for
    // nodes that have been removed from the donor set.
    if (_rsConfig.isSplitConfig() && !memberData &&
        memberId.getData() >= _rsConfig.getNumMembers()) {
        LOGV2(6234605,
              "Skipping update from node",
              "data"_attr = memberId.getData(),
              "conf"_attr = _rsConfig);
        // Do not advance optime
        return false;
    }

    // While we can accept replSetUpdatePosition commands across config versions, we still do not
    // allow receiving them from a node that is not in our config.
    if (!memberData) {
        invariant(!_rsConfig.findMemberByID(memberId.getData()));

        static constexpr char errmsg[] =
            "Received replSetUpdatePosition for node which doesn't exist in our config";
        LOGV2_DEBUG(21814, 1, errmsg, "memberId"_attr = memberId);
        return Status(ErrorCodes::NodeNotFound,
                      str::stream() << errmsg << ", memberId: " << memberId);
    }

    invariant(memberId == memberData->getMemberId());

    auto durableOpTime = args.durableOpTime;
    auto durableWallTime = args.durableWallTime;

    // Arbiters are always expected to report null durable optimes (and wall times).
    // If that is not the case here, make sure to correct these times before ingesting them.
    auto& memberInConfig = _rsConfig.getMemberAt(memberData->getConfigIndex());
    if ((memberData->getState().arbiter() || memberInConfig.isArbiter()) &&
        (!args.durableOpTime.isNull() || args.durableWallTime != Date_t())) {
        LOGV2(5662001,
              "Received non-null durable optime/walltime for arbiter from "
              "replSetUpdatePosition. Ignoring value(s).",
              "memberId"_attr = memberId,
              "durableOpTime"_attr = args.durableOpTime,
              "durableWallTime"_attr = args.durableWallTime);
        durableOpTime = OpTime();
        durableWallTime = Date_t();
    }

    LOGV2_DEBUG(21815,
                3,
                "Node with memberID {memberId} currently has optime {oldLastAppliedOpTime} "
                "durable through {oldLastDurableOpTime}; updating to optime "
                "{newAppliedOpTime} and durable through {newDurableOpTime}",
                "Updating member data due to replSetUpdatePosition",
                "memberId"_attr = memberId,
                "oldLastAppliedOpTime"_attr = memberData->getLastAppliedOpTime(),
                "oldLastDurableOpTime"_attr = memberData->getLastDurableOpTime(),
                "newAppliedOpTime"_attr = args.appliedOpTime,
                "newDurableOpTime"_attr = durableOpTime);

    bool advancedOpTime = memberData->advanceLastAppliedOpTimeAndWallTime(
        {args.appliedOpTime, args.appliedWallTime}, now);
    advancedOpTime =
        memberData->advanceLastDurableOpTimeAndWallTime({durableOpTime, durableWallTime}, now) ||
        advancedOpTime;
    return advancedOpTime;
}

void TopologyCoordinator::updateLastCommittedInPrevConfig() {
    _lastCommittedInPrevConfig = _lastCommittedOpTimeAndWallTime.opTime;
}

OpTime TopologyCoordinator::getLastCommittedInPrevConfig() {
    return _lastCommittedInPrevConfig;
}

OpTime TopologyCoordinator::getConfigOplogCommitmentOpTime() {
    // If we were previously a secondary, we must make sure that we commit a new op as primary
    // before we can commit any other oplog entries, which necessitates the need for using the
    // '_firstOpTimeOfMyTerm' value here.
    return std::max(_lastCommittedInPrevConfig, _firstOpTimeOfMyTerm);
}

MemberData* TopologyCoordinator::_findMemberDataByMemberId(const int memberId) {
    const int memberIndex = _getMemberIndex(memberId);
    if (memberIndex >= 0)
        return &_memberData[memberIndex];
    return nullptr;
}

void TopologyCoordinator::_updatePrimaryFromHBDataV1(Date_t now) {
    // Updates the local notion of which remote node, if any is primary.
    invariant(_selfIndex != -1);

    // If we are the primary, there must be no other primary, otherwise its higher term would
    // have already made us step down.
    if (_currentPrimaryIndex == _selfIndex) {
        return;
    }

    // Scan the member list's heartbeat data for who is primary, and update _currentPrimaryIndex.
    int primaryIndex = -1;
    for (size_t i = 0; i < _memberData.size(); i++) {
        const MemberData& member = _memberData.at(i);
        if (member.getState().primary() && member.up()) {
            if (primaryIndex == -1 || _memberData.at(primaryIndex).getTerm() < member.getTerm()) {
                primaryIndex = i;
            }
        }
    }
    LOGV2_DEBUG(5676401,
                2,
                "Updating primary index from heartbeat data",
                "primaryIndex"_attr = primaryIndex,
                "previousPrimaryIndex"_attr = _currentPrimaryIndex);
    _currentPrimaryIndex = primaryIndex;

    // Clear last heartbeat message on ourselves.
    setMyHeartbeatMessage(now, "");
}

HeartbeatResponseAction TopologyCoordinator::_shouldTakeOverPrimary(int updatedConfigIndex) {
    // Takeover when the replset is stable.
    //
    // Take over the primary only if the remote primary is in the latest term I know.
    // This is done only when we get a heartbeat response from the primary.
    // Otherwise, there must be an outstanding election, which may succeed or not, but
    // the remote primary will become aware of that election eventually and step down.

    if (_currentPrimaryIndex == -1) {
        return HeartbeatResponseAction::makeNoAction();
    }

    auto primaryIndex = _currentPrimaryIndex;
    if (_memberData.at(primaryIndex).getTerm() != _term || updatedConfigIndex != primaryIndex) {
        return HeartbeatResponseAction::makeNoAction();
    }

    // Don't schedule catchup takeover if catchup takeover or primary catchup is disabled.
    const bool catchupTakeoverDisabled =
        ReplSetConfig::kCatchUpDisabled == _rsConfig.getCatchUpTimeoutPeriod() ||
        ReplSetConfig::kCatchUpTakeoverDisabled == _rsConfig.getCatchUpTakeoverDelay();

    bool scheduleCatchupTakeover = false;
    bool schedulePriorityTakeover = false;

    // If we have a stale view of the new primary's opTime and believe its opTime to be
    // less than our own, we may end up scheduling an unecessary takeover. Primaries
    // increment the term as soon as they start a real election, but they do not write
    // anything in that new term until they have finished their full state transition.
    // Thus, if we have applied anything in the new term, it means that the primary is
    // already past the catchup phase and we should not be attempting a catchup takeover.
    const bool primaryAlreadyCaughtUp = (getMyLastAppliedOpTime().getTerm() == getTerm());

    if (!catchupTakeoverDisabled && !primaryAlreadyCaughtUp &&
        (_memberData.at(primaryIndex).getLastAppliedOpTime() <
         _memberData.at(_selfIndex).getLastAppliedOpTime())) {
        LOGV2_FOR_ELECTION(23975,
                           2,
                           "I can take over the primary due to fresher data",
                           "primaryIndex"_attr = primaryIndex,
                           "primaryTerm"_attr = _memberData.at(primaryIndex).getTerm(),
                           "primaryOpTime"_attr =
                               _memberData.at(primaryIndex).getLastAppliedOpTime(),
                           "myOpTime"_attr = _memberData.at(_selfIndex).getLastAppliedOpTime(),
                           "replicaSetStatus"_attr = _getReplSetStatusString());

        scheduleCatchupTakeover = true;
    }

    if (_rsConfig.getMemberAt(primaryIndex).getPriority() <
        _rsConfig.getMemberAt(_selfIndex).getPriority()) {
        LOGV2_FOR_ELECTION(23977,
                           2,
                           "I can take over the primary due to higher priority",
                           "primaryIndex"_attr = primaryIndex,
                           "primaryTerm"_attr = _memberData.at(primaryIndex).getTerm(),
                           "replicaSetStatus"_attr = _getReplSetStatusString());

        schedulePriorityTakeover = true;
    }

    // Calculate rank of current node. A rank of 0 indicates that it has the highest priority.
    const auto currentNodePriority = _rsConfig.getMemberAt(_selfIndex).getPriority();

    // Schedule a priority takeover early only if we know that the current node has the highest
    // priority in the replica set, has a higher priority than the primary, and is the most
    // up to date node.
    // Otherwise, prefer to schedule a catchup takeover over a priority takeover
    if (scheduleCatchupTakeover && schedulePriorityTakeover &&
        _rsConfig.calculatePriorityRank(currentNodePriority) == 0) {
        LOGV2_FOR_ELECTION(
            23979,
            2,
            "I can take over the primary because I have a higher priority, the highest "
            "priority in the replica set, and fresher data. Current primary index: "
            "{primaryIndex} in term {primaryTerm}",
            "I can take over the primary because I have a higher priority, the highest "
            "priority in the replica set, and fresher data",
            "primaryIndex"_attr = primaryIndex,
            "primaryTerm"_attr = _memberData.at(primaryIndex).getTerm());
        return HeartbeatResponseAction::makePriorityTakeoverAction();
    }
    if (scheduleCatchupTakeover) {
        return HeartbeatResponseAction::makeCatchupTakeoverAction();
    }
    if (schedulePriorityTakeover) {
        return HeartbeatResponseAction::makePriorityTakeoverAction();
    }
    return HeartbeatResponseAction::makeNoAction();
}

bool TopologyCoordinator::_aMajoritySeemsToBeUp() const {
    int vUp = 0;
    for (std::vector<MemberData>::const_iterator it = _memberData.begin(); it != _memberData.end();
         ++it) {
        const int itIndex = indexOfIterator(_memberData, it);
        if (itIndex == _selfIndex || it->up()) {
            vUp += _rsConfig.getMemberAt(itIndex).getNumVotes();
        }
    }

    return vUp * 2 > _rsConfig.getTotalVotingMembers();
}

int TopologyCoordinator::_findHealthyPrimaryOfEqualOrGreaterPriority(
    const int candidateIndex) const {
    const double candidatePriority = _rsConfig.getMemberAt(candidateIndex).getPriority();
    for (auto it = _memberData.begin(); it != _memberData.end(); ++it) {
        if (!it->up() || it->getState() != MemberState::RS_PRIMARY) {
            continue;
        }
        const int itIndex = indexOfIterator(_memberData, it);
        const double priority = _rsConfig.getMemberAt(itIndex).getPriority();
        if (itIndex != candidateIndex && priority >= candidatePriority) {
            return itIndex;
        }
    }

    return -1;
}

bool TopologyCoordinator::_amIFreshEnoughForPriorityTakeover() const {
    const OpTime ourLatestKnownOpTime = latestKnownOpTime();

    // Rules are:
    // - If the terms don't match, we don't call for priority takeover.
    // - If our optime and the latest optime happen in different seconds, our optime must be within
    // at least priorityTakeoverFreshnessWindowSeconds seconds of the latest optime.
    // - If our optime and the latest optime happen in the same second, our optime must be within
    // at least 1000 oplog entries of the latest optime (i.e. the increment portion of the timestamp
    // must be within 1000).  This is to handle the case where a primary had its clock set far into
    // the future, took some writes, then had its clock set back.  In that case the timestamp
    // component of all future oplog entries generated will be the same, until real world time
    // passes the timestamp component of the last oplog entry.

    const OpTime ourLastOpApplied = getMyLastAppliedOpTime();
    if (ourLastOpApplied.getTerm() != ourLatestKnownOpTime.getTerm()) {
        return false;
    }

    if (ourLastOpApplied.getTimestamp().getSecs() !=
        ourLatestKnownOpTime.getTimestamp().getSecs()) {
        return ourLastOpApplied.getTimestamp().getSecs() +
            gPriorityTakeoverFreshnessWindowSeconds >=
            ourLatestKnownOpTime.getTimestamp().getSecs();
    } else {
        return ourLastOpApplied.getTimestamp().getInc() + 1000 >=
            ourLatestKnownOpTime.getTimestamp().getInc();
    }
}

bool TopologyCoordinator::_amIFreshEnoughForCatchupTakeover() const {

    const OpTime ourLatestKnownOpTime = latestKnownOpTime();

    // Rules are:
    // - We must have the freshest optime of all the up nodes.
    // - We must specifically have a fresher optime than the primary (can't be equal).
    // - The term of our last applied op must be less than the current term. This ensures that no
    // writes have happened since the most recent election and that the primary is still in
    // catchup mode.

    // There is no point to a catchup takeover if we aren't the freshest node because
    // another node would immediately perform another catchup takeover when we become primary.
    const OpTime ourLastOpApplied = getMyLastAppliedOpTime();
    if (ourLastOpApplied < ourLatestKnownOpTime) {
        return false;
    }

    if (_currentPrimaryIndex == -1) {
        return false;
    }

    // If we aren't ahead of the primary, there is no point to having a catchup takeover.
    const OpTime primaryLastOpApplied = _memberData[_currentPrimaryIndex].getLastAppliedOpTime();

    if (ourLastOpApplied <= primaryLastOpApplied) {
        return false;
    }

    // If the term of our last applied op is less than the current term, the primary didn't write
    // anything and it is still in catchup mode.
    return ourLastOpApplied.getTerm() < _term;
}

bool TopologyCoordinator::_iAmPrimary() const {
    if (_role == Role::kLeader) {
        invariant(_currentPrimaryIndex == _selfIndex);
        invariant(_leaderMode != LeaderMode::kNotLeader);
        return true;
    }
    return false;
}

bool TopologyCoordinator::prepareForUnconditionalStepDown() {
    if (_leaderMode == LeaderMode::kSteppingDown) {
        // Can only be processing one required stepdown at a time.
        return false;
    }
    // Heartbeat and reconfig (via cmd or heartbeat) initiated stepdowns take precedence over
    // stepdown command initiated stepdowns, so it's safe to transition from kAttemptingStepDown
    // to kSteppingDown.
    _setLeaderMode(LeaderMode::kSteppingDown);
    return true;
}

StatusWith<TopologyCoordinator::StepDownAttemptAbortFn>
TopologyCoordinator::prepareForStepDownAttempt() {
    if (_leaderMode == LeaderMode::kSteppingDown ||
        _leaderMode == LeaderMode::kAttemptingStepDown) {
        return Status{ErrorCodes::ConflictingOperationInProgress,
                      "This node is already in the process of stepping down"};
    }

    if (_leaderMode == LeaderMode::kNotLeader) {
        return Status{ErrorCodes::NotWritablePrimary, "This node is not a primary."};
    }

    invariant(_leaderMode == LeaderMode::kWritablePrimary ||
              _leaderMode == LeaderMode::kLeaderElect);
    const auto previousLeaderMode = _leaderMode;
    _setLeaderMode(LeaderMode::kAttemptingStepDown);

    return {[this, previousLeaderMode] {
        if (_leaderMode == TopologyCoordinator::LeaderMode::kAttemptingStepDown) {
            _setLeaderMode(previousLeaderMode);
        }
    }};
}

void TopologyCoordinator::changeMemberState_forTest(const MemberState& newMemberState,
                                                    const Timestamp& electionTime) {
    invariant(_selfIndex != -1);
    if (newMemberState == getMemberState())
        return;
    switch (newMemberState.s) {
        case MemberState::RS_PRIMARY:
            _role = Role::kCandidate;
            processWinElection(OID(), electionTime);
            invariant(_role == Role::kLeader);
            break;
        case MemberState::RS_SECONDARY:
        case MemberState::RS_ROLLBACK:
        case MemberState::RS_RECOVERING:
        case MemberState::RS_STARTUP2:
            _role = Role::kFollower;
            _followerMode = newMemberState.s;
            if (_currentPrimaryIndex == _selfIndex) {
                _currentPrimaryIndex = -1;
                _setLeaderMode(LeaderMode::kNotLeader);
            }
            break;
        case MemberState::RS_STARTUP:
            updateConfig(ReplSetConfig(), -1, Date_t());
            break;
        default:
            LOGV2_FATAL(21840,
                        "Cannot switch to state {newMemberState}",
                        "Cannot change to this member state",
                        "newMemberState"_attr = newMemberState);
            MONGO_UNREACHABLE;
    }
    if (getMemberState() != newMemberState.s) {
        LOGV2_FATAL(
            21841,
            "Expected to enter state {expectedMemberState} but am now in {actualMemberState}",
            "Failed to change member state",
            "expectedMemberState"_attr = newMemberState,
            "actualMemberState"_attr = getMemberState());
        MONGO_UNREACHABLE;
    }
    LOGV2(
        21816, "{newMemberState}", "Changed member state", "newMemberState"_attr = newMemberState);
}

void TopologyCoordinator::setCurrentPrimary_forTest(int primaryIndex,
                                                    const Timestamp& electionTime) {
    if (primaryIndex == _selfIndex) {
        changeMemberState_forTest(MemberState::RS_PRIMARY);
    } else {
        if (_iAmPrimary()) {
            changeMemberState_forTest(MemberState::RS_SECONDARY);
        }
        if (primaryIndex != -1) {
            ReplSetHeartbeatResponse hbResponse;
            hbResponse.setState(MemberState::RS_PRIMARY);
            hbResponse.setElectionTime(electionTime);
            hbResponse.setAppliedOpTimeAndWallTime(
                {_memberData.at(primaryIndex).getHeartbeatAppliedOpTime(), Date_t() + Seconds(1)});
            hbResponse.setSyncingTo(HostAndPort());
            _memberData.at(primaryIndex)
                .setUpValues(_memberData.at(primaryIndex).getLastHeartbeat(),
                             std::move(hbResponse));
        }
        _currentPrimaryIndex = primaryIndex;
    }
}

const MemberConfig* TopologyCoordinator::getCurrentPrimaryMember() const {
    if (_currentPrimaryIndex == -1)
        return nullptr;

    return &(_rsConfig.getMemberAt(_currentPrimaryIndex));
}

void TopologyCoordinator::populateAllMembersConfigVersionAndTerm_forTest() {
    for (auto i = 0; i < _rsConfig.getNumMembers(); i++) {
        auto memberConfig = _rsConfig.getMemberAt(i);
        if (i < static_cast<int>(_memberData.size())) {
            MemberData& member = _memberData.at(i);
            member.setConfigVersion(_rsConfig.getConfigVersion());
            member.setConfigTerm(_rsConfig.getConfigTerm());
            member.setMemberId(memberConfig.getId());
        }
    }
}

TopologyCoordinator::RecentSyncSourceChanges*
TopologyCoordinator::getRecentSyncSourceChanges_forTest() {
    return &_recentSyncSourceChanges;
}

std::string TopologyCoordinator::_getReplSetStatusString() {
    // Construct a ReplSetStatusArgs using default parameters. Missing parameters will not be
    // included in the status string.
    ReplSetStatusArgs rsStatusArgs{
        Date_t::now(), 0U, OpTime(), BSONObj(), BSONObj(), BSONObj(), boost::none};
    BSONObjBuilder builder;
    Status result(ErrorCodes::InternalError, "didn't set status in prepareStatusResponse");
    prepareStatusResponse(rsStatusArgs, &builder, &result);
    if (!result.isOK()) {
        return str::stream() << "Could not get replSetGetStatus output: " << result.toString();
    }
    return str::stream() << "Current replSetGetStatus output: " << builder.done().toString();
}

void TopologyCoordinator::prepareStatusResponse(const ReplSetStatusArgs& rsStatusArgs,
                                                BSONObjBuilder* response,
                                                Status* result) {
    // output for each member
    std::vector<BSONObj> membersOut;
    const MemberState myState = getMemberState();
    const Date_t now = rsStatusArgs.now;
    const OpTime lastOpApplied = getMyLastAppliedOpTime();
    const Date_t lastOpAppliedWall = getMyLastAppliedOpTimeAndWallTime().wallTime;
    const OpTime lastOpDurable = getMyLastDurableOpTime();
    const Date_t lastOpDurableWall = getMyLastDurableOpTimeAndWallTime().wallTime;
    const BSONObj& initialSyncStatus = rsStatusArgs.initialSyncStatus;
    const BSONObj& electionCandidateMetrics = rsStatusArgs.electionCandidateMetrics;
    const BSONObj& electionParticipantMetrics = rsStatusArgs.electionParticipantMetrics;
    const boost::optional<Timestamp>& lastStableRecoveryTimestamp =
        rsStatusArgs.lastStableRecoveryTimestamp;

    if (_selfIndex == -1) {
        // We're REMOVED or have an invalid config
        response->append("state", static_cast<int>(myState.s));
        response->append("stateStr", myState.toString());
        response->append("uptime", static_cast<int>(rsStatusArgs.selfUptime));

        appendOpTime(response, "optime", lastOpApplied);

        response->appendDate("optimeDate",
                             Date_t::fromDurationSinceEpoch(Seconds(lastOpApplied.getSecs())));
        if (_maintenanceModeCalls) {
            response->append("maintenanceMode", _maintenanceModeCalls);
        }
        response->append("lastHeartbeatMessage", "");
        response->append("syncSourceHost", "");
        response->append("syncSourceId", -1);

        response->append("infoMessage", _getHbmsg(now));
        *result = Status(ErrorCodes::InvalidReplicaSetConfig,
                         "Our replica set config is invalid or we are not a member of it");
        return;
    }

    for (std::vector<MemberData>::const_iterator it = _memberData.begin(); it != _memberData.end();
         ++it) {
        const int itIndex = indexOfIterator(_memberData, it);
        if (itIndex == _selfIndex) {
            // add self
            BSONObjBuilder bb;
            bb.append("_id", _selfConfig().getId().getData());
            bb.append("name", _selfConfig().getHostAndPort().toString());
            bb.append("health", 1.0);
            bb.append("state", static_cast<int>(myState.s));
            bb.append("stateStr", myState.toString());
            bb.append("uptime", static_cast<int>(rsStatusArgs.selfUptime));
            if (!_selfConfig().isArbiter()) {
                appendOpTime(&bb, "optime", lastOpApplied);
                bb.appendDate("optimeDate",
                              Date_t::fromDurationSinceEpoch(Seconds(lastOpApplied.getSecs())));
                bb.appendDate("lastAppliedWallTime", it->getLastAppliedWallTime());
                bb.appendDate("lastDurableWallTime", it->getLastDurableWallTime());
            }

            if (!_syncSource.empty() && !_iAmPrimary()) {
                bb.append("syncSourceHost", _syncSource.toString());
                const MemberConfig* member = _rsConfig.findMemberByHostAndPort(_syncSource);
                bb.append("syncSourceId", member ? member->getId().getData() : -1);
            } else {
                bb.append("syncSourceHost", "");
                bb.append("syncSourceId", -1);
            }

            if (_maintenanceModeCalls) {
                bb.append("maintenanceMode", _maintenanceModeCalls);
            }

            bb.append("infoMessage", _getHbmsg(now));

            if (myState.primary()) {
                bb.append("electionTime", _electionTime);
                bb.appendDate("electionDate",
                              Date_t::fromDurationSinceEpoch(Seconds(_electionTime.getSecs())));
            }
            bb.appendNumber("configVersion", static_cast<long long>(_rsConfig.getConfigVersion()));
            bb.appendNumber("configTerm", static_cast<long long>(_rsConfig.getConfigTerm()));
            bb.append("self", true);
            bb.append("lastHeartbeatMessage", "");
            membersOut.push_back(bb.obj());
        } else {
            // add non-self member
            const MemberConfig& itConfig = _rsConfig.getMemberAt(itIndex);
            BSONObjBuilder bb;
            bb.append("_id", itConfig.getId().getData());
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

            const int uptime = static_cast<int>((
                it->getUpSince() != Date_t() ? durationCount<Seconds>(now - it->getUpSince()) : 0));
            bb.append("uptime", uptime);
            if (!itConfig.isArbiter()) {
                appendOpTime(&bb, "optime", it->getHeartbeatAppliedOpTime());
                appendOpTime(&bb, "optimeDurable", it->getHeartbeatDurableOpTime());

                bb.appendDate("optimeDate",
                              Date_t::fromDurationSinceEpoch(
                                  Seconds(it->getHeartbeatAppliedOpTime().getSecs())));
                bb.appendDate("optimeDurableDate",
                              Date_t::fromDurationSinceEpoch(
                                  Seconds(it->getHeartbeatDurableOpTime().getSecs())));

                bb.appendDate("lastAppliedWallTime", it->getLastAppliedWallTime());
                bb.appendDate("lastDurableWallTime", it->getLastDurableWallTime());
            }
            bb.appendDate("lastHeartbeat", it->getLastHeartbeat());
            bb.appendDate("lastHeartbeatRecv", it->getLastHeartbeatRecv());
            Milliseconds ping = _getPing(itConfig.getHostAndPort());
            bb.append("pingMs", durationCount<Milliseconds>(ping));
            bb.append("lastHeartbeatMessage", it->getLastHeartbeatMsg());
            if (it->hasAuthIssue()) {
                bb.append("authenticated", false);
            }
            const HostAndPort& syncSource = it->getSyncSource();
            if (!syncSource.empty() && !state.primary()) {
                bb.append("syncSourceHost", syncSource.toString());
                const MemberConfig* member = _rsConfig.findMemberByHostAndPort(syncSource);
                bb.append("syncSourceId", member ? member->getId().getData() : -1);
            } else {
                bb.append("syncSourceHost", "");
                bb.append("syncSourceId", -1);
            }

            bb.append("infoMessage", "");

            if (state == MemberState::RS_PRIMARY) {
                bb.append("electionTime", it->getElectionTime());
                bb.appendDate(
                    "electionDate",
                    Date_t::fromDurationSinceEpoch(Seconds(it->getElectionTime().getSecs())));
            }
            bb.appendNumber("configVersion", it->getConfigVersion());
            bb.appendNumber("configTerm", it->getConfigTerm());
            membersOut.push_back(bb.obj());
        }
    }

    // sort members bson
    sort(membersOut.begin(), membersOut.end(), SimpleBSONObjComparator::kInstance.makeLessThan());

    response->append("set", _rsConfig.isInitialized() ? _rsConfig.getReplSetName() : "");
    response->append("date", now);
    response->append("myState", myState.s);
    response->append("term", _term);

    if (rsStatusArgs.tooStale) {
        response->append("tooStale", true);
    }

    // Add sync source info
    if (!_syncSource.empty() && !myState.primary() && !myState.removed()) {
        response->append("syncSourceHost", _syncSource.toString());
        const MemberConfig* member = _rsConfig.findMemberByHostAndPort(_syncSource);
        response->append("syncSourceId", member ? member->getId().getData() : -1);
    } else {
        response->append("syncSourceHost", "");
        response->append("syncSourceId", -1);
    }

    if (_rsConfig.getConfigServer()) {
        response->append("configsvr", true);
    }

    response->append("heartbeatIntervalMillis",
                     durationCount<Milliseconds>(_rsConfig.getHeartbeatInterval()));

    response->append("majorityVoteCount", _rsConfig.getMajorityVoteCount());
    response->append("writeMajorityCount", _rsConfig.getWriteMajority());
    response->append("votingMembersCount", _rsConfig.getTotalVotingMembers());
    response->append("writableVotingMembersCount", _rsConfig.getWritableVotingMembersCount());

    // New optimes, to hold them all.
    BSONObjBuilder optimes;
    _lastCommittedOpTimeAndWallTime.opTime.append(&optimes, "lastCommittedOpTime");

    optimes.appendDate("lastCommittedWallTime", _lastCommittedOpTimeAndWallTime.wallTime);

    if (!rsStatusArgs.readConcernMajorityOpTime.isNull()) {
        rsStatusArgs.readConcernMajorityOpTime.append(&optimes, "readConcernMajorityOpTime");
    }

    appendOpTime(&optimes, "appliedOpTime", lastOpApplied);
    appendOpTime(&optimes, "durableOpTime", lastOpDurable);

    optimes.appendDate("lastAppliedWallTime", lastOpAppliedWall);
    optimes.appendDate("lastDurableWallTime", lastOpDurableWall);

    response->append("optimes", optimes.obj());
    if (lastStableRecoveryTimestamp) {
        // Only include this field if the storage engine supports RTT.
        response->append("lastStableRecoveryTimestamp", *lastStableRecoveryTimestamp);
    }

    if (!initialSyncStatus.isEmpty()) {
        response->append("initialSyncStatus", initialSyncStatus);
    }

    if (!electionCandidateMetrics.isEmpty()) {
        response->append("electionCandidateMetrics", electionCandidateMetrics);
    }

    if (!electionParticipantMetrics.isEmpty()) {
        response->append("electionParticipantMetrics", electionParticipantMetrics);
    }

    response->append("members", membersOut);
    *result = Status::OK();
}

StatusWith<BSONObj> TopologyCoordinator::prepareReplSetUpdatePositionCommand(
    OpTime currentCommittedSnapshotOpTime) const {
    BSONObjBuilder cmdBuilder;
    invariant(_rsConfig.isInitialized());
    // Do not send updates if we have been removed from the config.
    if (_selfIndex == -1) {
        return Status(ErrorCodes::NodeNotFound,
                      "This node is not in the current replset configuration.");
    }
    cmdBuilder.append(UpdatePositionArgs::kCommandFieldName, 1);
    // Create an array containing objects each live member connected to us and for ourself.
    BSONArrayBuilder arrayBuilder(cmdBuilder.subarrayStart("optimes"));
    for (const auto& memberData : _memberData) {
        if (memberData.getLastAppliedOpTime().isNull()) {
            // Don't include info on members we haven't heard from yet.
            continue;
        }
        // Don't include members we think are down.
        if (!memberData.isSelf() && memberData.lastUpdateStale()) {
            continue;
        }

        BSONObjBuilder entry(arrayBuilder.subobjStart());
        memberData.getLastDurableOpTime().append(&entry,
                                                 UpdatePositionArgs::kDurableOpTimeFieldName);
        entry.appendDate(UpdatePositionArgs::kDurableWallTimeFieldName,
                         memberData.getLastDurableWallTime());
        memberData.getLastAppliedOpTime().append(&entry,
                                                 UpdatePositionArgs::kAppliedOpTimeFieldName);
        entry.appendDate(UpdatePositionArgs::kAppliedWallTimeFieldName,
                         memberData.getLastAppliedWallTime());
        entry.append(UpdatePositionArgs::kMemberIdFieldName, memberData.getMemberId().getData());
        entry.append(UpdatePositionArgs::kConfigVersionFieldName, _rsConfig.getConfigVersion());
    }
    arrayBuilder.done();

    // Add metadata to command
    prepareReplSetMetadata(currentCommittedSnapshotOpTime)
        .writeToMetadata(&cmdBuilder)
        .transitional_ignore();
    return cmdBuilder.obj();
}

void TopologyCoordinator::fillMemberData(BSONObjBuilder* result) {
    BSONArrayBuilder replicationProgress(result->subarrayStart("replicationProgress"));
    {
        for (const auto& memberData : _memberData) {
            BSONObjBuilder entry(replicationProgress.subobjStart());
            entry.append("host", memberData.getHostAndPort().toString());

            const auto lastDurableOpTime = memberData.getLastDurableOpTime();
            entry.append("optime", lastDurableOpTime.toBSON());

            const auto lastAppliedOpTime = memberData.getLastAppliedOpTime();
            entry.append("lastAppliedOpTime", lastAppliedOpTime.toBSON());

            const auto heartbeatAppliedOpTime = memberData.getHeartbeatAppliedOpTime();
            entry.append("heartbeatAppliedOpTime", heartbeatAppliedOpTime.toBSON());

            const auto heartbeatDurableOpTime = memberData.getHeartbeatDurableOpTime();
            entry.append("heartbeatDurableOpTime", heartbeatDurableOpTime.toBSON());

            if (_selfIndex >= 0) {
                entry.append("memberId", memberData.getMemberId().getData());
            }
        }
    }
}

void TopologyCoordinator::fillHelloForReplSet(std::shared_ptr<HelloResponse> response,
                                              const StringData& horizonString) const {
    invariant(_rsConfig.isInitialized());
    response->setTopologyVersion(getTopologyVersion());
    const MemberState myState = getMemberState();

    response->setReplSetName(_rsConfig.getReplSetName());
    if (myState.removed()) {
        response->markAsNoConfig();
        return;
    }

    invariant(!_rsConfig.members().empty());

    for (const auto& member : _rsConfig.members()) {
        if (member.isHidden() || member.getSecondaryDelay() > Seconds{0}) {
            continue;
        }
        auto hostView = member.getHostAndPort(horizonString);

        if (member.isElectable()) {
            response->addHost(std::move(hostView));
        } else if (member.isArbiter()) {
            response->addArbiter(std::move(hostView));
        } else {
            response->addPassive(std::move(hostView));
        }
    }

    response->setReplSetVersion(_rsConfig.getConfigVersion());
    // Depending on whether or not the client sent a hello/isMaster request, we set the
    // "isWritablePrimary"/"ismaster" field to false if we are not primary. If we're stepping down,
    // we're waiting for the Replication State Transition Lock before we can change to secondary,
    // but we should report "isWritablePrimary"/"ismaster" false to indicate that we can't accept
    // new writes.
    response->setIsWritablePrimary(myState.primary() && !isSteppingDown());
    response->setIsSecondary(myState.secondary());

    const MemberConfig* curPrimary = getCurrentPrimaryMember();
    if (curPrimary) {
        response->setPrimary(curPrimary->getHostAndPort(horizonString));
    }

    const MemberConfig& selfConfig = _rsConfig.getMemberAt(_selfIndex);
    if (selfConfig.isArbiter()) {
        response->setIsArbiterOnly(true);
    } else if (selfConfig.getPriority() == 0) {
        response->setIsPassive(true);
    }
    if (selfConfig.getSecondaryDelay() > Seconds(0)) {
        response->setSecondaryDelaySecs(selfConfig.getSecondaryDelay());
    }
    if (selfConfig.isHidden()) {
        response->setIsHidden(true);
    }
    if (!selfConfig.shouldBuildIndexes()) {
        response->setShouldBuildIndexes(false);
    }
    const ReplSetTagConfig tagConfig = _rsConfig.getTagConfig();
    if (selfConfig.hasTags()) {
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
    response->setMe(selfConfig.getHostAndPort(horizonString));
    if (_iAmPrimary()) {
        response->setElectionId(_electionId);
    }
}

StatusWith<TopologyCoordinator::PrepareFreezeResponseResult>
TopologyCoordinator::prepareFreezeResponse(Date_t now, int secs, BSONObjBuilder* response) {
    if (_role != TopologyCoordinator::Role::kFollower) {
        static constexpr char msg[] = "Cannot freeze node when primary or running for election";
        const auto state =
            (_role == TopologyCoordinator::Role::kLeader ? "Primary" : "Running-Election");
        LOGV2(21817, msg, "state"_attr = state);
        return Status(ErrorCodes::NotSecondary, str::stream() << msg << ", state: " << state);
    }

    if (secs == 0) {
        _stepDownUntil = now;
        LOGV2(21818, "Unfreezing");
        response->append("info", "unfreezing");
        return PrepareFreezeResponseResult::kSingleNodeSelfElect;
    } else {
        if (secs == 1)
            response->append("warning", "you really want to freeze for only 1 second?");

        _stepDownUntil = std::max(_stepDownUntil, now + Seconds(secs));
        LOGV2(21819, "'freezing' for {freezeSecs} seconds", "Freezing", "freezeSecs"_attr = secs);
    }

    return PrepareFreezeResponseResult::kNoAction;
}

Timestamp TopologyCoordinator::getElectionTime() const {
    return _electionTime;
}

OID TopologyCoordinator::getElectionId() const {
    return _electionId;
}

int TopologyCoordinator::getCurrentPrimaryIndex() const {
    return _currentPrimaryIndex;
}

Date_t TopologyCoordinator::getStepDownTime() const {
    return _stepDownUntil;
}

void TopologyCoordinator::_updateHeartbeatDataForReconfig(const ReplSetConfig& newConfig,
                                                          int selfIndex,
                                                          Date_t now) {
    std::vector<MemberData> oldHeartbeats;
    _memberData.swap(oldHeartbeats);

    int index = 0;
    for (ReplSetConfig::MemberIterator it = newConfig.membersBegin(); it != newConfig.membersEnd();
         ++it, ++index) {
        const MemberConfig& newMemberConfig = *it;
        MemberData newHeartbeatData;
        for (auto&& oldMemberData : oldHeartbeats) {
            if ((oldMemberData.getMemberId() == newMemberConfig.getId() &&
                 oldMemberData.getHostAndPort() == newMemberConfig.getHostAndPort()) ||
                (index == selfIndex && oldMemberData.isSelf())) {
                // This member existed in the old config with the same member ID and
                // HostAndPort, so copy its heartbeat data over.
                newHeartbeatData = oldMemberData;
                break;
            }
        }
        newHeartbeatData.setConfigIndex(index);
        newHeartbeatData.setIsSelf(index == selfIndex);
        newHeartbeatData.setHostAndPort(newMemberConfig.getHostAndPort());
        newHeartbeatData.setMemberId(newMemberConfig.getId());
        _memberData.push_back(newHeartbeatData);
    }
    if (selfIndex < 0) {
        // It's necessary to have self member data even if self isn't in the configuration.
        // We don't need data for the other nodes (which no longer know about us, or soon won't)
        _memberData.clear();
        // We're not in the config, we can't sync any more.
        _clearSyncSource();
        // We shouldn't get a sync source until we've received pings for our new config.
        pingsInConfig = 0;
        MemberData newHeartbeatData;
        for (auto&& oldMemberData : oldHeartbeats) {
            if (oldMemberData.isSelf()) {
                newHeartbeatData = oldMemberData;
                break;
            }
        }
        newHeartbeatData.setConfigIndex(-1);
        newHeartbeatData.setIsSelf(true);
        _memberData.push_back(newHeartbeatData);
    }
}

// This function installs a new config object and recreates MemberData objects
// that reflect the new config.
void TopologyCoordinator::updateConfig(const ReplSetConfig& newConfig, int selfIndex, Date_t now) {
    invariant(_role != Role::kCandidate);
    invariant(selfIndex < newConfig.getNumMembers());

    // Reset term on startup.
    if (!_rsConfig.isInitialized()) {
        _term = OpTime::kInitialTerm;
        LOGV2_DEBUG(21820,
                    1,
                    "Updated term in topology coordinator to {term} due to new config",
                    "Updated term in topology coordinator due to new config",
                    "term"_attr = _term);
    }

    _updateHeartbeatDataForReconfig(newConfig, selfIndex, now);
    _rsConfig = newConfig;
    _selfIndex = selfIndex;
    if (_selfIndex >= 0) {
        // If selfIndex is -1, we are removed from the current config and clear our _memberData.
        // Do not repopulate it.
        _memberData.at(_selfIndex).setConfigVersion(_rsConfig.getConfigVersion());
        _memberData.at(_selfIndex).setConfigTerm(_rsConfig.getConfigTerm());
    }
    _forceSyncSourceIndex = -1;

    if (_role == Role::kLeader) {
        if (_selfIndex == -1) {
            LOGV2(21821, "Could not remain primary because no longer a member of the replica set");
        } else if (!_selfConfig().isElectable()) {
            LOGV2(21822, "Could not remain primary because no longer electable");
        } else {
            // Don't stepdown if you don't have to.
            _currentPrimaryIndex = _selfIndex;
            return;
        }
        _role = Role::kFollower;
        _setLeaderMode(LeaderMode::kNotLeader);
    }

    // By this point we know we are in Role::kFollower
    _currentPrimaryIndex = -1;  // force secondaries to re-detect who the primary is
}
std::string TopologyCoordinator::_getHbmsg(Date_t now) const {
    // ignore messages over 2 minutes old
    if ((now - _hbmsgTime) > Seconds{120}) {
        return "";
    }
    return _hbmsg;
}

void TopologyCoordinator::setMyHeartbeatMessage(const Date_t now, const std::string& message) {
    _hbmsgTime = now;
    _hbmsg = message;
}

const MemberConfig& TopologyCoordinator::_selfConfig() const {
    return _rsConfig.getMemberAt(_selfIndex);
}

const MemberData& TopologyCoordinator::_selfMemberData() const {
    return _memberData[_selfMemberDataIndex()];
}

MemberData& TopologyCoordinator::_selfMemberData() {
    return _memberData[_selfMemberDataIndex()];
}

int TopologyCoordinator::_selfMemberDataIndex() const {
    invariant(!_memberData.empty());
    if (_selfIndex >= 0)
        return _selfIndex;
    // If there is no config or we're not in the config, the first-and-only entry should be for
    // self.
    return 0;
}

TopologyCoordinator::UnelectableReasonMask TopologyCoordinator::_getUnelectableReason(
    int index) const {
    invariant(index != _selfIndex);
    const MemberConfig& memberConfig = _rsConfig.getMemberAt(index);
    const MemberData& hbData = _memberData.at(index);
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
    if (hbData.up() && hbData.isUnelectable()) {
        result |= StepDownPeriodActive;
    }
    invariant(result || memberConfig.isElectable());
    return result;
}

TopologyCoordinator::UnelectableReasonMask TopologyCoordinator::_getMyUnelectableReason(
    const Date_t now, StartElectionReasonEnum reason) const {
    UnelectableReasonMask result = None;
    const OpTime lastApplied = getMyLastAppliedOpTime();
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

    if (reason == StartElectionReasonEnum::kPriorityTakeover &&
        !_amIFreshEnoughForPriorityTakeover()) {
        result |= NotCloseEnoughToLatestForPriorityTakeover;
    }

    if (reason == StartElectionReasonEnum::kCatchupTakeover &&
        !_amIFreshEnoughForCatchupTakeover()) {
        result |= NotFreshEnoughForCatchupTakeover;
    }
    return result;
}

std::string TopologyCoordinator::_getUnelectableReasonString(const UnelectableReasonMask ur) const {
    invariant(ur);
    str::stream ss;
    bool hasWrittenToStream = false;
    if (ur & NoData) {
        ss << "node has no applied oplog entries";
        hasWrittenToStream = true;
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
    if (ur & NotCloseEnoughToLatestForPriorityTakeover) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "member is not caught up enough to the most up-to-date member to call for priority "
              "takeover - must be within "
           << gPriorityTakeoverFreshnessWindowSeconds << " seconds";
    }
    if (ur & NotFreshEnoughForCatchupTakeover) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "member is either not the most up-to-date member or not ahead of the primary, and "
              "therefore cannot call for catchup takeover";
    }
    if (ur & NotInitialized) {
        if (hasWrittenToStream) {
            ss << "; ";
        }
        hasWrittenToStream = true;
        ss << "node is not a member of a valid replica set configuration";
    }
    if (!hasWrittenToStream) {
        LOGV2_FATAL(26011,
                    "Invalid UnelectableReasonMask value 0x{value}",
                    "Invalid UnelectableReasonMask value",
                    "value"_attr = unsignedHex(ur));
    }
    ss << " (mask 0x" << unsignedHex(ur) << ")";
    return ss;
}

Milliseconds TopologyCoordinator::_getPing(const HostAndPort& host) {
    return _pings[host].getMillis();
}

void TopologyCoordinator::setPing_forTest(const HostAndPort& host, const Milliseconds ping) {
    PingStats& pingStats = _pings[host];
    pingStats.set_forTest(ping);
}

void TopologyCoordinator::_setElectionTime(const Timestamp& newElectionTime) {
    _electionTime = newElectionTime;
}

bool TopologyCoordinator::isSteppingDownUnconditionally() const {
    return _leaderMode == LeaderMode::kSteppingDown;
}

bool TopologyCoordinator::isSteppingDown() const {
    return isSteppingDownUnconditionally() || _leaderMode == LeaderMode::kAttemptingStepDown;
}

void TopologyCoordinator::_setLeaderMode(TopologyCoordinator::LeaderMode newMode) {
    // Invariants for valid state transitions.
    switch (_leaderMode) {
        case LeaderMode::kNotLeader:
            invariant(newMode == LeaderMode::kLeaderElect);
            break;
        case LeaderMode::kLeaderElect:
            invariant(newMode == LeaderMode::kNotLeader ||  // TODO(SERVER-30852): remove this case
                      newMode == LeaderMode::kWritablePrimary ||
                      newMode == LeaderMode::kAttemptingStepDown ||
                      newMode == LeaderMode::kSteppingDown);
            break;
        case LeaderMode::kWritablePrimary:
            invariant(newMode == LeaderMode::kNotLeader ||  // TODO(SERVER-30852): remove this case
                      newMode == LeaderMode::kAttemptingStepDown ||
                      newMode == LeaderMode::kSteppingDown);
            break;
        case LeaderMode::kAttemptingStepDown:
            invariant(newMode == LeaderMode::kNotLeader ||
                      newMode == LeaderMode::kWritablePrimary ||
                      newMode == LeaderMode::kSteppingDown || newMode == LeaderMode::kLeaderElect);
            break;
        case LeaderMode::kSteppingDown:
            invariant(newMode == LeaderMode::kNotLeader);
            break;
    }
    _leaderMode = std::move(newMode);
}

MemberState TopologyCoordinator::getMemberState() const {
    if (_selfIndex == -1) {
        if (_rsConfig.isInitialized()) {
            return MemberState::RS_REMOVED;
        }
        return MemberState::RS_STARTUP;
    }

    if (_rsConfig.getConfigServer()) {
        if (_options.clusterRole != ClusterRole::ConfigServer && !skipShardingConfigurationChecks) {
            return MemberState::RS_REMOVED;
        } else {
            invariant(_storageEngineSupportsReadCommitted != ReadCommittedSupport::kUnknown);
            if (_storageEngineSupportsReadCommitted == ReadCommittedSupport::kNo) {
                return MemberState::RS_REMOVED;
            }
        }
    } else {
        if (_options.clusterRole == ClusterRole::ConfigServer && !skipShardingConfigurationChecks) {
            return MemberState::RS_REMOVED;
        }
    }

    if (_role == Role::kLeader) {
        invariant(_currentPrimaryIndex == _selfIndex);
        invariant(_leaderMode != LeaderMode::kNotLeader);
        return MemberState::RS_PRIMARY;
    }
    const MemberConfig& myConfig = _selfConfig();
    if (myConfig.isArbiter()) {
        return MemberState::RS_ARBITER;
    }
    if (((_maintenanceModeCalls > 0) || (_hasOnlyAuthErrorUpHeartbeats(_memberData, _selfIndex))) &&
        (_followerMode == MemberState::RS_SECONDARY)) {
        return MemberState::RS_RECOVERING;
    }
    return _followerMode;
}

std::vector<MemberData> TopologyCoordinator::getMemberData() const {
    return _memberData;
}

bool TopologyCoordinator::canAcceptWrites() const {
    return _leaderMode == LeaderMode::kWritablePrimary;
}

void TopologyCoordinator::setElectionInfo(OID electionId, Timestamp electionOpTime) {
    invariant(_role == Role::kLeader);
    _electionTime = electionOpTime;
    _electionId = electionId;
}

void TopologyCoordinator::processWinElection(OID electionId, Timestamp electionOpTime) {
    invariant(_role == Role::kCandidate);
    invariant(_leaderMode == LeaderMode::kNotLeader);
    _role = Role::kLeader;
    _setLeaderMode(LeaderMode::kLeaderElect);
    setElectionInfo(electionId, electionOpTime);
    _currentPrimaryIndex = _selfIndex;
    _clearSyncSource();
    _forceSyncSourceIndex = -1;
    // Prevent last committed optime from updating until we finish draining.
    _firstOpTimeOfMyTerm =
        OpTime(Timestamp(std::numeric_limits<int>::max(), 0), std::numeric_limits<int>::max());
}

void TopologyCoordinator::processLoseElection() {
    invariant(_role == Role::kCandidate);
    invariant(_leaderMode == LeaderMode::kNotLeader);
    const HostAndPort syncSourceAddress = getSyncSourceAddress();
    _electionTime = Timestamp(0, 0);
    _electionId = OID();
    _role = Role::kFollower;
}

bool TopologyCoordinator::tryToStartStepDown(
    long long termAtStart, Date_t now, Date_t waitUntil, Date_t stepDownUntil, bool force) {

    if (_role != Role::kLeader || _leaderMode == LeaderMode::kSteppingDown ||
        _term != termAtStart) {
        uasserted(ErrorCodes::PrimarySteppedDown,
                  "While waiting for secondaries to catch up before stepping down, "
                  "this node decided to step down for other reasons");
    }
    invariant(_leaderMode == LeaderMode::kAttemptingStepDown);

    if (now >= stepDownUntil) {
        uasserted(ErrorCodes::ExceededTimeLimit,
                  "By the time we were ready to step down, we were already past the "
                  "time we were supposed to step down until");
    }

    if (!_canCompleteStepDownAttempt(now, waitUntil, force)) {
        // Stepdown attempt failed.

        // Check waitUntil after at least one stepdown attempt, so that stepdown could succeed even
        // if secondaryCatchUpPeriodSecs == 0.
        if (now >= waitUntil) {
            uasserted(ErrorCodes::ExceededTimeLimit,
                      str::stream() << "No electable secondaries caught up as of "
                                    << dateToISOStringLocal(now)
                                    << ". Please use the replSetStepDown command with the argument "
                                    << "{force: true} to force node to step down.");
        }

        // Stepdown attempt failed, but in a way that can be retried
        return false;
    }

    // Stepdown attempt success!
    _stepDownUntil = stepDownUntil;
    prepareForUnconditionalStepDown();
    return true;
}

bool TopologyCoordinator::_canCompleteStepDownAttempt(Date_t now, Date_t waitUntil, bool force) {
    const bool forceNow = force && (now >= waitUntil);
    if (forceNow) {
        return true;
    }

    return isSafeToStepDown();
}

bool TopologyCoordinator::_isCaughtUpAndElectable(int memberIndex, OpTime lastApplied) {
    if (_getUnelectableReason(memberIndex)) {
        return false;
    }

    return (_memberData.at(memberIndex).getLastAppliedOpTime() >= lastApplied);
}

bool TopologyCoordinator::isSafeToStepDown() {
    if (!_rsConfig.isInitialized() || _selfIndex < 0) {
        return false;
    }

    OpTime lastApplied = getMyLastAppliedOpTime();

    // No need to wait for secondaries to catch up if this node has not yet written in the current
    // term.
    if (lastApplied.getTerm() != _term) {
        return true;
    }

    auto tagStatus = _rsConfig.findCustomWriteMode(ReplSetConfig::kMajorityWriteConcernModeName);
    invariant(tagStatus.isOK());

    // Check if a majority of nodes have reached the last applied optime.
    if (!haveTaggedNodesReachedOpTime(lastApplied, tagStatus.getValue(), false)) {
        return false;
    }

    // Now check that we also have at least one caught up node that is electable.
    for (int memberIndex = 0; memberIndex < _rsConfig.getNumMembers(); memberIndex++) {
        // ignore your self
        if (memberIndex == _selfIndex) {
            continue;
        }

        if (_isCaughtUpAndElectable(memberIndex, lastApplied)) {
            return true;
        }
    }

    return false;
}

int TopologyCoordinator::chooseElectionHandoffCandidate() {

    OpTime lastApplied = getMyLastAppliedOpTime();

    int bestCandidateIndex = -1;
    int highestPriority = -1;

    for (int memberIndex = 0; memberIndex < _rsConfig.getNumMembers(); memberIndex++) {

        // Skip your own member index.
        if (memberIndex == _selfIndex) {
            continue;
        }

        // Skip this node if it is not eligible to become primary. This includes nodes with
        // priority 0.
        if (!_isCaughtUpAndElectable(memberIndex, lastApplied)) {
            continue;
        }

        // Only update best if priority is strictly greater. This guarantees that
        // we will pick the member with the lowest index in case of a tie. Note that
        // member priority is always a non-negative number.
        auto memberPriority = _rsConfig.getMemberAt(memberIndex).getPriority();
        if (memberPriority > highestPriority) {
            bestCandidateIndex = memberIndex;
            highestPriority = memberPriority;
        }
    }

    // This is the most suitable node.
    return bestCandidateIndex;
}

void TopologyCoordinator::setFollowerMode(MemberState::MS newMode) {
    invariant(_role == Role::kFollower);
    switch (newMode) {
        case MemberState::RS_RECOVERING:
        case MemberState::RS_ROLLBACK:
        case MemberState::RS_SECONDARY:
        case MemberState::RS_STARTUP2:
            _followerMode = newMode;
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

bool TopologyCoordinator::isElectableNodeInSingleNodeReplicaSet() const {
    auto isSingleNode = _rsConfig.getNumMembers() == 1 && _selfIndex == 0;
    // Single node replset must be electable.
    invariant(!isSingleNode || _rsConfig.getMemberAt(_selfIndex).isElectable());
    return (getMemberState() == MemberState::RS_SECONDARY) && isSingleNode;
}

void TopologyCoordinator::finishUnconditionalStepDown() {
    invariant(_leaderMode == LeaderMode::kSteppingDown);

    int remotePrimaryIndex = -1;
    for (std::vector<MemberData>::const_iterator it = _memberData.begin(); it != _memberData.end();
         ++it) {
        const int itIndex = indexOfIterator(_memberData, it);
        if (itIndex == _selfIndex) {
            continue;
        }

        if (it->getState().primary() && it->up()) {
            if (remotePrimaryIndex != -1) {
                // two other nodes think they are primary (asynchronously polled)
                // -- wait for things to settle down.
                remotePrimaryIndex = -1;
                LOGV2_WARNING(21838, "Two remote primaries (transiently)");
                break;
            }
            remotePrimaryIndex = itIndex;
        }
    }
    _stepDownSelfAndReplaceWith(remotePrimaryIndex);
}

void TopologyCoordinator::_stepDownSelfAndReplaceWith(int newPrimary) {
    invariant(_role == Role::kLeader);
    invariant(_selfIndex != -1);
    invariant(_selfIndex != newPrimary);
    invariant(_selfIndex == _currentPrimaryIndex);
    _currentPrimaryIndex = newPrimary;
    _role = Role::kFollower;
    _setLeaderMode(LeaderMode::kNotLeader);
}

bool TopologyCoordinator::updateLastCommittedOpTimeAndWallTime() {
    // If we're not primary or we're stepping down due to learning of a new term then we must not
    // advance the commit point.  If we are stepping down due to a user request, however, then it
    // is safe to advance the commit point, and in fact we must since the stepdown request may be
    // waiting for the commit point to advance enough to be able to safely complete the step down.
    if (!_iAmPrimary() || _leaderMode == LeaderMode::kSteppingDown) {
        return false;
    }

    // Whether we use the applied or durable OpTime for the commit point is decided here.
    const bool useDurableOpTime = _rsConfig.getWriteConcernMajorityShouldJournal();

    std::vector<OpTimeAndWallTime> votingNodesOpTimesAndWallTimes;
    for (const auto& memberData : _memberData) {
        int memberIndex = memberData.getConfigIndex();
        invariant(memberIndex >= 0);
        const auto& memberConfig = _rsConfig.getMemberAt(memberIndex);
        if (memberConfig.isVoter()) {
            const OpTimeAndWallTime durableOpTime = {memberData.getLastDurableOpTime(),
                                                     memberData.getLastDurableWallTime()};
            const OpTimeAndWallTime appliedOpTime = {memberData.getLastAppliedOpTime(),
                                                     memberData.getLastAppliedWallTime()};
            const OpTimeAndWallTime opTime = useDurableOpTime ? durableOpTime : appliedOpTime;
            votingNodesOpTimesAndWallTimes.push_back(opTime);
        }
    }

    invariant(votingNodesOpTimesAndWallTimes.size() > 0);
    if (votingNodesOpTimesAndWallTimes.size() <
        static_cast<unsigned long>(_rsConfig.getWriteMajority())) {
        return false;
    }
    std::sort(votingNodesOpTimesAndWallTimes.begin(), votingNodesOpTimesAndWallTimes.end());

    // need the majority to have this OpTime
    OpTimeAndWallTime committedOpTime =
        votingNodesOpTimesAndWallTimes[votingNodesOpTimesAndWallTimes.size() -
                                       _rsConfig.getWriteMajority()];

    const bool fromSyncSource = false;
    return advanceLastCommittedOpTimeAndWallTime(committedOpTime, fromSyncSource);
}

bool TopologyCoordinator::advanceLastCommittedOpTimeAndWallTime(OpTimeAndWallTime committedOpTime,
                                                                bool fromSyncSource,
                                                                bool forInitiate) {
    if (forInitiate) {
        // Force update in the replSetInitiate case.
        LOGV2_INFO(5872100,
                   "Updating commit point for initiate",
                   "_lastCommittedOpTimeAndWallTime"_attr = committedOpTime);
        _lastCommittedOpTimeAndWallTime = committedOpTime;
        return true;
    }

    if (_selfIndex == -1) {
        // The config hasn't been installed or we are not in the config. This could happen
        // on heartbeats before installing a config.
        return false;
    }

    // This check is performed to ensure primaries do not commit an OpTime from a previous term.
    if (_iAmPrimary() && committedOpTime.opTime < _firstOpTimeOfMyTerm) {
        LOGV2_DEBUG(21823,
                    1,
                    "Ignoring older committed snapshot from before I became primary, optime: "
                    "{committedOpTime}, firstOpTimeOfMyTerm: {firstOpTimeOfMyTerm}",
                    "Ignoring older committed snapshot from before I became primary",
                    "committedOpTime"_attr = committedOpTime.opTime,
                    "firstOpTimeOfMyTerm"_attr = _firstOpTimeOfMyTerm);
        return false;
    }

    // Arbiters don't have data so they always advance their commit point via heartbeats.
    if (!_selfConfig().isArbiter() &&
        getMyLastAppliedOpTime().getTerm() != committedOpTime.opTime.getTerm()) {
        if (fromSyncSource) {
            committedOpTime = std::min(committedOpTime, getMyLastAppliedOpTimeAndWallTime());
        } else {
            LOGV2_DEBUG(21824,
                        1,
                        "Ignoring commit point with different term than my lastApplied, since it "
                        "may "
                        "not be on the same oplog branch as mine. optime: {committedOpTime}, my "
                        "last applied: {myLastAppliedOpTimeAndWallTime}",
                        "Ignoring commit point with different term than my lastApplied, since it "
                        "may not be on the same oplog branch as mine",
                        "committedOpTime"_attr = committedOpTime,
                        "myLastAppliedOpTimeAndWallTime"_attr =
                            getMyLastAppliedOpTimeAndWallTime());
            return false;
        }
    }

    if (committedOpTime.opTime == _lastCommittedOpTimeAndWallTime.opTime) {
        return false;  // Hasn't changed, so ignore it.
    }

    if (committedOpTime.opTime < _lastCommittedOpTimeAndWallTime.opTime) {
        LOGV2_DEBUG(21825,
                    1,
                    "Ignoring older committed snapshot optime: {committedOpTime}, "
                    "currentCommittedOpTime: {currentCommittedOpTime}",
                    "Ignoring older committed snapshot optime",
                    "committedOpTime"_attr = committedOpTime,
                    "currentCommittedOpTime"_attr = _lastCommittedOpTimeAndWallTime);
        return false;
    }

    if (committedOpTime.opTime.getTerm() != _lastCommittedOpTimeAndWallTime.opTime.getTerm()) {
        LOGV2(6795400,
              "Advancing committed opTime to a new term",
              "newCommittedOpTime"_attr = committedOpTime.opTime,
              "newCommittedWallime"_attr = committedOpTime.wallTime,
              "oldTerm"_attr = _lastCommittedOpTimeAndWallTime.opTime.getTerm());
    }

    LOGV2_DEBUG(21826,
                2,
                "Updating _lastCommittedOpTimeAndWallTime to {_lastCommittedOpTimeAndWallTime}",
                "Updating _lastCommittedOpTimeAndWallTime",
                "_lastCommittedOpTimeAndWallTime"_attr = committedOpTime);
    _lastCommittedOpTimeAndWallTime = committedOpTime;
    return true;
}

OpTime TopologyCoordinator::getLastCommittedOpTime() const {
    return _lastCommittedOpTimeAndWallTime.opTime;
}

OpTimeAndWallTime TopologyCoordinator::getLastCommittedOpTimeAndWallTime() const {
    return _lastCommittedOpTimeAndWallTime;
}

bool TopologyCoordinator::canCompleteTransitionToPrimary(long long termWhenDrainCompleted) const {

    if (termWhenDrainCompleted != _term) {
        return false;
    }
    // Allow completing the transition to primary even when in the middle of a stepdown attempt,
    // in case the stepdown attempt fails.
    if (_leaderMode != LeaderMode::kLeaderElect && _leaderMode != LeaderMode::kAttemptingStepDown &&
        _leaderMode != LeaderMode::kSteppingDown) {
        return false;
    }

    return true;
}

void TopologyCoordinator::completeTransitionToPrimary(const OpTime& firstOpTimeOfTerm) {
    invariant(canCompleteTransitionToPrimary(firstOpTimeOfTerm.getTerm()));

    if (_leaderMode == LeaderMode::kLeaderElect) {
        _setLeaderMode(LeaderMode::kWritablePrimary);
    }

    _firstOpTimeOfMyTerm = firstOpTimeOfTerm;
}

void TopologyCoordinator::adjustMaintenanceCountBy(int inc) {
    invariant(_role == Role::kFollower);
    _maintenanceModeCalls += inc;
    invariant(_maintenanceModeCalls >= 0);
}

void TopologyCoordinator::resetMaintenanceCount() {
    invariant(_role == Role::kFollower);
    _maintenanceModeCalls = 0;
}

int TopologyCoordinator::getMaintenanceCount() const {
    return _maintenanceModeCalls;
}

TopologyCoordinator::UpdateTermResult TopologyCoordinator::updateTerm(long long term, Date_t now) {
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
    LOGV2_DEBUG(21827,
                1,
                "Updating term from {oldTerm} to {newTerm}",
                "Updating term",
                "oldTerm"_attr = _term,
                "newTerm"_attr = term);
    _term = term;
    return TopologyCoordinator::UpdateTermResult::kUpdatedTerm;
}


long long TopologyCoordinator::getTerm() const {
    return _term;
}

bool TopologyCoordinator::shouldChangeSyncSource(const HostAndPort& currentSource,
                                                 const rpc::ReplSetMetadata& replMetadata,
                                                 const rpc::OplogQueryMetadata& oqMetadata,
                                                 const OpTime& lastOpTimeFetched,
                                                 Date_t now) const {
    // Methodology:
    // If there exists a viable sync source member other than currentSource, whose oplog has
    // reached an optime greater than _options.maxSyncSourceLagSecs later than currentSource's,
    // return true.
    // If the currentSource has the same replication progress as we do and has no source for further
    // progress, return true.

    auto [initialDecision, currentSourceIndex] =
        _shouldChangeSyncSourceInitialChecks(currentSource);
    if (initialDecision != ChangeSyncSourceDecision::kMaybe) {
        return initialDecision == ChangeSyncSourceDecision::kYes;
    }

    if (!replMetadata.getIsPrimary() &&
        _shouldChangeSyncSourceDueToNewPrimary(currentSource, currentSourceIndex)) {
        return true;
    }

    OpTime currentSourceOpTime =
        std::max(oqMetadata.getLastOpApplied(),
                 _memberData.at(currentSourceIndex).getHeartbeatAppliedOpTime());

    fassert(4612000, !currentSourceOpTime.isNull());

    int syncSourceIndex = oqMetadata.getSyncSourceIndex();
    std::string syncSourceHost = oqMetadata.getSyncSourceHost();

    // Change sync source if they are not ahead of us, and don't have a sync source,
    // unless they are primary.
    if (_shouldChangeSyncSourceDueToSourceNotAhead(currentSource,
                                                   syncSourceIndex,
                                                   replMetadata.getIsPrimary(),
                                                   currentSourceOpTime,
                                                   lastOpTimeFetched))
        return true;

    if (_shouldChangeSyncSourceToBreakCycle(
            currentSource, syncSourceHost, syncSourceIndex, currentSourceOpTime, lastOpTimeFetched))
        return true;

    if (_shouldChangeSyncSourceDueToLag(currentSource, currentSourceOpTime, lastOpTimeFetched, now))
        return true;

    if (_shouldChangeSyncSourceDueToBetterEligibleSource(
            currentSource, currentSourceIndex, lastOpTimeFetched, now))
        return true;

    return false;
}

bool TopologyCoordinator::shouldChangeSyncSourceOnError(const HostAndPort& currentSource,
                                                        const OpTime& lastOpTimeFetched,
                                                        Date_t now) const {
    // We change sync source on error if
    // 1) A forced sync source change has been requested.
    // 2) Chaining is disabled and a new primary has been detected.
    // 3) A more eligible node exists. Note this covers the case where our current sync source is
    //    down.

    auto [initialDecision, currentSourceIndex] =
        _shouldChangeSyncSourceInitialChecks(currentSource);
    if (initialDecision != ChangeSyncSourceDecision::kMaybe) {
        return initialDecision == ChangeSyncSourceDecision::kYes;
    }

    if (_shouldChangeSyncSourceDueToNewPrimary(currentSource, currentSourceIndex)) {
        return true;
    }

    if (_shouldChangeSyncSourceDueToBetterEligibleSource(
            currentSource, currentSourceIndex, lastOpTimeFetched, now))
        return true;

    return false;
}

std::pair<TopologyCoordinator::ChangeSyncSourceDecision, int>
TopologyCoordinator::_shouldChangeSyncSourceInitialChecks(const HostAndPort& currentSource) const {
    if (_selfIndex == -1) {
        LOGV2(21828, "Not choosing new sync source because we are not in the config");
        return {ChangeSyncSourceDecision::kNo, -1};
    }

    // If the user requested a sync source change, return kYes.
    if (_forceSyncSourceIndex != -1) {
        LOGV2(21829,
              "Choosing new sync source because the user has requested to use "
              "{syncSource} as a sync source",
              "Choosing new sync source because the user has requested a sync source",
              "syncSource"_attr = _rsConfig.getMemberAt(_forceSyncSourceIndex).getHostAndPort());
        return {ChangeSyncSourceDecision::kYes, -1};
    }

    // While we can allow data replication across config versions, we still do not allow syncing
    // from a node that is not in our config.
    const int currentSourceIndex = _rsConfig.findMemberIndexByHostAndPort(currentSource);
    if (currentSourceIndex == -1) {
        LOGV2(21831,
              "Choosing new sync source because {currentSyncSource} is not in our config",
              "Choosing new sync source because current sync source is not in our config",
              "currentSyncSource"_attr = currentSource.toString());
        return {ChangeSyncSourceDecision::kYes, -1};
    }

    invariant(currentSourceIndex != _selfIndex);
    return {ChangeSyncSourceDecision::kMaybe, currentSourceIndex};
}

bool TopologyCoordinator::_shouldChangeSyncSourceDueToNewPrimary(const HostAndPort& currentSource,
                                                                 int currentSourceIndex) const {
    // Change sync source if chaining is disabled (without overrides), we are not syncing from the
    // primary, and we know who the new primary is. We do not consider chaining disabled if we are
    // the primary, since we are in catchup mode.
    auto chainingDisabled = !_rsConfig.isChainingAllowed() &&
        !enableOverrideClusterChainingSetting.load() && _currentPrimaryIndex != _selfIndex;
    auto foundNewPrimary = _currentPrimaryIndex != -1 && _currentPrimaryIndex != currentSourceIndex;
    if (chainingDisabled && foundNewPrimary) {
        auto newPrimary = _rsConfig.getMemberAt(_currentPrimaryIndex).getHostAndPort();
        LOGV2(3962100,
              "Choosing new sync source because chaining is disabled and we are aware of a new "
              "primary",
              "syncSource"_attr = currentSource,
              "newPrimary"_attr = newPrimary);
        return true;
    }
    return false;
}

bool TopologyCoordinator::_shouldChangeSyncSourceDueToSourceNotAhead(
    const HostAndPort& currentSource,
    int syncSourceIndex,
    bool syncSourceIsPrimary,
    const OpTime& currentSourceOpTime,
    const OpTime& lastOpTimeFetched) const {
    if (syncSourceIndex == -1 && currentSourceOpTime <= lastOpTimeFetched && !syncSourceIsPrimary) {
        LOGV2(21832,
              "Choosing new sync source. Our current sync source is not primary and does "
              "not have a sync source, so we require that it is ahead of us",
              "syncSource"_attr = currentSource,
              "lastOpTimeFetched"_attr = lastOpTimeFetched,
              "syncSourceLatestOplogOpTime"_attr = currentSourceOpTime,
              "isPrimary"_attr = syncSourceIsPrimary);
        return true;
    }
    return false;
}

bool TopologyCoordinator::_shouldChangeSyncSourceToBreakCycle(
    const HostAndPort& currentSource,
    const std::string& syncSourceHost,
    int syncSourceIndex,
    const OpTime& currentSourceOpTime,
    const OpTime& lastOpTimeFetched) const {
    // Change sync source if our sync source is also syncing from us when we are in primary
    // catchup mode, forming a sync source selection cycle, and the sync source is not ahead
    // of us. This is to prevent a deadlock situation. See SERVER-58988 for details.
    // When checking the sync source, we use syncSourceHost if it is set, otherwise fall back
    // to use syncSourceIndex. The difference is that syncSourceIndex might not point to the
    // node that we think of because it was inferred from the sender node, which could have
    // a different config. This is acceptable since we are just choosing a different sync
    // source if that happens and reconfigs are rare.
    const bool isSyncingFromMe = !syncSourceHost.empty()
        ? syncSourceHost == _selfMemberData().getHostAndPort().toString()
        : syncSourceIndex == _selfIndex;

    if (isSyncingFromMe && _currentPrimaryIndex == _selfIndex &&
        currentSourceOpTime <= lastOpTimeFetched) {
        LOGV2(5898800,
              "Choosing new sync source because we are in primary catchup but our current sync "
              "source is also syncing from us but is not ahead of us",
              "syncSource"_attr = currentSource,
              "lastOpTimeFetched"_attr = lastOpTimeFetched,
              "syncSourceLatestOplogOpTime"_attr = currentSourceOpTime);
        return true;
    }
    return false;
}

bool TopologyCoordinator::_shouldChangeSyncSourceDueToLag(const HostAndPort& currentSource,
                                                          const OpTime& currentSourceOpTime,
                                                          const OpTime& lastOpTimeFetched,
                                                          Date_t now) const {
    if (MONGO_unlikely(disableMaxSyncSourceLagSecs.shouldFail())) {
        LOGV2(
            21833,
            "disableMaxSyncSourceLagSecs fail point enabled - not checking the most recent OpTime "
            "of our current sync source against the OpTimes of the other nodes in this replica set",
            "currentSyncSourceOpTime"_attr = currentSourceOpTime.toString(),
            "syncSource"_attr = currentSource);
    } else {
        unsigned int currentSourceOpTimeSecs = currentSourceOpTime.getSecs();
        unsigned int currentSourceLagThresholdSecs =
            currentSourceOpTimeSecs + durationCount<Seconds>(_options.maxSyncSourceLagSecs);

        for (size_t i = 0; i < _memberData.size(); i++) {
            const auto& member = _memberData[i];
            if (currentSourceLagThresholdSecs < member.getHeartbeatAppliedOpTime().getSecs() &&
                _isEligibleSyncSource(i,
                                      now,
                                      lastOpTimeFetched,
                                      ReadPreference::Nearest,
                                      true /* firstAttempt */,
                                      true /* shouldCheckStaleness */)) {
                invariant(i != (size_t)_selfIndex,
                          str::stream()
                              << "Node " << i << " was eligible as a sync source for itself");
                LOGV2(21834,
                      "Choosing new sync source because the most recent OpTime of our sync source "
                      "is more than maxSyncSourceLagSecs behind another member",
                      "syncSource"_attr = currentSource,
                      "syncSourceOpTime"_attr = currentSourceOpTime.toString(),
                      "maxSyncSourceLagSecs"_attr = _options.maxSyncSourceLagSecs,
                      "otherMember"_attr = member.getHostAndPort().toString(),
                      "otherMemberHearbeatAppliedOpTime"_attr =
                          member.getHeartbeatAppliedOpTime().toString());
                return true;
            }
        }
    }
    return false;
}

bool TopologyCoordinator::_shouldChangeSyncSourceDueToBetterEligibleSource(
    const HostAndPort& currentSource,
    const int currentSourceIndex,
    const OpTime& lastOpTimeFetched,
    Date_t now) const {
    // Change sync source if our current sync source is not a preferred sync source node choice due
    // to non-staleness issues, such as being a non-voter when we are a voter, or being hidden, or
    // any of the other conditions checked in _isEligibleSyncSource with firstAttempt=true, and
    // another eligible node exists which does meet these criteria. Note that while we bypass
    // staleness checks for our current node, we should not do this for a potential new node,
    // because we could end up with a situation where shouldChangeSyncSource returns true, causing
    // the sync source to be cleared, but then being reset to our previous sync source repeatedly
    // because the new source is not actually valid. Note that _isEligibleSyncSource only checks for
    // ReadPreference::Secondary*, so any choice besides those for the read preference is fine.
    if (!_isEligibleSyncSource(currentSourceIndex,
                               now,
                               lastOpTimeFetched,
                               ReadPreference::Nearest,
                               true /* firstAttempt */,
                               false /* shouldCheckStaleness */)) {

        for (size_t i = 0; i < _memberData.size(); i++) {
            if (_isEligibleSyncSource(i,
                                      now,
                                      lastOpTimeFetched,
                                      ReadPreference::Nearest,
                                      true /* firstAttempt */,
                                      true /* shouldCheckStaleness */)) {
                invariant(i != (size_t)_selfIndex,
                          str::stream()
                              << "Node " << i << " was eligible as a sync source for itself");
                LOGV2(5929000,
                      "Choosing new sync source because our current sync source does not satisfy "
                      "our strict criteria for candidates, but there is another member which does "
                      "satisfy these criteria",
                      "currentSyncSource"_attr = currentSource,
                      "eligibleCandidateSyncSource"_attr =
                          _rsConfig.getMemberAt(i).getHostAndPort().toString());
                return true;
            }
        }
    }

    return false;
}

bool TopologyCoordinator::shouldChangeSyncSourceDueToPingTime(const HostAndPort& currentSource,
                                                              const MemberState& memberState,
                                                              const OpTime& previousOpTimeFetched,
                                                              Date_t now,
                                                              const ReadPreference readPreference) {
    // If we find an eligible sync source that is significantly closer than our current sync source,
    // return true.

    // Do not re-evaluate our sync source if it was set via the replSetSyncFrom command or the
    // forceSyncSourceCandidate failpoint.
    auto sfp = forceSyncSourceCandidate.scoped();
    if (_replSetSyncFromSet || MONGO_unlikely(sfp.isActive())) {
        return false;
    }

    // If we are in initial sync, do not re-evaluate our sync source.
    const bool nodeInInitialSync = (memberState.startup() || memberState.startup2());
    if (nodeInInitialSync) {
        return false;
    }

    // If we are configured with secondaryDelaySecs, do not re-evaluate our sync source.
    if (_selfIndex == -1 || _selfConfig().getSecondaryDelay() > Seconds(0)) {
        return false;
    }

    // If we have already changed sync sources more than 'maxNumSyncSourceChangesPerHour' in the
    // past hour, do not re-evaluate our sync source.
    if (_recentSyncSourceChanges.changedTooOftenRecently(now)) {
        return false;
    }

    const bool primaryOnly = (readPreference == ReadPreference::PrimaryOnly);
    const bool primaryPreferredAndAlreadySyncing =
        (readPreference == ReadPreference::PrimaryPreferred &&
         (currentSource == getCurrentPrimaryMember()->getHostAndPort()));

    if (primaryOnly || primaryPreferredAndAlreadySyncing) {
        return false;
    }

    const auto changeSyncSourceThreshold = changeSyncSourceThresholdMillis.load();
    // If the threshold is set to zero, do not consider changing sync sources due to ping time.
    if (changeSyncSourceThreshold == 0LL) {
        return false;
    }

    // If we have not yet received 5N pings (not counting ourselves), do not re-evaluate our sync
    // source.
    int numPingsNeeded = (_memberData.size() - 1) * 5 - pingsInConfig;
    if (numPingsNeeded > 0) {
        return false;
    }

    if (_pings.count(currentSource) == 0) {
        // Ping data for our current sync source could not be found.
        return false;
    }

    const auto syncSourcePingTime =
        durationCount<Milliseconds>(_pings.at(currentSource).getMillis());

    // Use ping times to look for another viable sync source that is significantly closer.
    for (size_t candidateIndex = 0; candidateIndex < _memberData.size(); candidateIndex++) {
        const auto candidateNode = _memberData[candidateIndex].getHostAndPort();
        if (_pings.count(candidateNode) == 0) {
            // Either we are the candidate node or ping data for the candidateNode could not be
            // found. Continue to the next node.
            continue;
        }

        // Only choose a new sync source if ping times indicate that the candidate is significantly
        // closer than our current sync source and it is an eligible sync source.
        const auto candidateSyncSourcePingTime =
            durationCount<Milliseconds>(_pings.at(candidateNode).getMillis());
        if (syncSourcePingTime - candidateSyncSourcePingTime <= changeSyncSourceThreshold) {
            continue;
        }

        if (_isEligibleSyncSource(candidateIndex,
                                  now,
                                  previousOpTimeFetched,
                                  readPreference,
                                  true /* firstAttempt */,
                                  true /* shouldCheckStaleness */)) {
            LOGV2(4744901,
                  "Choosing new sync source because we have found another potential sync "
                  "source that is significantly closer than our current sync source",
                  "syncSourcePingTime"_attr = syncSourcePingTime,
                  "changeSyncSourceThreshold"_attr = changeSyncSourceThreshold,
                  "candidateNode"_attr = candidateNode,
                  "candidatePingTime"_attr = candidateSyncSourcePingTime);
            numSyncSourceChangesDueToSignificantlyCloserNode.increment();
            return true;
        }
    }
    return false;
}

rpc::ReplSetMetadata TopologyCoordinator::prepareReplSetMetadata(
    const OpTime& lastVisibleOpTime) const {
    return rpc::ReplSetMetadata(_term,
                                _lastCommittedOpTimeAndWallTime,
                                lastVisibleOpTime,
                                _rsConfig.getConfigVersion(),
                                _rsConfig.getConfigTerm(),
                                _rsConfig.getReplicaSetId(),
                                _rsConfig.findMemberIndexByHostAndPort(getSyncSourceAddress()),
                                _role == Role::kLeader /* isPrimary */);
}

rpc::OplogQueryMetadata TopologyCoordinator::prepareOplogQueryMetadata(int rbid) const {
    return rpc::OplogQueryMetadata(_lastCommittedOpTimeAndWallTime,
                                   getMyLastAppliedOpTime(),
                                   rbid,
                                   _currentPrimaryIndex,
                                   _rsConfig.findMemberIndexByHostAndPort(getSyncSourceAddress()),
                                   getSyncSourceAddress().toString());
}

void TopologyCoordinator::processReplSetRequestVotes(const ReplSetRequestVotesArgs& args,
                                                     ReplSetRequestVotesResponse* response) {
    response->setTerm(_term);

    if (MONGO_unlikely(voteNoInElection.shouldFail())) {
        LOGV2(21835, "Failpoint voteNoInElection enabled");
        response->setVoteGranted(false);
        response->setReason(
            "forced to vote no during dry run election due to failpoint voteNoInElection set");
        return;
    }

    if (MONGO_unlikely(voteYesInDryRunButNoInRealElection.shouldFail())) {
        LOGV2(21836, "Failpoint voteYesInDryRunButNoInRealElection enabled");
        if (args.isADryRun()) {
            response->setVoteGranted(true);
            response->setReason(
                "forced to vote yes in dry run due to failpoint "
                "voteYesInDryRunButNoInRealElection set");
        } else {
            response->setVoteGranted(false);
            response->setReason(
                "forced to vote no in real election due to failpoint "
                "voteYesInDryRunButNoInRealElection set");
        }
        return;
    }

    if (args.getConfigVersionAndTerm() < _rsConfig.getConfigVersionAndTerm()) {
        response->setVoteGranted(false);
        response->setReason("candidate's config with {} is older than mine with {}"_format(
            args.getConfigVersionAndTerm(), _rsConfig.getConfigVersionAndTerm()));
    } else if (args.getTerm() < _term) {
        response->setVoteGranted(false);
        response->setReason(
            "candidate's term ({}) is lower than mine ({})"_format(args.getTerm(), _term));
    } else if (args.getSetName() != _rsConfig.getReplSetName()) {
        response->setVoteGranted(false);
        response->setReason("candidate's set name ({}) differs from mine ({})"_format(
            args.getSetName(), _rsConfig.getReplSetName()));
    } else if (args.getLastAppliedOpTime() < getMyLastAppliedOpTime()) {
        response->setVoteGranted(false);
        response->setReason(
            "candidate's data is staler than mine. candidate's last applied OpTime: {}, "
            "my last applied OpTime: {}"_format(args.getLastAppliedOpTime().toString(),
                                                getMyLastAppliedOpTime().toString()));
    } else if (!args.isADryRun() && _lastVote.getTerm() == args.getTerm()) {
        response->setVoteGranted(false);
        response->setReason("already voted for another candidate ({}) this term ({})"_format(
            _rsConfig.getMemberAt(_lastVote.getCandidateIndex()).getHostAndPort(),
            _lastVote.getTerm()));
    } else {
        bool isSameConfig = args.getConfigVersionAndTerm() == _rsConfig.getConfigVersionAndTerm();
        int betterPrimary = _findHealthyPrimaryOfEqualOrGreaterPriority(args.getCandidateIndex());
        // Do not grant vote if we are arbiter and can see a healthy primary of greater or equal
        // priority, to prevent primary flapping when there are two nodes that can't talk to each
        // other but we that can talk to both as arbiter. We only do this if the voter's config
        // is same as ours, otherwise the primary information might be stale and we might not be
        // arbiter in the candidate's newer config. We might also hit an invariant described in
        // SERVER-46387 without the check for same config.
        if (isSameConfig && _selfConfig().isArbiter() && betterPrimary >= 0) {
            response->setVoteGranted(false);
            response->setReason(
                "can see a healthy primary ({}) of equal or greater priority"_format(
                    _rsConfig.getMemberAt(betterPrimary).getHostAndPort()));
        } else {
            if (!args.isADryRun()) {
                _lastVote.setTerm(args.getTerm());
                _lastVote.setCandidateIndex(args.getCandidateIndex());
                LOGV2_DEBUG(5972100, 0, "Voting yes in election");
            }
            response->setVoteGranted(true);
        }
    }

    LOGV2_FOR_ELECTION(23980,
                       0,
                       "Responding to vote request",
                       "request"_attr = args.toString(),
                       "response"_attr = response->toString(),
                       "replicaSetStatus"_attr = _getReplSetStatusString());
}

void TopologyCoordinator::loadLastVote(const LastVote& lastVote) {
    _lastVote = lastVote;
}

void TopologyCoordinator::voteForMyselfV1() {
    _lastVote.setTerm(_term);
    _lastVote.setCandidateIndex(_selfIndex);
}

void TopologyCoordinator::setPrimaryIndex(long long primaryIndex) {
    _currentPrimaryIndex = primaryIndex;
}

Status TopologyCoordinator::becomeCandidateIfElectable(const Date_t now,
                                                       StartElectionReasonEnum reason) {
    if (_role == Role::kLeader) {
        return {ErrorCodes::NodeNotElectable, "Not standing for election again; already primary"};
    }

    if (_role == Role::kCandidate) {
        return {ErrorCodes::NodeNotElectable, "Not standing for election again; already candidate"};
    }

    const UnelectableReasonMask unelectableReason = _getMyUnelectableReason(now, reason);
    if (unelectableReason) {
        return {ErrorCodes::NodeNotElectable,
                str::stream() << "Not standing for election because "
                              << _getUnelectableReasonString(unelectableReason)};
    }

    // All checks passed, become a candidate and start election proceedings.
    _role = Role::kCandidate;

    return Status::OK();
}

void TopologyCoordinator::setStorageEngineSupportsReadCommitted(bool supported) {
    _storageEngineSupportsReadCommitted =
        supported ? ReadCommittedSupport::kYes : ReadCommittedSupport::kNo;
}

void TopologyCoordinator::restartHeartbeat(const Date_t now, const HostAndPort& target) {
    for (auto&& member : _memberData) {
        if (member.getHostAndPort() == target) {
            member.restart();
            member.updateLiveness(now);
            return;
        }
    }
}

void TopologyCoordinator::incrementTopologyVersion() {
    auto counter = _topologyVersion.getCounter();
    _topologyVersion.setCounter(counter + 1);
}

OpTime TopologyCoordinator::latestKnownOpTime() const {
    OpTime latest = getMyLastAppliedOpTime();
    for (std::vector<MemberData>::const_iterator it = _memberData.begin(); it != _memberData.end();
         ++it) {
        // Ignore self
        if (it->isSelf()) {
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

        OpTime optime = it->getHeartbeatAppliedOpTime();

        if (optime > latest) {
            latest = optime;
        }
    }

    return latest;
}

boost::optional<OpTime> TopologyCoordinator::latestKnownOpTimeSinceHeartbeatRestart() const {
    // The smallest OpTime in PV1.
    OpTime latest(Timestamp(0, 0), 0);
    for (size_t i = 0; i < _memberData.size(); i++) {
        auto& peer = _memberData[i];

        if (static_cast<int>(i) == _selfIndex) {
            continue;
        }
        // If any heartbeat is not fresh enough, return none.
        if (!peer.isUpdatedSinceRestart()) {
            return boost::none;
        }
        // Ignore down members
        if (!peer.up()) {
            continue;
        }
        if (peer.getHeartbeatAppliedOpTime() > latest) {
            latest = peer.getHeartbeatAppliedOpTime();
        }
    }
    return latest;
}

std::map<MemberId, boost::optional<OpTime>>
TopologyCoordinator::latestKnownOpTimeSinceHeartbeatRestartPerMember() const {
    std::map<MemberId, boost::optional<OpTime>> opTimesPerMember;
    for (size_t i = 0; i < _memberData.size(); i++) {
        auto& member = _memberData[i];
        MemberId memberId = _rsConfig.getMemberAt(i).getId();

        if (!member.isUpdatedSinceRestart()) {
            opTimesPerMember[memberId] = boost::none;
            continue;
        }

        if (!member.up()) {
            opTimesPerMember[memberId] = boost::none;
            continue;
        }

        opTimesPerMember[memberId] = member.getHeartbeatAppliedOpTime();
    }
    return opTimesPerMember;
}

Status TopologyCoordinator::checkIfCommitQuorumCanBeSatisfied(
    const CommitQuorumOptions& commitQuorum) const {
    if (!commitQuorum.mode.empty() && commitQuorum.mode != CommitQuorumOptions::kMajority &&
        commitQuorum.mode != CommitQuorumOptions::kVotingMembers) {
        StatusWith<ReplSetTagPattern> tagPatternStatus =
            _rsConfig.findCustomWriteMode(commitQuorum.mode);
        if (!tagPatternStatus.isOK()) {
            return tagPatternStatus.getStatus();
        }

        ReplSetTagMatch matcher(tagPatternStatus.getValue());
        for (auto&& member : _rsConfig.members()) {
            for (MemberConfig::TagIterator it = member.tagsBegin(); it != member.tagsEnd(); ++it) {
                if (matcher.update(*it)) {
                    return Status::OK();
                }
            }
        }

        // Even if all the nodes in the set had a given write it still would not satisfy this
        // commit quorum.
        return {ErrorCodes::UnsatisfiableCommitQuorum,
                "Commit quorum cannot be satisfied with the current replica set configuration"};
    }

    int nodesRemaining = commitQuorum.numNodes;
    if (!commitQuorum.mode.empty()) {
        if (commitQuorum.mode == CommitQuorumOptions::kMajority) {
            nodesRemaining = _rsConfig.getWriteMajority();
        } else if (commitQuorum.mode == CommitQuorumOptions::kVotingMembers) {
            nodesRemaining = _rsConfig.getWritableVotingMembersCount();
        }
    }

    bool buildIndexesFalseNodes = false;
    for (auto&& member : _rsConfig.members()) {
        // Only count data-bearing nodes.
        if (member.isArbiter()) {
            continue;
        }

        // Only count nodes that build indexes.
        if (!member.shouldBuildIndexes()) {
            buildIndexesFalseNodes = true;
            continue;
        }

        --nodesRemaining;
        if (nodesRemaining <= 0) {
            return Status::OK();
        }
    }

    // buildIndexes:false should not be included in a commitQuorum because they never actually build
    // indexes and vote to commit. Provide a helpful error message to prevent users from starting
    // index builds that will never commit.
    if (buildIndexesFalseNodes) {
        return {ErrorCodes::UnsatisfiableCommitQuorum,
                str::stream() << "Commit quorum cannot depend on buildIndexes:false nodes; "
                              << "use a commit quorum that excludes these nodes"};
    }

    return {ErrorCodes::UnsatisfiableCommitQuorum,
            "Not enough data-bearing nodes to satisfy commit quorum"};
}

}  // namespace repl
}  // namespace mongo
