// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/aggregated_metric.h"

#include "mongo/bson/bsonobjbuilder.h"

#include <string_view>

namespace mongo::query_stats {
using namespace std::literals::string_view_literals;
namespace agg_metric_detail {
template void AggregatedMetric<uint64_t>::appendTo(BSONObjBuilder& builder,
                                                   std::string_view fieldName) const;
}  // namespace agg_metric_detail

void AggregatedBool::appendTo(BSONObjBuilder& builder, std::string_view fieldName) const {
    BSONObjBuilder{builder.subobjStart(fieldName)}
        .append("true"sv, static_cast<long long>(trueCount))
        .append("false"sv, static_cast<long long>(falseCount));
}

void AggregatedCardinalityEstimationMethods::appendTo(BSONObjBuilder& builder,
                                                      std::string_view fieldName) const {
    BSONObjBuilder{builder.subobjStart(fieldName)}
        .append("Histogram", static_cast<long long>(counts.getHistogram().value_or(0)))
        .append("Sampling", static_cast<long long>(counts.getSampling().value_or(0)))
        .append("Heuristics", static_cast<long long>(counts.getHeuristics().value_or(0)))
        .append("Mixed", static_cast<long long>(counts.getMixed().value_or(0)))
        .append("Metadata", static_cast<long long>(counts.getMetadata().value_or(0)))
        .append("Code", static_cast<long long>(counts.getCode().value_or(0)));
}
}  // namespace mongo::query_stats
