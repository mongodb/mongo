// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/bson/oid.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo::sdam {

/**
 * This class is used in the unit tests to construct TopologyDescription instances. For production
 * code, TopologyDescription instances should be constructed using its constructors.
 */
class [[MONGO_MOD_PUBLIC]] TopologyDescriptionBuilder {
public:
    TopologyDescriptionBuilder() = default;

    /**
     * Return the configured TopologyDescription instance.
     */
    TopologyDescriptionPtr instance() const;


    TopologyDescriptionBuilder& withUUID(const UUID& uuid);

    TopologyDescriptionBuilder& withTopologyType(TopologyType topologyType);

    TopologyDescriptionBuilder& withSetName(const std::string& setName);
    TopologyDescriptionBuilder& withMaxSetVersion(int maxSetVersion);

    TopologyDescriptionBuilder& withServers(const std::vector<ServerDescriptionPtr>& servers);
    TopologyDescriptionBuilder& withMaxElectionID(const OID& maxElectionId);
    TopologyDescriptionBuilder& withCompatibleError(const std::string& error);
    TopologyDescriptionBuilder& withLogicalSessionTimeoutMinutes(int timeout);

private:
    TopologyDescriptionPtr _instance = std::make_shared<TopologyDescription>();
};
}  // namespace mongo::sdam
