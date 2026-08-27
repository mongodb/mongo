// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/histogram_server_status_metric.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/op_debug.h"
#include "mongo/db/query/compiler/optimizer/join/fallback_reason.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <array>
#include <cstdint>
#include <string_view>

/**
 * Server-wide join optimization metrics, reported under 'metrics.query.joinOptimization' in
 * serverStatus. Unlike the equivalent query stats metrics these are not sampled, so they cover
 * every aggregation the join optimizer looked at.
 */
namespace mongo::join_ordering {

/**
 * Server-wide counts of why join optimization did not do more than it did, keyed by
 * 'JoinFallbackReason'. Reasons that have never been hit are omitted from serverStatus.
 */
class JoinFallbackReasonCounters {
public:
    void increment(JoinFallbackReason reason) {
        _counts[static_cast<size_t>(reason)].fetchAndAdd(1);
    }

    // Used as a 'CustomMetricBuilder' value policy: 'value()' is the reference the builder hands
    // back to the caller, and this class is its own value.
    JoinFallbackReasonCounters& value() {
        return *this;
    }

    void appendTo(BSONObjBuilder& b, std::string_view leafName) const {
        BSONObjBuilder sub{b.subobjStart(leafName)};
        for (size_t i = 0; i < kNumJoinFallbackReasons; ++i) {
            if (auto count = _counts[i].load(); count > 0) {
                sub.append(toReasonName(static_cast<JoinFallbackReason>(i)), count);
            }
        }
    }

private:
    std::array<Atomic<int64_t>, kNumJoinFallbackReasons> _counts{};
};

// Bounds for histograms over microsecond durations and over unbounded counts, respectively.
inline const auto kMicrosBounds = HistogramServerStatusMetric::pow(11, 1024, 4);
inline const auto kCountBounds = HistogramServerStatusMetric::pow(11, 1, 4);

// Bounds for the histogram over join graph node counts. A join graph always has at least 2 nodes.
// 'internalMaxNodesInJoinGraph' defaults to 10 and is capped at 'kHardMaxNodesInJoin' (64), so use
// exact buckets over the default range and coarse buckets to cover the rest of the range.
inline const std::vector<uint64_t> kJoinGraphNodeBounds{2, 3, 4, 5, 6, 7, 8, 9, 10, 16, 32, 64};

struct JoinOptimizationServerStatusMetrics {
    // Number of aggregations that passed the up-front join optimization eligibility check.
    Counter64& considered =
        *MetricBuilder<Counter64>{"query.joinOptimization.joinOptimizationConsidered"};
    // Of those, the number for which a join model was successfully built. Note that a query can
    // still bail out after this point, e.g. if generating single-table access plans fails.
    Counter64& optimized = *MetricBuilder<Counter64>{"query.joinOptimization.joinOptimizedTrue"};
    JoinFallbackReasonCounters& fallbackReasons =
        *CustomMetricBuilder<JoinFallbackReasonCounters>{"query.joinOptimization.fallbackReasons"};

    // These counters aggregate the total number of enumerations, join nodes of each type, sampling
    // calls, etc across all queries that go through join optimization.
    Counter64& numEnumerations =
        *MetricBuilder<Counter64>{"query.joinOptimization.numEnumerations"};
    Counter64& numFinalPlanHashJoins =
        *MetricBuilder<Counter64>{"query.joinOptimization.numFinalPlanHashJoins"};
    Counter64& numFinalPlanIndexedNestedLoopJoins =
        *MetricBuilder<Counter64>{"query.joinOptimization.numFinalPlanIndexedNestedLoopJoins"};
    Counter64& numFinalPlanNestedLoopJoins =
        *MetricBuilder<Counter64>{"query.joinOptimization.numFinalPlanNestedLoopJoins"};
    Counter64& numSamplingCalls =
        *MetricBuilder<Counter64>{"query.joinOptimization.numSamplingCalls"};
    Counter64& numPersistentSamplesUsed =
        *MetricBuilder<Counter64>{"query.joinOptimization.numPersistentSamplesUsed"};
    Counter64& numPersistentNDVStatsUsed =
        *MetricBuilder<Counter64>{"query.joinOptimization.numPersistentNDVStatsUsed"};
    Counter64& numSuffixSourcesPushedToSbe =
        *MetricBuilder<Counter64>{"query.joinOptimization.numSuffixSourcesPushedToSbe"};
    Counter64& numResidualClassicSources =
        *MetricBuilder<Counter64>{"query.joinOptimization.numResidualClassicSources"};

    // The histograms below track metrics pertaining to the join graph and join plan enumeration
    // across all queries participating in join optimization.
    HistogramServerStatusMetric& numJoinGraphNodes =
        *MetricBuilder<HistogramServerStatusMetric>{
            "query.joinOptimization.histograms.numJoinGraphNodes"}
             .bind(kJoinGraphNodeBounds);
    HistogramServerStatusMetric& numPlansEnumerated =
        *MetricBuilder<HistogramServerStatusMetric>{
            "query.joinOptimization.histograms.numPlansEnumerated"}
             .bind(kCountBounds);
    HistogramServerStatusMetric& numMemoizedNodes =
        *MetricBuilder<HistogramServerStatusMetric>{
            "query.joinOptimization.histograms.numMemoizedNodes"}
             .bind(kCountBounds);
    HistogramServerStatusMetric& numJoinNodesRejectedByCost =
        *MetricBuilder<HistogramServerStatusMetric>{
            "query.joinOptimization.histograms.numJoinNodesRejectedByCost"}
             .bind(kCountBounds);

    // Similarly, the histograms below track time in micros across queries, broken down into
    // different stages of join optimization.
    HistogramServerStatusMetric& joinModelingTimeMicros =
        *MetricBuilder<HistogramServerStatusMetric>{
            "query.joinOptimization.histograms.joinModelingTimeMicros"}
             .bind(kMicrosBounds);
    HistogramServerStatusMetric& sbeLoweringTimeMicros =
        *MetricBuilder<HistogramServerStatusMetric>{
            "query.joinOptimization.histograms.sbeLoweringTimeMicros"}
             .bind(kMicrosBounds);
    HistogramServerStatusMetric& planEnumerationTimeMicros =
        *MetricBuilder<HistogramServerStatusMetric>{
            "query.joinOptimization.histograms.planEnumerationTimeMicros"}
             .bind(kMicrosBounds);
    HistogramServerStatusMetric& cbrPlanningTimeMicros =
        *MetricBuilder<HistogramServerStatusMetric>{
            "query.joinOptimization.histograms.cbrPlanningTimeMicros"}
             .bind(kMicrosBounds);
    HistogramServerStatusMetric& samplingTimeMicros =
        *MetricBuilder<HistogramServerStatusMetric>{
            "query.joinOptimization.histograms.samplingTimeMicros"}
             .bind(kMicrosBounds);
    HistogramServerStatusMetric& ceTimeMicros =
        *MetricBuilder<HistogramServerStatusMetric>{
            "query.joinOptimization.histograms.ceTimeMicros"}
             .bind(kMicrosBounds);
};

/**
 * The server-wide metrics. Increment these directly: a reason detected before the per-query metrics
 * in 'OpDebug::JoinOptimizationMetrics' were initialized has nowhere else to go, so its call site
 * does 'joinOptMetrics.fallbackReasons.increment(reason)'. Reasons recorded after that point are
 * read off 'metrics.fallbackReason' by 'recordJoinOptimizationMetrics()' instead.
 */
extern JoinOptimizationServerStatusMetrics joinOptMetrics;

/**
 * Aggregates one query's worth of join optimization metrics.
 */
void recordJoinOptimizationMetrics(const OpDebug::JoinOptimizationMetrics& metrics);

}  // namespace mongo::join_ordering
