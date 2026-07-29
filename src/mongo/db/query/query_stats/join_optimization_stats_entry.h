// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/op_debug.h"
#include "mongo/db/query/query_stats/aggregated_metric.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"
#include "mongo/util/modules.h"

#include <cstdint>
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
          numJoinGraphNodes(metrics.numJoinGraphNodes),
          numSyntacticEdges(metrics.numSyntacticEdges),
          numInferredEdges(metrics.numInferredEdges),
          numSyntacticExprJoinPredicates(metrics.numSyntacticExprJoinPredicates),
          numSyntacticEqJoinPredicates(metrics.numSyntacticEqJoinPredicates),
          numInferredEqJoinPredicates(metrics.numInferredEqJoinPredicates),
          numInferredSingleTablePredicates(metrics.numInferredSingleTablePredicates) {
        joinOptimizable.aggregate(metrics.joinOptimizable);
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

    AggregatedMetric<int64_t> numNamespaces;
    AggregatedMetric<int64_t> numLookupsInSuffix;
    AggregatedMetric<int64_t> numJoinGraphNodes;
    AggregatedMetric<int64_t> numSyntacticEdges;
    AggregatedMetric<int64_t> numInferredEdges;
    AggregatedMetric<int64_t> numSyntacticExprJoinPredicates;
    AggregatedMetric<int64_t> numSyntacticEqJoinPredicates;
    AggregatedMetric<int64_t> numInferredEqJoinPredicates;
    AggregatedMetric<int64_t> numInferredSingleTablePredicates;
};

}  // namespace mongo::query_stats
