/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/client/streamable_replica_set_monitor_for_testing.h"

#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"

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

    auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(networkInterface.get());

    _taskExecutor =
        std::make_shared<executor::ThreadPoolTaskExecutor>(std::move(pool), networkInterface);
    _taskExecutor->startup();

    _replSetMonitor = std::make_shared<StreamableReplicaSetMonitor>(
        uri, _taskExecutor, _connectionManager, [] {});
    auto topologyManager = std::make_unique<sdam::MockTopologyManager>();
    _topologyManagerPtr = topologyManager.get();

    _replSetMonitor->initForTesting(std::move(topologyManager));
    ReplicaSetMonitorManager::get()->installMonitor_forTests(_replSetMonitor);
}

sdam::MockTopologyManager* StreamableReplicaSetMonitorForTesting::getTopologyManager() {
    return _topologyManagerPtr;
}

}  // namespace mongo
