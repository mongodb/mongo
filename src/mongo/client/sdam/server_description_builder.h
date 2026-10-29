// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/bson/oid.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/db/repl/optime.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

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
