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

#include "mongo/platform/basic.h"

#include <climits>

#include "mongo/db/repl/member_heartbeat_data.h"

namespace mongo {
namespace repl {

    MemberHeartbeatData::MemberHeartbeatData(int configIndex) :
        _configIndex(configIndex),
        _state(MemberState::RS_UNKNOWN), 
        _health(-1), 
        _upSince(0),
        _lastHeartbeat(0),
        _lastHeartbeatRecv(0),
        _skew(INT_MIN), 
        _authIssue(false) {
    }

    void MemberHeartbeatData::updateFrom(const MemberHeartbeatData& newInfo) {
        _state = newInfo.getState();
        _health = newInfo.getHealth();
        _upSince = newInfo.getUpSince();
        _lastHeartbeat = newInfo.getLastHeartbeat();
        _lastHeartbeatMsg = newInfo.getLastHeartbeatMsg();
        _syncSource = newInfo.getSyncSource();
        _opTime = newInfo.getOpTime();
        _skew = newInfo.getSkew();
        _authIssue = newInfo.hasAuthIssue();
        _electionTime = newInfo.getElectionTime();
    }

    void MemberHeartbeatData::setUpValues(Date_t now,
                                          MemberState state,
                                          OpTime electionTime,
                                          OpTime optime,
                                          const std::string& syncingTo,
                                          const std::string& heartbeatMessage) {
        _state = state;
        _health = 1;
        if (_upSince == 0) {
            _upSince = now;
        }
        _lastHeartbeat = now;
        _lastHeartbeatMsg = heartbeatMessage;
        _syncSource = syncingTo;
        _opTime = optime;
        _authIssue = false;
        _electionTime = electionTime;
    }

    void MemberHeartbeatData::setDownValues(Date_t now,
                                            const std::string& heartbeatMessage) {
        _state = MemberState::RS_DOWN;
        _health = 0;
        _upSince = 0;
        _lastHeartbeat = now;
        _lastHeartbeatMsg = heartbeatMessage;
        _authIssue = false;
    }

} // namespace repl
} // namespace mongo
