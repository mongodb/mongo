/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include <memory>
#include <string>
#include <unordered_set>

#include "boost/optional/optional.hpp"

#include "mongo/bson/oid.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/platform/basic.h"

namespace mongo::sdam {
class SdamConfiguration {
public:
    SdamConfiguration() : SdamConfiguration(boost::none){};

    /**
     * Initialize the TopologyDescription. This constructor may uassert if the provided
     * configuration options are not valid according to the Server Discovery & Monitoring Spec.
     *
     * Initial Servers
     * initial servers may be set to a seed list of one or more server addresses.
     *
     * Initial TopologyType
     * The initial TopologyType may be set to Single, Unknown, or ReplicaSetNoPrimary.
     *
     * Initial setName
     * The client's initial replica set name is required in order to initially configure the
     * topology type as ReplicaSetNoPrimary.
     *
     * Allowed configuration combinations
     * TopologyType Single cannot be used with multiple seeds.
     * If setName is not null, only TopologyType ReplicaSetNoPrimary and Single, are
     * allowed.
     */
    SdamConfiguration(boost::optional<std::vector<ServerAddress>> seedList,
                      TopologyType initialType = TopologyType::kUnknown,
                      mongo::Milliseconds heartBeatFrequencyMs = kDefaultHeartbeatFrequencyMs,
                      boost::optional<std::string> setName = boost::none);

    const boost::optional<std::vector<ServerAddress>>& getSeedList() const;
    TopologyType getInitialType() const;
    Milliseconds getHeartBeatFrequency() const;
    const boost::optional<std::string>& getSetName() const;

    static inline const mongo::Milliseconds kDefaultHeartbeatFrequencyMs = mongo::Seconds(10);
    static inline const mongo::Milliseconds kMinHeartbeatFrequencyMS = mongo::Milliseconds(500);

private:
    boost::optional<std::vector<ServerAddress>> _seedList;
    TopologyType _initialType;
    mongo::Milliseconds _heartBeatFrequencyMs;
    boost::optional<std::string> _setName;
};

class TopologyDescription {
public:
    TopologyDescription() : TopologyDescription(SdamConfiguration()) {}
    TopologyDescription(const TopologyDescription& source) = default;

    /**
     * Initialize the TopologyDescription with the given configuration.
     */
    TopologyDescription(SdamConfiguration config);

    const UUID& getId() const;
    TopologyType getType() const;
    const boost::optional<std::string>& getSetName() const;

    const boost::optional<int>& getMaxSetVersion() const;
    const boost::optional<OID>& getMaxElectionId() const;

    const std::vector<ServerDescriptionPtr>& getServers() const;

    bool isWireVersionCompatible() const;
    const boost::optional<std::string>& getWireVersionCompatibleError() const;

    const boost::optional<int>& getLogicalSessionTimeoutMinutes() const;
    const Milliseconds& getHeartBeatFrequency() const;

    const boost::optional<ServerDescriptionPtr> findServerByAddress(ServerAddress address) const;
    bool containsServerAddress(const ServerAddress& address) const;
    std::vector<ServerDescriptionPtr> findServers(
        std::function<bool(const ServerDescriptionPtr&)> predicate) const;

    /**
     * Adds the given ServerDescription or swaps it with an existing one
     * using the description's ServerAddress as the lookup key. If present, the previous server
     * description is returned.
     */
    boost::optional<ServerDescriptionPtr> installServerDescription(
        const ServerDescriptionPtr& newServerDescription);
    void removeServerDescription(const ServerAddress& serverAddress);

    void setType(TopologyType type);

    BSONObj toBSON();
    std::string toString();

private:
    /**
     * Checks if all server descriptions are compatible with this server's WireVersion. If an
     * incompatible description is found, we set the topologyDescription's _compatible flag to false
     * and store an error message in _compatibleError. A ServerDescription which is not Unknown is
     * incompatible if:
     *  minWireVersion > serverMaxWireVersion, or maxWireVersion < serverMinWireVersion
     */
    void checkWireCompatibilityVersions();

    /**
     * Used in error string for wire compatibility check.
     *
     * Source:
     * https://github.com/mongodb/specifications/blob/master/source/wireversion-featurelist.rst
     */
    const std::string minimumRequiredMongoVersionString(int version);

    /**
     * From Server Discovery and Monitoring:
     * Updates the TopologyDescription.logicalSessionTimeoutMinutes to the smallest
     * logicalSessionTimeoutMinutes value among ServerDescriptions of all data-bearing server types.
     * If any have a null logicalSessionTimeoutMinutes, then
     * TopologyDescription.logicalSessionTimeoutMinutes is set to null.
     *
     * https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#logical-session-timeout
     */
    void calculateLogicalSessionTimeout();

    // unique id for this topology
    UUID _id = UUID::gen();

    // a TopologyType enum value.
    TopologyType _type = TopologyType::kUnknown;

    // setName: the replica set name. Default null.
    boost::optional<std::string> _setName;

    // maxSetVersion: an integer or null. The largest setVersion ever reported by a primary.
    // Default null.
    boost::optional<int> _maxSetVersion;

    // maxElectionId: an ObjectId or null. The largest electionId ever reported by a primary.
    // Default null.
    boost::optional<OID> _maxElectionId;

    // servers: a set of ServerDescription instances. Default contains one server:
    // "localhost:27017", ServerType Unknown.
    std::vector<ServerDescriptionPtr> _servers{
        std::make_shared<ServerDescription>("localhost:27017")};

    // compatible: a boolean. False if any server's wire protocol version range is incompatible with
    // the client's. Default true.
    bool _compatible = true;

    // compatibilityError: a string. The error message if "compatible" is false, otherwise null.
    boost::optional<std::string> _compatibleError;

    // logicalSessionTimeoutMinutes: integer or null. Default null.
    boost::optional<int> _logicalSessionTimeoutMinutes;

    friend class TopologyStateMachine;
};
}  // namespace mongo::sdam
