// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/client/streamable_replica_set_monitor_discovery_time_processor.h"

#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/logv2/log.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

void StreamableReplicaSetMonitor::StreamableReplicaSetMonitorDiscoveryTimeProcessor::
    onTopologyDescriptionChangedEvent(sdam::TopologyDescriptionPtr previousDescription,
                                      sdam::TopologyDescriptionPtr newDescription) {


    const auto oldPrimary = previousDescription->getPrimary();
    const auto oldHost = oldPrimary ? (*oldPrimary)->getAddress().toString() : "Unknown";

    const auto newPrimary = newDescription->getPrimary();
    const auto newHost = newPrimary ? (*newPrimary)->getAddress().toString() : "Unknown";

    if (newHost != oldHost) {
        std::lock_guard lock(_mutex);
        LOGV2(6006301,
              "Replica set primary server change detected",
              "replicaSet"_attr = newDescription->getSetName(),
              "topologyType"_attr = newDescription->getType(),
              "primary"_attr = newHost,
              "durationMillisSinceLastChange"_attr = _elapsedTime.millis());
        _elapsedTime.reset();
    }
}
Milliseconds StreamableReplicaSetMonitor::StreamableReplicaSetMonitorDiscoveryTimeProcessor::
    getPrimaryServerChangeElapsedTime() const {
    std::lock_guard lock(_mutex);
    return Milliseconds(_elapsedTime.millis());
}

};  // namespace mongo
