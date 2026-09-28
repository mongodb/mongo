// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_latency_accumulator.h"

#include "mongo/db/commands/server_status/histogram_server_status_metric.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/query/query_lifespan.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/topology/cluster_role.h"

#include <fmt/format.h>

namespace mongo {

namespace {
auto getQueryLatencyAccumulator = QueryLifespan::declareOpCtxDecoration<QueryLatencyAccumulator>();

// Bucket edges for the per-strategy latency histograms: 64us, 128us, ... ~19.1h. Every edge is
// also an opLatencies bucket edge, so the two metrics stay comparable. The 64us floor is well
// below any achievable whole-query time.
std::vector<uint64_t> queryLatencyBucketBounds() {
    return HistogramServerStatusMetric::pow(31, 64, 2);
}

// The metrics published in serverStatus for one plan-selection strategy, as the single leaf
// metrics.queryLatencies.<strategy>: {histogram: [{lowerBound, count}, ...], latency, totalCount}.
// The 'histogram' gives the distribution and 'latency' the running sum. 'totalCount' is computed
// at serialization time as the sum of the bucket counts just appended, so it is always consistent
// with them. All three fields are appended by one appendTo call.
class QueryLatencyMetrics {
public:
    QueryLatencyMetrics() : _histogram{queryLatencyBucketBounds()} {}

    auto& value() {
        return *this;
    }

    void record(Microseconds elapsed) {
        _histogram.increment(static_cast<uint64_t>(durationCount<Microseconds>(elapsed)));
        _latency.increment(elapsed);
    }

    void appendTo(BSONObjBuilder& b, std::string_view leafName) const {
        BSONObjBuilder sub{b.subobjStart(leafName)};
        long long totalCount = 0;
        {
            BSONArrayBuilder arr{sub.subarrayStart("histogram")};
            for (auto&& [count, lower, upper] : _histogram.hist()) {
                BSONObjBuilder{arr.subobjStart()}
                    .append("lowerBound", static_cast<long long>(lower ? *lower : 0))
                    .append("count", static_cast<long long>(count));
                totalCount += count;
            }
        }
        sub.append("latency", static_cast<long long>(_latency.get().count()));
        sub.append("totalCount", totalCount);
    }

private:
    HistogramServerStatusMetric _histogram;
    DurationCounter64<Microseconds> _latency;
};

QueryLatencyMetrics& makeQueryLatencyMetrics(std::string_view strategyName) {
    return *CustomMetricBuilder<QueryLatencyMetrics>{fmt::format("queryLatencies.{}", strategyName)}
                .setRole(ClusterRole::ShardServer);
}

// Register metrics for all plan selection strategies.
QueryLatencyMetrics& multiPlannerMetrics = makeQueryLatencyMetrics("multiPlanner");
QueryLatencyMetrics& costBasedMetrics = makeQueryLatencyMetrics("costBased");
QueryLatencyMetrics& singlePlanMetrics = makeQueryLatencyMetrics("singlePlan");
QueryLatencyMetrics& cachedPlanMetrics = makeQueryLatencyMetrics("cachedPlan");
QueryLatencyMetrics& joinOptimizationMetrics = makeQueryLatencyMetrics("joinOptimization");
QueryLatencyMetrics& joinCachedPlanMetrics = makeQueryLatencyMetrics("joinCachedPlan");

// No default case: a new PlanSelectionStrategy fails to compile until it names its metrics.
QueryLatencyMetrics& queryLatencyMetricsFor(PlanSelectionStrategy strategy) {
    switch (strategy) {
        case PlanSelectionStrategy::kMultiPlanner:
            return multiPlannerMetrics;
        case PlanSelectionStrategy::kCostBasedRanker:
            return costBasedMetrics;
        case PlanSelectionStrategy::kSinglePlan:
            return singlePlanMetrics;
        case PlanSelectionStrategy::kCachedPlan:
            return cachedPlanMetrics;
        case PlanSelectionStrategy::kJoinOptimization:
            return joinOptimizationMetrics;
        case PlanSelectionStrategy::kJoinCachedPlan:
            return joinCachedPlanMetrics;
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
    queryLatencyMetricsFor(*_strategy).record(_total);
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
