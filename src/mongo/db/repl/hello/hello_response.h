// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <string>
#include <string_view>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <sys/types.h>

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class Status;

namespace repl {

/**
 * Response structure for the hello command. Only handles responses from nodes
 * that are in replset mode.
 */
class [[MONGO_MOD_PUBLIC]] HelloResponse {
public:
    HelloResponse();

    /**
     * Initializes this HelloResponse from the contents of "doc".
     */
    Status initialize(const BSONObj& doc);

    /**
     * Appends all non-default values to "builder". When true, "useLegacyResponseFields" indicates
     * that we are responding to an isMaster command and not a hello command. Attach the legacy
     * "ismaster" field if true, and the "isWritablePrimary" field otherwise. There are two values
     * that are handled specially: if _inShutdown is true or _configSet is false, we will add a
     * standard response to "builder" indicating either that we are in the middle of shutting down
     * or we do not have a valid replica set config, ignoring the values of all other member
     * variables.
     */
    void addToBSON(BSONObjBuilder* builder, bool useLegacyResponseFields) const;

    /**
     * Returns a BSONObj consisting the results of calling addToBSON on an otherwise empty
     * BSONObjBuilder.
     */
    BSONObj toBSON(bool useLegacyResponseFields = true) const;


    // ===================== Accessors for member variables ================================= //

    bool isWritablePrimary() const {
        return _isWritablePrimary;
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

    Seconds getSecondaryDelaySecs() const {
        return _secondaryDelaySecs;
    }

    stdx::unordered_map<std::string, std::string> getTags() const {
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

    boost::optional<TopologyVersion> getTopologyVersion() const {
        return _topologyVersion;
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
        return false;
    }


    // ===================== Mutators for member variables ================================= //

    void setIsWritablePrimary(bool isWritablePrimary);

    void setIsSecondary(bool secondary);

    void setReplSetName(std::string_view setName);

    void setReplSetVersion(long long version);

    void addHost(HostAndPort host);

    void addPassive(HostAndPort passive);

    void addArbiter(HostAndPort arbiter);

    void setPrimary(HostAndPort primary);

    void setIsArbiterOnly(bool arbiterOnly);

    void setIsPassive(bool passive);

    void setIsHidden(bool hidden);

    void setShouldBuildIndexes(bool buildIndexes);

    void setTopologyVersion(TopologyVersion topologyVersion);

    void setSecondaryDelaySecs(Seconds secondaryDelaySecs);

    void addTag(const std::string& tagKey, const std::string& tagValue);

    void setMe(HostAndPort me);

    void setElectionId(const OID& electionId);

    void setLastWrite(const OpTime& lastWriteOpTime, time_t lastWriteDate);

    void setLastMajorityWrite(const OpTime& lastMajorityWriteOpTime, time_t lastMajorityWriteDate);

    /**
     * Marks _configSet as false, which will cause future calls to toBSON/addToBSON to ignore
     * all other member variables and output a hardcoded response indicating that we have no
     * valid replica set config.
     */
    void markAsNoConfig();

private:
    bool _isWritablePrimary;
    bool _isWritablePrimarySet;
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
    Seconds _secondaryDelaySecs;
    bool _secondaryDelaySecsSet;
    stdx::unordered_map<std::string, std::string> _tags;
    bool _tagsSet;
    HostAndPort _me;
    bool _meSet;
    OID _electionId;
    boost::optional<OpTimeWith<time_t>> _lastWrite;
    boost::optional<OpTimeWith<time_t>> _lastMajorityWrite;
    boost::optional<TopologyVersion> _topologyVersion;

    // If _configSet is false this means we don't have a valid repl set config, so toBSON
    // will return a set of hardcoded values that indicate this.
    bool _configSet;
};

}  // namespace repl
}  // namespace mongo
