/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"

namespace mongo {

class OperationContext;
class ServiceContext;

/**
 * Periodically refreshes the cache of the balancer configuration on a primary shard.
 */
class PeriodicBalancerSettingsRefresher {
    MONGO_DISALLOW_COPYING(PeriodicBalancerSettingsRefresher);

public:
    PeriodicBalancerSettingsRefresher(bool isPrimary);
    ~PeriodicBalancerSettingsRefresher();

    /**
     * Instantiates an instance of the PeriodicBalancerSettingsRefresher and installs it on the
     * specified service context.
     *
     * This method is not thread-safe and must be called only once when the service is starting.
     */
    static void create(ServiceContext* serviceContext, bool isPrimary);

    /**
     * Retrieves the per-service instance of the PeriodicBalancerSettingsRefresher.
     */
    static PeriodicBalancerSettingsRefresher* get(ServiceContext* serviceContext);

    /**
     * Invoked when the shard server primary enters the 'PRIMARY' state to start the
     * PeriodicBalancerSettingsRefresher.
     */
    void start();

    /**
     * Invoked when this node which is currently serving as a 'PRIMARY' steps down.
     *
     * This method might be called multiple times in succession, which is what happens as a result
     * of incomplete transition to primary so it is resilient to that.
     */
    void stop();

    /**
     * Signals shutdown and blocks until the refresher thread has stopped.
     */
    void shutdown();

private:
    /**
     * The main loop that refreshes the cache periodically. This runs in a separate thread.
     */
    void _periodicRefresh();

    /**
     * Use to check whether or not shutdown has been requested for the running thread.
     */
    bool _shutDownRequested();

    // The background thread to refresh balancer settings
    stdx::thread _thread;

    // Protects the state below
    stdx::mutex _mutex;

    // The PeriodicBalancerSettingsRefresher is only active on a primary node.
    bool _isPrimary;

    // Used to shut down the background thread
    bool _isShutdown{false};
};

}  // namespace mongo
