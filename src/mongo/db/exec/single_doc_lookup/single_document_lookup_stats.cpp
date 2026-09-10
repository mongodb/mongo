// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

const SingleDocumentLookupStats kSearchIdLookupAggregationStats{
    .found = createUpdateLookupCounter(
        otel::metrics::MetricNames::kSearchIdLookupAggregationFound,
        "search.idLookup.aggregation.found",
        "Number of search id lookups via aggregation engine that found the document."),
    .notFound = createUpdateLookupCounter(
        otel::metrics::MetricNames::kSearchIdLookupAggregationNotFound,
        "search.idLookup.aggregation.notFound",
        "Number of search id lookups via aggregation where the document was absent."),
    .notHandled = createUpdateLookupCounter(
        otel::metrics::MetricNames::kSearchIdLookupAggregationNotHandled,
        "search.idLookup.aggregation.notHandled",
        "Number of search id lookups via aggregation the engine declined."),
    .latencyMicros =
        createUpdateLookupLatency(otel::metrics::MetricNames::kSearchIdLookupAggregationLatency,
                                  "search.idLookup.aggregation.latencyMicros"),
};

const SingleDocumentLookupStats kSearchIdLookupSbeStats{
    .found = createUpdateLookupCounter(otel::metrics::MetricNames::kSearchIdLookupSbeFound,
                                       "search.idLookup.sbe.found",
                                       "Number of search id lookups via SBE that found the "
                                       "document."),
    .notFound = createUpdateLookupCounter(
        otel::metrics::MetricNames::kSearchIdLookupSbeNotFound,
        "search.idLookup.sbe.notFound",
        "Number of search id lookups via SBE where the document was absent."),
    .notHandled =
        createUpdateLookupCounter(otel::metrics::MetricNames::kSearchIdLookupSbeNotHandled,
                                  "search.idLookup.sbe.notHandled",
                                  "Number of search id lookups via SBE that the engine declined."),
    .latencyMicros = createUpdateLookupLatency(
        otel::metrics::MetricNames::kSearchIdLookupSbeLatency, "search.idLookup.sbe.latencyMicros"),
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

SingleDocumentLookupStatsRecorder
SingleDocumentLookupStatsRecorder::makeSearchIdLookupAggregationRecorder() {
    return SingleDocumentLookupStatsRecorder(kSearchIdLookupAggregationStats.found,
                                             kSearchIdLookupAggregationStats.notFound,
                                             kSearchIdLookupAggregationStats.notHandled,
                                             kSearchIdLookupAggregationStats.latencyMicros);
}

SingleDocumentLookupStatsRecorder
SingleDocumentLookupStatsRecorder::makeSearchIdLookupSbeRecorder() {
    return SingleDocumentLookupStatsRecorder(kSearchIdLookupSbeStats.found,
                                             kSearchIdLookupSbeStats.notFound,
                                             kSearchIdLookupSbeStats.notHandled,
                                             kSearchIdLookupSbeStats.latencyMicros);
}

}  // namespace mongo::exec
