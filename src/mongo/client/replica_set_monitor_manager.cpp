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

#include "mongo/platform/basic.h"

#include "mongo/client/replica_set_monitor_manager.h"

#include <memory>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_server_parameters.h"
#include "mongo/client/scanning_replica_set_monitor.h"
#include "mongo/client/streamable_replica_set_monitor.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/util/map_util.h"

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
    const BSONObj& isMasterRequest,
    const executor::RemoteCommandResponse& isMasterReply) {
    if (gReplicaSetMonitorProtocol != ReplicaSetMonitorProtocol::kScanning) {
        auto monitor = ReplicaSetMonitorManager::get()->getMonitorForHost(remoteHost);
        if (!monitor) {
            return Status::OK();
        }

        if (std::shared_ptr<StreamableReplicaSetMonitor> streamableMonitor =
                std::dynamic_pointer_cast<StreamableReplicaSetMonitor>(
                    ReplicaSetMonitorManager::get()->getMonitorForHost(remoteHost))) {

            auto publisher = streamableMonitor->getEventsPublisher();
            if (publisher) {
                publisher->onServerHandshakeCompleteEvent(
                    isMasterReply.elapsedMillis.get(), remoteHost.toString(), isMasterReply.data);
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

    if (auto monitor = _monitors[setName].lock()) {
        return monitor;
    } else {
        return shared_ptr<ReplicaSetMonitor>();
    }
}

void ReplicaSetMonitorManager::_setupTaskExecutorInLock() {
    if (_isShutdown || _taskExecutor) {
        // do not restart taskExecutor if is in shutdown
        return;
    }

    // construct task executor
    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    auto networkConnectionHook = std::make_unique<ReplicaSetMonitorManagerNetworkConnectionHook>();
    auto net = executor::makeNetworkInterface(
        "ReplicaSetMonitor-TaskExecutor", std::move(networkConnectionHook), std::move(hookList));
    auto pool = std::make_unique<NetworkInterfaceThreadPool>(net.get());
    _taskExecutor = std::make_shared<ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
    _taskExecutor->startup();
}

namespace {
void uassertNotMixingSSL(transport::ConnectSSLMode a, transport::ConnectSSLMode b) {
    uassert(51042, "Mixing ssl modes with a single replica set is disallowed", a == b);
}
}  // namespace

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getOrCreateMonitor(
    const ConnectionString& connStr) {
    return getOrCreateMonitor(MongoURI(connStr));
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getOrCreateMonitor(const MongoURI& uri) {
    invariant(uri.type() == ConnectionString::SET);
    stdx::lock_guard<Latch> lk(_mutex);
    uassert(ErrorCodes::ShutdownInProgress,
            str::stream() << "Unable to get monitor for '" << uri << "' due to shutdown",
            !_isShutdown);

    _setupTaskExecutorInLock();
    const auto& setName = uri.getSetName();
    auto monitor = _monitors[setName].lock();
    if (monitor) {
        uassertNotMixingSSL(monitor->getOriginalUri().getSSLMode(), uri.getSSLMode());
        return monitor;
    }

    std::shared_ptr<ReplicaSetMonitor> newMonitor;
    if (gReplicaSetMonitorProtocol == ReplicaSetMonitorProtocol::kScanning) {
        LOGV2(4333204, "Starting Scanning ReplicaSetMonitor", "uri"_attr = uri.toString());
        newMonitor = std::make_shared<ScanningReplicaSetMonitor>(uri);
        newMonitor->init();
    } else {
        LOGV2(4333205, "Starting Streamable ReplicaSetMonitor", "uri"_attr = uri.toString());
        newMonitor = StreamableReplicaSetMonitor::make(uri, getExecutor());
    }
    _monitors[setName] = newMonitor;
    return newMonitor;
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getMonitorForHost(const HostAndPort& host) {
    stdx::lock_guard<Latch> lk(_mutex);

    for (auto entry : _monitors) {
        auto monitor = entry.second.lock();
        if (monitor && monitor->contains(host)) {
            return monitor;
        }
    }

    return shared_ptr<ReplicaSetMonitor>();
}

vector<string> ReplicaSetMonitorManager::getAllSetNames() {
    vector<string> allNames;

    stdx::lock_guard<Latch> lk(_mutex);

    for (const auto& entry : _monitors) {
        allNames.push_back(entry.first);
    }

    return allNames;
}

void ReplicaSetMonitorManager::removeMonitor(StringData setName) {
    stdx::lock_guard<Latch> lk(_mutex);
    ReplicaSetMonitorsMap::const_iterator it = _monitors.find(setName);
    if (it != _monitors.end()) {
        if (auto monitor = it->second.lock()) {
            monitor->drop();
        }
        _monitors.erase(it);
        LOGV2(
            20187, "Removed ReplicaSetMonitor for replica set {setName}", "setName"_attr = setName);
    }
}

void ReplicaSetMonitorManager::shutdown() {
    decltype(_monitors) monitors;
    decltype(_taskExecutor) taskExecutor;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (std::exchange(_isShutdown, true)) {
            return;
        }

        monitors = std::exchange(_monitors, {});
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

std::shared_ptr<executor::TaskExecutor> ReplicaSetMonitorManager::getExecutor() {
    invariant(_taskExecutor);
    return _taskExecutor;
}

ReplicaSetChangeNotifier& ReplicaSetMonitorManager::getNotifier() {
    return _notifier;
}

bool ReplicaSetMonitorManager::isShutdown() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _isShutdown;
}
}  // namespace mongo
