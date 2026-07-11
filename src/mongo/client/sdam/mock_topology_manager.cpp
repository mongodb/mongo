// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/client/sdam/mock_topology_manager.h"

#include "mongo/client/sdam/topology_description.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/observable_mutex.h"

#include <memory>
#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::sdam {

MockTopologyManager::MockTopologyManager() {}

bool MockTopologyManager::onServerDescription(const HelloOutcome& helloOutcome) {
    fasserted(5429100);  // MockTopologyManager does not support onServerDescription
    return true;
}

std::shared_ptr<TopologyDescription> MockTopologyManager::getTopologyDescription() const {
    std::lock_guard<ObservableMutex<std::mutex>> lock(_mutex);
    return _getTopologyDescriptionWithLock(lock);
}

void MockTopologyManager::onServerRTTUpdated(HostAndPort hostAndPort, HelloRTT rtt) {}

void MockTopologyManager::setTopologyDescription(TopologyDescriptionPtr newDescription) {
    std::lock_guard<ObservableMutex<std::mutex>> lock(_mutex);
    _topologyDescription = newDescription;
}

std::shared_ptr<TopologyDescription> MockTopologyManager::_getTopologyDescriptionWithLock(
    WithLock) const {
    return _topologyDescription;
}

}  // namespace mongo::sdam
