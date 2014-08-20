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

#include "mongo/bson/optime.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

    /**
     * This class contains the data returned from a heartbeat command for one member
     * of a replica set.
     **/
    class MemberHeartbeatData {
    public:
        explicit MemberHeartbeatData(int configIndex);

        int getConfigIndex() const { return _configIndex; }
        const MemberState& getState() const { return _state; }
        int getHealth() const { return _health; }
        Date_t getUpSince() const { return _upSince; }
        Date_t getLastHeartbeat() const { return _lastHeartbeat; }
        Date_t getLastHeartbeatRecv() const { return _lastHeartbeatRecv; }
        void setLastHeartbeatRecv(time_t newHeartbeatRecvTime) {
            _lastHeartbeatRecv = newHeartbeatRecvTime;
        }
        const std::string& getLastHeartbeatMsg() const { return _lastHeartbeatMsg; }
        const std::string& getSyncSource() const { return _syncSource; }
        OpTime getOpTime() const { return _opTime; }
        int getSkew() const { return _skew; }
        bool hasAuthIssue() const { return _authIssue; }

        OpTime getElectionTime() const { return _electionTime; }

        // Was this member up for the last heartbeat?
        bool up() const { return _health > 0; }
        // Was this member up for the last hearbeeat
        // (or we haven't received the first heartbeat yet)
        bool maybeUp() const { return _health != 0; }

        /**
         * Updates this with the info received from the command result we got from
         * the last heartbeat command.
         */
        void updateFrom(const MemberHeartbeatData& newInfo);

        /**
         * Sets values in this object from the results of a successful heartbeat command.
         * _authIssues is set to false, _health is set to 1, other values are set as specified.
         */
        void setUpValues(Date_t now,
                         MemberState state,
                         OpTime electionTime,
                         OpTime optime,
                         const std::string& syncingTo,
                         const std::string& heartbeatMessage);


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
        void setAuthIssue();

    private:
        // This member's index into the ReplicaSetConfig
        int _configIndex;

        // This member's state
        MemberState _state;

        // -1 = not checked yet, 0 = member is down/unreachable, 1 = member is up
        int _health;

        // Time of first successful heartbeat, if currently still up
        Date_t _upSince;
        // This is the last time we got a response from a heartbeat request to a given member.
        Date_t _lastHeartbeat;
        // This is the last time we got a heartbeat request from a given member.
        Date_t _lastHeartbeatRecv;

        // This is the custom message corresponding to the disposition of the member
        std::string _lastHeartbeatMsg;

        // This is the member's current sync source
        std::string _syncSource;

        // Member's latest applied optime
        OpTime _opTime;

        // Number of seconds positive or negative the remote member's clock is, 
        // relative to the local server's clock
        int _skew;

        // Did the last heartbeat show a failure to authenticate?
        bool _authIssue;

        // Time node was elected primary
        OpTime _electionTime;
    };

} // namespace repl
} // namespace mongo
