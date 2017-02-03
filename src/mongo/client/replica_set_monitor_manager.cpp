/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/map_util.h"

namespace mongo {

using std::shared_ptr;
using std::set;
using std::string;
using std::vector;

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutorPool;
using executor::TaskExecutor;
using executor::ThreadPoolTaskExecutor;

ReplicaSetMonitorManager::ReplicaSetMonitorManager() {}

ReplicaSetMonitorManager::~ReplicaSetMonitorManager() {
    shutdown();
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getMonitor(StringData setName) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (auto monitor = _monitors[setName].lock()) {
        return monitor;
    } else {
        return shared_ptr<ReplicaSetMonitor>();
    }
}

void ReplicaSetMonitorManager::_setupTaskExecutorInLock(const std::string& name) {
    auto hookList = stdx::make_unique<rpc::EgressMetadataHookList>();
    // TODO SERVER-27750: add LogicalTimeMetadataHook

    // do not restart taskExecutor if is in shutdown
    if (!_taskExecutor && !_isShutdown) {
        // construct task executor
        auto net = executor::makeNetworkInterface(
            "ReplicaSetMonitor-TaskExecutor", nullptr, std::move(hookList));
        auto netPtr = net.get();
        _taskExecutor = stdx::make_unique<ThreadPoolTaskExecutor>(
            stdx::make_unique<NetworkInterfaceThreadPool>(netPtr), std::move(net));
        LOG(1) << "Starting up task executor for monitoring replica sets in response to request to "
                  "monitor set: "
               << redact(name);
        _taskExecutor->startup();
    }
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getOrCreateMonitor(
    const ConnectionString& connStr) {
    invariant(connStr.type() == ConnectionString::SET);

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _setupTaskExecutorInLock(connStr.toString());
    auto setName = connStr.getSetName();
    auto monitor = _monitors[setName].lock();
    if (monitor) {
        return monitor;
    }

    const std::set<HostAndPort> servers(connStr.getServers().begin(), connStr.getServers().end());

    log() << "Starting new replica set monitor for " << connStr.toString();

    auto newMonitor = std::make_shared<ReplicaSetMonitor>(setName, servers);
    _monitors[setName] = newMonitor;
    newMonitor->init();
    return newMonitor;
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getOrCreateMonitor(const MongoURI& uri) {
    invariant(uri.type() == ConnectionString::SET);

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _setupTaskExecutorInLock(uri.toString());
    const auto& setName = uri.getSetName();
    auto monitor = _monitors[setName].lock();
    if (monitor) {
        return monitor;
    }

    log() << "Starting new replica set monitor for " << uri.toString();

    auto newMonitor = std::make_shared<ReplicaSetMonitor>(uri);
    _monitors[setName] = newMonitor;
    newMonitor->init();
    return newMonitor;
}

vector<string> ReplicaSetMonitorManager::getAllSetNames() {
    vector<string> allNames;

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    for (const auto& entry : _monitors) {
        allNames.push_back(entry.first);
    }

    return allNames;
}

void ReplicaSetMonitorManager::removeMonitor(StringData setName) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    ReplicaSetMonitorsMap::const_iterator it = _monitors.find(setName);
    if (it != _monitors.end()) {
        if (auto monitor = it->second.lock()) {
            monitor->markAsRemoved();
        }
        _monitors.erase(it);
        log() << "Removed ReplicaSetMonitor for replica set " << setName;
    }
}

void ReplicaSetMonitorManager::shutdown() {

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (!_taskExecutor || _isShutdown) {
            return;
        }
        _isShutdown = true;
    }

    LOG(1) << "Shutting down task executor used for monitoring replica sets";
    _taskExecutor->shutdown();
    _taskExecutor->join();
}

void ReplicaSetMonitorManager::removeAllMonitors() {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _monitors = ReplicaSetMonitorsMap();
        if (!_taskExecutor || _isShutdown) {
            return;
        }
        _isShutdown = true;
    }

    LOG(1) << "Shutting down task executor used for monitoring replica sets";
    _taskExecutor->shutdown();
    _taskExecutor->join();
    _taskExecutor.reset();

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _isShutdown = false;
    }
}

void ReplicaSetMonitorManager::report(BSONObjBuilder* builder) {
    // Don't hold _mutex the whole time to avoid ever taking a monitor's mutex while holding the
    // manager's mutex.  Otherwise we could get a deadlock between the manager's, monitor's, and
    // ShardRegistry's mutex due to the ReplicaSetMonitor's AsynchronousConfigChangeHook potentially
    // calling ShardRegistry::updateConfigServerConnectionString.
    auto setNames = getAllSetNames();
    for (const auto& setName : setNames) {
        auto monitor = getMonitor(setName);
        if (!monitor) {
            continue;
        }
        BSONObjBuilder monitorInfo(builder->subobjStart(setName));
        monitor->appendInfo(monitorInfo);
    }
}

TaskExecutor* ReplicaSetMonitorManager::getExecutor() {
    invariant(_taskExecutor);
    return _taskExecutor.get();
}

}  // namespace mongo
