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

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/client/replica_set_monitor_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
/**
 * An abstract class, defines the external interface for static ReplicaSetMonitor methods and
 * provides a means to refresh the local view.
 * A ReplicaSetMonitor holds a state about the replica set and provides a means to refresh the local
 * view. All methods perform the required synchronization to allow callers from multiple threads.
 */
class ReplicaSetMonitor : public ReplicaSetMonitorInterface {
public:
    virtual ~ReplicaSetMonitor() = default;

    /**
     * Creates a new ReplicaSetMonitor, if it doesn't already exist.
     */
    static std::shared_ptr<ReplicaSetMonitor> createIfNeeded(const std::string& name,
                                                             const std::set<HostAndPort>& servers);

    static std::shared_ptr<ReplicaSetMonitor> createIfNeeded(const MongoURI& uri);

    /**
     * gets a cached Monitor per name. If the monitor is not found and createFromSeed is false,
     * it will return none. If createFromSeed is true, it will try to look up the last known
     * servers list for this set and will create a new monitor using that as the seed list.
     */
    static std::shared_ptr<ReplicaSetMonitor> get(const std::string& name);

    /**
     * Removes the ReplicaSetMonitor for the given set name from _sets, which will delete it.
     * If clearSeedCache is true, then the cached seed std::string for this Replica Set will be
     * removed from _seedServers.
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
};

}  // namespace mongo
