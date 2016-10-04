/*
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

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

/**
 * This class contains the data returned from a heartbeat command for one member
 * of a replica set.
 **/
class MemberHeartbeatData {
public:
    MemberHeartbeatData();

    MemberState getState() const {
        return _lastResponse.getState();
    }
    int getHealth() const {
        return _health;
    }
    Date_t getUpSince() const {
        return _upSince;
    }
    Date_t getLastHeartbeat() const {
        return _lastHeartbeat;
    }
    Date_t getLastHeartbeatRecv() const {
        return _lastHeartbeatRecv;
    }
    void setLastHeartbeatRecv(Date_t newHeartbeatRecvTime) {
        _lastHeartbeatRecv = newHeartbeatRecvTime;
    }
    const std::string& getLastHeartbeatMsg() const {
        return _lastResponse.getHbMsg();
    }
    const HostAndPort& getSyncSource() const {
        return _lastResponse.getSyncingTo();
    }
    OpTime getAppliedOpTime() const {
        return _lastResponse.getAppliedOpTime();
    }
    OpTime getDurableOpTime() const {
        return _lastResponse.hasDurableOpTime() ? _lastResponse.getDurableOpTime() : OpTime();
    }
    int getConfigVersion() const {
        return _lastResponse.getConfigVersion();
    }
    bool hasAuthIssue() const {
        return _authIssue;
    }

    Timestamp getElectionTime() const {
        return _lastResponse.getElectionTime();
    }

    long long getTerm() const {
        return _lastResponse.getTerm();
    }

    // Returns true if the last heartbeat data explicilty stated that the node
    // is not electable.
    bool isUnelectable() const {
        return _lastResponse.hasIsElectable() && !_lastResponse.isElectable();
    }

    // Was this member up for the last heartbeat?
    bool up() const {
        return _health > 0;
    }
    // Was this member up for the last hearbeeat
    // (or we haven't received the first heartbeat yet)
    bool maybeUp() const {
        return _health != 0;
    }

    /**
     * Sets values in this object from the results of a successful heartbeat command.
     */
    void setUpValues(Date_t now, const HostAndPort& host, ReplSetHeartbeatResponse&& hbResponse);

    /**
     * Sets values in this object from the results of a erroring/failed heartbeat command.
     * _authIssues is set to false, _health is set to 0, _state is set to RS_DOWN, and
     * other values are set as specified.
     */
    void setDownValues(Date_t now, const std::string& heartbeatMessage);

    /**
     * Sets values in this object that indicate there was an auth issue on the last heartbeat
     * command.
     */
    void setAuthIssue(Date_t now);

private:
    // -1 = not checked yet, 0 = member is down/unreachable, 1 = member is up
    int _health;

    // Time of first successful heartbeat, if currently still up
    Date_t _upSince;
    // This is the last time we got a response from a heartbeat request to a given member.
    Date_t _lastHeartbeat;
    // This is the last time we got a heartbeat request from a given member.
    Date_t _lastHeartbeatRecv;

    // Did the last heartbeat show a failure to authenticate?
    bool _authIssue;

    // The last heartbeat response we received.
    ReplSetHeartbeatResponse _lastResponse;
};

}  // namespace repl
}  // namespace mongo
