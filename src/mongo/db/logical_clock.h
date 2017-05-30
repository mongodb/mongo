/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include "mongo/db/logical_time.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
class ServiceContext;
class OperationContext;

/**
 * LogicalClock maintain the clusterTime for a clusterNode. Every cluster node in a replica set has
 * an instance of the LogicalClock installed as a ServiceContext decoration.
 */
class LogicalClock {
public:
    // Decorate ServiceContext with LogicalClock instance.
    static LogicalClock* get(ServiceContext* service);
    static LogicalClock* get(OperationContext* ctx);
    static void set(ServiceContext* service, std::unique_ptr<LogicalClock> logicalClock);

    static constexpr Seconds kMaxAcceptableLogicalClockDrift =
        Seconds(365 * 24 * 60 * 60);  // 1 year

    /**
     *  Creates an instance of LogicalClock.
     */
    LogicalClock(ServiceContext*);

    /**
     * The method sets current time to newTime if the newTime > current time and it passes the rate
     * check.
     *
     * Returns an error if the newTime does not pass the rate check.
     */
    Status advanceClusterTime(const LogicalTime newTime);

    /**
     * Returns the current clusterTime.
     */
    LogicalTime getClusterTime();

    /**
     * Returns the next clusterTime value and provides a guarantee that any future call to
     * reserveTicks() will return a value at least 'nTicks' ticks in the future from the current
     * clusterTime.
     */
    LogicalTime reserveTicks(uint64_t nTicks);

    /**
     * Resets current time to newTime. Should only be used for initializing this clock from an
     * oplog timestamp.
     */
    void setClusterTimeFromTrustedSource(LogicalTime newTime);

private:
    /**
     * Rate limiter for advancing logical time. Rejects newTime if its seconds value is more than
     * kMaxAcceptableLogicalClockDrift seconds ahead of this node's wall clock.
     */
    Status _passesRateLimiter_inlock(LogicalTime newTime);

    ServiceContext* const _service;

    // The mutex protects _clusterTime.
    stdx::mutex _mutex;
    LogicalTime _clusterTime;
};

}  // namespace mongo
