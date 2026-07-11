// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/client/streamable_replica_set_monitor_query_processor.h"

#include "mongo/base/checked_cast.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/logv2/log.h"

#include <memory>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
void StreamableReplicaSetMonitor::StreamableReplicaSetMonitorQueryProcessor::shutdown() {
    std::lock_guard lock(_mutex);
    _isShutdown = true;
}

void StreamableReplicaSetMonitor::StreamableReplicaSetMonitorQueryProcessor::
    onTopologyDescriptionChangedEvent(sdam::TopologyDescriptionPtr previousDescription,
                                      sdam::TopologyDescriptionPtr newDescription) {
    {
        std::lock_guard lock(_mutex);
        if (_isShutdown)
            return;
    }

    const auto& setName = newDescription->getSetName();
    if (setName) {
        auto replicaSetMonitor = checked_pointer_cast<StreamableReplicaSetMonitor>(
            ReplicaSetMonitorManager::get()->getMonitor(*setName));
        if (!replicaSetMonitor) {
            LOGV2_DEBUG(4333215,
                        kLogLevel,
                        "Could not find rsm instance for query processing",
                        "replicaSet"_attr = *setName);
            return;
        }
        replicaSetMonitor->_processOutstanding(newDescription);
    }

    // No set name occurs when there is an error monitoring "hello" replies (e.g. HostUnreachable).
    // There is nothing to do in that case.
}
};  // namespace mongo
