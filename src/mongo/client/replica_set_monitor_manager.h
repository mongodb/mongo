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

#include "mongo/base/counter.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/client/replica_set_monitor_stats.h"
#include "mongo/executor/egress_connection_closer.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObjBuilder;
class ConnectionString;
class ReplicaSetMonitor;
class MongoURI;

class ReplicaSetMonitorManagerNetworkConnectionHook final : public executor::NetworkConnectionHook {
public:
    ReplicaSetMonitorManagerNetworkConnectionHook() = default;
    ~ReplicaSetMonitorManagerNetworkConnectionHook() override = default;

    Status validateHost(const HostAndPort& remoteHost,
                        const BSONObj& helloRequest,
                        const executor::RemoteCommandResponse& helloReply) override;

    StatusWith<boost::optional<executor::RemoteCommandRequest>> makeRequest(
        const HostAndPort& remoteHost) override;

    Status handleReply(const HostAndPort& remoteHost,
                       executor::RemoteCommandResponse&& response) override;
};

class ReplicaSetMonitorConnectionManager : public executor::EgressConnectionCloser {
    ReplicaSetMonitorConnectionManager() = delete;

public:
    ReplicaSetMonitorConnectionManager(std::shared_ptr<executor::NetworkInterface> network)
        : _network(std::move(network)) {}

    void dropConnections(const HostAndPort& target, const Status& status) override;

    // Not supported.
    void dropConnections(const Status& status) override {
        MONGO_UNREACHABLE;
    };
    // Not supported.
    void setKeepOpen(const HostAndPort& hostAndPort, bool keepOpen) override {
        MONGO_UNREACHABLE;
    }

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
     * @param cleanupCallback will be executed when the instance of ReplicaSetMonitor is deleted.
     */
    std::shared_ptr<ReplicaSetMonitor> getMonitor(StringData setName);
    std::shared_ptr<ReplicaSetMonitor> getOrCreateMonitor(const ConnectionString& connStr,
                                                          std::function<void()> cleanupCallback);
    std::shared_ptr<ReplicaSetMonitor> getOrCreateMonitor(const MongoURI& uri,
                                                          std::function<void()> cleanupCallback);

    /**
     * Retrieves the names of all sets tracked by this manager.
     */
    std::vector<std::string> getAllSetNames() const;

    /**
     * Returns current cache size including empty items, that will be garbage
     * collected later. This is intended for tests.
     */
    size_t getNumMonitors() const;

    /**
     * Removes the specified replica set monitor from being tracked, if it exists. Otherwise
     * does nothing. Once all shared_ptr references to that monitor are released, the monitor
     * will be destroyed and will no longer be tracked.
     */
    void removeMonitor(StringData setName);

    /**
     * Adds the 'setName' to the garbage collect queue for later cleanup.
     * The 2-step GC is implemented to avoid deadlocks.
     */
    void registerForGarbageCollection(StringData setName);

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

    void installMonitor_forTests(std::shared_ptr<ReplicaSetMonitor> monitor);

    /**
     *  Returns garbage collected monitors count for tests.
     */
    size_t getGarbageCollectedMonitorsCount() const {
        return _monitorsGarbageCollected.get();
    }

private:
    /**
     * Returns an EgressConnectionCloser controlling the executor's network interface.
     */
    std::shared_ptr<executor::EgressConnectionCloser> _getConnectionManager();

    using ReplicaSetMonitorsMap = StringMap<std::weak_ptr<ReplicaSetMonitor>>;

    // Protects access to the replica set monitors and several fields.
    mutable stdx::mutex _mutex;

    // Fields guarded by _mutex:

    // Executor for monitoring replica sets.
    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    // Allows closing connections established by the network interface associated with the
    // _taskExecutor instance
    std::shared_ptr<ReplicaSetMonitorConnectionManager> _connectionManager;

    // Widget to notify listeners when a RSM notices a change.
    ReplicaSetChangeNotifier _notifier;

    // Needs to be after `_taskExecutor`, so that it will be destroyed before the `_taskExecutor`.
    ReplicaSetMonitorsMap _monitors;

    int _numMonitorsCreated;

    void _setupTaskExecutorAndStats(WithLock);

    // Set to true when shutdown has been called.
    bool _isShutdown{false};

    // Leaf lvl 1 mutex guarding the pending garbage collection.
    // It is necessary to avoid deadlock while invoking the 'registerForGarbageCollection()' while
    // already holding any lvl 2-6 mutex up the stack. The 'registerForGarbageCollection()' method
    // is not locking the lvl 6 _mutex above.
    mutable stdx::mutex _gcMutex;

    // Fields guarded by _gcMutex.

    std::deque<std::string> _gcQueue;

    // Removes the already deleted monitors pending in '_gcQueue' from the internal map.
    // Do nothing if the queue is empty.
    // This requires the parent lvl 6 _mutex to be already locked.
    void _doGarbageCollectionLocked(WithLock);

    // Used for tests.
    Counter64 _monitorsGarbageCollected;

    // Pointee is internally synchronized.
    const std::shared_ptr<ReplicaSetMonitorManagerStats> _stats =
        std::make_shared<ReplicaSetMonitorManagerStats>();
};

}  // namespace mongo
