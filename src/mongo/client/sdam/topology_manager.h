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
#include <memory>

#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/sdam/topology_listener.h"
#include "mongo/client/sdam/topology_state_machine.h"

namespace mongo::sdam {
/**
 * This class serves as the public interface to the functionality described in the Service Discovery
 * and Monitoring spec:
 *   https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst
 */
class TopologyManager {
    TopologyManager() = delete;
    TopologyManager(const TopologyManager&) = delete;

public:
    explicit TopologyManager(SdamConfiguration config,
                             ClockSource* clockSource,
                             TopologyEventsPublisherPtr eventsPublisher = nullptr);

    /**
     * This function atomically:
     *   1. Clones the current TopologyDescription
     *   2. Executes the state machine logic given the cloned TopologyDescription and provided
     * HelloOutcome (containing the new ServerDescription).
     *   3. Installs the cloned (and possibly modified) TopologyDescription as the current one.
     *
     * Multiple threads may call this function concurrently. However, the manager will process the
     * IsMasterOutcomes serially, as required by:
     *   https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#process-one-ismaster-outcome-at-a-time
     */
    bool onServerDescription(const HelloOutcome& isMasterOutcome);


    /**
     * This function updates the RTT value for a server without executing any state machine actions.
     * It atomically:
     *   1. Clones the current TopologyDescription
     *   2. Clones the ServerDescription corresponding to hostAndPort such that it contains the new
     * RTT value.
     *   3. Installs the cloned ServerDescription into the TopologyDescription from step 1
     *   4. Installs the cloned TopologyDescription as the current one.
     */
    void onServerRTTUpdated(HostAndPort hostAndPort, HelloRTT rtt);

    /**
     * Get the current TopologyDescription. This is safe to call from multiple threads.
     */
    const TopologyDescriptionPtr getTopologyDescription() const;

    /**
     * Executes the given function with the current TopologyDescription while holding the mutex.
     */
    SemiFuture<std::vector<HostAndPort>> executeWithLock(
        std::function<SemiFuture<std::vector<HostAndPort>>(const TopologyDescriptionPtr&)> func);

private:
    void _publishTopologyDescriptionChanged(
        const TopologyDescriptionPtr& oldTopologyDescription,
        const TopologyDescriptionPtr& newTopologyDescription) const;

    mutable mongo::Mutex _mutex = MONGO_MAKE_LATCH("TopologyManager");
    const SdamConfiguration _config;
    ClockSource* _clockSource;
    TopologyDescriptionPtr _topologyDescription;
    TopologyStateMachinePtr _topologyStateMachine;
    TopologyEventsPublisherPtr _topologyEventsPublisher;
};
}  // namespace mongo::sdam
