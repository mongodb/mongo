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


#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/metrics_updown_counter.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>
namespace mongo::change_stream {

inline const otel::metrics::CounterOptions kCursorsTotalOpenedOpts = [] {
    otel::metrics::CounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.totalOpened",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return opts;
}();

// Constructs the counter for the OTEL metric "change_streams.cursor.total_opened".
inline otel::metrics::Counter<int64_t>& createCurorsTotalOpened() {
    return otel::metrics::MetricsService::instance().createInt64Counter(
        otel::metrics::MetricNames::kChangeStreamCursorsTotalOpened,
        "Total number of change stream cursors opened (on router or shard).",
        otel::metrics::MetricUnit::kCursors,
        kCursorsTotalOpenedOpts);
}

// Constructs the histogram for the OTEL metric "change_streams.cursor.lifespan". The change stream
// lifespan histogram is updated after a change stream cursor is closed. A histogram provides
// accurate and thread-safe average for every bucket. This is achieved by locks, so there might be
// some overhead.
inline const otel::metrics::HistogramOptions kLifespanOpts = [] {
    otel::metrics::HistogramOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.lifespan",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    // Using the same histogram buckets as 'metrics.cursor.lifespan'. For change stream cursors we
    // expect that most of the cursors will land in (10min, +inf) bucket.
    opts.explicitBucketBoundaries = std::vector<double>({
        1 * 1e6,        // lifespan <= 1 second will probably capture one-fetch or no-fetch cursors,
                        // unless the query is slow for some reason
        10 * 1e6,       // lifespan <= 10 seconds will probably capture other 'short-lived' change
                        // stream cursors
        10 * 60 * 1e6,  // lifespan <= 10 minutes will probably capture not 'short-lived', but
                        // before the default cursor timeout
        20 * 60 * 1e6,  // lifespan <= 20 minutes will probably capture increased probability for
                        // cursor timeouts
        60 * 60 * 1e6,  // lifespan <= 1 hour will probably capture some hourly patterns
        24 * 60 * 60 * 1e6,     // lifetime <= 1 day will probably capture some daily patterns
        7 * 24 * 60 * 60 * 1e6  // lifetime <= 1 week will probably capture some weekly patterns
    });
    return opts;
}();

inline otel::metrics::Histogram<int64_t>& createCursorsLifespan() {
    return otel::metrics::MetricsService::instance().createInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamCursorsLifespan,
        "Lifespan of closed change stream cursors in microseconds.",
        otel::metrics::MetricUnit::kMicroseconds,
        kLifespanOpts);
}

inline const otel::metrics::UpDownCounterOptions kCursorsOpenTotalOpts = [] {
    otel::metrics::UpDownCounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.open.total",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return opts;
}();

// Constructs the counter for currently open change stream cursors (idle or pinned).
inline otel::metrics::UpDownCounter<int64_t>& createCursorsOpenTotal() {
    return otel::metrics::MetricsService::instance().createInt64UpDownCounter(
        otel::metrics::MetricNames::kChangeStreamCursorsOpenTotal,
        "Current number of open change stream cursors.",
        otel::metrics::MetricUnit::kCursors,
        kCursorsOpenTotalOpts);
}


inline const otel::metrics::UpDownCounterOptions kCursorsOpenPinnedOpts = [] {
    otel::metrics::UpDownCounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.open.pinned",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return opts;
}();

// Constructs the counter for the number of currently pinned (active) change stream cursors. This
// counter corresponds to the OTEL metric "change_streams.cursor.open.pinned".
inline otel::metrics::UpDownCounter<int64_t>& createCursorsOpenPinned() {
    return otel::metrics::MetricsService::instance().createInt64UpDownCounter(
        otel::metrics::MetricNames::kChangeStreamCursorsOpenPinned,
        "Current number of open change stream cursors.",
        otel::metrics::MetricUnit::kCursors,
        kCursorsOpenPinnedOpts);
}

}  // namespace mongo::change_stream
