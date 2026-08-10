// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/op_debug.h"
#include "mongo/db/query/query_stats/aggregated_metric.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <map>
#include <memory>

namespace mongo::query_stats {

/**
 * Supplemental query stats metrics collected by the join optimizer for a given query shape.
 */
class JoinOptimizationStatsEntry : public SupplementalStatsEntry {
public:
    explicit JoinOptimizationStatsEntry(const OpDebug::JoinOptimizationMetrics& metrics)
        : SupplementalStatsEntry(SupplementalMetricType::JoinOptimization),
          numNamespaces(metrics.numNamespaces),
          numLookupsInSuffix(metrics.numLookupsInSuffix),
          numSuffixSourcesPushedToSbe(metrics.numSuffixSourcesPushedToSbe),
          numResidualClassicSources(metrics.numResidualClassicSources),
          numJoinGraphNodes(metrics.numJoinGraphNodes),
          numSyntacticEdges(metrics.numSyntacticEdges),
          numInferredEdges(metrics.numInferredEdges),
          numSyntacticExprJoinPredicates(metrics.numSyntacticExprJoinPredicates),
          numSyntacticEqJoinPredicates(metrics.numSyntacticEqJoinPredicates),
          numInferredEqJoinPredicates(metrics.numInferredEqJoinPredicates),
          numInferredSingleTablePredicates(metrics.numInferredSingleTablePredicates),
          joinModelingTimeMicros(metrics.joinModelingTimeMicros),
          sbeLoweringTimeMicros(metrics.sbeLoweringTimeMicros) {
        joinOptimizable.aggregate(metrics.joinOptimizable);
        isClique.aggregate(metrics.isClique);
        isStar.aggregate(metrics.isStar);
        isCycle.aggregate(metrics.isCycle);
        isChain.aggregate(metrics.isChain);
        if (metrics.fallbackReason) {
            fallbackReasonCounts[*metrics.fallbackReason] = 1;
        }
        if (const auto& pe = metrics.planEnumerationMetrics) {
            planEnumerationMetrics = PlanEnumerationMetrics{
                1,
                AggregatedMetric<int64_t>(pe->numPlansEnumerated),
                AggregatedMetric<int64_t>(pe->numHashJoins),
                AggregatedMetric<int64_t>(pe->numIndexedNestedLoopJoins),
                AggregatedMetric<int64_t>(pe->numNestedLoopJoins),
                AggregatedMetric<int64_t>(pe->numFinalPlanHashJoins),
                AggregatedMetric<int64_t>(pe->numFinalPlanIndexedNestedLoopJoins),
                AggregatedMetric<int64_t>(pe->numFinalPlanNestedLoopJoins),
                AggregatedMetric<int64_t>(pe->numJoinNodesRejectedByCost),
                AggregatedMetric<int64_t>(pe->numMemoizedNodes),
                AggregatedMetric<double>(pe->winningPlanCost),
                AggregatedMetric<int64_t>(pe->numSamplingCalls),
                AggregatedMetric<int64_t>(pe->numPersistentSamplesUsed),
                AggregatedMetric<int64_t>(pe->numUniqueIndexesUsedForNDV),
                AggregatedMetric<int64_t>(pe->samplingTimeMicros),
                AggregatedMetric<int64_t>(pe->cbrPlanningTimeMicros),
                AggregatedMetric<int64_t>(pe->planEnumerationTimeMicros),
                AggregatedMetric<int64_t>(pe->ceTimeMicros),
            };
        }
        updateCount++;
    }

    void updateStats(const SupplementalStatsEntry* other) override;
    void appendTo(BSONObjBuilder& builder) const override;
    std::unique_ptr<SupplementalStatsEntry> clone() const override;

    /**
     * Once the metrics is created the updateCount is 1 i.e. the metricsEntry contains non
     * aggregated data from one data point. Every consequent update increments the updateCount by 1.
     */
    uint64_t updateCount = 0;

    AggregatedBool joinOptimizable;

    // How many executions of this shape stopped join optimization early for each reason. Stored
    // sparsely, like 'PlanShapeCounts', so that only reasons actually hit take up space and appear
    // in the output. Read alongside 'joinOptimizable', which says whether such an execution
    // fell back entirely or was optimized over a shortened prefix.
    std::map<join_ordering::JoinFallbackReason, int64_t> fallbackReasonCounts;

    AggregatedMetric<int64_t> numNamespaces;
    AggregatedMetric<int64_t> numLookupsInSuffix;
    AggregatedMetric<int64_t> numSuffixSourcesPushedToSbe;
    AggregatedMetric<int64_t> numResidualClassicSources;
    AggregatedMetric<int64_t> numJoinGraphNodes;
    AggregatedMetric<int64_t> numSyntacticEdges;
    AggregatedMetric<int64_t> numInferredEdges;
    AggregatedMetric<int64_t> numSyntacticExprJoinPredicates;
    AggregatedMetric<int64_t> numSyntacticEqJoinPredicates;
    AggregatedMetric<int64_t> numInferredEqJoinPredicates;
    AggregatedMetric<int64_t> numInferredSingleTablePredicates;

    // The topology of the join graph. These are not mutually exclusive: a two-node graph is a
    // clique, a star and a chain at once, and a three-node path is both a chain and a star.
    AggregatedBool isClique;
    AggregatedBool isStar;
    AggregatedBool isCycle;
    AggregatedBool isChain;

    // Timing metrics for the phases that run on every join-optimized query. The phases that only
    // run on a join plan cache miss live in 'PlanEnumerationMetrics' below.
    AggregatedMetric<int64_t> joinModelingTimeMicros;
    AggregatedMetric<int64_t> sbeLoweringTimeMicros;

    struct PlanEnumerationMetrics {
        // These metrics are only populated when we actually enumerate a plan- so we keep a count.
        uint64_t numPlanEnumerations = 0;
        AggregatedMetric<int64_t> numPlansEnumerated;
        AggregatedMetric<int64_t> numHashJoins;
        AggregatedMetric<int64_t> numIndexedNestedLoopJoins;
        AggregatedMetric<int64_t> numNestedLoopJoins;
        AggregatedMetric<int64_t> numFinalPlanHashJoins;
        AggregatedMetric<int64_t> numFinalPlanIndexedNestedLoopJoins;
        AggregatedMetric<int64_t> numFinalPlanNestedLoopJoins;
        AggregatedMetric<int64_t> numJoinNodesRejectedByCost;
        AggregatedMetric<int64_t> numMemoizedNodes;
        AggregatedMetric<double> winningPlanCost;
        AggregatedMetric<int64_t> numSamplingCalls;
        AggregatedMetric<int64_t> numPersistentSamplesUsed;
        AggregatedMetric<int64_t> numUniqueIndexesUsedForNDV;
        AggregatedMetric<int64_t> samplingTimeMicros;
        AggregatedMetric<int64_t> cbrPlanningTimeMicros;
        AggregatedMetric<int64_t> planEnumerationTimeMicros;
        AggregatedMetric<int64_t> ceTimeMicros;
    };
    boost::optional<PlanEnumerationMetrics> planEnumerationMetrics;
};

}  // namespace mongo::query_stats
