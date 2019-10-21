/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/util/log.h"
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

ReplicaSetMonitorManager::ReplicaSetMonitorManager() {}

ReplicaSetMonitorManager::~ReplicaSetMonitorManager() {
    shutdown();
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
    auto net = executor::makeNetworkInterface(
        "ReplicaSetMonitor-TaskExecutor", nullptr, std::move(hookList));
    auto pool = std::make_unique<NetworkInterfaceThreadPool>(net.get());
    _taskExecutor = std::make_unique<ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
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

    log() << "Starting new replica set monitor for " << uri.toString();

    auto newMonitor = std::make_shared<ReplicaSetMonitor>(uri);
    _monitors[setName] = newMonitor;
    newMonitor->init();
    return newMonitor;
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
        log() << "Removed ReplicaSetMonitor for replica set " << setName;
    }
}

void ReplicaSetMonitorManager::shutdown() {
    // Sadly, this function can run very late in the post-main shutdown because there is still
    // a globalRSMonitorManager. We have to be very carefully how we log because this can actually
    // shutdown later than the logging subsystem. This will be less of an issue once SERVER-42437 is
    // done.

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

    if (taskExecutor) {
        LOG(1) << "Shutting down task executor used for monitoring replica sets";
        taskExecutor->shutdown();
    }

    if (monitors.size()) {
        log() << "Dropping all ongoing scans against replica sets";
    }
    for (auto& [name, monitor] : monitors) {
        auto anchor = monitor.lock();
        if (!anchor) {
            continue;
        }

        anchor->drop();
    }

    if (taskExecutor) {
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
    // ShardRegistry's mutex due to the ReplicaSetMonitor's AsynchronousConfigChangeHook potentially
    // calling ShardRegistry::updateConfigServerConnectionString.
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

TaskExecutor* ReplicaSetMonitorManager::getExecutor() {
    invariant(_taskExecutor);
    return _taskExecutor.get();
}

ReplicaSetChangeNotifier& ReplicaSetMonitorManager::getNotifier() {
    return _notifier;
}

bool ReplicaSetMonitorManager::isShutdown() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _isShutdown;
}

}  // namespace mongo
