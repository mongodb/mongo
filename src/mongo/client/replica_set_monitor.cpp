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

#include "mongo/client/replica_set_monitor.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/connpool.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#include <limits>
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

std::function<void()> ReplicaSetMonitor::_getCleanupCallback(StringData name) {
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
