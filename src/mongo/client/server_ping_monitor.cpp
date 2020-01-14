/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork
#include "mongo/client/server_ping_monitor.h"

#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/util/log.h"

namespace mongo {

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutor;
using executor::ThreadPoolTaskExecutor;

SingleServerPingMonitor::SingleServerPingMonitor(sdam::ServerAddress hostAndPort,
                                                 sdam::TopologyListener* rttListener,
                                                 Seconds pingFrequency,
                                                 std::shared_ptr<TaskExecutor> executor)
    : _hostAndPort(hostAndPort),
      _rttListener(rttListener),
      _pingFrequency(pingFrequency),
      _executor(executor) {}

void SingleServerPingMonitor::drop() { /** TODO SERVER-45051: Implement drop() functionality. **/
}

ServerPingMonitor::ServerPingMonitor(sdam::TopologyListener* rttListener,
                                     Seconds pingFrequency,
                                     boost::optional<std::shared_ptr<TaskExecutor>> executor)
    : _rttListener(rttListener), _pingFrequency(pingFrequency) {
    // Create an executor by default. Don't create an executor if one was passed in for testing.
    _setupTaskExecutor(executor);
}

void ServerPingMonitor::_setupTaskExecutor(
    boost::optional<std::shared_ptr<TaskExecutor>> executor) {
    if (executor) {
        // An executor was already provided for testing.
        _executor = std::move(executor.value());
    } else {
        auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
        auto net = executor::makeNetworkInterface(
            "ServerPingMonitor-TaskExecutor", nullptr, std::move(hookList));
        auto pool = std::make_unique<NetworkInterfaceThreadPool>(net.get());
        _executor = std::make_shared<ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
    }
    _executor->startup();
}

void ServerPingMonitor::onServerHandshakeCompleteEvent(sdam::ServerAddress address,
                                                       OID topologyId) {
    stdx::lock_guard lk(_mutex);
    invariant(_serverPingMonitorMap.find(address) == _serverPingMonitorMap.end());
    _serverPingMonitorMap.emplace(address,
                                  std::make_unique<SingleServerPingMonitor>(
                                      address, _rttListener, _pingFrequency, _executor));
    LOG(1) << "ServerPingMonitor is now monitoring " << address;
}

void ServerPingMonitor::onServerClosedEvent(sdam::ServerAddress address, OID topologyId) {
    stdx::lock_guard lk(_mutex);
    auto it = _serverPingMonitorMap.find(address);
    invariant(it != _serverPingMonitorMap.end());
    it->second->drop();
    _serverPingMonitorMap.erase(it);
    LOG(1) << "ServerPingMonitor stopped  monitoring " << address;
}


}  // namespace mongo
