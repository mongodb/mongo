// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/historical_placement_fetcher_metrics.h"

#include "mongo/db/change_stream_metrics_util.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using namespace change_stream;

/**
 * References to the OTEL instruments for the placement-history lookups issued by the v2 change
 * stream shard targeters: one counter per outcome plus a shared latency histogram. The
 * instruments are process-global singletons owned by the OTEL MetricsService; this struct only
 * borrows references to them.
 */
struct HistoricalPlacementFetcherMetrics {
    otel::metrics::Counter<int64_t>& ok;
    otel::metrics::Counter<int64_t>& futureClusterTime;
    otel::metrics::Counter<int64_t>& notAvailable;
    otel::metrics::Histogram<int64_t>& latencyMillis;
};

const HistoricalPlacementFetcherMetrics kHistoricalPlacementFetcherMetrics{
    .ok = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingPlacementHistoryLookupOk,
        "changeStreams.shardTargeting.placementHistoryLookup.ok",
        "Number of v2 change stream placement-history lookups that returned successfully."),
    .futureClusterTime = createShardTargetingCounter(
        otel::metrics::MetricNames::
            kChangeStreamShardTargetingPlacementHistoryLookupFutureClusterTime,
        "changeStreams.shardTargeting.placementHistoryLookup.futureClusterTime",
        "Number of v2 change stream placement-history lookups for a cluster time in the future."),
    .notAvailable = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingPlacementHistoryLookupNotAvailable,
        "changeStreams.shardTargeting.placementHistoryLookup.notAvailable",
        "Number of v2 change stream placement-history lookups for which no placement history is "
        "available."),
    .latencyMillis = createPlacementHistoryLatency(
        otel::metrics::MetricNames::kChangeStreamShardTargetingPlacementHistoryLookupLatencyMillis,
        "changeStreams.shardTargeting.placementHistoryLookup.latencyMillis"),
};

}  // namespace

void HistoricalPlacementFetcherMetricsRecorder::recordLookup(HistoricalPlacementStatus status,
                                                             Milliseconds latency) {
    latencyMillis.record(durationCount<Milliseconds>(latency));
    switch (status) {
        case HistoricalPlacementStatus::OK:
            ok.add(1);
            return;
        case HistoricalPlacementStatus::NotAvailable:
            notAvailable.add(1);
            return;
        case HistoricalPlacementStatus::FutureClusterTime:
            futureClusterTime.add(1);
            return;
    }
    MONGO_UNREACHABLE_TASSERT(13565602);
}

HistoricalPlacementFetcherMetricsRecorder getHistoricalPlacementFetcherMetricsRecorder() {
    return HistoricalPlacementFetcherMetricsRecorder{
        .ok = kHistoricalPlacementFetcherMetrics.ok,
        .futureClusterTime = kHistoricalPlacementFetcherMetrics.futureClusterTime,
        .notAvailable = kHistoricalPlacementFetcherMetrics.notAvailable,
        .latencyMillis = kHistoricalPlacementFetcherMetrics.latencyMillis,
    };
}

}  // namespace mongo
