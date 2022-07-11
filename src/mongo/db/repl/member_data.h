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

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/member_id.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

/**
 * This class contains the data from heartbeat responses and replSetUpdatePosition commands for one
 * member of a replica set.
 **/
class MemberData {
public:
    class HeartbeatChanges {
    public:
        HeartbeatChanges(bool opTimeAdvanced, bool configChanged, bool memberStateChanged)
            : _opTimeAdvanced(opTimeAdvanced),
              _configChanged(configChanged),
              _memberStateChanged(memberStateChanged) {}

        bool getOpTimeAdvanced() const {
            return _opTimeAdvanced;
        }

        bool getConfigChanged() const {
            return _configChanged;
        }

        bool getMemberStateChanged() const {
            return _memberStateChanged;
        }

    private:
        const bool _opTimeAdvanced;
        const bool _configChanged;
        const bool _memberStateChanged;
    };

    MemberData();

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
        return _lastHeartbeatMessage;
    }
    const HostAndPort& getSyncSource() const {
        return _lastResponse.getSyncingTo();
    }
    OpTime getHeartbeatAppliedOpTime() const {
        return _lastResponse.getAppliedOpTime();
    }
    OpTime getHeartbeatDurableOpTime() const {
        return _lastResponse.hasDurableOpTime() ? _lastResponse.getDurableOpTime() : OpTime();
    }
    int getConfigVersion() const {
        return _configVersion;
    }
    long long getConfigTerm() const {
        return _configTerm;
    }
    /**
     * Gets the ReplSetConfig (version, term) pair from the last heartbeatResponse.
     */
    ConfigVersionAndTerm getConfigVersionAndTerm() const {
        return ConfigVersionAndTerm(_configVersion, _configTerm);
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

    // Was this member up for the last heartbeat?
    bool up() const {
        return _health > 0;
    }
    // Was this member up for the last hearbeeat
    // (or we haven't received the first heartbeat yet)
    bool maybeUp() const {
        return _health != 0;
    }

    OpTime getLastAppliedOpTime() const {
        return _lastAppliedOpTime;
    }

    OpTime getLastDurableOpTime() const {
        return _lastDurableOpTime;
    }

    Date_t getLastAppliedWallTime() const {
        return _lastAppliedWallTime;
    }
    Date_t getLastDurableWallTime() const {
        return _lastDurableWallTime;
    }

    // When was the last time this data was updated via any means?
    Date_t getLastUpdate() const {
        return _lastUpdate;
    }
    // Was the last update stale as of the last check?
    bool lastUpdateStale() const {
        return _lastUpdateStale;
    }

    // Index of this member in the replica set config member list.
    int getConfigIndex() const {
        return _configIndex;
    }

    MemberId getMemberId() const {
        return _memberId;
    }

    bool isSelf() const {
        return _isSelf;
    }

    HostAndPort getHostAndPort() const {
        return _hostAndPort;
    }

    /*
     * Returns true if the last heartbeat data explicilty stated that the node is not electable.
     */
    bool isUnelectable() const {
        return _lastResponse.hasIsElectable() && !_lastResponse.isElectable();
    }

    /**
     * Sets values in this object from the results of a successful heartbeat command.  Returns a
     * value indicating whether the lastApplied/lastDurable values advanced, we've received a newer
     * config, and/or the member state changed since the last heartbeat response.
     */
    HeartbeatChanges setUpValues(Date_t now, ReplSetHeartbeatResponse&& hbResponse);

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

    /**
     * Reset the boolean to record the last restart.
     */
    void restart() {
        _updatedSinceRestart = false;
    }

    bool isUpdatedSinceRestart() const {
        return _updatedSinceRestart;
    }

    /**
     * Performs setLastAppliedOpTime and also sets the wall clock time corresponding to the last
     * applied opTime. Should only be used on the current node.
     */
    void setLastAppliedOpTimeAndWallTime(OpTimeAndWallTime opTime, Date_t now);

    /**
     * Performs setLastDurableOpTime and also sets the wall clock time corresponding to the last
     * durable opTime. Should only be used on the current node.
     */
    void setLastDurableOpTimeAndWallTime(OpTimeAndWallTime opTime, Date_t now);

    /**
     * Sets the last applied op time (not the heartbeat applied op time) iff the new optime is
     * later than the current optime, and updates the lastUpdate time.  Returns true if the
     * optime was advanced.
     * Performs advanceLastAppliedOpTime and also sets the wall clock time corresponding to the last
     * applied opTime. Should only be used on the current node.
     */
    bool advanceLastAppliedOpTimeAndWallTime(OpTimeAndWallTime opTime, Date_t now);

    /**
     * Sets the last durable op time (not the heartbeat applied op time) iff the new optime is
     * later than the current optime, and updates the lastUpdate time.  Returns true if the
     * optime was advanced.
     * Performs advanceLastDurableOpTime and also sets the wall clock time corresponding to the last
     * durable opTime. Should only be used on the current node.
     */
    bool advanceLastDurableOpTimeAndWallTime(OpTimeAndWallTime opTime, Date_t now);

    /*
     * Indicates that this data is stale, based on _lastUpdateTime.
     */
    void markLastUpdateStale() {
        _lastUpdateStale = true;
    }

    /*
     * Updates the _lastUpdateTime and clears staleness without changing anything else.
     */
    void updateLiveness(Date_t now) {
        _lastUpdate = now;
        _lastUpdateStale = false;
    }

    void setConfigIndex(int configIndex) {
        _configIndex = configIndex;
    }

    void setIsSelf(bool isSelf) {
        _isSelf = isSelf;
    }

    void setHostAndPort(HostAndPort hostAndPort) {
        _hostAndPort = hostAndPort;
    }

    void setMemberId(MemberId memberId) {
        _memberId = memberId;
    }

    void setConfigVersion(int version) {
        _configVersion = version;
    }

    void setConfigTerm(long long term) {
        _configTerm = term;
    }

private:
    bool _checkAndSetLastDurableOpTime(OpTime opTime, Date_t now);
    // -1 = not checked yet, 0 = member is down/unreachable, 1 = member is up
    int _health;

    // Time of first successful heartbeat, if currently still up
    Date_t _upSince;
    // This is the last time we got a response from a heartbeat request to a given member.
    Date_t _lastHeartbeat;
    // This is the last time we got a heartbeat request from a given member.
    Date_t _lastHeartbeatRecv;

    // This is the error message we got last time from contacting a given member.
    std::string _lastHeartbeatMessage;

    // Did the last heartbeat show a failure to authenticate?
    bool _authIssue;

    // The last heartbeat response we received.
    ReplSetHeartbeatResponse _lastResponse;

    // Have we received heartbeats since the last restart?
    bool _updatedSinceRestart = false;

    // Last time we got any information about this member, whether heartbeat
    // or replSetUpdatePosition.
    Date_t _lastUpdate;

    // Set when lastUpdate time exceeds the election timeout.  Implies that the member is down
    // on the primary, but not the secondaries.
    bool _lastUpdateStale = false;

    // Last known OpTime that the replica has applied and journaled to.
    OpTime _lastDurableOpTime;
    Date_t _lastDurableWallTime = Date_t();

    // Last known OpTime that the replica has applied, whether journaled or unjournaled.
    OpTime _lastAppliedOpTime;
    Date_t _lastAppliedWallTime = Date_t();

    // Last known configVersion.
    int _configVersion = -1;

    // Last known configTerm.
    long long _configTerm = OpTime::kUninitializedTerm;

    // TODO(russotto): Since memberData is kept in config order, _configIndex
    // and _isSelf may not be necessary.
    // Index of this member in the replica set configuration.
    int _configIndex;

    // Is this the data for this member?
    bool _isSelf;

    // This member's member ID.  memberId and hostAndPort duplicate information in the
    // set's ReplSetConfig.
    MemberId _memberId;

    // Client address of this member.
    HostAndPort _hostAndPort;
};

}  // namespace repl
}  // namespace mongo
