// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/client/sdam/server_description_builder.h"

#include <map>
#include <set>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sdam {
ServerDescriptionPtr ServerDescriptionBuilder::instance() const {
    return _instance;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withAddress(const HostAndPort& address) {
    _instance->_address = address;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withError(const std::string& error) {
    _instance->_error = error;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withRtt(const HelloRTT& rtt) {
    _instance->_rtt = rtt;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withLastWriteDate(const Date_t& lastWriteDate) {
    _instance->_lastWriteDate = lastWriteDate;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withOpTime(const repl::OpTime opTime) {
    _instance->_opTime = opTime;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withType(const ServerType type) {
    _instance->_type = type;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withMinWireVersion(int minVersion) {
    _instance->_minWireVersion = minVersion;
    return *this;
}
ServerDescriptionBuilder& ServerDescriptionBuilder::withMaxWireVersion(int maxVersion) {
    _instance->_maxWireVersion = maxVersion;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withMe(const HostAndPort& me) {
    _instance->_me = me;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withHost(const HostAndPort& host) {
    _instance->_hosts.emplace(host);
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withPassive(const HostAndPort& passive) {
    _instance->_passives.emplace(passive);
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withArbiter(const HostAndPort& arbiter) {
    _instance->_arbiters.emplace(arbiter);
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withTag(const std::string key,
                                                            const std::string value) {
    _instance->_tags[key] = value;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withSetName(const std::string setName) {
    _instance->_setName = std::move(setName);
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withSetVersion(const int setVersion) {
    _instance->_setVersion = setVersion;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withElectionId(const OID& electionId) {
    _instance->_electionId = electionId;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withPrimary(const HostAndPort& primary) {
    _instance->_primary = primary;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withLastUpdateTime(
    const Date_t& lastUpdateTime) {
    _instance->_lastUpdateTime = lastUpdateTime;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withLogicalSessionTimeoutMinutes(
    const boost::optional<int> logicalSessionTimeoutMinutes) {
    _instance->_logicalSessionTimeoutMinutes = logicalSessionTimeoutMinutes;
    return *this;
}

ServerDescriptionBuilder& ServerDescriptionBuilder::withTopologyVersion(
    TopologyVersion topologyVersion) {
    _instance->_topologyVersion = topologyVersion;
    return *this;
}
};  // namespace mongo::sdam
