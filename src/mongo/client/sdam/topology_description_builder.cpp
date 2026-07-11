// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/sdam/topology_description_builder.h"

#include "mongo/client/sdam/election_id_set_version_pair.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sdam {


/**
 * Return the configured TopologyDescription instance.
 */
TopologyDescriptionPtr TopologyDescriptionBuilder::instance() const {
    return _instance;
}

TopologyDescriptionBuilder& TopologyDescriptionBuilder::withUUID(const UUID& uuid) {
    _instance->_id = uuid;
    return *this;
}

TopologyDescriptionBuilder& TopologyDescriptionBuilder::withTopologyType(TopologyType type) {
    _instance->_type = type;
    return *this;
}

TopologyDescriptionBuilder& TopologyDescriptionBuilder::withSetName(const std::string& setName) {
    _instance->_setName = setName;
    return *this;
}

TopologyDescriptionBuilder& TopologyDescriptionBuilder::withMaxSetVersion(int maxSetVersion) {
    _instance->_maxElectionIdSetVersionPair.setVersion = maxSetVersion;
    return *this;
}

TopologyDescriptionBuilder& TopologyDescriptionBuilder::withServers(
    const std::vector<ServerDescriptionPtr>& servers) {
    _instance->_servers = servers;
    return *this;
}

TopologyDescriptionBuilder& TopologyDescriptionBuilder::withMaxElectionID(
    const OID& maxElectionId) {
    _instance->_maxElectionIdSetVersionPair.electionId = maxElectionId;
    return *this;
}

TopologyDescriptionBuilder& TopologyDescriptionBuilder::withCompatibleError(
    const std::string& error) {
    _instance->_compatible = false;
    _instance->_compatibleError = error;
    return *this;
}

TopologyDescriptionBuilder& TopologyDescriptionBuilder::withLogicalSessionTimeoutMinutes(
    int timeout) {
    _instance->_logicalSessionTimeoutMinutes = timeout;
    return *this;
}

}  // namespace mongo::sdam
