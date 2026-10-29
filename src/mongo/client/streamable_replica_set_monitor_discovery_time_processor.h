// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/client/sdam/sdam.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_listener.h"
#include "mongo/client/streamable_replica_set_monitor.h"
#include "mongo/util/duration.h"
#include "mongo/util/timer.h"

#include <mutex>

#include <boost/move/utility_core.hpp>

namespace mongo {
class StreamableReplicaSetMonitor::StreamableReplicaSetMonitorDiscoveryTimeProcessor final
    : public sdam::TopologyListener {
public:
    void onTopologyDescriptionChangedEvent(sdam::TopologyDescriptionPtr previousDescription,
                                           sdam::TopologyDescriptionPtr newDescription) override;

    Milliseconds getPrimaryServerChangeElapsedTime() const;

private:
    mutable std::mutex _mutex;
    Timer _elapsedTime;
};
}  // namespace mongo
