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


#include <absl/container/flat_hash_map.h>
#include <boost/none.hpp>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/client/replica_set_monitor_server_parameters.h"
#include "mongo/client/sdam/topology_listener.h"
#include "mongo/client/streamable_replica_set_monitor.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

using std::set;
using std::shared_ptr;
using std::string;
using std::vector;

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutor;
using executor::TaskExecutorPool;
using executor::ThreadPoolTaskExecutor;

namespace {
const auto getGlobalRSMMonitorManager =
    ServiceContext::declareDecoration<ReplicaSetMonitorManager>();
}  // namespace

Status ReplicaSetMonitorManagerNetworkConnectionHook::validateHost(
    const HostAndPort& remoteHost,
    const BSONObj& helloRequest,
    const executor::RemoteCommandResponse& helloReply) {
    auto monitor = ReplicaSetMonitorManager::get()->getMonitorForHost(remoteHost);
    if (!monitor) {
        return Status::OK();
    }

    if (auto streamableMonitor = checked_pointer_cast<StreamableReplicaSetMonitor>(
            ReplicaSetMonitorManager::get()->getMonitorForHost(remoteHost))) {

        auto publisher = streamableMonitor->getEventsPublisher();
        if (publisher) {
            try {
                if (helloReply.status.isOK()) {
                    publisher->onServerHandshakeCompleteEvent(
                        *helloReply.elapsed, remoteHost, helloReply.data);
                } else {
                    publisher->onServerHandshakeFailedEvent(
                        remoteHost, helloReply.status, helloReply.data);
                }
            } catch (const DBException& exception) {
                LOGV2_ERROR(4712101,
                            "An error occurred publishing a ReplicaSetMonitor handshake event",
                            "error"_attr = exception.toStatus(),
                            "replicaSet"_attr = monitor->getName(),
                            "handshakeStatus"_attr = helloReply.status);
                return exception.toStatus();
            }
        }
    }

    return Status::OK();
}

StatusWith<boost::optional<executor::RemoteCommandRequest>>
ReplicaSetMonitorManagerNetworkConnectionHook::makeRequest(const HostAndPort& remoteHost) {
    return {boost::none};
}

Status ReplicaSetMonitorManagerNetworkConnectionHook::handleReply(
    const HostAndPort& remoteHost, executor::RemoteCommandResponse&& response) {
    MONGO_UNREACHABLE;
}

ReplicaSetMonitorManager::~ReplicaSetMonitorManager() {
    shutdown();
}

ReplicaSetMonitorManager* ReplicaSetMonitorManager::get() {
    return &getGlobalRSMMonitorManager(getGlobalServiceContext());
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getMonitor(StringData setName) {
    stdx::lock_guard<Latch> lk(_mutex);
    _doGarbageCollectionLocked(lk);

    if (auto monitor = _monitors[setName].lock()) {
        return monitor;
    } else {
        return shared_ptr<ReplicaSetMonitor>();
    }
}

void ReplicaSetMonitorManager::_setupTaskExecutorAndStatsInLock() {
    if (_isShutdown || _taskExecutor) {
        // do not restart taskExecutor if is in shutdown
        return;
    }

    if (!_stats) {
        _stats = std::make_shared<ReplicaSetMonitorManagerStats>();
    }

    // construct task executor
    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    auto networkConnectionHook = std::make_unique<ReplicaSetMonitorManagerNetworkConnectionHook>();

    std::shared_ptr<NetworkInterface> networkInterface = executor::makeNetworkInterface(
        "ReplicaSetMonitor-TaskExecutor", std::move(networkConnectionHook), std::move(hookList));
    _connectionManager = std::make_unique<ReplicaSetMonitorConnectionManager>(networkInterface);

    auto pool = std::make_unique<NetworkInterfaceThreadPool>(networkInterface.get());

    _taskExecutor = std::make_shared<ThreadPoolTaskExecutor>(std::move(pool), networkInterface);
    _taskExecutor->startup();
}

namespace {
void uassertNotMixingSSL(transport::ConnectSSLMode a, transport::ConnectSSLMode b) {
    uassert(51042, "Mixing ssl modes with a single replica set is disallowed", a == b);
}
}  // namespace

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getOrCreateMonitor(
    const ConnectionString& connStr, std::function<void()> cleanupCallback) {
    return getOrCreateMonitor(MongoURI(connStr), cleanupCallback);
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getOrCreateMonitor(
    const MongoURI& uri, std::function<void()> cleanupCallback) {
    invariant(uri.type() == ConnectionString::ConnectionType::kReplicaSet);
    stdx::lock_guard<Latch> lk(_mutex);
    uassert(ErrorCodes::ShutdownInProgress,
            str::stream() << "Unable to get monitor for '" << uri << "' due to shutdown",
            !_isShutdown);

    _doGarbageCollectionLocked(lk);
    _setupTaskExecutorAndStatsInLock();
    const auto& setName = uri.getSetName();
    auto monitor = _monitors[setName].lock();
    if (monitor) {
        uassertNotMixingSSL(monitor->getOriginalUri().getSSLMode(), uri.getSSLMode());
        return monitor;
    }

    std::shared_ptr<ReplicaSetMonitor> newMonitor;
    LOGV2(4603701,
          "Starting Replica Set Monitor",
          "protocol"_attr = toString(gReplicaSetMonitorProtocol),
          "uri"_attr = uri.toString());
    invariant(_taskExecutor);

    newMonitor = StreamableReplicaSetMonitor::make(
        uri, _taskExecutor, _getConnectionManager(), cleanupCallback, _stats);

    _monitors[setName] = newMonitor;
    _numMonitorsCreated++;
    return newMonitor;
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getMonitorForHost(const HostAndPort& host) {
    // Hold the shared_ptrs for each of the ReplicaSetMonitors to extend the lifetime of the
    // ReplicaSetMonitor objects to ensure that we do not call their destructors while still holding
    // the mutex.
    vector<shared_ptr<ReplicaSetMonitor>> rsmPtrs;

    stdx::lock_guard<Latch> lk(_mutex);

    for (const auto& entry : _monitors) {
        auto monitor = entry.second.lock();
        if (monitor && monitor->contains(host)) {
            return monitor;
        }
        rsmPtrs.push_back(std::move(monitor));
    }

    return shared_ptr<ReplicaSetMonitor>();
}

vector<string> ReplicaSetMonitorManager::getAllSetNames() const {
    vector<string> allNames;

    stdx::lock_guard<Latch> lk(_mutex);

    for (const auto& entry : _monitors) {
        if (entry.second.lock()) {
            allNames.push_back(entry.first);
        }
    }

    return allNames;
}

size_t ReplicaSetMonitorManager::getNumMonitors() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _monitors.size();
}

void ReplicaSetMonitorManager::removeMonitor(StringData setName) {
    stdx::lock_guard<Latch> lk(_mutex);
    ReplicaSetMonitorsMap::const_iterator it = _monitors.find(setName);
    if (it != _monitors.end()) {
        if (auto monitor = it->second.lock()) {
            monitor->drop();
        }
        _monitors.erase(it);
        LOGV2(20187,
              "Removed ReplicaSetMonitor for replica set {replicaSet}",
              "Removed ReplicaSetMonitor for replica set",
              "replicaSet"_attr = setName);
    }
}

void ReplicaSetMonitorManager::registerForGarbageCollection(StringData setName) {
    stdx::lock_guard<Latch> lk(_gcMutex);
    _gcQueue.emplace_back(setName);
}

void ReplicaSetMonitorManager::_doGarbageCollectionLocked(WithLock) {
    if (_isShutdown) {
        return;
    }
    stdx::lock_guard<Latch> lk(_gcMutex);

    while (!_gcQueue.empty()) {
        std::string setName = std::move(_gcQueue.front());
        _gcQueue.pop_front();
        ReplicaSetMonitorsMap::const_iterator it = _monitors.find(setName);
        if (it != _monitors.end() && !it->second.lock()) {
            _monitors.erase(it);
            _monitorsGarbageCollected.increment();
        }
    }
}

void ReplicaSetMonitorManager::shutdown() {
    decltype(_monitors) monitors;
    decltype(_taskExecutor) taskExecutor;
    decltype(_connectionManager) connectionManager;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (std::exchange(_isShutdown, true)) {
            return;
        }

        monitors = std::exchange(_monitors, {});
        connectionManager = std::exchange(_connectionManager, {});
        taskExecutor = std::exchange(_taskExecutor, {});
    }

    for (auto& [name, monitor] : monitors) {
        auto anchor = monitor.lock();
        if (!anchor) {
            continue;
        }
        anchor->drop();
    }

    if (taskExecutor) {
        LOGV2_DEBUG(20188, 1, "Shutting down task executor used for monitoring replica sets");
        taskExecutor->shutdown();
        taskExecutor->join();
    }
}

void ReplicaSetMonitorManager::removeAllMonitors() {
    shutdown();

    {
        stdx::lock_guard<Latch> lk(_mutex);
        _isShutdown = false;
    }
}

void ReplicaSetMonitorManager::report(BSONObjBuilder* builder, bool forFTDC) {
    // Don't hold _mutex the whole time to avoid ever taking a monitor's mutex while holding the
    // manager's mutex.  Otherwise we could get a deadlock between the manager's, monitor's, and
    // ShardRegistry's mutex due to the ReplicaSetMonitor's AsynchronousConfigChangeHook
    // potentially calling ShardRegistry::updateConfigServerConnectionString.
    auto setNames = getAllSetNames();

    builder->appendNumber("numReplicaSetMonitorsCreated", _numMonitorsCreated);

    {
        BSONObjBuilder setStats(
            builder->subobjStart(forFTDC ? "replicaSetPingTimesMillis" : "replicaSets"));

        for (const auto& setName : setNames) {
            auto monitor = getMonitor(setName);
            if (!monitor) {
                continue;
            }
            monitor->appendInfo(setStats, forFTDC);
        }
    }

    if (_stats) {
        _stats->report(builder, forFTDC);
    }
}

std::shared_ptr<executor::TaskExecutor> ReplicaSetMonitorManager::getExecutor() {
    auto lk = stdx::lock_guard(_mutex);
    return _taskExecutor;
}

std::shared_ptr<executor::EgressConnectionCloser>
ReplicaSetMonitorManager::_getConnectionManager() {
    invariant(_connectionManager);
    return _connectionManager;
}

ReplicaSetChangeNotifier& ReplicaSetMonitorManager::getNotifier() {
    return _notifier;
}

bool ReplicaSetMonitorManager::isShutdown() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _isShutdown;
}

void ReplicaSetMonitorConnectionManager::dropConnections(const HostAndPort& hostAndPort) {
    _network->dropConnections(hostAndPort);
}

void ReplicaSetMonitorManager::installMonitor_forTests(std::shared_ptr<ReplicaSetMonitor> monitor) {
    stdx::lock_guard<Latch> lk(_mutex);
    _monitors[monitor->getName()] = monitor;
}

}  // namespace mongo
