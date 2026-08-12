// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/server_status_metrics.h"

namespace mongo::join_ordering {

JoinOptimizationServerStatusMetrics joinOptMetrics;

void recordJoinOptimizationMetrics(const OpDebug::JoinOptimizationMetrics& metrics) {
    joinOptMetrics.considered.increment(1);
    if (metrics.joinOptimizable) {
        joinOptMetrics.optimized.increment(1);
    }
    if (metrics.fallbackReason) {
        joinOptMetrics.fallbackReasons.increment(*metrics.fallbackReason);
    }
    joinOptMetrics.numSuffixSourcesPushedToSbe.increment(metrics.numSuffixSourcesPushedToSbe);
    joinOptMetrics.numResidualClassicSources.increment(metrics.numResidualClassicSources);
    joinOptMetrics.numJoinGraphNodes.increment(metrics.numJoinGraphNodes);
    joinOptMetrics.joinModelingTimeMicros.increment(metrics.joinModelingTimeMicros);
    joinOptMetrics.sbeLoweringTimeMicros.increment(metrics.sbeLoweringTimeMicros);

    if (const auto& pe = metrics.planEnumerationMetrics) {
        joinOptMetrics.numEnumerations.increment(1);
        joinOptMetrics.numFinalPlanHashJoins.increment(pe->numFinalPlanHashJoins);
        joinOptMetrics.numFinalPlanIndexedNestedLoopJoins.increment(
            pe->numFinalPlanIndexedNestedLoopJoins);
        joinOptMetrics.numFinalPlanNestedLoopJoins.increment(pe->numFinalPlanNestedLoopJoins);
        joinOptMetrics.numSamplingCalls.increment(pe->numSamplingCalls);
        joinOptMetrics.numPersistentSamplesUsed.increment(pe->numPersistentSamplesUsed);
        joinOptMetrics.numPlansEnumerated.increment(pe->numPlansEnumerated);
        joinOptMetrics.numMemoizedNodes.increment(pe->numMemoizedNodes);
        joinOptMetrics.numJoinNodesRejectedByCost.increment(pe->numJoinNodesRejectedByCost);
        joinOptMetrics.planEnumerationTimeMicros.increment(pe->planEnumerationTimeMicros);
        joinOptMetrics.cbrPlanningTimeMicros.increment(pe->cbrPlanningTimeMicros);
        joinOptMetrics.samplingTimeMicros.increment(pe->samplingTimeMicros);
        joinOptMetrics.ceTimeMicros.increment(pe->ceTimeMicros);
    }
}

}  // namespace mongo::join_ordering
