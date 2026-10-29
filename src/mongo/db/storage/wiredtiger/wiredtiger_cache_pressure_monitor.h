// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/modules.h"

namespace mongo {

class WiredTigerKVEngine;

/**
 * This class monitors the cache pressure conditions in WiredTiger.
 */
class WiredTigerCachePressureMonitor {
public:
    explicit WiredTigerCachePressureMonitor(WiredTigerKVEngine& engine,
                                            ClockSource* clockSource,
                                            double evictionDirtyTriggerMB,
                                            double evictionUpdatesTriggerMB);
    /**
     * Updates cache pressure metrics and determines whether the system is under cache pressure.
     */
    bool isUnderCachePressure(StatsCollectionPermit& permit, int concurrentOpOuts);

private:
    /**
     *  This function calculates cache pressure related statistics for WiredTiger over a window,
     *  and determines whether eviction stall thresholds are exceeded based on those metrics.
     */
    bool _windowTrackingStorageAppWaitTimeAndWriteLoad(WiredTigerSession& session,
                                                       int concurrentOpOuts);

    /**
     * Tracks WiredTiger's dirty and updates cache ratios to detect eviction thresholds.
     */
    bool _trackCacheRatioEvictionTrigger(WiredTigerSession& session);

    // Tracks the state of statistics relevant to cache pressure. These only make sense as deltas
    // against the previous value.
    struct CachePressureStats {
        int64_t cacheWaitUsecs = 0;
        int64_t txnsCommittedCount = 0;
        // Record when the current stats were generated.
        int64_t timestamp = 0;
    };

    WiredTigerKVEngine& _engine;
    ClockSource* _clockSource;

    // Record the stats for use in calculating deltas between calls to underCachePressure().
    CachePressureStats _lastStats;

    // Tracks the last time we saw the cache pressure result for thread pressure was not exceeded.
    Date_t _lastGoodputObservedTimestamp;

    // Configuration values for eviction triggers.
    double _evictionDirtyTriggerMB;
    double _evictionUpdatesTriggerMB;

    // Since we are tracking the total tickets over a duration, and the tickets we hold are an
    // instantaneous value, we will use an exponentially decaying moving average to smooth out
    // noise and avoid aggressive short-term over-corrections due to short-term changes.
    double _totalTicketsEDMA = 1.0;  // Setting a default value.
};

}  // namespace mongo
