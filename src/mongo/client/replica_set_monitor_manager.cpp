
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/client.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"
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

namespace {
const int kMaxRsmThreads = 1024;
const int kMaxRsmConnectionsPerHost = 128;
}

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

auto makeThreadPool(const std::string& poolName) {
    ThreadPool::Options threadPoolOptions;
    threadPoolOptions.poolName = poolName;

    // Two threads are for the Scan and the hello request.
    threadPoolOptions.minThreads = 2;

    // This setting is a hedge against the issue described in
    // SERVER-56854. Generally an RSM instance will use 1 thread
    // to make the hello request. If there are delays in the network
    // interface delivering the replies, the RSM will timeout and
    // spawn additional threads to make progress.
    threadPoolOptions.maxThreads = kMaxRsmThreads;

    threadPoolOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return stdx::make_unique<ThreadPool>(threadPoolOptions);
}

void ReplicaSetMonitorManager::_setupTaskExecutorAndStatsInLock(const std::string& name) {
    auto hookList = stdx::make_unique<rpc::EgressMetadataHookList>();

    // do not restart taskExecutor if is in shutdown
    if (!_taskExecutor && !_isShutdown) {
        // construct task executor
        auto threadName = "ReplicaSetMonitor-TaskExecutor";

        // This is to limit the number of threads that a failed host can consume
        // when there is a TCP blackhole. The RSM will attempt to contact the host repeatly
        // spawning a new thread on each attempt if there is no response from the last attempt.
        // Eventually, the connections will timeout according to TCP keepalive settings.
        // See SERVER-56854 for more information.
        executor::ConnectionPool::Options connPoolOptions;
        connPoolOptions.maxConnections = kMaxRsmConnectionsPerHost;

        auto net = executor::makeNetworkInterface(
            threadName, nullptr, std::move(hookList), connPoolOptions);
        _taskExecutor =
            stdx::make_unique<ThreadPoolTaskExecutor>(makeThreadPool(threadName), std::move(net));
        LOG(1) << "Starting up task executor for monitoring replica sets in response to request to "
                  "monitor set: "
               << redact(name);
        _taskExecutor->startup();
    }

    if (!_stats) {
        _stats = std::make_shared<ReplicaSetMonitorManagerStats>();
    }
}

namespace {
void uassertNotMixingSSL(transport::ConnectSSLMode a, transport::ConnectSSLMode b) {
    uassert(51042, "Mixing ssl modes with a single replica set is disallowed", a == b);
}
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getOrCreateMonitor(
    const ConnectionString& connStr, ReplicaSetMonitorTransportPtr transport) {
    invariant(connStr.type() == ConnectionString::SET);

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _setupTaskExecutorAndStatsInLock(connStr.toString());
    auto setName = connStr.getSetName();
    auto monitor = _monitors[setName].lock();
    if (monitor) {
        uassertNotMixingSSL(monitor->getOriginalUri().getSSLMode(), transport::kGlobalSSLMode);
        return monitor;
    }

    const std::set<HostAndPort> servers(connStr.getServers().begin(), connStr.getServers().end());

    log() << "Starting new replica set monitor for " << connStr.toString();

    if (!transport) {
        transport = makeRsmTransport();
    }
    invariant(_stats);
    auto newMonitor =
        std::make_shared<ReplicaSetMonitor>(setName, servers, std::move(transport), _stats);
    _monitors[setName] = newMonitor;
    newMonitor->init();
    return newMonitor;
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorManager::getOrCreateMonitor(
    const MongoURI& uri, ReplicaSetMonitorTransportPtr transport) {
    invariant(uri.type() == ConnectionString::SET);

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _setupTaskExecutorAndStatsInLock(uri.toString());
    const auto& setName = uri.getSetName();
    auto monitor = _monitors[setName].lock();
    if (monitor) {
        uassertNotMixingSSL(monitor->getOriginalUri().getSSLMode(), uri.getSSLMode());
        return monitor;
    }

    log() << "Starting new replica set monitor for " << uri.toString();

    invariant(_stats);
    auto newMonitor = std::make_shared<ReplicaSetMonitor>(uri, std::move(transport), _stats);
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

void ReplicaSetMonitorManager::report(BSONObjBuilder* builder, bool forFTDC) {
    // Don't hold _mutex the whole time to avoid ever taking a monitor's mutex while holding the
    // manager's mutex.  Otherwise we could get a deadlock between the manager's, monitor's, and
    // ShardRegistry's mutex due to the ReplicaSetMonitor's AsynchronousConfigChangeHook potentially
    // calling ShardRegistry::updateConfigServerConnectionString.
    auto setNames = getAllSetNames();

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

TaskExecutor* ReplicaSetMonitorManager::getExecutor() {
    invariant(_taskExecutor);
    return _taskExecutor.get();
}


ReplicaSetMonitorTransportPtr ReplicaSetMonitorManager::makeRsmTransport() {
    return std::make_unique<ReplicaSetMonitorExecutorTransport>(getExecutor());
}
}  // namespace mongo
