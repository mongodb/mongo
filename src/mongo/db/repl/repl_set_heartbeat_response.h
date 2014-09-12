/**
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

#include <string>

#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/util/time_support.h"

namespace mongo {

    class BSONObj;
    class BSONObjBuilder;
    class Status;

namespace repl {

    /**
     * Repsonse structure for the replSetHeartbeat command.
     */
    class ReplSetHeartbeatResponse {
    public:
        ReplSetHeartbeatResponse();

        /**
         * Initializes this ReplSetHeartbeatResponse from the contents of "doc".
         */
        Status initialize(const BSONObj& doc);

        /**
         * Appends all non-default values to "builder".
         */
        void addToBSON(BSONObjBuilder* builder) const;

        /**
         * Returns a BSONObj consisting of all non-default values to "builder".
         */
        BSONObj toBSON() const;

        bool isMismatched() const { return _mismatch; }
        bool isReplSet() const { return _isReplSet; }
        bool isStateDisagreement() const { return _stateDisagreement; }
        const std::string& getReplicaSetName() const { return _setName; }
        bool hasState() const { return _stateSet; }
        MemberState getState() const;
        bool hasElectionTime() const { return _electionTimeSet; }
        OpTime getElectionTime() const;
        bool hasIsElectable() const { return _electableSet; }
        bool isElectable() const;
        const std::string& getHbMsg() const { return _hbmsg; }
        bool hasTime() const { return _timeSet; }
        Seconds getTime() const;
        bool hasOpTime() const { return _opTimeSet; }
        OpTime getOpTime() const;
        const std::string& getSyncingTo() const { return _syncingTo; }
        int getVersion() const { return _version; }
        bool hasConfig() const { return _configSet; }
        const ReplicaSetConfig& getConfig() const;

        /**
         * Sets _mismatch to true.
         */
        void noteMismatched() { _mismatch = true; }

        /**
         * Sets _isReplSet to true.
         */
        void noteReplSet() { _isReplSet = true; }

        /**
         * Sets _stateDisagreement to true.
         */
        void noteStateDisagreement() { _stateDisagreement = true; }

        /**
         * Sets _setName to "name".
         */
        void setSetName(std::string name) { _setName = name; }

        /**
         * Sets _state to "state".
         */
        void setState(MemberState state) { _stateSet = true; _state = state; }

        /**
         * Sets the optional "electionTime" field to the given OpTime.
         */
        void setElectionTime(OpTime time) { _electionTimeSet = true; _electionTime = time; }

        /**
         * Sets _electable to "electable" and sets _electableSet to true to indicate
         * that the value of _electable has been modified.
         */
        void setElectable(bool electable) { _electableSet = true; _electable = electable; }

        /**
         * Sets _hbmsg to "hbmsg".
         */
        void setHbMsg(std::string hbmsg) { _hbmsg = hbmsg; }

        /**
         * Sets the optional "time" field of the response to "theTime", which is 
         * a count of seconds since the UNIX epoch.
         */
        void setTime(Seconds theTime) { _timeSet = true; _time = theTime; }

        /**
         * Sets _opTime to "time" and sets _opTimeSet to true to indicate that the value
         * of _opTime has been modified.
         */
        void setOpTime(OpTime time) { _opTimeSet = true; _opTime = time; }

        /**
         * Sets _syncingTo to "syncingTo".
         */
        void setSyncingTo(std::string syncingTo) { _syncingTo = syncingTo; }

        /**
         * Sets _version to "version".
         */
        void setVersion(int version) { _version = version; }

        /**
         * Initializes _config with "config".
         */
        void setConfig(const ReplicaSetConfig& config) { _configSet = true; _config = config; }

    private:
        bool _electionTimeSet;
        OpTime _electionTime;

        bool _timeSet;
        Seconds _time;  // Seconds since UNIX epoch.

        bool _opTimeSet;
        OpTime _opTime;

        bool _electableSet;
        bool _electable;

        bool _mismatch;
        bool _isReplSet;
        bool _stateDisagreement;

        bool _stateSet;
        MemberState _state;

        int _version;
        std::string _setName;
        std::string _hbmsg;
        std::string _syncingTo;

        bool _configSet;
        ReplicaSetConfig _config;
    };

} // namespace repl
} // namespace mongo
