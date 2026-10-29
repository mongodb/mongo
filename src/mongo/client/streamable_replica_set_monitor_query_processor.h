// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/client/sdam/sdam.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_listener.h"
#include "mongo/client/streamable_replica_set_monitor.h"

#include <mutex>

namespace mongo {
class StreamableReplicaSetMonitor::StreamableReplicaSetMonitorQueryProcessor final
    : public sdam::TopologyListener {
public:
    void shutdown();

    void onTopologyDescriptionChangedEvent(sdam::TopologyDescriptionPtr previousDescription,
                                           sdam::TopologyDescriptionPtr newDescription) override;

private:
    static inline const auto kLogLevel = 1;

    mutable std::mutex _mutex;
    bool _isShutdown = false;
};
}  // namespace mongo
