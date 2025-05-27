/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace mongo {

// Common and aggregate stats for all RSMs.
class ReplicaSetMonitorManagerStats {
public:
    static constexpr Microseconds kMaxLatencyDefaultResetTimeout{1 * 1000 * 1000};

    explicit ReplicaSetMonitorManagerStats(
        Microseconds resetTimeout = kMaxLatencyDefaultResetTimeout);

    /**
     * Reports information about the replica sets tracked by us, for diagnostic purposes.
     */
    void report(BSONObjBuilder* builder, bool forFTDC);

    void enterGetHostAndRefresh();
    void leaveGetHostAndRefresh(Microseconds latency);

    void enterHello();
    void leaveHello(Microseconds latency);

private:
    const Microseconds _resetTimeout;

    mutable stdx::mutex _mutex;

    // Stats for the outer loop of getHostAndRefresh().

    Counter64 _getHostAndRefreshTotal;
    Counter64 _getHostAndRefreshCurrent;
    Counter64 _getHostAndRefreshAggregateLatency;

    Timer _lastTimeGetHostAndRefreshMaxLatencyUpdated;
    // Keeps track of largest individual RSM latency registered during last time period.
    Microseconds _getHostAndRefreshMaxLatency;

    Counter64 _helloTotal;
    Counter64 _helloCurrent;
    Counter64 _helloAggregateLatency;

    Timer _lastTimeHelloMaxLatencyUpdated;
    Microseconds _helloMaxLatency;
};

// Stats for an RSM.
class ReplicaSetMonitorStats : public std::enable_shared_from_this<ReplicaSetMonitorStats> {
public:
    explicit ReplicaSetMonitorStats(std::shared_ptr<ReplicaSetMonitorManagerStats> managerStats);

    /**
     * Returns a scope guard class instance to collect statistics.
     * Invokes 'completionCallback' on leaving the scope.
     * Callbacks arg: calculated latency.
     */
    auto collectGetHostAndRefreshStats() {
        _enterGetHostAndRefresh();
        return ScopeGuard<std::function<void()>>(
            [self = shared_from_this(), timer = std::make_shared<Timer>()] {
                self->_leaveGetHostAndRefresh(*timer);
            });
    }

    auto collectHelloStats() {
        _enterHello();
        std::function<void()> func = [self = shared_from_this(),
                                      timer = std::make_shared<Timer>()] {
            self->_leaveHello(*timer);
        };
        return std::make_shared<ScopeGuard<std::function<void()>>>(std::move(func));
    }

private:
    void _enterGetHostAndRefresh();
    void _leaveGetHostAndRefresh(const Timer& suppliedTimer);

    void _enterHello();
    void _leaveHello(const Timer& suppliedTimer);

    // Parent stats.
    std::shared_ptr<ReplicaSetMonitorManagerStats> _managerStats;

    // Stats for the outer loop of getHostAndRefresh().

    Counter64 _getHostAndRefreshTotal;
    Counter64 _getHostAndRefreshCurrent;
    Counter64 _getHostAndRefreshAggregateLatency;

    Counter64 _helloTotal;
    Counter64 _helloCurrent;
    Counter64 _helloLatency;
};

}  // namespace mongo
