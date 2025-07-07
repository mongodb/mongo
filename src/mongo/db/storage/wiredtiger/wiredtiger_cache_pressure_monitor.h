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

#pragma once

#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/util/clock_source.h"

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
    bool isUnderCachePressure(StatsCollectionPermit& permit,
                              int concurrentWriteOuts,
                              int concurrentReadOuts);

private:
    /**
     *  This function calculates cache pressure related statistics for WiredTiger over a window,
     *  and determines whether eviction stall thresholds are exceeded based on those metrics.
     */
    bool _windowTrackingStorageAppWaitTimeAndWriteLoad(WiredTigerSession& session,
                                                       int concurrentWriteOuts,
                                                       int concurrentReadOuts);

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
