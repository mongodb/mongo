// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <utility>

namespace mongo::exec {

// Flattened snapshot of one (consumer x engine) cell: outcome counters + latency totals. Cells are
// process-global singletons, so tests compare deltas against a before-snapshot rather than absolute
// values.
struct CellSnapshot {
    int64_t found = 0;
    int64_t notFound = 0;
    int64_t notHandled = 0;
    uint64_t latencyCount = 0;
    int64_t latencySum = 0;
};

// Reads the given histogram's count/sum, defaulting to zero if it has no data yet.
inline std::pair<uint64_t, int64_t> readHistogramOrZero(otel::metrics::OtelMetricsCapturer& c,
                                                        otel::metrics::MetricName name) {
    try {
        auto lat = c.readInt64Histogram(name);
        return {lat.count, lat.sum};
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::KeyNotFound) {
            return {0, 0};
        }
        throw;
    }
}

// Returns the current counters and latency totals for the given cell. If the cell has not been
// registered yet (lazy init hasn't fired), returns an all-zero snapshot so before/after delta
// assertions work correctly regardless of test order.
inline CellSnapshot snapshotExpressCell(otel::metrics::OtelMetricsCapturer& c) {
    using otel::metrics::MetricNames;
    CellSnapshot snap;
    snap.found = c.readInt64Counter(MetricNames::kChangeStreamUpdateLookupExpressFound);
    snap.notFound = c.readInt64Counter(MetricNames::kChangeStreamUpdateLookupExpressNotFound);
    snap.notHandled = c.readInt64Counter(MetricNames::kChangeStreamUpdateLookupExpressNotHandled);
    std::tie(snap.latencyCount, snap.latencySum) =
        readHistogramOrZero(c, MetricNames::kChangeStreamUpdateLookupExpressLatency);
    return snap;
}

inline CellSnapshot snapshotAggregationCell(otel::metrics::OtelMetricsCapturer& c) {
    using otel::metrics::MetricNames;
    CellSnapshot snap;
    snap.found = c.readInt64Counter(MetricNames::kChangeStreamUpdateLookupAggregationFound);
    snap.notFound = c.readInt64Counter(MetricNames::kChangeStreamUpdateLookupAggregationNotFound);
    snap.notHandled =
        c.readInt64Counter(MetricNames::kChangeStreamUpdateLookupAggregationNotHandled);
    std::tie(snap.latencyCount, snap.latencySum) =
        readHistogramOrZero(c, MetricNames::kChangeStreamUpdateLookupAggregationLatency);
    return snap;
}

inline CellSnapshot snapshotSbeCell(otel::metrics::OtelMetricsCapturer& c) {
    using otel::metrics::MetricNames;
    CellSnapshot snap;
    snap.found = c.readInt64Counter(MetricNames::kChangeStreamUpdateLookupSbeFound);
    snap.notFound = c.readInt64Counter(MetricNames::kChangeStreamUpdateLookupSbeNotFound);
    snap.notHandled = c.readInt64Counter(MetricNames::kChangeStreamUpdateLookupSbeNotHandled);
    std::tie(snap.latencyCount, snap.latencySum) =
        readHistogramOrZero(c, MetricNames::kChangeStreamUpdateLookupSbeLatency);
    return snap;
}

}  // namespace mongo::exec
