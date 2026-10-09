// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Counts a placement-history lookup issued by 'HistoricalPlacementFetcherImpl::fetch()' into
 * 'changeStreams.shardTargeting.placementHistoryLookup.<status>', and records its latency into
 * 'changeStreams.shardTargeting.placementHistoryLookup.latencyMillis'. Held by the fetcher as a
 * member and invoked from 'fetch()', instead of being reached for via a free function.
 */
struct HistoricalPlacementFetcherMetricsRecorder {
    otel::metrics::Counter<int64_t>& ok;
    otel::metrics::Counter<int64_t>& futureClusterTime;
    otel::metrics::Counter<int64_t>& notAvailable;
    otel::metrics::Histogram<int64_t>& latencyMillis;

    /**
     * Records 'latency' unconditionally, then counts 'status' into its matching outcome counter.
     * Lookups that fail with a thrown error (transport/command failure) never reach this: they
     * have no status to record.
     */
    void recordLookup(HistoricalPlacementStatus status, Milliseconds latency);
};

/**
 * Factory returning a recorder bound to the process-global OTEL instruments. Called once, at
 * fetcher-construction time.
 */
[[MONGO_MOD_PUBLIC]] HistoricalPlacementFetcherMetricsRecorder
getHistoricalPlacementFetcherMetricsRecorder();

}  // namespace mongo
