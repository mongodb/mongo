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
    numJoinGraphNodes.appendTo(metricsEntryBuilder, "numJoinGraphNodes");
    numSyntacticEdges.appendTo(metricsEntryBuilder, "numSyntacticEdges");
    numInferredEdges.appendTo(metricsEntryBuilder, "numInferredEdges");
    numSyntacticExprJoinPredicates.appendTo(metricsEntryBuilder, "numSyntacticExprJoinPredicates");
    numSyntacticEqJoinPredicates.appendTo(metricsEntryBuilder, "numSyntacticEqJoinPredicates");
    numInferredEqJoinPredicates.appendTo(metricsEntryBuilder, "numInferredEqJoinPredicates");
    numInferredSingleTablePredicates.appendTo(metricsEntryBuilder,
                                              "numInferredSingleTablePredicates");
}

void JoinOptimizationStatsEntry::updateStats(const SupplementalStatsEntry* other) {
    const JoinOptimizationStatsEntry* updateVal =
        dynamic_cast<const JoinOptimizationStatsEntry*>(other);
    tassert(11000100, "Unexpected type of statistic metric", updateVal != nullptr);
    joinOptimizable.trueCount += updateVal->joinOptimizable.trueCount;
    joinOptimizable.falseCount += updateVal->joinOptimizable.falseCount;
    numNamespaces.combine(updateVal->numNamespaces);
    numLookupsInSuffix.combine(updateVal->numLookupsInSuffix);
    numJoinGraphNodes.combine(updateVal->numJoinGraphNodes);
    numSyntacticEdges.combine(updateVal->numSyntacticEdges);
    numInferredEdges.combine(updateVal->numInferredEdges);
    numSyntacticExprJoinPredicates.combine(updateVal->numSyntacticExprJoinPredicates);
    numSyntacticEqJoinPredicates.combine(updateVal->numSyntacticEqJoinPredicates);
    numInferredEqJoinPredicates.combine(updateVal->numInferredEqJoinPredicates);
    numInferredSingleTablePredicates.combine(updateVal->numInferredSingleTablePredicates);
    updateCount++;
}

std::unique_ptr<SupplementalStatsEntry> JoinOptimizationStatsEntry::clone() const {
    return std::make_unique<JoinOptimizationStatsEntry>(*this);
}

}  // namespace mongo::query_stats
