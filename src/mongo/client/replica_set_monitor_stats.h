// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/counter.h"
#include "mongo/util/duration.h"
#include "mongo/util/observable_mutex.h"
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

    mutable ObservableMutex<std::mutex> _mutex;

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
