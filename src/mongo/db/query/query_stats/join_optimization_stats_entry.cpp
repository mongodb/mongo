// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/join_optimization_stats_entry.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"
#include "mongo/util/assert_util.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::query_stats {

void JoinOptimizationStatsEntry::appendTo(BSONObjBuilder& builder) const {
    BSONObjBuilder metricsEntryBuilder = builder.subobjStart(toStringData(metricType));
    metricsEntryBuilder.append("updateCount", static_cast<long long>(updateCount));
    joinOptimizable.appendTo(metricsEntryBuilder, "joinOptimizable");
    numNamespaces.appendTo(metricsEntryBuilder, "numNamespaces");
    numLookupsInSuffix.appendTo(metricsEntryBuilder, "numLookupsInSuffix");
    numSuffixSourcesPushedToSbe.appendTo(metricsEntryBuilder, "numSuffixSourcesPushedToSbe");
    numResidualClassicSources.appendTo(metricsEntryBuilder, "numResidualClassicSources");
    numJoinGraphNodes.appendTo(metricsEntryBuilder, "numJoinGraphNodes");
    numSyntacticEdges.appendTo(metricsEntryBuilder, "numSyntacticEdges");
    numInferredEdges.appendTo(metricsEntryBuilder, "numInferredEdges");
    numSyntacticExprJoinPredicates.appendTo(metricsEntryBuilder, "numSyntacticExprJoinPredicates");
    numSyntacticEqJoinPredicates.appendTo(metricsEntryBuilder, "numSyntacticEqJoinPredicates");
    numInferredEqJoinPredicates.appendTo(metricsEntryBuilder, "numInferredEqJoinPredicates");
    numInferredSingleTablePredicates.appendTo(metricsEntryBuilder,
                                              "numInferredSingleTablePredicates");
    joinModelingTimeMicros.appendTo(metricsEntryBuilder, "joinModelingTimeMicros");
    sbeLoweringTimeMicros.appendTo(metricsEntryBuilder, "sbeLoweringTimeMicros");
    if (planEnumerationMetrics) {
        metricsEntryBuilder.append(
            "numPlanEnumerations",
            static_cast<long long>(planEnumerationMetrics->numPlanEnumerations));
        planEnumerationMetrics->numPlansEnumerated.appendTo(metricsEntryBuilder,
                                                            "numPlansEnumerated");
        planEnumerationMetrics->numHashJoins.appendTo(metricsEntryBuilder, "numHashJoins");
        planEnumerationMetrics->numIndexedNestedLoopJoins.appendTo(metricsEntryBuilder,
                                                                   "numIndexedNestedLoopJoins");
        planEnumerationMetrics->numNestedLoopJoins.appendTo(metricsEntryBuilder,
                                                            "numNestedLoopJoins");
        planEnumerationMetrics->numFinalPlanHashJoins.appendTo(metricsEntryBuilder,
                                                               "numFinalPlanHashJoins");
        planEnumerationMetrics->numFinalPlanIndexedNestedLoopJoins.appendTo(
            metricsEntryBuilder, "numFinalPlanIndexedNestedLoopJoins");
        planEnumerationMetrics->numFinalPlanNestedLoopJoins.appendTo(metricsEntryBuilder,
                                                                     "numFinalPlanNestedLoopJoins");
        planEnumerationMetrics->numJoinNodesRejectedByCost.appendTo(metricsEntryBuilder,
                                                                    "numJoinNodesRejectedByCost");
        planEnumerationMetrics->numMemoizedNodes.appendTo(metricsEntryBuilder, "numMemoizedNodes");
        planEnumerationMetrics->winningPlanCost.appendTo(metricsEntryBuilder, "winningPlanCost");
        planEnumerationMetrics->samplingTimeMicros.appendTo(metricsEntryBuilder,
                                                            "samplingTimeMicros");
        planEnumerationMetrics->cbrPlanningTimeMicros.appendTo(metricsEntryBuilder,
                                                               "cbrPlanningTimeMicros");
        planEnumerationMetrics->planEnumerationTimeMicros.appendTo(metricsEntryBuilder,
                                                                   "planEnumerationTimeMicros");
        planEnumerationMetrics->ceTimeMicros.appendTo(metricsEntryBuilder, "ceTimeMicros");
    }
}

void JoinOptimizationStatsEntry::updateStats(const SupplementalStatsEntry* other) {
    const JoinOptimizationStatsEntry* updateVal =
        dynamic_cast<const JoinOptimizationStatsEntry*>(other);
    tassert(11000100, "Unexpected type of statistic metric", updateVal != nullptr);
    joinOptimizable.trueCount += updateVal->joinOptimizable.trueCount;
    joinOptimizable.falseCount += updateVal->joinOptimizable.falseCount;
    numNamespaces.combine(updateVal->numNamespaces);
    numLookupsInSuffix.combine(updateVal->numLookupsInSuffix);
    numSuffixSourcesPushedToSbe.combine(updateVal->numSuffixSourcesPushedToSbe);
    numResidualClassicSources.combine(updateVal->numResidualClassicSources);
    numJoinGraphNodes.combine(updateVal->numJoinGraphNodes);
    numSyntacticEdges.combine(updateVal->numSyntacticEdges);
    numInferredEdges.combine(updateVal->numInferredEdges);
    numSyntacticExprJoinPredicates.combine(updateVal->numSyntacticExprJoinPredicates);
    numSyntacticEqJoinPredicates.combine(updateVal->numSyntacticEqJoinPredicates);
    numInferredEqJoinPredicates.combine(updateVal->numInferredEqJoinPredicates);
    numInferredSingleTablePredicates.combine(updateVal->numInferredSingleTablePredicates);
    joinModelingTimeMicros.combine(updateVal->joinModelingTimeMicros);
    sbeLoweringTimeMicros.combine(updateVal->sbeLoweringTimeMicros);
    if (updateVal->planEnumerationMetrics) {
        const auto& other = *updateVal->planEnumerationMetrics;
        if (!planEnumerationMetrics) {
            planEnumerationMetrics = other;
        } else {
            planEnumerationMetrics->numPlanEnumerations += other.numPlanEnumerations;
            planEnumerationMetrics->numPlansEnumerated.combine(other.numPlansEnumerated);
            planEnumerationMetrics->numHashJoins.combine(other.numHashJoins);
            planEnumerationMetrics->numIndexedNestedLoopJoins.combine(
                other.numIndexedNestedLoopJoins);
            planEnumerationMetrics->numNestedLoopJoins.combine(other.numNestedLoopJoins);
            planEnumerationMetrics->numFinalPlanHashJoins.combine(other.numFinalPlanHashJoins);
            planEnumerationMetrics->numFinalPlanIndexedNestedLoopJoins.combine(
                other.numFinalPlanIndexedNestedLoopJoins);
            planEnumerationMetrics->numFinalPlanNestedLoopJoins.combine(
                other.numFinalPlanNestedLoopJoins);
            planEnumerationMetrics->numJoinNodesRejectedByCost.combine(
                other.numJoinNodesRejectedByCost);
            planEnumerationMetrics->numMemoizedNodes.combine(other.numMemoizedNodes);
            planEnumerationMetrics->winningPlanCost.combine(other.winningPlanCost);
            planEnumerationMetrics->samplingTimeMicros.combine(other.samplingTimeMicros);
            planEnumerationMetrics->cbrPlanningTimeMicros.combine(other.cbrPlanningTimeMicros);
            planEnumerationMetrics->planEnumerationTimeMicros.combine(
                other.planEnumerationTimeMicros);
            planEnumerationMetrics->ceTimeMicros.combine(other.ceTimeMicros);
        }
    }
    updateCount++;
}

std::unique_ptr<SupplementalStatsEntry> JoinOptimizationStatsEntry::clone() const {
    return std::make_unique<JoinOptimizationStatsEntry>(*this);
}

}  // namespace mongo::query_stats
