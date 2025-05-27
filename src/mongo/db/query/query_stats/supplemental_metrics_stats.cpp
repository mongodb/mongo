/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
            if (opDebug.estimatedCost && opDebug.estimatedCardinality) {
                supplementalMetrics.emplace_back(std::make_unique<OptimizerMetricsBonsaiStatsEntry>(
                    opDebug.planningTime.count(),
                    *opDebug.estimatedCost,
                    *opDebug.estimatedCardinality,
                    metricType));
            } else {
                supplementalMetrics.emplace_back(
                    std::make_unique<OptimizerMetricsClassicStatsEntry>(
                        opDebug.planningTime.count(), metricType));
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
