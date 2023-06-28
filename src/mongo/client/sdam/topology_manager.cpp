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

#include "mongo/client/sdam/topology_manager.h"

#include <memory>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/oid.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/sdam/topology_state_machine.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/topology_version_gen.h"

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
      _topologyEventsPublisher(eventsPublisher) {}

bool TopologyManagerImpl::onServerDescription(const HelloOutcome& helloOutcome) {
    stdx::lock_guard<mongo::Mutex> lock(_mutex);

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
              "Ignoring this hello response because our topologyVersion: {lastTopologyVersion} is "
              "fresher than the provided topologyVersion: {newTopologyVersion}",
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

std::shared_ptr<TopologyDescription> TopologyManagerImpl::getTopologyDescription() const {
    stdx::lock_guard<mongo::Mutex> lock(_mutex);
    return _topologyDescription;
}

void TopologyManagerImpl::onServerRTTUpdated(HostAndPort hostAndPort, HelloRTT rtt) {
    {
        stdx::lock_guard<mongo::Mutex> lock(_mutex);

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
          "Not updating RTT. Server {server} does not exist in {replicaSet}",
          "Not updating RTT. The server does not exist in the replica set",
          "server"_attr = hostAndPort,
          "replicaSet"_attr = getTopologyDescription()->getSetName());
}

SemiFuture<std::vector<HostAndPort>> TopologyManagerImpl::executeWithLock(
    std::function<SemiFuture<std::vector<HostAndPort>>(const TopologyDescriptionPtr&)> func) {
    stdx::lock_guard<mongo::Mutex> lock(_mutex);
    return func(_topologyDescription);
}

void TopologyManagerImpl::_publishTopologyDescriptionChanged(
    const TopologyDescriptionPtr& oldTopologyDescription,
    const TopologyDescriptionPtr& newTopologyDescription) const {
    if (_topologyEventsPublisher)
        _topologyEventsPublisher->onTopologyDescriptionChangedEvent(oldTopologyDescription,
                                                                    newTopologyDescription);
}
};  // namespace mongo::sdam
