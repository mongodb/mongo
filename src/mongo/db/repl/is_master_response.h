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
#include <vector>

#include "mongo/bson/oid.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class Status;

namespace repl {

/**
 * Response structure for the ismaster command.  Only handles responses from nodes
 * that are in replset mode.
 */
class IsMasterResponse {
public:
    IsMasterResponse();

    /**
     * Initializes this IsMasterResponse from the contents of "doc".
     */
    Status initialize(const BSONObj& doc);

    /**
     * Appends all non-default values to "builder".
     * There are two values that are handled specially: if _inShutdown is true or _configSet
     * is false, we will add a standard response to "builder" indicating either that we are
     * in the middle of shutting down or we do not have a valid replica set config, ignoring
     * the values of all other member variables.
     */
    void addToBSON(BSONObjBuilder* builder) const;

    /**
     * Returns a BSONObj consisting the results of calling addToBSON on an otherwise empty
     * BSONObjBuilder.
     */
    BSONObj toBSON() const;


    // ===================== Accessors for member variables ================================= //

    bool isMaster() const {
        return _isMaster;
    }

    bool isSecondary() const {
        return _secondary;
    }

    const std::string& getReplSetName() const {
        return _setName;
    }

    long long getReplSetVersion() const {
        return _setVersion;
    }

    const std::vector<HostAndPort>& getHosts() const {
        return _hosts;
    }

    const std::vector<HostAndPort>& getPassives() const {
        return _passives;
    }

    const std::vector<HostAndPort>& getArbiters() const {
        return _arbiters;
    }

    const HostAndPort& getPrimary() const {
        return _primary;
    }

    bool hasPrimary() const {
        return _primarySet;
    }

    bool isArbiterOnly() const {
        return _arbiterOnly;
    }

    bool isPassive() const {
        return _passive;
    }

    bool isHidden() const {
        return _hidden;
    }

    bool shouldBuildIndexes() const {
        return _buildIndexes;
    }

    Seconds getSlaveDelay() const {
        return _slaveDelay;
    }

    const unordered_map<std::string, std::string> getTags() const {
        return _tags;
    }

    const HostAndPort& getMe() const {
        return _me;
    }

    const OID& getElectionId() const {
        return _electionId;
    }

    OpTime getLastWriteOpTime() const {
        if (!_lastWrite) {
            return OpTime();
        }
        return _lastWrite->opTime;
    }

    time_t getLastWriteDate() const {
        if (!_lastWrite) {
            return 0;
        }
        return _lastWrite->value;
    }

    OpTime getLastMajorityWriteOpTime() const {
        if (!_lastMajorityWrite) {
            return OpTime();
        }
        return _lastMajorityWrite->opTime;
    }

    time_t getLastMajorityWriteDate() const {
        if (!_lastMajorityWrite) {
            return 0;
        }
        return _lastMajorityWrite->value;
    }

    /**
     * If false, calls to toBSON/addToBSON will ignore all other fields and add a specific
     * message to indicate that we have no replica set config.
     */
    bool isConfigSet() const {
        return _configSet;
    }

    /**
     * If false, calls to toBSON/addToBSON will ignore all other fields and add a specific
     * message to indicate that we are in the middle of shutting down.
     */
    bool isShutdownInProgress() const {
        return _shutdownInProgress;
    }


    // ===================== Mutators for member variables ================================= //

    void setIsMaster(bool isMaster);

    void setIsSecondary(bool secondary);

    void setReplSetName(const std::string& setName);

    void setReplSetVersion(long long version);

    void addHost(const HostAndPort& host);

    void addPassive(const HostAndPort& passive);

    void addArbiter(const HostAndPort& arbiter);

    void setPrimary(const HostAndPort& primary);

    void setIsArbiterOnly(bool arbiterOnly);

    void setIsPassive(bool passive);

    void setIsHidden(bool hidden);

    void setShouldBuildIndexes(bool buildIndexes);

    void setSlaveDelay(Seconds slaveDelay);

    void addTag(const std::string& tagKey, const std::string& tagValue);

    void setMe(const HostAndPort& me);

    void setElectionId(const OID& electionId);

    void setLastWrite(const OpTime& lastWriteOpTime, const time_t lastWriteDate);

    void setLastMajorityWrite(const OpTime& lastMajorityWriteOpTime,
                              const time_t lastMajorityWriteDate);

    /**
     * Marks _configSet as false, which will cause future calls to toBSON/addToBSON to ignore
     * all other member variables and output a hardcoded response indicating that we have no
     * valid replica set config.
     */
    void markAsNoConfig();

    /**
     * Marks _shutdownInProgress as true, which will cause future calls to toBSON/addToBSON to
     * ignore all other member variables and output a hardcoded response indicating that we are
     * in the middle of shutting down.
     */
    void markAsShutdownInProgress();

private:
    bool _isMaster;
    bool _isMasterSet;
    bool _secondary;
    bool _isSecondarySet;
    std::string _setName;
    bool _setNameSet;
    long long _setVersion;
    bool _setVersionSet;
    std::vector<HostAndPort> _hosts;
    bool _hostsSet;
    std::vector<HostAndPort> _passives;
    bool _passivesSet;
    std::vector<HostAndPort> _arbiters;
    bool _arbitersSet;
    HostAndPort _primary;
    bool _primarySet;
    bool _arbiterOnly;
    bool _arbiterOnlySet;
    bool _passive;
    bool _passiveSet;
    bool _hidden;
    bool _hiddenSet;
    bool _buildIndexes;
    bool _buildIndexesSet;
    Seconds _slaveDelay;
    bool _slaveDelaySet;
    unordered_map<std::string, std::string> _tags;
    bool _tagsSet;
    HostAndPort _me;
    bool _meSet;
    OID _electionId;
    boost::optional<OpTimeWith<time_t>> _lastWrite;
    boost::optional<OpTimeWith<time_t>> _lastMajorityWrite;

    // If _configSet is false this means we don't have a valid repl set config, so toBSON
    // will return a set of hardcoded values that indicate this.
    bool _configSet;
    // If _shutdownInProgress is true toBSON will return a set of hardcoded values to indicate
    // that we are mid shutdown
    bool _shutdownInProgress;
};

}  // namespace repl
}  // namespace mongo
