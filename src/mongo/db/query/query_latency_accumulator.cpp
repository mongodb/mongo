// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_latency_accumulator.h"

#include "mongo/db/query/query_lifespan.h"
#include "mongo/db/stats/operation_latency_histogram.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/server_status_options.h"

#include <fmt/format.h>

namespace mongo {

namespace {
auto getQueryLatencyAccumulator = QueryLifespan::declareOpCtxDecoration<QueryLatencyAccumulator>();

// Per-strategy query latency OTel histogram, published in serverStatus at
// metrics.queryLatencies.<strategy>. Shares the opLatencies bucket edges so the two are comparable.
otel::metrics::Histogram<int64_t>& makeQueryLatencyHistogram(otel::metrics::MetricName name,
                                                             std::string_view strategyName) {
    return otel::metrics::MetricsService::instance().createInt64Histogram(
        name,
        fmt::format("Wall-clock latency of completed user queries (originating command plus all "
                    "getMores) that used the {} plan-selection strategy.",
                    strategyName),
        otel::metrics::MetricUnit::kMicroseconds,
        {.serverStatusOptions =
             otel::metrics::ServerStatusOptions{.dottedPath =
                                                    fmt::format("queryLatencies.{}", strategyName),
                                                .role = ClusterRole::ShardServer},
         .explicitBucketBoundaries =
             operation_latency_histogram_details::makeOperationLatencyBucketBoundaries(),
         // Serialize only non-empty buckets.
         .serializationFormat =
             otel::metrics::HistogramSerializationFormat::kNonEmptyBucketCounts});
}

// Register histograms for all four plan-selection strategies.
otel::metrics::Histogram<int64_t>& multiPlannerHistogram = makeQueryLatencyHistogram(
    otel::metrics::MetricNames::kQueryLatencyMultiPlanner, "multiPlanner");
otel::metrics::Histogram<int64_t>& costBasedHistogram =
    makeQueryLatencyHistogram(otel::metrics::MetricNames::kQueryLatencyCostBased, "costBased");
otel::metrics::Histogram<int64_t>& singlePlanHistogram =
    makeQueryLatencyHistogram(otel::metrics::MetricNames::kQueryLatencySinglePlan, "singlePlan");
otel::metrics::Histogram<int64_t>& cachedPlanHistogram =
    makeQueryLatencyHistogram(otel::metrics::MetricNames::kQueryLatencyCachedPlan, "cachedPlan");

// No default case: a new PlanSelectionStrategy fails to compile until it names its histogram.
otel::metrics::Histogram<int64_t>& queryLatencyHistogramFor(PlanSelectionStrategy strategy) {
    switch (strategy) {
        case PlanSelectionStrategy::kMultiPlanner:
            return multiPlannerHistogram;
        case PlanSelectionStrategy::kCostBasedRanker:
            return costBasedHistogram;
        case PlanSelectionStrategy::kSinglePlan:
            return singlePlanHistogram;
        case PlanSelectionStrategy::kCachedPlan:
            return cachedPlanHistogram;
    }
    MONGO_UNREACHABLE_TASSERT(12765301);
}
}  // namespace

QueryLatencyAccumulator& QueryLatencyAccumulator::get(OperationContext* opCtx) {
    return getQueryLatencyAccumulator(opCtx);
}

QueryLatencyAccumulator::~QueryLatencyAccumulator() {
    // One measurement per logical query. Skipped when excluded, when plan selection never ran (no
    // strategy), or when no time was recorded.
    if (_excluded || !_strategy || _total <= Microseconds{0}) {
        return;
    }
    queryLatencyHistogramFor(*_strategy).record(durationCount<Microseconds>(_total));
}

void QueryLatencyAccumulator::recordStrategy(PlanSelectionStrategy strategy) {
    if (!_strategy) {
        _strategy = strategy;
    }
}

void QueryLatencyAccumulator::addLatency(Microseconds elapsed) {
    if (_excluded || !_strategy) {
        return;
    }
    _total += elapsed;
}

void QueryLatencyAccumulator::exclude() {
    _excluded = true;
}

}  // namespace mongo
