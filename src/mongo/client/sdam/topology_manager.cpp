// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/sdam/topology_manager.h"

#include "mongo/bson/oid.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/sdam/topology_state_machine.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/util/observable_mutex_registry.h"

#include <memory>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::sdam {
namespace {

/* Compare topologyVersions to determine if the hello response's topologyVersion is stale
 * according to the following rules:
 * 1. If the response's topologyVersion is unset or the lastServerDescription's topologyVersion is
 * null, the client MUST assume the response is more recent.
 * 2. If the response’s topologyVersion.processId != to lastServerDescription's, the client MUST
 * assume the response is more recent.
 * 3. If the response’s topologyVersion.processId == to lastServerDescription's and response's
 * topologyVersion.counter < lastServerDescription's topologyVersion.counter, the client MUST ignore
 * this reply because the lastServerDescription is fresher.
 */
bool isStaleTopologyVersion(boost::optional<TopologyVersion> lastTopologyVersion,
                            boost::optional<TopologyVersion> newTopologyVersion) {
    if (lastTopologyVersion && newTopologyVersion &&
        ((lastTopologyVersion->getProcessId() == newTopologyVersion->getProcessId()) &&
         (lastTopologyVersion->getCounter() > newTopologyVersion->getCounter()))) {
        return true;
    }

    return false;
}
}  // namespace


TopologyManagerImpl::TopologyManagerImpl(SdamConfiguration config,
                                         ClockSource* clockSource,
                                         TopologyEventsPublisherPtr eventsPublisher)
    : _config(std::move(config)),
      _clockSource(clockSource),
      _topologyDescription(TopologyDescription::create(_config)),
      _topologyStateMachine(std::make_unique<TopologyStateMachine>(_config)),
      _topologyEventsPublisher(eventsPublisher) {
    ObservableMutexRegistry::get().add("topologyManagerImplMutex", _mutex);
}

bool TopologyManagerImpl::onServerDescription(const HelloOutcome& helloOutcome) {
    std::lock_guard<ObservableMutex<std::mutex>> lock(_mutex);

    boost::optional<HelloRTT> lastRTT;
    boost::optional<TopologyVersion> lastTopologyVersion;

    const auto& lastServerDescription =
        _topologyDescription->findServerByAddress(helloOutcome.getServer());
    if (lastServerDescription) {
        lastRTT = (*lastServerDescription)->getRtt();
        lastTopologyVersion = (*lastServerDescription)->getTopologyVersion();
    }

    boost::optional<TopologyVersion> newTopologyVersion = helloOutcome.getTopologyVersion();
    if (isStaleTopologyVersion(lastTopologyVersion, newTopologyVersion)) {
        LOGV2(23930,
              "Ignoring this hello response because our last topologyVersion is fresher than the "
              "new topologyVersion provided",
              "lastTopologyVersion"_attr = lastTopologyVersion->toBSON(),
              "newTopologyVersion"_attr = newTopologyVersion->toBSON());
        return false;
    }

    auto newServerDescription = std::make_shared<ServerDescription>(
        _clockSource, helloOutcome, lastRTT, newTopologyVersion);

    auto oldTopologyDescription = _topologyDescription;
    _topologyDescription = TopologyDescription::clone(*oldTopologyDescription);

    // if we are equal to the old description, just install the new description without
    // performing any actions on the state machine.
    auto isEqualToOldServerDescription =
        (lastServerDescription && (*lastServerDescription->get()) == *newServerDescription);
    if (isEqualToOldServerDescription) {
        _topologyDescription->installServerDescription(newServerDescription);
    } else {
        _topologyStateMachine->onServerDescription(*_topologyDescription, newServerDescription);
    }

    _publishTopologyDescriptionChanged(oldTopologyDescription, _topologyDescription);
    return true;
}

TopologyDescriptionPtr TopologyManagerImpl::getTopologyDescription() const {
    std::lock_guard<ObservableMutex<std::mutex>> lock(_mutex);
    return _getTopologyDescriptionWithLock(lock);
}

void TopologyManagerImpl::onServerRTTUpdated(HostAndPort hostAndPort, HelloRTT rtt) {
    {
        std::lock_guard<ObservableMutex<std::mutex>> lock(_mutex);

        auto oldServerDescription = _topologyDescription->findServerByAddress(hostAndPort);
        if (oldServerDescription) {
            auto newServerDescription = (*oldServerDescription)->cloneWithRTT(rtt);

            auto oldTopologyDescription = _topologyDescription;
            _topologyDescription = TopologyDescription::clone(*oldTopologyDescription);
            _topologyDescription->installServerDescription(newServerDescription);

            _publishTopologyDescriptionChanged(oldTopologyDescription, _topologyDescription);

            return;
        }
    }
    // otherwise, the server was removed from the topology. Nothing to do.
    LOGV2(4333201,
          "Not updating RTT. The server does not exist in the replica set",
          "server"_attr = hostAndPort,
          "replicaSet"_attr = getTopologyDescription()->getSetName());
}

void TopologyManagerImpl::_publishTopologyDescriptionChanged(
    const TopologyDescriptionPtr& oldTopologyDescription,
    const TopologyDescriptionPtr& newTopologyDescription) const {
    if (_topologyEventsPublisher)
        _topologyEventsPublisher->onTopologyDescriptionChangedEvent(oldTopologyDescription,
                                                                    newTopologyDescription);
}

TopologyDescriptionPtr TopologyManagerImpl::_getTopologyDescriptionWithLock(WithLock) const {
    return _topologyDescription;
}

};  // namespace mongo::sdam
