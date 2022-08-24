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

#include "mongo/client/sdam/server_description.h"

namespace mongo::sdam {

/**
 * This class is used in the unit tests to construct ServerDescription instances. For production
 * code, ServerDescription instances should be constructed using its constructors.
 */
class ServerDescriptionBuilder {
public:
    ServerDescriptionBuilder() = default;

    /**
     * Return the configured ServerDescription instance.
     */
    ServerDescriptionPtr instance() const;

    // server identity
    ServerDescriptionBuilder& withAddress(const HostAndPort& address);
    ServerDescriptionBuilder& withType(ServerType type);
    ServerDescriptionBuilder& withMe(const HostAndPort& me);
    ServerDescriptionBuilder& withTag(std::string key, std::string value);
    ServerDescriptionBuilder& withSetName(std::string setName);

    // network attributes
    ServerDescriptionBuilder& withRtt(const HelloRTT& rtt);
    ServerDescriptionBuilder& withError(const std::string& error);
    ServerDescriptionBuilder& withLogicalSessionTimeoutMinutes(
        boost::optional<int> logicalSessionTimeoutMinutes);

    // server capabilities
    ServerDescriptionBuilder& withMinWireVersion(int minVersion);
    ServerDescriptionBuilder& withMaxWireVersion(int maxVersion);

    // server 'time'
    ServerDescriptionBuilder& withLastWriteDate(const Date_t& lastWriteDate);
    ServerDescriptionBuilder& withOpTime(repl::OpTime opTime);
    ServerDescriptionBuilder& withLastUpdateTime(const Date_t& lastUpdateTime);

    // topology membership
    ServerDescriptionBuilder& withPrimary(const HostAndPort& primary);
    ServerDescriptionBuilder& withHost(const HostAndPort& host);
    ServerDescriptionBuilder& withPassive(const HostAndPort& passive);
    ServerDescriptionBuilder& withArbiter(const HostAndPort& arbiter);
    ServerDescriptionBuilder& withSetVersion(int setVersion);
    ServerDescriptionBuilder& withElectionId(const OID& electionId);
    ServerDescriptionBuilder& withTopologyVersion(TopologyVersion topologyVersion);

private:
    constexpr static auto kHostAndPortNotSet = "address.not.set:1234";
    ServerDescriptionPtr _instance =
        std::make_shared<ServerDescription>(HostAndPort(kHostAndPortNotSet));
};
}  // namespace mongo::sdam
