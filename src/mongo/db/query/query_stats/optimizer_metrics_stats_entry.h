// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_stats/aggregated_metric.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

namespace mongo::query_stats {

/**
 * Cost-based optimizer optional metrics.
 */
class OptimizerMetricsBonsaiStatsEntry : public SupplementalStatsEntry {
public:
    OptimizerMetricsBonsaiStatsEntry(double optimizationTimeMicros,
                                     double estimatedCost,
                                     double estimatedCardinality,
                                     SupplementalMetricType metricType)
        : SupplementalStatsEntry(metricType),
          optimizationTimeMicros(optimizationTimeMicros),
          estimatedCost(estimatedCost),
          estimatedCardinality(estimatedCardinality) {
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

    AggregatedMetric<double> optimizationTimeMicros;
    AggregatedMetric<double> estimatedCost;
    AggregatedMetric<double> estimatedCardinality;
};

/**
 * Classic optimizer optional metrics.
 */
class OptimizerMetricsClassicStatsEntry : public SupplementalStatsEntry {
public:
    OptimizerMetricsClassicStatsEntry(
        double optimizationTimeMicros,
        SupplementalMetricType metricType = SupplementalMetricType::Classic)
        : SupplementalStatsEntry(metricType), optimizationTimeMicros(optimizationTimeMicros) {
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

    AggregatedMetric<double> optimizationTimeMicros;
};

}  // namespace mongo::query_stats
