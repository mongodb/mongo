// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/replica_set_monitor.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/connpool.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo {

using std::set;
using std::shared_ptr;
using std::string;
using std::vector;

// Failpoint for changing the default refresh period
MONGO_FAIL_POINT_DEFINE(modifyReplicaSetMonitorDefaultRefreshPeriod);

ReplicaSetMonitor::ReplicaSetMonitor(const std::function<void()> cleanupCallback)
    : _cleanupCallback(cleanupCallback) {}

ReplicaSetMonitor::~ReplicaSetMonitor() {
    if (_cleanupCallback) {
        _cleanupCallback();
    }
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitor::createIfNeeded(const string& name,
                                                                const set<HostAndPort>& servers) {
    return ReplicaSetMonitorManager::get()->getOrCreateMonitor(
        ConnectionString::forReplicaSet(name, vector<HostAndPort>(servers.begin(), servers.end())),
        _getCleanupCallback(name));
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitor::createIfNeeded(const MongoURI& uri) {
    const auto& setName = uri.getSetName();
    auto cleanupCb = !setName.empty() ? _getCleanupCallback(setName) : std::function<void()>{};
    return ReplicaSetMonitorManager::get()->getOrCreateMonitor(uri, cleanupCb);
}

shared_ptr<ReplicaSetMonitor> ReplicaSetMonitor::get(const std::string& name) {
    return ReplicaSetMonitorManager::get()->getMonitor(name);
}

void ReplicaSetMonitor::remove(const string& name) {
    ReplicaSetMonitorManager::get()->removeMonitor(name);

    // Kill all pooled ReplicaSetConnections for this set. They will not function correctly
    // after we kill the ReplicaSetMonitor.
    globalConnPool.removeHost(name);
}

ReplicaSetChangeNotifier& ReplicaSetMonitor::getNotifier() {
    return ReplicaSetMonitorManager::get()->getNotifier();
}

void ReplicaSetMonitor::shutdown() {
    ReplicaSetMonitorManager::get()->shutdown();
}

void ReplicaSetMonitor::cleanup() {
    ReplicaSetMonitorManager::get()->removeAllMonitors();
}

std::function<void()> ReplicaSetMonitor::_getCleanupCallback(std::string_view name) {
    return [n = std::string{name}] {
        LOGV2(5046701, "ReplicaSetMonitor cleanup callback invoked", "name"_attr = n);
        // This callback should never invoke ReplicaSetMonitorManager::removeMonitor() because it's
        // a race: the RSM stored in ReplicaSetMonitorManager could be a new one. However, we can
        // safely garbage collect RSM for the 'name'.
        ReplicaSetMonitorManager::get()->registerForGarbageCollection(n);
        // We need to cleanup the global connection pool for the 'name', as those connections will
        // not function properly.
        globalConnPool.removeHost(n);
    };
}

}  // namespace mongo
