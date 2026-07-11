// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/client/sdam/sdam_configuration.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/sdam/topology_listener.h"
#include "mongo/client/sdam/topology_state_machine.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/observable_mutex.h"

#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace mongo::sdam {

class TopologyManager {
public:
    virtual ~TopologyManager() {}

    virtual bool onServerDescription(const HelloOutcome& helloOutcome) = 0;

    virtual void onServerRTTUpdated(HostAndPort hostAndPort, HelloRTT rtt) = 0;

    virtual std::shared_ptr<TopologyDescription> getTopologyDescription() const = 0;

    /**
     * Executes the given function with the current TopologyDescription while holding the mutex.
     */
    decltype(auto) executeWithLock(std::invocable<std::shared_ptr<TopologyDescription>> auto func) {
        auto lock = std::lock_guard{_mutex};
        return func(_getTopologyDescriptionWithLock(lock));
    }

protected:
    mutable mongo::ObservableMutex<std::mutex> _mutex;
    virtual std::shared_ptr<TopologyDescription> _getTopologyDescriptionWithLock(
        WithLock) const = 0;
};

/**
 * This class serves as the public interface to the functionality described in the Service Discovery
 * and Monitoring spec:
 *   https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.md
 */
class TopologyManagerImpl final : public TopologyManager {
    TopologyManagerImpl() = delete;
    TopologyManagerImpl(const TopologyManagerImpl&) = delete;

public:
    explicit TopologyManagerImpl(SdamConfiguration config,
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
     * HelloOutcomes serially, as required by:
     *   https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.md#process-one-hello-or-legacy-hello-outcome-at-a-time
     */
    bool onServerDescription(const HelloOutcome& helloOutcome) override;

    /**
     * This function updates the RTT value for a server without executing any state machine actions.
     * It atomically:
     *   1. Clones the current TopologyDescription
     *   2. Clones the ServerDescription corresponding to hostAndPort such that it contains the new
     * RTT value.
     *   3. Installs the cloned ServerDescription into the TopologyDescription from step 1
     *   4. Installs the cloned TopologyDescription as the current one.
     */
    void onServerRTTUpdated(HostAndPort hostAndPort, HelloRTT rtt) override;

    /**
     * Get the current TopologyDescription. This is safe to call from multiple threads.
     */
    std::shared_ptr<TopologyDescription> getTopologyDescription() const override;

private:
    void _publishTopologyDescriptionChanged(
        const std::shared_ptr<TopologyDescription>& oldTopologyDescription,
        const std::shared_ptr<TopologyDescription>& newTopologyDescription) const;

    std::shared_ptr<TopologyDescription> _getTopologyDescriptionWithLock(WithLock) const override;

    const SdamConfiguration _config;
    ClockSource* _clockSource;
    TopologyDescriptionPtr _topologyDescription;
    TopologyStateMachinePtr _topologyStateMachine;
    TopologyEventsPublisherPtr _topologyEventsPublisher;
};
}  // namespace mongo::sdam
