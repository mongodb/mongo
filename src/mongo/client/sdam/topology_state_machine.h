// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/client/sdam/sdam_configuration.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/util/net/hostandport.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sdam {
// Actions that mutate the state of the topology description via events.
using TransitionAction = std::function<void(TopologyDescription&, const ServerDescriptionPtr&)>;

// indexed by ServerType
using StateTransitionTableRow = std::vector<TransitionAction>;

/**
 * StateTransitionTable[t][s] returns the action to
 * take given that the topology currently has type t, and we receive a ServerDescription
 * with type s.
 */
using StateTransitionTable = std::vector<StateTransitionTableRow>;

class TopologyStateMachine {
public:
    TopologyStateMachine(const SdamConfiguration& config);

    /**
     * Provides input to the state machine, and triggers the correct action based on the current
     * TopologyDescription and the incoming ServerDescription. The topologyDescription instance may
     * be modified as a result.
     */
    void onServerDescription(TopologyDescription& topologyDescription,
                             const ServerDescriptionPtr& serverDescription);

private:
    void initTransitionTable();

    // State machine actions
    // These are implemented, in an almost verbatim fashion, from the description
    // here:
    // https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#actions
    void updateUnknownWithStandalone(TopologyDescription&, const ServerDescriptionPtr&);
    void updateRSWithoutPrimary(TopologyDescription&, const ServerDescriptionPtr&);
    void updateRSWithPrimaryFromMember(TopologyDescription&, const ServerDescriptionPtr&);
    void updateRSFromPrimary(TopologyDescription&, const ServerDescriptionPtr&);
    void removeAndStopMonitoring(TopologyDescription&, const ServerDescriptionPtr&);
    void checkIfHasPrimary(TopologyDescription&, const ServerDescriptionPtr&);
    void removeAndCheckIfHasPrimary(TopologyDescription&, const ServerDescriptionPtr&);
    void setTopologyTypeAndUpdateRSWithoutPrimary(TopologyDescription&,
                                                  const ServerDescriptionPtr&);
    TransitionAction setTopologyTypeAction(TopologyType type);
    TransitionAction setTopologyTypeAndUpdateRSFromPrimary(TopologyType type);

    void addUnknownServers(TopologyDescription& topologyDescription,
                           const ServerDescriptionPtr& serverDescription);

    // The functions below mutate the state of the topology description
    void installServerDescription(TopologyDescription& topologyDescription,
                                  ServerDescriptionPtr newServerDescription,
                                  bool newServer);
    void removeServerDescription(TopologyDescription& topologyDescription,
                                 HostAndPort serverAddress);

    void modifyTopologyType(TopologyDescription& topologyDescription, TopologyType topologyType);
    void modifySetName(TopologyDescription& topologyDescription,
                       const boost::optional<std::string>& setName);

    StateTransitionTable _stt;
    SdamConfiguration _config;
};
using TopologyStateMachinePtr = std::unique_ptr<TopologyStateMachine>;
}  // namespace mongo::sdam
