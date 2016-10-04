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
#include "mongo/db/repl/optime.h"
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
class ReplSetHeartbeatResponse {
public:
    /**
     * Initializes this ReplSetHeartbeatResponse from the contents of "doc".
     * "term" is only used to complete a V0 OpTime (which is really a Timestamp).
     */
    Status initialize(const BSONObj& doc, long long term);

    /**
     * Appends all non-default values to "builder".
     */
    void addToBSON(BSONObjBuilder* builder, bool isProtocolVersionV1) const;

    /**
     * Returns a BSONObj consisting of all non-default values to "builder".
     */
    BSONObj toBSON(bool isProtocolVersionV1) const;

    /**
     * Returns toBSON().toString()
     */
    const std::string toString() const {
        return toBSON(true).toString();
    }

    bool hasDataSet() const {
        return _hasDataSet;
    }
    bool hasData() const {
        return _hasData;
    }
    bool isMismatched() const {
        return _mismatch;
    }
    bool isReplSet() const {
        return _isReplSet;
    }
    bool isStateDisagreement() const {
        return _stateDisagreement;
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
    bool hasIsElectable() const {
        return _electableSet;
    }
    bool isElectable() const;
    const std::string& getHbMsg() const {
        return _hbmsg;
    }
    bool hasTime() const {
        return _timeSet;
    }
    Seconds getTime() const;
    const HostAndPort& getSyncingTo() const {
        return _syncingTo;
    }
    int getConfigVersion() const {
        return _configVersion;
    }
    bool hasConfig() const {
        return _configSet;
    }
    const ReplicaSetConfig& getConfig() const;
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
    bool hasDurableOpTime() const {
        return _durableOpTimeSet;
    }
    OpTime getDurableOpTime() const;

    /**
     * Sets _mismatch to true.
     */
    void noteMismatched() {
        _mismatch = true;
    }

    /**
     * Sets _isReplSet to true.
     */
    void noteReplSet() {
        _isReplSet = true;
    }

    /**
     * Sets _stateDisagreement to true.
     */
    void noteStateDisagreement() {
        _stateDisagreement = true;
    }

    /**
     * Sets _hasData to true, and _hasDataSet to true to indicate _hasData has been modified
     */
    void noteHasData() {
        _hasDataSet = _hasData = true;
    }

    /**
     * Sets _setName to "name".
     */
    void setSetName(std::string name) {
        _setName = name;
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
     * Sets _electable to "electable" and sets _electableSet to true to indicate
     * that the value of _electable has been modified.
     */
    void setElectable(bool electable) {
        _electableSet = true;
        _electable = electable;
    }

    /**
     * Sets _hbmsg to "hbmsg".
     */
    void setHbMsg(std::string hbmsg) {
        _hbmsg = hbmsg;
    }

    /**
     * Sets the optional "time" field of the response to "theTime", which is
     * a count of seconds since the UNIX epoch.
     */
    void setTime(Seconds theTime) {
        _timeSet = true;
        _time = theTime;
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
     * Initializes _config with "config".
     */
    void setConfig(const ReplicaSetConfig& config) {
        _configSet = true;
        _config = config;
    }

    void setPrimaryId(long long primaryId) {
        _primaryIdSet = true;
        _primaryId = primaryId;
    }
    void setAppliedOpTime(OpTime time) {
        _appliedOpTimeSet = true;
        _appliedOpTime = time;
    }
    void setDurableOpTime(OpTime time) {
        _durableOpTimeSet = true;
        _durableOpTime = time;
    }
    void setTerm(long long term) {
        _term = term;
    }

private:
    bool _electionTimeSet = false;
    Timestamp _electionTime;

    bool _timeSet = false;
    Seconds _time = Seconds(0);  // Seconds since UNIX epoch.

    bool _appliedOpTimeSet = false;
    OpTime _appliedOpTime;

    bool _durableOpTimeSet = false;
    OpTime _durableOpTime;

    bool _electableSet = false;
    bool _electable = false;

    bool _hasDataSet = false;
    bool _hasData = false;

    bool _mismatch = false;
    bool _isReplSet = false;
    bool _stateDisagreement = false;

    bool _stateSet = false;
    MemberState _state;

    int _configVersion = -1;
    std::string _setName;
    std::string _hbmsg;
    HostAndPort _syncingTo;

    bool _configSet = false;
    ReplicaSetConfig _config;

    bool _primaryIdSet = false;
    long long _primaryId = -1;
    long long _term = -1;
};

}  // namespace repl
}  // namespace mongo
