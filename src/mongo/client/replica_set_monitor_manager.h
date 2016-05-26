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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BSONObjBuilder;
class ConnectionString;
class ReplicaSetMonitor;

/**
 * Manages the lifetime of a set of replica set monitors.
 */
class ReplicaSetMonitorManager {
    MONGO_DISALLOW_COPYING(ReplicaSetMonitorManager);

public:
    ReplicaSetMonitorManager();
    ~ReplicaSetMonitorManager();

    /**
     * Create or retrieve a monitor for a particular replica set. The getter method returns
     * nullptr if there is no monitor registered for the particular replica set.
     */
    std::shared_ptr<ReplicaSetMonitor> getMonitor(StringData setName);
    std::shared_ptr<ReplicaSetMonitor> getOrCreateMonitor(const ConnectionString& connStr);

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

    /**
     * Removes and destroys all replica set monitors.
     */
    void removeAllMonitors();

    /**
     * Reports information about the replica sets tracked by us, for diagnostic purposes.
     */
    void report(BSONObjBuilder* builder);

    /**
     * Returns an executor for running RSM tasks.
     */
    executor::TaskExecutor* getExecutor();

private:
    using ReplicaSetMonitorsMap = StringMap<std::shared_ptr<ReplicaSetMonitor>>;

    // Protects access to the replica set monitors
    stdx::mutex _mutex;
    ReplicaSetMonitorsMap _monitors;

    // Executor for monitoring replica sets.
    std::unique_ptr<executor::TaskExecutor> _taskExecutor;
};

}  // namespace mongo
