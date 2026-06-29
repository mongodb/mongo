/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/util/modules.h"

#include <cstdint>

namespace MONGO_MOD_PUB mongo {

class OperationContext;

// Updates the global counter metrics from the CurOp of an OperationContext.
// This is mainly used on primaries or secondaries during requests handling.
void recordCurOpMetrics(OperationContext* opCtx);

// Similar to recordCurOpMetrics, but mainly used on secondaries to collect
// metrics during oplog application.
void recordCurOpMetricsOplogApplication(OperationContext* opCtx);

/**
 * Tracks non-ticketed execution intervals for a single aggregation command. An aggregation pipeline
 * releases its execution ticket between storage-read stages (e.g. $cursor) and in-memory stages
 * (e.g. $sort, $group). These intervals are recorded per-opCtx and flushed to global counters by
 * recordCurOpMetrics().
 */
struct AggNonTicketedIntervalTracker {
    int64_t intervalStartTick{0};    // TickSource::Tick when the current interval started
    bool hasIntervalStart{false};    // true while a non-ticketed interval is in progress
    bool hadLongInterval{false};     // true if any completed interval exceeded the threshold
    int64_t longIntervalCount{0};    // # of completed intervals that exceeded the threshold
    int64_t longIntervalTotalMs{0};  // cumulative ms in those intervals
    int64_t longIntervalMaxMs{0};    // longest single interval that exceeded the threshold

    void openInterval(int64_t startTick) {
        intervalStartTick = startTick;
        hasIntervalStart = true;
    }

    void closeInterval(int64_t elapsedMs, int64_t threshold) {
        hasIntervalStart = false;
        if (elapsedMs >= threshold) {
            longIntervalCount++;
            longIntervalTotalMs += elapsedMs;
            if (elapsedMs > longIntervalMaxMs)
                longIntervalMaxMs = elapsedMs;
            hadLongInterval = true;
        }
    }
};

// Returns a reference to the per-opCtx non-ticketed interval tracker.
AggNonTicketedIntervalTracker& getAggNonTicketedIntervalTracker(OperationContext* opCtx);

// Returns the configured threshold (ms) for a "long" non-ticketed interval, reusing the
// execution-control delinquency parameter for consistency.
int64_t aggNonTicketedIntervalThresholdMillis();

// Closes the tracker's current interval if one is open, recording it against the threshold.
// Called from both DSCatalogResourceHandleBase::acquire() (mid-command re-acquisition) and
// _flushAggNonTicketedStats() (command end).
void closeAggNonTicketedIntervalIfOpen(AggNonTicketedIntervalTracker& tracker,
                                       OperationContext* opCtx);

}  // namespace MONGO_MOD_PUB mongo
