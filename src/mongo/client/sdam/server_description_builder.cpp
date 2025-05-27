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
