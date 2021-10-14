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

#include "mongo/client/sdam/topology_description_builder.h"

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
