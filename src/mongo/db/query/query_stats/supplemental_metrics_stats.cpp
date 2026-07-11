// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_stats/optimizer_metrics_stats_entry.h"
#include "mongo/db/query/query_stats/vector_search_stats_entry.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryStats

namespace mongo::query_stats {

namespace {
void maybeAddOptimizerMetrics(
    const OpDebug& opDebug,
    std::vector<std::unique_ptr<SupplementalStatsEntry>>& supplementalMetrics) {
    if (internalQueryCollectOptimizerMetrics.load()) {
        auto metricType(SupplementalMetricType::Unknown);

        switch (opDebug.queryFramework) {
            case PlanExecutor::QueryFramework::kClassicOnly:
            case PlanExecutor::QueryFramework::kClassicHybrid:
                metricType = SupplementalMetricType::Classic;
                break;
            case PlanExecutor::QueryFramework::kSBEOnly:
            case PlanExecutor::QueryFramework::kSBEHybrid:
                metricType = SupplementalMetricType::SBE;
                break;
            case PlanExecutor::QueryFramework::kUnknown:
                break;
        }

        if (metricType != SupplementalMetricType::Unknown) {
            const auto& planningTimeMicros =
                opDebug.getAdditiveMetrics().planningTime.value_or(Microseconds{0}).count();
            if (opDebug.estimatedCost && opDebug.estimatedCardinality) {
                supplementalMetrics.emplace_back(std::make_unique<OptimizerMetricsBonsaiStatsEntry>(
                    planningTimeMicros,
                    *opDebug.estimatedCost,
                    *opDebug.estimatedCardinality,
                    metricType));
            } else {
                supplementalMetrics.emplace_back(
                    std::make_unique<OptimizerMetricsClassicStatsEntry>(planningTimeMicros,
                                                                        metricType));
            }
        }
    }
}

void maybeAddVectorSearchMetrics(
    const OpDebug& opDebug,
    std::vector<std::unique_ptr<SupplementalStatsEntry>>& supplementalMetrics) {
    if (const auto& metrics = opDebug.vectorSearchMetrics) {
        supplementalMetrics.emplace_back(std::make_unique<VectorSearchStatsEntry>(
            metrics->limit, metrics->numCandidatesLimitRatio));
    }
}
}  // namespace

BSONObj SupplementalStatsMap::toBSON() const {
    BSONObjBuilder builder{sizeof(SupplementalStatsMap)};
    for (const auto& entry : _metrics) {
        entry.second->appendTo(builder);
    }
    return builder.obj();
}

void SupplementalStatsMap::update(std::unique_ptr<SupplementalStatsEntry> metric) {
    auto&& [metricEntry, wasInserted] = _metrics.try_emplace(metric->metricType, std::move(metric));
    if (!wasInserted) {
        metricEntry->second->updateStats(&*(std::move(metric)));
    }
}
std::unique_ptr<SupplementalStatsMap> SupplementalStatsMap::clone() const {
    return std::make_unique<SupplementalStatsMap>(*this);
}

std::vector<std::unique_ptr<SupplementalStatsEntry>> computeSupplementalQueryStatsMetrics(
    const OpDebug& opDebug) {
    std::vector<std::unique_ptr<SupplementalStatsEntry>> supplementalMetrics;
    maybeAddOptimizerMetrics(opDebug, supplementalMetrics);
    maybeAddVectorSearchMetrics(opDebug, supplementalMetrics);
    return supplementalMetrics;
}

}  // namespace mongo::query_stats
