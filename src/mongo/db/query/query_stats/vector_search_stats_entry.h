// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_stats/aggregated_metric.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"
#include "mongo/util/modules.h"

namespace mongo::query_stats {

/**
 * $vectorSearch aggregation stage metrics.
 */
class VectorSearchStatsEntry : public SupplementalStatsEntry {
public:
    VectorSearchStatsEntry(long limit, double numCandidatesLimitRatio)
        : SupplementalStatsEntry(SupplementalMetricType::VectorSearch),
          limit(limit),
          numCandidatesLimitRatio(numCandidatesLimitRatio) {}

    void updateStats(const SupplementalStatsEntry* other) override;
    void appendTo(BSONObjBuilder& builder) const override;
    std::unique_ptr<SupplementalStatsEntry> clone() const override;

    AggregatedMetric<long> limit;
    AggregatedMetric<double> numCandidatesLimitRatio;
};

}  // namespace mongo::query_stats
