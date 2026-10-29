// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/member_data.h"

#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

MemberData::MemberData() : _health(-1), _authIssue(false), _configIndex(-1), _isSelf(false) {
    _lastResponse.setState(MemberState::RS_UNKNOWN);
    _lastResponse.setElectionTime(Timestamp());
    _lastResponse.setAppliedOpTimeAndWallTime(OpTimeAndWallTime());
    _lastResponse.setWrittenOpTimeAndWallTime(OpTimeAndWallTime());
}

MemberData::HeartbeatChanges MemberData::setUpValues(Date_t now,
                                                     ReplSetHeartbeatResponse&& hbResponse) {
    _health = 1;
    if (_upSince == Date_t()) {
        _upSince = now;
    }
    _authIssue = false;
    _lastHeartbeat = now;
    _lastUpdate = now;
    _lastUpdateStale = false;
    _updatedSinceRestart = true;
    _lastHeartbeatMessage.clear();

    if (!hbResponse.hasState()) {
        hbResponse.setState(MemberState::RS_UNKNOWN);
    }
    if (!hbResponse.hasElectionTime()) {
        hbResponse.setElectionTime(_lastResponse.getElectionTime());
    }
    if (!hbResponse.hasAppliedOpTime()) {
        hbResponse.setAppliedOpTimeAndWallTime(_lastResponse.getAppliedOpTimeAndWallTime());
    }
    if (!hbResponse.hasWrittenOpTime()) {
        hbResponse.setWrittenOpTimeAndWallTime(_lastResponse.getWrittenOpTimeAndWallTime());
    }
    // Log if the state changes
    const bool memberStateChanged = _lastResponse.getState() != hbResponse.getState();
    if (memberStateChanged) {
        LOGV2(21215,
              "Member is in new state",
              "hostAndPort"_attr = _hostAndPort.toString(),
              "priorityPort"_attr = _priorityPort,
              "newState"_attr = hbResponse.getState().toString());
    }

    bool opTimeAdvanced =
        advanceLastAppliedOpTimeAndWallTime(hbResponse.getAppliedOpTimeAndWallTime(), now);
    opTimeAdvanced =
        advanceLastWrittenOpTimeAndWallTime(hbResponse.getWrittenOpTimeAndWallTime(), now) ||
        opTimeAdvanced;
    auto durableOpTimeAndWallTime = hbResponse.hasDurableOpTime()
        ? hbResponse.getDurableOpTimeAndWallTime()
        : OpTimeAndWallTime();
    opTimeAdvanced =
        advanceLastDurableOpTimeAndWallTime(durableOpTimeAndWallTime, now) || opTimeAdvanced;

    bool configChanged = (getConfigVersionAndTerm() < hbResponse.getConfigVersionAndTerm());
    if (configChanged) {
        _configTerm = hbResponse.getConfigTerm();
        _configVersion = hbResponse.getConfigVersion();
    }

    if (hbResponse.hasLastStableRecoveryTimestamp()) {
        _lastStableRecoveryTimestamp = hbResponse.getLastStableRecoveryTimestamp();
    }

    _lastResponse = std::move(hbResponse);

    return {opTimeAdvanced, configChanged, memberStateChanged};
}

void MemberData::setDownValues(Date_t now, const std::string& heartbeatMessage) {
    _health = 0;
    _upSince = Date_t();
    _lastHeartbeat = now;
    _authIssue = false;
    _updatedSinceRestart = true;
    _lastHeartbeatMessage = heartbeatMessage;

    if (_lastResponse.getState() != MemberState::RS_DOWN) {
        LOGV2(21216,
              "Member is now in state DOWN",
              "hostAndPort"_attr = _hostAndPort.toString(),
              "heartbeatMessage"_attr = redact(heartbeatMessage));
    }

    _lastResponse = ReplSetHeartbeatResponse();
    _lastResponse.setState(MemberState::RS_DOWN);
    _lastResponse.setElectionTime(Timestamp());
    _lastResponse.setAppliedOpTimeAndWallTime(OpTimeAndWallTime());
    _lastResponse.setWrittenOpTimeAndWallTime(OpTimeAndWallTime());
    _lastResponse.setSyncingTo(HostAndPort());

    // The _lastAppliedOpTime/_lastDurableOpTime fields don't get cleared merely by missing a
    // heartbeat.
}

void MemberData::setAuthIssue(Date_t now) {
    _health = 0;  // set health to 0 so that this doesn't count towards majority.
    _upSince = Date_t();
    _lastHeartbeat = now;
    _authIssue = true;
    _updatedSinceRestart = true;
    _lastHeartbeatMessage.clear();

    if (_lastResponse.getState() != MemberState::RS_UNKNOWN) {
        LOGV2(21217,
              "Member is now in state UNKNOWN due to authentication issue",
              "hostAndPort"_attr = _hostAndPort.toString());
    }

    _lastResponse = ReplSetHeartbeatResponse();
    _lastResponse.setState(MemberState::RS_UNKNOWN);
    _lastResponse.setElectionTime(Timestamp());
    _lastResponse.setAppliedOpTimeAndWallTime(OpTimeAndWallTime());
    _lastResponse.setWrittenOpTimeAndWallTime(OpTimeAndWallTime());
    _lastResponse.setSyncingTo(HostAndPort());
}

void MemberData::setLastWrittenOpTimeAndWallTime(OpTimeAndWallTime opTime, Date_t now) {
    invariant(opTime.opTime.isNull() || opTime.wallTime > Date_t());
    _lastUpdate = now;
    _lastUpdateStale = false;
    _lastWrittenOpTime = opTime.opTime;
    _lastWrittenWallTime = opTime.wallTime;
}

void MemberData::setLastAppliedOpTimeAndWallTime(OpTimeAndWallTime opTime, Date_t now) {
    invariant(opTime.opTime.isNull() || opTime.wallTime > Date_t());
    _lastUpdate = now;
    _lastUpdateStale = false;
    _lastAppliedOpTime = opTime.opTime;
    _lastAppliedWallTime = opTime.wallTime;
}

void MemberData::setLastDurableOpTimeAndWallTime(OpTimeAndWallTime opTime, Date_t now) {
    invariant(opTime.opTime.isNull() || opTime.wallTime > Date_t());
    _lastUpdate = now;
    _lastUpdateStale = false;
    _lastDurableOpTime = opTime.opTime;
    _lastDurableWallTime = opTime.wallTime;
}


bool MemberData::advanceLastWrittenOpTimeAndWallTime(OpTimeAndWallTime opTime, Date_t now) {
    invariant(opTime.opTime.isNull() || opTime.wallTime > Date_t());
    _lastUpdate = now;
    _lastUpdateStale = false;
    if (_lastWrittenOpTime < opTime.opTime) {
        setLastWrittenOpTimeAndWallTime(opTime, now);
        return true;
    }
    return false;
}

bool MemberData::advanceLastAppliedOpTimeAndWallTime(OpTimeAndWallTime opTime, Date_t now) {
    invariant(opTime.opTime.isNull() || opTime.wallTime > Date_t());
    _lastUpdate = now;
    _lastUpdateStale = false;
    if (_lastAppliedOpTime < opTime.opTime) {
        setLastAppliedOpTimeAndWallTime(opTime, now);
        return true;
    }
    return false;
}

bool MemberData::advanceLastDurableOpTimeAndWallTime(OpTimeAndWallTime opTime, Date_t now) {
    invariant(opTime.opTime.isNull() || opTime.wallTime > Date_t());
    _lastUpdate = now;
    _lastUpdateStale = false;
    if (_lastDurableOpTime < opTime.opTime) {
        _lastDurableOpTime = opTime.opTime;
        _lastDurableWallTime = opTime.wallTime;
        return true;
    }
    return false;
}

}  // namespace repl
}  // namespace mongo
