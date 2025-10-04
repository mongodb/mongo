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

#include "mongo/client/replica_set_monitor_stats.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

constexpr Microseconds ReplicaSetMonitorManagerStats::kMaxLatencyDefaultResetTimeout;

static constexpr char kRSMPrefix[] = "replicaSetMonitor";
static constexpr char kGetHostPrefix[] = "getHostAndRefresh";
static constexpr char kHelloPrefix[] = "hello";

ReplicaSetMonitorManagerStats::ReplicaSetMonitorManagerStats(Microseconds resetTimeout)
    : _resetTimeout(resetTimeout) {}

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

        stdx::lock_guard<stdx::mutex> lk(_mutex);
        getHostStats.appendNumber("maxLatencyMicros",
                                  durationCount<Microseconds>(_getHostAndRefreshMaxLatency));
    }
    {
        BSONObjBuilder helloStats(rsmStats.subobjStart(kHelloPrefix));
        helloStats.appendNumber("totalCalls", _helloTotal.get());
        helloStats.appendNumber("currentlyActive", _helloCurrent.get());
        helloStats.appendNumber("totalLatencyMicros", _helloAggregateLatency.get());

        stdx::lock_guard<stdx::mutex> lk(_mutex);
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

    stdx::lock_guard<stdx::mutex> lk(_mutex);
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

    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
