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

#pragma once

#include <string>
#include <vector>

#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/executor/egress_tag_closer.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BSONObjBuilder;
class ConnectionString;
class ReplicaSetMonitor;
class MongoURI;

class ReplicaSetMonitorManagerNetworkConnectionHook final : public executor::NetworkConnectionHook {
public:
    ReplicaSetMonitorManagerNetworkConnectionHook() = default;
    virtual ~ReplicaSetMonitorManagerNetworkConnectionHook() = default;

    Status validateHost(const HostAndPort& remoteHost,
                        const BSONObj& isMasterRequest,
                        const executor::RemoteCommandResponse& isMasterReply) override;

    StatusWith<boost::optional<executor::RemoteCommandRequest>> makeRequest(
        const HostAndPort& remoteHost) override;

    Status handleReply(const HostAndPort& remoteHost,
                       executor::RemoteCommandResponse&& response) override;
};

class ReplicaSetMonitorConnectionManager : public executor::EgressTagCloser {
    ReplicaSetMonitorConnectionManager() = delete;

public:
    ReplicaSetMonitorConnectionManager(std::shared_ptr<executor::NetworkInterface> network)
        : _network(network) {}

    void dropConnections(const HostAndPort& hostAndPort) override;

    // Not supported.
    void dropConnections(transport::Session::TagMask tags) override {
        MONGO_UNREACHABLE;
    };
    // Not supported.
    void mutateTags(const HostAndPort& hostAndPort,
                    const std::function<transport::Session::TagMask(transport::Session::TagMask)>&
                        mutateFunc) override {
        MONGO_UNREACHABLE;
    };

private:
    std::shared_ptr<executor::NetworkInterface> _network;
};

/**
 * Manages the lifetime of a set of replica set monitors.
 */
class ReplicaSetMonitorManager {
    ReplicaSetMonitorManager(const ReplicaSetMonitorManager&) = delete;
    ReplicaSetMonitorManager& operator=(const ReplicaSetMonitorManager&) = delete;

public:
    ReplicaSetMonitorManager() = default;
    ~ReplicaSetMonitorManager();

    static ReplicaSetMonitorManager* get();

    /**
     * Create or retrieve a monitor for a particular replica set. The getter method returns
     * nullptr if there is no monitor registered for the particular replica set.
     */
    std::shared_ptr<ReplicaSetMonitor> getMonitor(StringData setName);
    std::shared_ptr<ReplicaSetMonitor> getOrCreateMonitor(const ConnectionString& connStr);
    std::shared_ptr<ReplicaSetMonitor> getOrCreateMonitor(const MongoURI& uri);

    /**
     * Retrieves the names of all sets tracked by this manager.
     */
    std::vector<std::string> getAllSetNames();

    /**
     * Removes the specified replica set monitor from being tracked, if it exists. Otherwise
     * does nothing. Once all shared_ptr references to that monitor are released, the monitor
     * will be destroyed and will no longer be tracked.
     */
    void removeMonitor(StringData setName);

    std::shared_ptr<ReplicaSetMonitor> getMonitorForHost(const HostAndPort& host);

    /**
     * Removes and destroys all replica set monitors. Should be used for unit tests only.
     */
    void removeAllMonitors();

    /**
     * Shuts down _taskExecutor.
     */
    void shutdown();

    /**
     * Reports information about the replica sets tracked by us, for diagnostic purposes. If
     * forFTDC, trim to minimize its size for full-time diagnostic data capture.
     */
    void report(BSONObjBuilder* builder, bool forFTDC = false);

    /**
     * Returns an executor for running RSM tasks.
     */
    std::shared_ptr<executor::TaskExecutor> getExecutor();

    ReplicaSetChangeNotifier& getNotifier();

    bool isShutdown() const;


private:
    /**
     * Returns an EgressTagCloser controlling the executor's network interface.
     */
    std::shared_ptr<executor::EgressTagCloser> _getConnectionManager();

    using ReplicaSetMonitorsMap = StringMap<std::weak_ptr<ReplicaSetMonitor>>;

    // Protects access to the replica set monitors
    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(6), "ReplicaSetMonitorManager::_mutex");

    // Executor for monitoring replica sets.
    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    // Allows closing connections established by the network interface associated with the
    // _taskExecutor instance
    std::shared_ptr<ReplicaSetMonitorConnectionManager> _connectionManager;

    // Widget to notify listeners when a RSM notices a change
    ReplicaSetChangeNotifier _notifier;

    // Needs to be after `_taskExecutor`, so that it will be destroyed before the `_taskExecutor`.
    ReplicaSetMonitorsMap _monitors;

    void _setupTaskExecutorInLock();

    // set to true when shutdown has been called.
    bool _isShutdown{false};
};

}  // namespace mongo
