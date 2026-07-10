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

#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats.h"

#include "mongo/db/change_stream_metrics_util.h"

namespace mongo::exec {
namespace {

// Eager file-scope initialization ensures the ServerStatusMetric entries are registered with
// MetricTree before it is frozen at startup. Meyer's singleton (function-level static) would defer
// registration to the first updateLookup request — after the freeze — and crash the server.
using namespace change_stream;

/**
 * References to the OTEL instruments of one (consumer x engine) single-document-lookup cell: the
 * three outcome counters plus a latency histogram. The instruments are process-global singletons
 * owned by the OTEL MetricsService; this struct only borrows references to them, so it is cheap to
 * copy and outlives no executor.
 */
struct SingleDocumentLookupStats {
    otel::metrics::Counter<int64_t>& found;
    otel::metrics::Counter<int64_t>& notFound;
    otel::metrics::Counter<int64_t>& notHandled;
    otel::metrics::Histogram<int64_t>& latencyMicros;
};

const SingleDocumentLookupStats kUpdateLookupExpressStats{
    .found = createUpdateLookupCounter(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupExpressFound,
        "changeStreams.updateLookup.express.found",
        "Number of change stream updateLookup express lookups that found the document."),
    .notFound = createUpdateLookupCounter(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupExpressNotFound,
        "changeStreams.updateLookup.express.notFound",
        "Number of change stream updateLookup express lookups where the document was absent."),
    .notHandled = createUpdateLookupCounter(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupExpressNotHandled,
        "changeStreams.updateLookup.express.notHandled",
        "Number of change stream updateLookup express lookups the engine declined."),
    .latencyMicros = createUpdateLookupLatency(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupExpressLatency,
        "changeStreams.updateLookup.express.latencyMicros"),
};

const SingleDocumentLookupStats kUpdateLookupAggregationStats{
    .found = createUpdateLookupCounter(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupAggregationFound,
        "changeStreams.updateLookup.aggregation.found",
        "Number of change stream updateLookup aggregation lookups that found the document."),
    .notFound = createUpdateLookupCounter(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupAggregationNotFound,
        "changeStreams.updateLookup.aggregation.notFound",
        "Number of change stream updateLookup aggregation lookups where the document was absent."),
    .notHandled = createUpdateLookupCounter(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupAggregationNotHandled,
        "changeStreams.updateLookup.aggregation.notHandled",
        "Number of change stream updateLookup aggregation lookups the engine declined."),
    .latencyMicros = createUpdateLookupLatency(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupAggregationLatency,
        "changeStreams.updateLookup.aggregation.latencyMicros"),
};

const SingleDocumentLookupStats kUpdateLookupSbeStats{
    .found = createUpdateLookupCounter(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupSbeFound,
        "changeStreams.updateLookup.sbe.found",
        "Number of change stream updateLookup SBE lookups that found the document."),
    .notFound = createUpdateLookupCounter(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupSbeNotFound,
        "changeStreams.updateLookup.sbe.notFound",
        "Number of change stream updateLookup SBE lookups where the document was absent."),
    .notHandled = createUpdateLookupCounter(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupSbeNotHandled,
        "changeStreams.updateLookup.sbe.notHandled",
        "Number of change stream updateLookup SBE lookups the engine declined."),
    .latencyMicros =
        createUpdateLookupLatency(otel::metrics::MetricNames::kChangeStreamUpdateLookupSbeLatency,
                                  "changeStreams.updateLookup.sbe.latencyMicros"),
};

}  // namespace

SingleDocumentLookupStatsRecorder
SingleDocumentLookupStatsRecorder::makeUpdateLookupAggregationRecorder() {
    return SingleDocumentLookupStatsRecorder(kUpdateLookupAggregationStats.found,
                                             kUpdateLookupAggregationStats.notFound,
                                             kUpdateLookupAggregationStats.notHandled,
                                             kUpdateLookupAggregationStats.latencyMicros);
}

SingleDocumentLookupStatsRecorder
SingleDocumentLookupStatsRecorder::makeUpdateLookupExpressRecorder() {
    return SingleDocumentLookupStatsRecorder(kUpdateLookupExpressStats.found,
                                             kUpdateLookupExpressStats.notFound,
                                             kUpdateLookupExpressStats.notHandled,
                                             kUpdateLookupExpressStats.latencyMicros);
}

SingleDocumentLookupStatsRecorder SingleDocumentLookupStatsRecorder::makeUpdateLookupSbeRecorder() {
    return SingleDocumentLookupStatsRecorder(kUpdateLookupSbeStats.found,
                                             kUpdateLookupSbeStats.notFound,
                                             kUpdateLookupSbeStats.notHandled,
                                             kUpdateLookupSbeStats.latencyMicros);
}

}  // namespace mongo::exec
