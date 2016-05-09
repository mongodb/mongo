/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/timer.h"

namespace mongo {

class OperationContext;

/**
 * Utility class, which is used to periodically record in the config server's metadata the mongos
 * instances, which are connected to the given config server and their uptime.
 *
 * NOTE: Not thread-safe, so it should not be used from more than one thread at a time.
 */
class ShardingUptimeReporter {
    MONGO_DISALLOW_COPYING(ShardingUptimeReporter);

public:
    ShardingUptimeReporter();
    ~ShardingUptimeReporter();

    /**
     * Optional call, which would start a thread to periodically invoke reportStatus.
     */
    void startPeriodicThread();

    /**
     * Returns the generated instance id string for reporting purposes.
     */
    const std::string& getInstanceId() const {
        return _instanceId;
    }

    /**
     * Reports the uptime status of the current instance to the config.pings collection. This method
     * is best-effort and never throws.
     *
     * isBalancerActive indicates to external balancer control scripts whether the sharding balancer
     *  is active or not.
     */
    void reportStatus(OperationContext* txn, bool isBalancerActive) const;

private:
    // String containing the hostname and the port of the server, which is running this reporter.
    // Initialized at startup time.
    const std::string _instanceId;

    // Time the reporter started running
    const Timer _timer;

    // The background uptime reporter thread (if started)
    stdx::thread _thread;
};

}  // namespace mongo
