/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
