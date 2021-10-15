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
#include "mongo/client/sdam/election_id_set_version_pair.h"
#include "mongo/client/sdam/sdam_configuration.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/platform/basic.h"

namespace mongo::sdam {
class TopologyDescription : public std::enable_shared_from_this<TopologyDescription> {
public:
    TopologyDescription() : TopologyDescription(SdamConfiguration()) {}
    TopologyDescription(const TopologyDescription& source) = default;

    /**
     * Initialize the TopologyDescription with the given configuration.
     */
    TopologyDescription(SdamConfiguration config);

    /**
     * Factory function to create TopologyDescriptions.
     */
    static TopologyDescriptionPtr create(SdamConfiguration config);

    /**
     * Deep copy the given TopologyDescription. The copy constructor won't work in this scenario
     * because shared_from_this cannot be used from within a constructor.
     */
    static TopologyDescriptionPtr clone(const TopologyDescription& source);

    const UUID& getId() const;
    TopologyType getType() const;
    const boost::optional<std::string>& getSetName() const;
    ElectionIdSetVersionPair getMaxElectionIdSetVersionPair() const;

    const std::vector<ServerDescriptionPtr>& getServers() const;

    bool isWireVersionCompatible() const;
    const boost::optional<std::string>& getWireVersionCompatibleError() const;

    const boost::optional<int>& getLogicalSessionTimeoutMinutes() const;
    const Milliseconds& getHeartBeatFrequency() const;

    const boost::optional<ServerDescriptionPtr> findServerByAddress(HostAndPort address) const;
    bool containsServerAddress(const HostAndPort& address) const;
    std::vector<ServerDescriptionPtr> findServers(
        std::function<bool(const ServerDescriptionPtr&)> predicate) const;
    boost::optional<ServerDescriptionPtr> getPrimary();

    /**
     * Adds the given ServerDescription or swaps it with an existing one
     * using the description's HostAndPort as the lookup key. If present, the previous server
     * description is returned.
     */
    boost::optional<ServerDescriptionPtr> installServerDescription(
        const ServerDescriptionPtr& newServerDescription);
    void removeServerDescription(const HostAndPort& HostAndPort);

    void setType(TopologyType type);

    BSONObj toBSON();
    std::string toString();

private:
    friend bool operator==(const TopologyDescription& lhs, const TopologyDescription& rhs) {
        return std::tie(lhs._setName,
                        lhs._type,
                        lhs._maxElectionIdSetVersionPair,
                        lhs._servers,
                        lhs._compatible,
                        lhs._logicalSessionTimeoutMinutes) ==
            std::tie(rhs._setName,
                     rhs._type,
                     rhs._maxElectionIdSetVersionPair,
                     rhs._servers,
                     rhs._compatible,
                     rhs._logicalSessionTimeoutMinutes);
    }

    /**
     * Checks if all server descriptions are compatible with this server's WireVersion. If an
     * incompatible description is found, we set the topologyDescription's _compatible flag to
     * false and store an error message in _compatibleError. A ServerDescription which is not
     * Unknown is incompatible if: minWireVersion > serverMaxWireVersion, or maxWireVersion <
     * serverMinWireVersion
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

    void updateMaxElectionIdSetVersionPair(const ElectionIdSetVersionPair& pair);

    // unique id for this topology
    UUID _id = UUID::gen();

    // a TopologyType enum value.
    TopologyType _type = TopologyType::kUnknown;

    // setName: the replica set name. Default null.
    boost::optional<std::string> _setName;

    // The tuple consisting of:
    // maxSetVersion: an integer or none. The largest setVersion ever reported by a primary.
    // Note: maxSetVersion can go backwards.
    // maxElectionId: an ObjectId or none. The largest electionId ever reported by a primary.
    // Default {none, none}.
    ElectionIdSetVersionPair _maxElectionIdSetVersionPair;

    // servers: a set of ServerDescription instances. Default contains one server:
    // "localhost:27017", ServerType Unknown.
    std::vector<ServerDescriptionPtr> _servers{
        std::make_shared<ServerDescription>(HostAndPort("localhost:27017"))};

    // compatible: a boolean. False if any server's wire protocol version range is incompatible with
    // the client's. Default true.
    bool _compatible = true;

    // compatibilityError: a string. The error message if "compatible" is false, otherwise null.
    boost::optional<std::string> _compatibleError;

    // logicalSessionTimeoutMinutes: integer or null. Default null.
    boost::optional<int> _logicalSessionTimeoutMinutes;

    friend class TopologyStateMachine;
    friend class TopologyDescriptionBuilder;
};
}  // namespace mongo::sdam
