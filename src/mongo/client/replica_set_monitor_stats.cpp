// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/replica_set_monitor_stats.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/observable_mutex_registry.h"

namespace mongo {

constexpr Microseconds ReplicaSetMonitorManagerStats::kMaxLatencyDefaultResetTimeout;

static constexpr char kRSMPrefix[] = "replicaSetMonitor";
static constexpr char kGetHostPrefix[] = "getHostAndRefresh";
static constexpr char kHelloPrefix[] = "hello";

ReplicaSetMonitorManagerStats::ReplicaSetMonitorManagerStats(Microseconds resetTimeout)
    : _resetTimeout(resetTimeout) {
    ObservableMutexRegistry::get().add("replicaSetMonitorManagerStatsMutex", _mutex);
}

void ReplicaSetMonitorManagerStats::report(BSONObjBuilder* builder, bool forFTDC) {
    if (!forFTDC) {
        return;
    }
    BSONObjBuilder rsmStats(builder->subobjStart(kRSMPrefix));
    {
        BSONObjBuilder getHostStats(rsmStats.subobjStart(kGetHostPrefix));
        getHostStats.appendNumber("totalCalls", _getHostAndRefreshTotal.get());
        getHostStats.appendNumber("currentlyActive", _getHostAndRefreshCurrent.get());
        getHostStats.appendNumber("totalLatencyMicros", _getHostAndRefreshAggregateLatency.get());

        std::lock_guard<ObservableMutex<std::mutex>> lk(_mutex);
        getHostStats.appendNumber("maxLatencyMicros",
                                  durationCount<Microseconds>(_getHostAndRefreshMaxLatency));
    }
    {
        BSONObjBuilder helloStats(rsmStats.subobjStart(kHelloPrefix));
        helloStats.appendNumber("totalCalls", _helloTotal.get());
        helloStats.appendNumber("currentlyActive", _helloCurrent.get());
        helloStats.appendNumber("totalLatencyMicros", _helloAggregateLatency.get());

        std::lock_guard<ObservableMutex<std::mutex>> lk(_mutex);
        helloStats.appendNumber("maxLatencyMicros", durationCount<Microseconds>(_helloMaxLatency));
    }
}

void ReplicaSetMonitorManagerStats::enterGetHostAndRefresh() {
    _getHostAndRefreshTotal.increment(1);
    _getHostAndRefreshCurrent.increment(1);
}

static auto kProcessLatency = [](Microseconds& currentMaxLatency,
                                 Microseconds& newLatency,
                                 Timer& resetTimer,
                                 Microseconds resetTimeout) {
    bool replace = false;
    if (newLatency > currentMaxLatency) {
        replace = true;
    }
    if (Microseconds(resetTimer.micros()) > resetTimeout) {
        replace = true;
        resetTimer.reset();
    }
    if (replace) {
        currentMaxLatency = newLatency;
    }
};

void ReplicaSetMonitorManagerStats::leaveGetHostAndRefresh(Microseconds latency) {
    _getHostAndRefreshCurrent.decrement(1);
    _getHostAndRefreshAggregateLatency.increment(latency.count());

    std::lock_guard<ObservableMutex<std::mutex>> lk(_mutex);
    kProcessLatency(_getHostAndRefreshMaxLatency,
                    latency,
                    _lastTimeGetHostAndRefreshMaxLatencyUpdated,
                    _resetTimeout);
}

void ReplicaSetMonitorManagerStats::enterHello() {
    _helloTotal.increment(1);
    _helloCurrent.increment(1);
}

void ReplicaSetMonitorManagerStats::leaveHello(Microseconds latency) {
    _helloCurrent.decrement(1);
    _helloAggregateLatency.increment(latency.count());

    std::lock_guard<ObservableMutex<std::mutex>> lk(_mutex);
    kProcessLatency(_helloMaxLatency, latency, _lastTimeHelloMaxLatencyUpdated, _resetTimeout);
}


ReplicaSetMonitorStats::ReplicaSetMonitorStats(
    std::shared_ptr<ReplicaSetMonitorManagerStats> managerStats)
    : _managerStats(std::move(managerStats)) {}

void ReplicaSetMonitorStats::_enterGetHostAndRefresh() {
    _getHostAndRefreshTotal.increment(1);
    _getHostAndRefreshCurrent.increment(1);
    _managerStats->enterGetHostAndRefresh();
}

void ReplicaSetMonitorStats::_leaveGetHostAndRefresh(const Timer& suppliedTimer) {
    const Microseconds latency = Microseconds(suppliedTimer.micros());
    _getHostAndRefreshCurrent.decrement(1);
    _getHostAndRefreshAggregateLatency.increment(latency.count());
    _managerStats->leaveGetHostAndRefresh(latency);
}

void ReplicaSetMonitorStats::_enterHello() {
    _helloTotal.increment(1);
    _helloCurrent.increment(1);
    _managerStats->enterHello();
}

void ReplicaSetMonitorStats::_leaveHello(const Timer& suppliedTimer) {
    const Microseconds latency = Microseconds(suppliedTimer.micros());
    _helloCurrent.decrement(1);
    _helloLatency.increment(latency.count());
    _managerStats->leaveHello(latency);
}

}  // namespace mongo
