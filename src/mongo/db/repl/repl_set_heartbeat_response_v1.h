/**
 *    Copyright (C) 2015 MongoDB Inc.
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
     * Response structure for the replSetHeartbeat command.
     */
    class ReplSetHeartbeatResponseV1 {
    public:
        /**
         * Initializes this ReplSetHeartbeatResponseV1 from the contents of "doc".
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

        /**
         * Returns toBSON().toString()
         */
        const std::string toString() const { return toBSON().toString(); }

        bool isReplSet() const { return _isReplSet; }
        std::string getReplSetName() const { return _setName; }
        MemberState getMemberState() const { return _state; }
        Timestamp getOpTime() const { return _opTime; }
        std::string getSyncTarget() const { return _syncingTo; }
        long long getConfigVersion() const { return _configVersion; }
        long long getPrimaryId() const { return _primaryId; }
        Timestamp getLastOpCommitted() const { return _lastOpCommitted; }
        long long getTerm() const { return _term; }
        bool isConfigSet() const { return _configSet; }
        ReplicaSetConfig getConfig() const;

        void noteReplSet() { _isReplSet = true; }
        void setSetName(std::string name) { _setName = name; }
        void setState(MemberState state) { _state = state; }
        void setOpTime(Timestamp time) { _opTime = time; }
        void setSyncingTo(std::string syncingTo) { _syncingTo = syncingTo; }
        void setConfigVersion(long long version) { _configVersion = version; }
        void setPrimaryId(long long primaryId) { _primaryId = primaryId; }
        void setLastOpCommitted(Timestamp time) { _lastOpCommitted = time; }
        void setTerm(long long term) { _term = term; }
        void setConfig(const ReplicaSetConfig& config) { _configSet = true; _config = config; }

    private:
        MemberState _state;
        ReplicaSetConfig _config;
        Timestamp _lastOpCommitted;
        Timestamp _opTime;
        bool _configSet = false;
        bool _isReplSet = false;
        long long _configVersion = -1;
        long long _primaryId = -1;
        long long _term = -1;
        std::string _setName;
        std::string _syncingTo;
    };

} // namespace repl
} // namespace mongo
