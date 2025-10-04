/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/bson/oid.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo::sdam {

/**
 * This class is used in the unit tests to construct TopologyDescription instances. For production
 * code, TopologyDescription instances should be constructed using its constructors.
 */
class TopologyDescriptionBuilder {
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
