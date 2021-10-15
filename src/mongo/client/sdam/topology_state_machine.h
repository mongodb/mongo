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

#include <functional>
#include <memory>
#include <vector>

#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/platform/mutex.h"

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
