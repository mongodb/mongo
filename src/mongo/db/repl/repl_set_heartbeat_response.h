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
        /**
         * Actions taken based on heartbeat responses
         */
        enum HeartbeatResultAction {
            StepDown,
            StartElection,
            NoAction
        };

        ReplSetHeartbeatResponse();

        /**
         * Initializes this ReplSetHeartbeatArgs from the contents of args.
         */
        Status initialize(const BSONObj& argsObj);

        /**
         * Appends all non-default values to "builder".
         */
        void addToBSON(BSONObjBuilder* builder) const;

        /**
         * Returns a BSONObj consisting of all non-default values to "builder".
         */
        BSONObj toBSON() const;

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
        void setState(int state) { _state = state; }

        /**
         * Sets _electionTime to "time" and sets _electionTimeSet to true to indicate
         * that the value of _electionTime has been modified.
         */
        void setElectionTime(Date_t time) { _electionTimeSet = true; _electionTime = time; }

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
         * Sets _time to "time" and sets _timeSet to true to indicate that the value
         * of _time has been modified.
         */
        void setTime(Date_t time) { _timeSet = true; _time = time; }

        /**
         * Sets _opTime to "time" and sets _opTimeSet to true to indicate that the value
         * of _opTime has been modified.
         */
        void setOpTime(Date_t time) { _opTimeSet = true; _opTime = time; }

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
        void setConfig(const ReplicaSetConfig& config) { _config = config; }

    private:
        Date_t _electionTime;
        Date_t _time;
        Date_t _opTime;
        bool _electionTimeSet;
        bool _timeSet;
        bool _opTimeSet;
        bool _electableSet;
        bool _electable;
        bool _mismatch;
        bool _isReplSet;
        bool _stateDisagreement;
        int _state;
        int _version;
        std::string _setName;
        std::string _hbmsg;
        std::string _syncingTo;
        ReplicaSetConfig _config;
    };

} // namespace repl
} // namespace mongo
