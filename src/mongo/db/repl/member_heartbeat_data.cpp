/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
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

#include <climits>

#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

MemberHeartbeatData::MemberHeartbeatData()
    : _health(-1), _authIssue(false), _configIndex(-1), _isSelf(false) {
    _lastResponse.setState(MemberState::RS_UNKNOWN);
    _lastResponse.setElectionTime(Timestamp());
    _lastResponse.setAppliedOpTime(OpTime());
}

bool MemberHeartbeatData::setUpValues(Date_t now,
                                      const HostAndPort& host,
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

    if (!hbResponse.hasState()) {
        hbResponse.setState(MemberState::RS_UNKNOWN);
    }
    if (!hbResponse.hasElectionTime()) {
        hbResponse.setElectionTime(_lastResponse.getElectionTime());
    }
    if (!hbResponse.hasAppliedOpTime()) {
        hbResponse.setAppliedOpTime(_lastResponse.getAppliedOpTime());
    }
    // Log if the state changes
    if (_lastResponse.getState() != hbResponse.getState()) {
        log() << "Member " << host.toString() << " is now in state "
              << hbResponse.getState().toString() << rsLog;
    }

    bool opTimeAdvanced = advanceLastAppliedOpTime(hbResponse.getAppliedOpTime(), now);
    auto durableOpTime = hbResponse.hasDurableOpTime() ? hbResponse.getDurableOpTime() : OpTime();
    opTimeAdvanced = advanceLastDurableOpTime(durableOpTime, now) || opTimeAdvanced;
    _lastResponse = std::move(hbResponse);
    return opTimeAdvanced;
}

void MemberHeartbeatData::setDownValues(Date_t now, const std::string& heartbeatMessage) {
    _health = 0;
    _upSince = Date_t();
    _lastHeartbeat = now;
    _authIssue = false;
    _updatedSinceRestart = true;

    _lastResponse = ReplSetHeartbeatResponse();
    _lastResponse.setState(MemberState::RS_DOWN);
    _lastResponse.setElectionTime(Timestamp());
    _lastResponse.setAppliedOpTime(OpTime());
    _lastResponse.setHbMsg(heartbeatMessage);
    _lastResponse.setSyncingTo(HostAndPort());

    // The _lastAppliedOpTime/_lastDurableOpTime fields don't get cleared merely by missing a
    // heartbeat.
}

void MemberHeartbeatData::setAuthIssue(Date_t now) {
    _health = 0;  // set health to 0 so that this doesn't count towards majority.
    _upSince = Date_t();
    _lastHeartbeat = now;
    _authIssue = true;
    _updatedSinceRestart = true;

    _lastResponse = ReplSetHeartbeatResponse();
    _lastResponse.setState(MemberState::RS_UNKNOWN);
    _lastResponse.setElectionTime(Timestamp());
    _lastResponse.setAppliedOpTime(OpTime());
    _lastResponse.setHbMsg("");
    _lastResponse.setSyncingTo(HostAndPort());
}

void MemberHeartbeatData::setLastAppliedOpTime(OpTime opTime, Date_t now) {
    _lastUpdate = now;
    _lastUpdateStale = false;
    _lastAppliedOpTime = opTime;
}

void MemberHeartbeatData::setLastDurableOpTime(OpTime opTime, Date_t now) {
    _lastUpdate = now;
    _lastUpdateStale = false;
    if (_lastAppliedOpTime < opTime) {
        // TODO(russotto): We think this should never happen, rollback or no rollback.  Make this an
        // invariant and see what happens.
        log() << "Durable progress (" << opTime << ") is ahead of the applied progress ("
              << _lastAppliedOpTime << ". This is likely due to a "
                                       "rollback."
              << " memberid: " << _memberId << " rid: " << _rid << " host "
              << _hostAndPort.toString() << " previous durable progress: " << _lastDurableOpTime;
    } else {
        _lastDurableOpTime = opTime;
    }
}

bool MemberHeartbeatData::advanceLastAppliedOpTime(OpTime opTime, Date_t now) {
    _lastUpdate = now;
    _lastUpdateStale = false;
    if (_lastAppliedOpTime < opTime) {
        setLastAppliedOpTime(opTime, now);
        return true;
    }
    return false;
}

bool MemberHeartbeatData::advanceLastDurableOpTime(OpTime opTime, Date_t now) {
    _lastUpdate = now;
    _lastUpdateStale = false;
    if (_lastDurableOpTime < opTime) {
        setLastDurableOpTime(opTime, now);
        return true;
    }
    return false;
}

}  // namespace repl
}  // namespace mongo
