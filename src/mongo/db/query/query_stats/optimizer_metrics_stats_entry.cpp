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

#include "mongo/db/query/query_stats/optimizer_metrics_stats_entry.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"
#include "mongo/util/assert_util.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::query_stats {

void OptimizerMetricsClassicStatsEntry::appendTo(BSONObjBuilder& builder) const {
    BSONObjBuilder metricsEntryBuilder = builder.subobjStart(toStringData(metricType));
    metricsEntryBuilder.append("updateCount", static_cast<long long>(updateCount));
    optimizationTimeMicros.appendTo(metricsEntryBuilder, "optimizationTimeMicros");
}

void OptimizerMetricsClassicStatsEntry::updateStats(const SupplementalStatsEntry* other) {
    const OptimizerMetricsClassicStatsEntry* updateVal =
        dynamic_cast<const OptimizerMetricsClassicStatsEntry*>(other);
    tassert(8198701, "Unexpected type of statistic metric", updateVal != nullptr);
    optimizationTimeMicros.combine(updateVal->optimizationTimeMicros);
    updateCount++;
}

std::unique_ptr<SupplementalStatsEntry> OptimizerMetricsClassicStatsEntry::clone() const {
    return std::make_unique<OptimizerMetricsClassicStatsEntry>(*this);
}

void OptimizerMetricsBonsaiStatsEntry::appendTo(BSONObjBuilder& builder) const {
    BSONObjBuilder metricsEntryBuilder = builder.subobjStart(toStringData(metricType));
    metricsEntryBuilder.append("updateCount", static_cast<long long>(updateCount));
    optimizationTimeMicros.appendTo(metricsEntryBuilder, "optimizationTimeMicros");
    estimatedCost.appendTo(metricsEntryBuilder, "estimatedCost");
    estimatedCardinality.appendTo(metricsEntryBuilder, "estimatedCardinality");
}

std::unique_ptr<SupplementalStatsEntry> OptimizerMetricsBonsaiStatsEntry::clone() const {
    return std::make_unique<OptimizerMetricsBonsaiStatsEntry>(*this);
}

void OptimizerMetricsBonsaiStatsEntry::updateStats(const SupplementalStatsEntry* other) {
    const OptimizerMetricsBonsaiStatsEntry* updateVal =
        dynamic_cast<const OptimizerMetricsBonsaiStatsEntry*>(other);
    tassert(8198702, "Unexpected type of statistic metric", updateVal != nullptr);
    optimizationTimeMicros.combine(updateVal->optimizationTimeMicros);
    estimatedCost.combine(updateVal->estimatedCost);
    estimatedCardinality.combine(updateVal->estimatedCardinality);
    updateCount++;
}

}  // namespace mongo::query_stats
