// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/streamable_replica_set_monitor_for_testing.h"

#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"

#include <utility>

namespace mongo {

StreamableReplicaSetMonitorForTesting::~StreamableReplicaSetMonitorForTesting() {
    if (_taskExecutor) {
        _taskExecutor->shutdown();
        _taskExecutor->join();
    }

    if (_replSetMonitor) {
        ReplicaSetMonitorManager::get()->removeMonitor(_replSetMonitor->getName());
    }
}

void StreamableReplicaSetMonitorForTesting::setup(const MongoURI& uri) {
    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    auto networkConnectionHook = std::make_unique<ReplicaSetMonitorManagerNetworkConnectionHook>();

    std::shared_ptr<executor::NetworkInterface> networkInterface =
        executor::makeNetworkInterface("ReplicaSetMonitor-TestTaskExecutor",
                                       std::move(networkConnectionHook),
                                       std::move(hookList));
    _connectionManager = std::make_unique<ReplicaSetMonitorConnectionManager>(networkInterface);
    _stats = std::make_shared<ReplicaSetMonitorManagerStats>();

    auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(networkInterface.get());

    _taskExecutor = executor::ThreadPoolTaskExecutor::create(std::move(pool), networkInterface);
    _taskExecutor->startup();

    _replSetMonitor = std::make_shared<StreamableReplicaSetMonitor>(
        uri, _taskExecutor, _connectionManager, [] {}, _stats);
    auto topologyManager = std::make_unique<sdam::MockTopologyManager>();
    _topologyManagerPtr = topologyManager.get();

    _replSetMonitor->initForTesting(std::move(topologyManager));
    ReplicaSetMonitorManager::get()->installMonitor_forTests(_replSetMonitor);
}

sdam::MockTopologyManager* StreamableReplicaSetMonitorForTesting::getTopologyManager() {
    return _topologyManagerPtr;
}

}  // namespace mongo
