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

#include "mongo/base/string_data.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/client/replica_set_monitor_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <memory>
#include <set>
#include <string>

namespace mongo {
/**
 * An abstract class, defines the external interface for static ReplicaSetMonitor methods and
 * provides a means to refresh the local view.
 * A ReplicaSetMonitor holds a state about the replica set and provides a means to refresh the local
 * view. All methods perform the required synchronization to allow callers from multiple threads.
 */
class ReplicaSetMonitor : public ReplicaSetMonitorInterface {
public:
    ~ReplicaSetMonitor() override;

    /**
     * Creates a new ReplicaSetMonitor, if it doesn't already exist.
     */
    static std::shared_ptr<ReplicaSetMonitor> createIfNeeded(const std::string& name,
                                                             const std::set<HostAndPort>& servers);

    static std::shared_ptr<ReplicaSetMonitor> createIfNeeded(const MongoURI& uri);

    /**
     * gets a cached Monitor per name. The getter method returns nullptr if there is no monitor
     * registered for the particular replica set.
     */
    static std::shared_ptr<ReplicaSetMonitor> get(const std::string& name);

    /**
     * Removes the ReplicaSetMonitor for the given set name from ReplicaSetMonitorManager.
     * Drop and remove the ReplicaSetMonitor for the given set name if it exists.
     * Then all connections for this host are deleted from the connection pool DBConnectionPool.
     * Those two steps are not performed atomically together, but the possible (unlikely) race:
     *  1. RSM is dropped and removed
     *  2. Another RSM is created for the same name
     *  3. Pooled connections are cleared
     * is not creating any incorrectness, it is only inefficient.
     */
    static void remove(const std::string& name);

    /**
     * Returns the change notifier for the underlying ReplicaMonitorManager
     */
    static ReplicaSetChangeNotifier& getNotifier();

    /**
     * Permanently stops all monitoring on replica sets and clears all cached information
     * as well. As a consequence, NEVER call this if you have other threads that have a
     * DBClientReplicaSet instance. This method should be used for unit test only.
     */
    static void cleanup();

    /**
     * Permanently stops all monitoring on replica sets.
     */
    static void shutdown();

protected:
    explicit ReplicaSetMonitor(std::function<void()> cleanupCallback);

private:
    /**
     * @return callback helper to safely cleanup 'ReplicaSetMonitor' and 'globalConnPool' when the
     * instance of ReplicaSetMonitor for the 'name' is being destroyed.
     */
    static std::function<void()> _getCleanupCallback(StringData name);

    const std::function<void()> _cleanupCallback;
};

}  // namespace mongo
