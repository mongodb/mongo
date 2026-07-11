// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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

}  // namespace mongo
