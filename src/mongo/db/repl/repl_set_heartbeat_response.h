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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <string>

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class Status;

namespace repl {

/**
 * Response structure for the replSetHeartbeat command.
 */
class MONGO_MOD_PUB ReplSetHeartbeatResponse {
public:
    /**
     * Initializes this ReplSetHeartbeatResponse from the contents of "doc".
     */
    Status initialize(const BSONObj& doc, long long term);

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
    std::string toString() const {
        return toBSON().toString();
    }

    const std::string& getReplicaSetName() const {
        return _setName;
    }
    bool hasState() const {
        return _stateSet;
    }
    MemberState getState() const;
    bool hasElectionTime() const {
        return _electionTimeSet;
    }
    Timestamp getElectionTime() const;
    const HostAndPort& getSyncingTo() const {
        return _syncingTo;
    }
    int getConfigVersion() const {
        return _configVersion;
    }
    int getConfigTerm() const {
        return _configTerm;
    }
    ConfigVersionAndTerm getConfigVersionAndTerm() const {
        return ConfigVersionAndTerm(_configVersion, _configTerm);
    }
    bool hasConfig() const {
        return _configSet;
    }
    const ReplSetConfig& getConfig() const;
    bool hasPrimaryId() const {
        return _primaryIdSet;
    }
    long long getPrimaryId() const;
    long long getTerm() const {
        return _term;
    }
    bool hasAppliedOpTime() const {
        return _appliedOpTimeSet;
    }
    OpTime getAppliedOpTime() const;
    OpTimeAndWallTime getAppliedOpTimeAndWallTime() const;
    bool hasWrittenOpTime() const {
        return _writtenOpTimeSet;
    }
    OpTime getWrittenOpTime() const;
    OpTimeAndWallTime getWrittenOpTimeAndWallTime() const;
    bool hasDurableOpTime() const {
        return _durableOpTimeSet;
    }
    OpTime getDurableOpTime() const;
    OpTimeAndWallTime getDurableOpTimeAndWallTime() const;
    bool hasIsElectable() const {
        return _electableSet;
    }
    bool isElectable() const;

    /**
     * Sets _setName to "name".
     */
    void setSetName(StringData name) {
        _setName = std::string{name};
    }

    /**
     * Sets _state to "state".
     */
    void setState(MemberState state) {
        _stateSet = true;
        _state = state;
    }

    /**
     * Sets the optional "electionTime" field to the given Timestamp.
     */
    void setElectionTime(Timestamp time) {
        _electionTimeSet = true;
        _electionTime = time;
    }

    /**
     * Sets _syncingTo to "syncingTo".
     */
    void setSyncingTo(const HostAndPort& syncingTo) {
        _syncingTo = syncingTo;
    }

    /**
     * Sets _configVersion to "configVersion".
     */
    void setConfigVersion(int configVersion) {
        _configVersion = configVersion;
    }

    /**
     * Sets _configTerm to "configTerm".
     */
    void setConfigTerm(int configTerm) {
        _configTerm = configTerm;
    }

    /**
     * Initializes _config with "config".
     */
    void setConfig(const ReplSetConfig& config) {
        _configSet = true;
        _config = config;
    }

    void setPrimaryId(long long primaryId) {
        _primaryIdSet = true;
        _primaryId = primaryId;
    }
    void setAppliedOpTimeAndWallTime(OpTimeAndWallTime time) {
        _appliedOpTimeSet = true;
        _appliedOpTime = time.opTime;
        _appliedWallTime = time.wallTime;
    }
    void setWrittenOpTimeAndWallTime(OpTimeAndWallTime time) {
        _writtenOpTimeSet = true;
        _writtenOpTime = time.opTime;
        _writtenWallTime = time.wallTime;
    }
    void setDurableOpTimeAndWallTime(OpTimeAndWallTime time) {
        _durableOpTimeSet = true;
        _durableOpTime = time.opTime;
        _durableWallTime = time.wallTime;
    }
    void unsetDurableOpTimeAndWallTime() {
        _durableOpTimeSet = false;
        _durableOpTime = OpTime();
        _durableWallTime = Date_t();
    }
    void setTerm(long long term) {
        _term = term;
    }
    void setElectable(bool electable) {
        _electableSet = true;
        _electable = electable;
    }

private:
    bool _electionTimeSet = false;
    Timestamp _electionTime;

    bool _appliedOpTimeSet = false;
    OpTime _appliedOpTime;
    Date_t _appliedWallTime;

    bool _writtenOpTimeSet = false;
    OpTime _writtenOpTime;
    Date_t _writtenWallTime;

    bool _durableOpTimeSet = false;
    OpTime _durableOpTime;
    Date_t _durableWallTime;

    bool _stateSet = false;
    MemberState _state;

    int _configVersion = -1;
    int _configTerm = OpTime::kUninitializedTerm;
    std::string _setName;
    HostAndPort _syncingTo;

    bool _configSet = false;
    ReplSetConfig _config;

    bool _primaryIdSet = false;
    long long _primaryId = -1;
    long long _term = -1;

    bool _electableSet = false;
    bool _electable = false;
};

}  // namespace repl
}  // namespace mongo
