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
#include "mongo/client/replica_set_monitor.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
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

ReplicaSetMonitorManager::~ReplicaSetMonitorManager() = default;

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getMonitor(StringData setName) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    return mapFindWithDefault(_monitors, setName, shared_ptr<ReplicaSetMonitor>());
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getOrCreateMonitor(
    const ConnectionString& connStr) {
    invariant(connStr.type() == ConnectionString::SET);

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (!_taskExecutor) {
        // construct task executor
        auto net = executor::makeNetworkInterface("ReplicaSetMonitor-TaskExecutor");
        auto netPtr = net.get();
        _taskExecutor = stdx::make_unique<ThreadPoolTaskExecutor>(
            stdx::make_unique<NetworkInterfaceThreadPool>(netPtr), std::move(net));
        LOG(1) << "Starting up task executor for monitoring replica sets in response to request to "
                  "monitor set: "
               << connStr.toString();
        _taskExecutor->startup();
    }

    shared_ptr<ReplicaSetMonitor>& monitor = _monitors[connStr.getSetName()];
    if (!monitor) {
        const std::set<HostAndPort> servers(connStr.getServers().begin(),
                                            connStr.getServers().end());

        log() << "Starting new replica set monitor for " << connStr.toString();

        monitor = std::make_shared<ReplicaSetMonitor>(connStr.getSetName(), servers);
        monitor->init();
    }

    return monitor;
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
        _monitors.erase(it);
    }
}

void ReplicaSetMonitorManager::removeAllMonitors() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    // Reset the _monitors map, which will release all registered monitors
    _monitors = ReplicaSetMonitorsMap();

    if (_taskExecutor) {
        LOG(1) << "Shutting down task executor used for monitoring replica sets";
        _taskExecutor->shutdown();
        _taskExecutor->join();
        _taskExecutor.reset();
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
