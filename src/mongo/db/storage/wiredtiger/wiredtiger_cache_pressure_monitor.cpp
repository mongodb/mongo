/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_cache_pressure_monitor.h"

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/util/system_tick_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

auto getStatisticValue(WiredTigerSession& session, int statKey) {
    auto statResult =
        WiredTigerUtil::getStatisticsValue(session, "statistics:", "statistics=(fast)", statKey);
    uassertStatusOK(statResult.getStatus());
    return statResult.getValue();
}

auto& cachePressureObservedCacheWaitTime =
    *MetricBuilder<Atomic64Metric>("cachePressure.observedCacheWaitTime");

auto& cachePressureWaitTimeThreshold =
    *MetricBuilder<Atomic64Metric>("cachePressure.waitTimeThreshold");

auto& cachePressureWaitTimeThresholdExceeded =
    *MetricBuilder<Atomic64Metric>("cachePressure.waitTimeThresholdExceeded");

auto& cachePressureCacheUpdatesThreshold =
    *MetricBuilder<Atomic64Metric>("cachePressure.cacheUpdatesThreshold");

auto& cachePressureCacheDirtyThreshold =
    *MetricBuilder<Atomic64Metric>("cachePressure.cacheDirtyThreshold");


WiredTigerCachePressureMonitor::WiredTigerCachePressureMonitor(WiredTigerKVEngine& engine,
                                                               ClockSource* clockSource,
                                                               double evictionDirtyTriggerMB,
                                                               double evictionUpdatesTriggerMB)
    : _engine(engine),
      _clockSource(clockSource),
      _lastStats{},
      _lastGoodputObservedTimestamp(clockSource->now()),
      _evictionDirtyTriggerMB(evictionDirtyTriggerMB),
      _evictionUpdatesTriggerMB(evictionUpdatesTriggerMB),
      _totalTicketsEDMA(1.0) {}

bool WiredTigerCachePressureMonitor::isUnderCachePressure(StatsCollectionPermit& permit,
                                                          int concurrentWriteOuts,
                                                          int concurrentReadOuts) {

    // Get a permit to access the WiredTiger Statistics.
    WiredTigerSession session(&_engine.getConnection(), permit);

    // Perform cache pressure checks.
    bool threadPressureResult = _windowTrackingStorageAppWaitTimeAndWriteLoad(
        session, concurrentWriteOuts, concurrentReadOuts);
    bool cacheRatioResult = _trackCacheRatioEvictionTrigger(session);

    LOGV2_DEBUG(10181703,
                2,
                "Cache pressure calculation statistics -- results",
                "threadPressureResult"_attr = threadPressureResult,
                "cacheRatioResult"_attr = cacheRatioResult);

    return threadPressureResult && cacheRatioResult;
}


bool WiredTigerCachePressureMonitor::_windowTrackingStorageAppWaitTimeAndWriteLoad(
    WiredTigerSession& session, int concurrentWriteOuts, int concurrentReadOuts) {
    CachePressureStats currentStats{
        .cacheWaitUsecs = getStatisticValue(session, WT_STAT_CONN_APPLICATION_CACHE_TIME),
        .txnsCommittedCount = getStatisticValue(session, WT_STAT_CONN_TXN_COMMIT),
        .timestamp = globalSystemTickSource()->getTicks(),
    };

    // These metrics are cumulative so we will calculate based on their last seen value.
    int64_t cacheWaitTimeUsecs = currentStats.cacheWaitUsecs - _lastStats.cacheWaitUsecs;
    int64_t txnsCommittedCount = currentStats.txnsCommittedCount - _lastStats.txnsCommittedCount;
    int64_t usBetweenStats = (currentStats.timestamp - _lastStats.timestamp) /
        (globalSystemTickSource()->getTicksPerSecond() / 1000000);

    bool isFirstIteration = (_lastStats.timestamp == 0);
    _lastStats = currentStats;

    // If this is the first iteration, we return false.
    if (isFirstIteration) {
        return false;
    }

    // Compute the exponentially decaying moving average from the current tickets.
    int64_t currentTickets = concurrentReadOuts + concurrentWriteOuts;
    double edmaAlpha = gCachePressureExponentiallyDecayingMovingAverageAlphaValue.load();
    _totalTicketsEDMA = (edmaAlpha * currentTickets) + ((1.0 - edmaAlpha) * _totalTicketsEDMA);

    int64_t totalTickets =
        std::max(static_cast<int64_t>(std::round(_totalTicketsEDMA)), int64_t(1));

    int64_t expectedThreadTime = totalTickets * usBetweenStats;
    int64_t waitTimeThreshold =
        gCachePressureEvictionStallThresholdProportion.load() * expectedThreadTime;
    bool perThreadWaitTimeExceedsThreshold = cacheWaitTimeUsecs > waitTimeThreshold;

    bool notEnoughTransactionsCommitted = (txnsCommittedCount == 0);

    cachePressureObservedCacheWaitTime.set(cacheWaitTimeUsecs);
    cachePressureWaitTimeThreshold.set(waitTimeThreshold);
    cachePressureWaitTimeThresholdExceeded.set(perThreadWaitTimeExceedsThreshold);

    LOGV2_DEBUG(10181701,
                2,
                "Cache pressure calculation statistics -- wait time",
                "cache wait time average"_attr = cacheWaitTimeUsecs,
                "total transactions committed"_attr = txnsCommittedCount,
                "total tickets"_attr = totalTickets,
                "wait time threshold"_attr = waitTimeThreshold,
                "wait time threshold exceeded"_attr = perThreadWaitTimeExceedsThreshold);

    if (!perThreadWaitTimeExceedsThreshold || !notEnoughTransactionsCommitted) {
        _lastGoodputObservedTimestamp = _clockSource->now();
        return false;
    }

    // Once the metrics are true over configured window, we have met the threshold. The window
    // will help us achieve more consistent results.
    if ((_clockSource->now() - _lastGoodputObservedTimestamp) >
        Seconds(gCachePressureEvictionStallDetectionWindowSeconds.load())) {
        return true;
    }

    return false;
}

bool WiredTigerCachePressureMonitor::_trackCacheRatioEvictionTrigger(WiredTigerSession& session) {
    auto cacheBytesUpdates = getStatisticValue(session, WT_STAT_CONN_CACHE_BYTES_UPDATES);
    auto cacheBytesDirty = getStatisticValue(session, WT_STAT_CONN_CACHE_BYTES_DIRTY);
    double cacheBytesMax = getStatisticValue(session, WT_STAT_CONN_CACHE_BYTES_MAX);

    double evictDirtyTrigger =
        _evictionDirtyTriggerMB ? (_evictionDirtyTriggerMB * 1024 * 1024) / cacheBytesMax : 0.20;

    double evictUpdatesTrigger = _evictionUpdatesTriggerMB
        ? (_evictionUpdatesTriggerMB * 1024 * 1024) / cacheBytesMax
        : evictDirtyTrigger / 2.0;

    // Check to see if the cache has reached unhealthy update and dirty cache ratios.
    bool cacheUpdatesThresholdExceeded = (cacheBytesUpdates / cacheBytesMax > evictUpdatesTrigger);
    bool cacheDirtyThresholdExceeded = (cacheBytesDirty / cacheBytesMax > evictDirtyTrigger);

    cachePressureCacheUpdatesThreshold.set(cacheUpdatesThresholdExceeded);
    cachePressureCacheDirtyThreshold.set(cacheDirtyThresholdExceeded);

    LOGV2_DEBUG(10181702,
                2,
                "Cache pressure calculation statistics -- cache load",
                "cacheBytesUpdates"_attr = cacheBytesUpdates,
                "cacheBytesDirty"_attr = cacheBytesDirty,
                "cacheBytesMax"_attr = cacheBytesMax,
                "dirty threshold exceeded"_attr = cacheDirtyThresholdExceeded,
                "updates threshold exceeded"_attr = cacheUpdatesThresholdExceeded);

    return cacheUpdatesThresholdExceeded || cacheDirtyThresholdExceeded;
}

}  // namespace mongo
