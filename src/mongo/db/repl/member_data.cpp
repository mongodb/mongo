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


#include "mongo/platform/basic.h"

#include <climits>

#include "mongo/db/repl/member_data.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

MemberData::MemberData() : _health(-1), _authIssue(false), _configIndex(-1), _isSelf(false) {
    _lastResponse.setState(MemberState::RS_UNKNOWN);
    _lastResponse.setElectionTime(Timestamp());
    _lastResponse.setAppliedOpTimeAndWallTime(OpTimeAndWallTime());
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
    // Log if the state changes
    const bool memberStateChanged = _lastResponse.getState() != hbResponse.getState();
    if (memberStateChanged) {
        LOGV2(21215,
              "Member {hostAndPort} is now in state {newState}",
              "Member is in new state",
              "hostAndPort"_attr = _hostAndPort.toString(),
              "newState"_attr = hbResponse.getState().toString());
    }

    bool opTimeAdvanced =
        advanceLastAppliedOpTimeAndWallTime(hbResponse.getAppliedOpTimeAndWallTime(), now);
    auto durableOpTimeAndWallTime = hbResponse.hasDurableOpTime()
        ? hbResponse.getDurableOpTimeAndWallTime()
        : OpTimeAndWallTime();
    opTimeAdvanced =
        advanceLastDurableOpTimeAndWallTime(durableOpTimeAndWallTime, now) || opTimeAdvanced;

    bool configChanged = (getConfigVersionAndTerm() < hbResponse.getConfigVersionAndTerm());
    _configTerm = hbResponse.getConfigTerm();
    _configVersion = hbResponse.getConfigVersion();

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
              "Member {hostAndPort} is now in state DOWN - {heartbeatMessage}",
              "Member is now in state DOWN",
              "hostAndPort"_attr = _hostAndPort.toString(),
              "heartbeatMessage"_attr = redact(heartbeatMessage));
    }

    _lastResponse = ReplSetHeartbeatResponse();
    _lastResponse.setState(MemberState::RS_DOWN);
    _lastResponse.setElectionTime(Timestamp());
    _lastResponse.setAppliedOpTimeAndWallTime(OpTimeAndWallTime());
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
              "Member {hostAndPort} is now in state UNKNOWN due to authentication issue.",
              "Member is now in state UNKNOWN due to authentication issue",
              "hostAndPort"_attr = _hostAndPort.toString());
    }

    _lastResponse = ReplSetHeartbeatResponse();
    _lastResponse.setState(MemberState::RS_UNKNOWN);
    _lastResponse.setElectionTime(Timestamp());
    _lastResponse.setAppliedOpTimeAndWallTime(OpTimeAndWallTime());
    _lastResponse.setSyncingTo(HostAndPort());
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
    // Since _lastDurableOpTime is set asynchronously from _lastAppliedOpTime, it is possible that
    // 'opTime' is ahead of _lastAppliedOpTime.
    if (_lastAppliedOpTime >= opTime.opTime) {
        _lastDurableOpTime = opTime.opTime;
        _lastDurableWallTime = opTime.wallTime;
    }
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
