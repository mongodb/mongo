// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/client/replica_set_monitor_stats.h"
#include "mongo/client/sdam/mock_topology_manager.h"
#include "mongo/client/streamable_replica_set_monitor.h"
#include "mongo/executor/task_executor.h"

#include <memory>

namespace mongo {

/**
 * Sets up a streamable replica set monitor for testing that does not automatically refresh on its
 * own, and with configurable topology.
 */
class StreamableReplicaSetMonitorForTesting {
public:
    ~StreamableReplicaSetMonitorForTesting();

    void setup(const MongoURI& uri);

    sdam::MockTopologyManager* getTopologyManager();

private:
    // Executor for monitoring replica sets.
    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    // Allows closing connections established by the network interface associated with the
    // _taskExecutor instance
    std::shared_ptr<ReplicaSetMonitorConnectionManager> _connectionManager;

    // Allows changing replica set topology for mocking replica set targetter response.
    sdam::MockTopologyManager* _topologyManagerPtr;

    // ReplicaSetMonitorManager only stores weak_ptrs, so we need to keep the monitor alive.
    std::shared_ptr<StreamableReplicaSetMonitor> _replSetMonitor;

    std::shared_ptr<ReplicaSetMonitorManagerStats> _stats;
};

}  // namespace mongo
