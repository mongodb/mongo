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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/client/sdam/topology_state_machine.h"

#include <ostream>

#include "mongo/client/sdam/election_id_set_version_pair.h"
#include "mongo/client/sdam/sdam_test_base.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

namespace mongo::sdam {
namespace {
static constexpr auto kLogLevel = 2;
}  // namespace

TopologyStateMachine::TopologyStateMachine(const SdamConfiguration& config) : _config(config) {
    initTransitionTable();
}

// This is used to make the syntax in initTransitionTable less verbose.
// Since we have enum class for TopologyType and ServerType there are no implicit int conversions.
template <typename T>
inline int idx(T enumType) {
    return static_cast<int>(enumType);
}

/**
 * This function encodes the transition table specified in
 * https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#topologytype-table
 */
void mongo::sdam::TopologyStateMachine::initTransitionTable() {
    auto bindThis = [&](auto&& pmf) { return [=](auto&&... a) { (this->*pmf)(a...); }; };

    // init the table to No-ops
    _stt.resize(allTopologyTypes().size() + 1);
    for (auto& row : _stt) {
        row.resize(allServerTypes().size() + 1, [](auto&&...) {});
    }

    // From TopologyType: Unknown
    _stt[idx(TopologyType::kUnknown)][idx(ServerType::kStandalone)] =
        bindThis(&TopologyStateMachine::updateUnknownWithStandalone);
    _stt[idx(TopologyType::kUnknown)][idx(ServerType::kMongos)] =
        setTopologyTypeAction(TopologyType::kSharded);
    _stt[idx(TopologyType::kUnknown)][idx(ServerType::kRSPrimary)] =
        setTopologyTypeAndUpdateRSFromPrimary(TopologyType::kReplicaSetWithPrimary);

    {
        const auto serverTypes = std::vector<ServerType>{
            ServerType::kRSSecondary, ServerType::kRSArbiter, ServerType::kRSOther};
        for (auto newServerType : serverTypes) {
            _stt[idx(TopologyType::kUnknown)][idx(newServerType)] =
                bindThis(&TopologyStateMachine::setTopologyTypeAndUpdateRSWithoutPrimary);
        }
    }

    // From TopologyType: Sharded
    {
        const auto serverTypes = std::vector<ServerType>{ServerType::kStandalone,
                                                         ServerType::kRSPrimary,
                                                         ServerType::kRSSecondary,
                                                         ServerType::kRSArbiter,
                                                         ServerType::kRSOther,
                                                         ServerType::kRSGhost};
        for (auto newServerType : serverTypes) {
            _stt[idx(TopologyType::kSharded)][idx(newServerType)] =
                bindThis(&TopologyStateMachine::removeAndStopMonitoring);
        }
    }

    // From TopologyType: ReplicaSetNoPrimary
    {
        const auto serverTypes =
            std::vector<ServerType>{ServerType::kStandalone, ServerType::kMongos};
        for (auto serverType : serverTypes) {
            _stt[idx(TopologyType::kReplicaSetNoPrimary)][idx(serverType)] =
                bindThis(&TopologyStateMachine::removeAndStopMonitoring);
        }
    }

    _stt[idx(TopologyType::kReplicaSetNoPrimary)][idx(ServerType::kRSPrimary)] =
        setTopologyTypeAndUpdateRSFromPrimary(TopologyType::kReplicaSetWithPrimary);

    {
        const auto serverTypes = std::vector<ServerType>{
            ServerType::kRSSecondary, ServerType::kRSArbiter, ServerType::kRSOther};
        for (auto serverType : serverTypes) {
            _stt[idx(TopologyType::kReplicaSetNoPrimary)][idx(serverType)] =
                bindThis(&TopologyStateMachine::updateRSWithoutPrimary);
        }
    }

    // From TopologyType: ReplicaSetWithPrimary
    {
        const auto serverTypes =
            std::vector<ServerType>{ServerType::kUnknown, ServerType::kRSGhost};
        for (auto serverType : serverTypes) {
            _stt[idx(TopologyType::kReplicaSetWithPrimary)][idx(serverType)] =
                bindThis(&TopologyStateMachine::checkIfHasPrimary);
        }
    }

    {
        const auto serverTypes = std::vector<ServerType>{ServerType::kMongos};
        for (auto serverType : serverTypes) {
            _stt[idx(TopologyType::kReplicaSetWithPrimary)][idx(serverType)] =
                bindThis(&TopologyStateMachine::removeAndCheckIfHasPrimary);
        }
    }

    _stt[idx(TopologyType::kReplicaSetWithPrimary)][idx(ServerType::kRSPrimary)] =
        bindThis(&TopologyStateMachine::updateRSFromPrimary);

    {
        const auto serverTypes = std::vector<ServerType>{
            ServerType::kRSSecondary, ServerType::kRSArbiter, ServerType::kRSOther};
        for (auto serverType : serverTypes) {
            _stt[idx(TopologyType::kReplicaSetWithPrimary)][idx(serverType)] =
                bindThis(&TopologyStateMachine::updateRSWithPrimaryFromMember);
        }
    }
}

void TopologyStateMachine::onServerDescription(TopologyDescription& topologyDescription,
                                               const ServerDescriptionPtr& serverDescription) {
    if (!topologyDescription.containsServerAddress(serverDescription->getAddress())) {
        LOGV2_DEBUG(20219,
                    kLogLevel,
                    "Ignoring isMaster reply from server that is not in the topology: "
                    "{serverAddress}",
                    "Ignoring isMaster reply from server that is not in the topology",
                    "serverAddress"_attr = serverDescription->getAddress());
        return;
    }

    ServerDescriptionPtr descriptionToInstall;
    if (topologyDescription.getType() != TopologyType::kSingle &&
        serverDescription->getType() == ServerType::kStandalone) {
        // Create unknown server description with same address
        descriptionToInstall =
            std::make_shared<ServerDescription>(serverDescription, ServerType::kUnknown);
    } else {
        descriptionToInstall = serverDescription;
    }

    installServerDescription(topologyDescription, descriptionToInstall, false);

    if (topologyDescription.getType() != TopologyType::kSingle) {
        auto& action =
            _stt[idx(topologyDescription.getType())][idx(descriptionToInstall->getType())];

        action(topologyDescription, descriptionToInstall);
    }
}

void TopologyStateMachine::updateUnknownWithStandalone(
    TopologyDescription& topologyDescription, const ServerDescriptionPtr& serverDescription) {
    if (!topologyDescription.containsServerAddress(serverDescription->getAddress()))
        return;

    if (_config.getSeedList() && (*_config.getSeedList()).size() == 1) {
        modifyTopologyType(topologyDescription, TopologyType::kSingle);
    } else {
        removeServerDescription(topologyDescription, serverDescription->getAddress());
    }
}

void TopologyStateMachine::updateRSWithoutPrimary(TopologyDescription& topologyDescription,
                                                  const ServerDescriptionPtr& serverDescription) {
    const auto& serverDescAddress = serverDescription->getAddress();

    if (!topologyDescription.containsServerAddress(serverDescAddress))
        return;

    const auto& currentSetName = topologyDescription.getSetName();
    const auto& serverDescSetName = serverDescription->getSetName();
    if (currentSetName == boost::none) {
        modifySetName(topologyDescription, serverDescSetName);
    } else if (currentSetName != serverDescSetName) {
        removeServerDescription(topologyDescription, serverDescription->getAddress());
        return;
    }

    addUnknownServers(topologyDescription, serverDescription);

    if (serverDescription->getMe() && serverDescAddress != serverDescription->getMe()) {
        removeServerDescription(topologyDescription, serverDescription->getAddress());
    }
}

void TopologyStateMachine::addUnknownServers(TopologyDescription& topologyDescription,
                                             const ServerDescriptionPtr& serverDescription) {
    const std::set<HostAndPort>* addressSets[3]{&serverDescription->getHosts(),
                                                &serverDescription->getPassives(),
                                                &serverDescription->getArbiters()};
    for (const auto addresses : addressSets) {
        for (const auto& addressFromSet : *addresses) {
            if (!topologyDescription.containsServerAddress(addressFromSet)) {
                installServerDescription(
                    topologyDescription, std::make_shared<ServerDescription>(addressFromSet), true);
            }
        }
    }
}

void TopologyStateMachine::updateRSWithPrimaryFromMember(
    TopologyDescription& topologyDescription, const ServerDescriptionPtr& serverDescription) {
    const auto& serverDescAddress = serverDescription->getAddress();
    if (!topologyDescription.containsServerAddress(serverDescAddress)) {
        return;
    }

    invariant(serverDescription->getSetName() != boost::none);
    if (topologyDescription.getSetName() != serverDescription->getSetName()) {
        removeAndCheckIfHasPrimary(topologyDescription, serverDescription);
        return;
    }

    if (serverDescription->getMe() &&
        serverDescription->getAddress() != serverDescription->getMe()) {
        removeAndCheckIfHasPrimary(topologyDescription, serverDescription);
        return;
    }

    auto primaries = topologyDescription.findServers([](const ServerDescriptionPtr& description) {
        return description->getType() == ServerType::kRSPrimary;
    });
    if (primaries.size() == 0) {
        modifyTopologyType(topologyDescription, TopologyType::kReplicaSetNoPrimary);
    }
}

void TopologyStateMachine::updateRSFromPrimary(TopologyDescription& topologyDescription,
                                               const ServerDescriptionPtr& serverDescription) {
    const auto& serverDescAddress = serverDescription->getAddress();
    if (!topologyDescription.containsServerAddress(serverDescAddress)) {
        return;
    }

    auto topologySetName = topologyDescription.getSetName();
    auto serverDescSetName = serverDescription->getSetName();
    if (!topologySetName && serverDescSetName) {
        modifySetName(topologyDescription, serverDescSetName);
    } else if (topologySetName != serverDescSetName) {
        // We found a primary but it doesn't have the setName
        // provided by the user or previously discovered.
        removeAndCheckIfHasPrimary(topologyDescription, serverDescription);
        return;
    }

    const ElectionIdSetVersionPair incomingElectionIdSetVersion =
        serverDescription->getElectionIdSetVersionPair();
    const ElectionIdSetVersionPair currentMaxElectionIdSetVersion =
        topologyDescription.getMaxElectionIdSetVersionPair();

    if (incomingElectionIdSetVersion < currentMaxElectionIdSetVersion) {
        LOGV2(5940901,
              "Stale primary detected, marking its state as unknown",
              "primary"_attr = serverDescription->getAddress(),
              "incomingElectionIdSetVersion"_attr = incomingElectionIdSetVersion,
              "currentMaxElectionIdSetVersion"_attr = currentMaxElectionIdSetVersion);
        installServerDescription(
            topologyDescription, std::make_shared<ServerDescription>(serverDescAddress), false);
        checkIfHasPrimary(topologyDescription, serverDescription);
        return;
    }

    topologyDescription.updateMaxElectionIdSetVersionPair(incomingElectionIdSetVersion);

    auto oldPrimaries = topologyDescription.findServers(
        [serverDescAddress](const ServerDescriptionPtr& description) {
            return (description->getAddress() != serverDescAddress &&
                    description->getType() == ServerType::kRSPrimary);
        });
    invariant(oldPrimaries.size() <= 1);
    for (const auto& server : oldPrimaries) {
        installServerDescription(
            topologyDescription, std::make_shared<ServerDescription>(server->getAddress()), false);
    }

    addUnknownServers(topologyDescription, serverDescription);

    std::vector<HostAndPort> toRemove;
    for (const auto& currentServerDescription : topologyDescription.getServers()) {
        const auto currentServerAddress = currentServerDescription->getAddress();
        auto hosts = serverDescription->getHosts().find(currentServerAddress);
        auto passives = serverDescription->getPassives().find(currentServerAddress);
        auto arbiters = serverDescription->getArbiters().find(currentServerAddress);

        if (hosts == serverDescription->getHosts().end() &&
            passives == serverDescription->getPassives().end() &&
            arbiters == serverDescription->getArbiters().end()) {
            toRemove.push_back(currentServerDescription->getAddress());
        }
    }
    for (const auto& serverAddress : toRemove) {
        removeServerDescription(topologyDescription, serverAddress);
    }

    checkIfHasPrimary(topologyDescription, serverDescription);
}

void TopologyStateMachine::removeAndStopMonitoring(TopologyDescription& topologyDescription,
                                                   const ServerDescriptionPtr& serverDescription) {
    removeServerDescription(topologyDescription, serverDescription->getAddress());
}

void TopologyStateMachine::checkIfHasPrimary(TopologyDescription& topologyDescription,
                                             const ServerDescriptionPtr& serverDescription) {
    auto foundPrimaries =
        topologyDescription.findServers([](const ServerDescriptionPtr& description) {
            return description->getType() == ServerType::kRSPrimary;
        });
    if (foundPrimaries.size() > 0) {
        modifyTopologyType(topologyDescription, TopologyType::kReplicaSetWithPrimary);
    } else {
        modifyTopologyType(topologyDescription, TopologyType::kReplicaSetNoPrimary);
    }
}

void TopologyStateMachine::removeAndCheckIfHasPrimary(
    TopologyDescription& topologyDescription, const ServerDescriptionPtr& serverDescription) {
    // Since serverDescription is passed by reference, make a copy of the ServerDescription
    // shared_ptr so that the underlying pointer is still valid for the call to checkIfHasPrimary.
    ServerDescriptionPtr serverDescriptionNoGC(serverDescription);
    removeAndStopMonitoring(topologyDescription, serverDescriptionNoGC);
    checkIfHasPrimary(topologyDescription, serverDescriptionNoGC);
}

TransitionAction TopologyStateMachine::setTopologyTypeAction(TopologyType type) {
    return [this, type](TopologyDescription& topologyDescription,
                        const ServerDescriptionPtr& newServerDescription) {
        modifyTopologyType(topologyDescription, type);
    };
}

TransitionAction TopologyStateMachine::setTopologyTypeAndUpdateRSFromPrimary(TopologyType type) {
    return [this, type](TopologyDescription& topologyDescription,
                        const ServerDescriptionPtr& newServerDescription) {
        modifyTopologyType(topologyDescription, type);
        updateRSFromPrimary(topologyDescription, newServerDescription);
    };
}

void TopologyStateMachine::setTopologyTypeAndUpdateRSWithoutPrimary(
    TopologyDescription& topologyDescription, const ServerDescriptionPtr& serverDescription) {
    modifyTopologyType(topologyDescription, TopologyType::kReplicaSetNoPrimary);
    updateRSWithoutPrimary(topologyDescription, serverDescription);
}

void TopologyStateMachine::removeServerDescription(TopologyDescription& topologyDescription,
                                                   const HostAndPort serverAddress) {
    topologyDescription.removeServerDescription(serverAddress);
    LOGV2_DEBUG(20220,
                kLogLevel,
                "Server '{serverAddress}' was removed from the topology",
                "Server was removed from the topology",
                "serverAddress"_attr = serverAddress);
}

void TopologyStateMachine::modifyTopologyType(TopologyDescription& topologyDescription,
                                              TopologyType topologyType) {
    topologyDescription._type = topologyType;
}

void TopologyStateMachine::modifySetName(TopologyDescription& topologyDescription,
                                         const boost::optional<std::string>& setName) {
    topologyDescription._setName = setName;
}

void TopologyStateMachine::installServerDescription(TopologyDescription& topologyDescription,
                                                    ServerDescriptionPtr newServerDescription,
                                                    bool newServer) {
    topologyDescription.installServerDescription(newServerDescription);
}
}  // namespace mongo::sdam
