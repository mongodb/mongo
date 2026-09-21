// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/batched_enrichment_stats.h"

#include "mongo/otel/metrics/metrics_service.h"

namespace mongo::exec::agg {
namespace {

otel::metrics::Counter<int64_t>& createEnrichBatchesStartedCounter(otel::metrics::MetricName name,
                                                                   std::string dottedPath,
                                                                   std::string description) {
    otel::metrics::CounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = std::move(dottedPath),
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return otel::metrics::MetricsService::instance().createInt64Counter(
        name, std::move(description), otel::metrics::MetricUnit::kEvents, opts);
}

otel::metrics::Counter<int64_t>& kChangeStreamUpdateLookupEnrichBatchesStarted =
    createEnrichBatchesStartedCounter(
        otel::metrics::MetricNames::kChangeStreamUpdateLookupEnrichBatchesStarted,
        "changeStreams.updateLookup.enrichBatchesStarted",
        "Number of change stream updateLookup enrich sub-batches started, regardless of engine.");

otel::metrics::Counter<int64_t>& kSearchIdLookupEnrichBatchesStarted =
    createEnrichBatchesStartedCounter(
        otel::metrics::MetricNames::kSearchIdLookupEnrichBatchesStarted,
        "search.idLookup.enrichBatchesStarted",
        "Number of $_internalSearchIdLookup enrich sub-batches started, regardless of engine.");

}  // namespace

BatchedEnrichmentStatsRecorder
BatchedEnrichmentStatsRecorder::makeChangeStreamUpdateLookupRecorder() {
    return BatchedEnrichmentStatsRecorder(kChangeStreamUpdateLookupEnrichBatchesStarted);
}

BatchedEnrichmentStatsRecorder BatchedEnrichmentStatsRecorder::makeSearchIdLookupRecorder() {
    return BatchedEnrichmentStatsRecorder(kSearchIdLookupEnrichBatchesStarted);
}

}  // namespace mongo::exec::agg
