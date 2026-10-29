// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_stats/query_stats_entry.h"
#include "mongo/db/query/util/represent_as_util.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo::query_stats {

// Upper bound on the $limit value for the top-K optimization to activate. Beyond this the
// optimization's benefit diminishes and heap overhead is unjustified.
inline constexpr long long kTopKOptimizationMaxLimit = 10'000;

/**
 * Carries the sort/limit parameters set during optimization, so that the exec stage can perform a
 * bounded top-K sort without materializing every entry.
 */
struct TopKSortSpec {
    std::string dottedMetricsPath;  // e.g. "metrics.workingTimeMillis.sum"
    bool isAscending;
    long long limit;

    /**
     * Returns true iff sort key 'a' ranks better than 'b' under this ordering. The comparator
     * lives on the spec, so the exec stage and the heap share exactly one definition of "better".
     */
    bool isBetter(int64_t a, int64_t b) const {
        return isAscending ? a < b : a > b;
    }
};

/**
 * A function pointer that extracts a cheap scalar sort key from a QueryStatsEntry
 * without materializing BSON or computing query shapes.
 */
using CheapMetricAccessor = int64_t (*)(const QueryStatsEntry&);

struct MetricTableEntry {
    std::string_view path;
    CheapMetricAccessor accessor;
};

// To add a new metric: append an entry here. Do NOT add paths that require shape serialization
// (key, keyHash, queryShapeHash).
inline constexpr MetricTableEntry kMetricTable[] = {
    {"metrics.execCount",
     [](const QueryStatsEntry& e) -> int64_t {
         return representAsChecked<int64_t>(e.execCount);
     }},
    {"metrics.workingTimeMillis.max",
     [](const QueryStatsEntry& e) -> int64_t {
         return e.workingTimeMillis.getMax();
     }},
    {"metrics.workingTimeMillis.min",
     [](const QueryStatsEntry& e) -> int64_t {
         return e.workingTimeMillis.getMin();
     }},
    {"metrics.workingTimeMillis.sum",
     [](const QueryStatsEntry& e) -> int64_t {
         return e.workingTimeMillis.getSum();
     }},
};

/**
 * Returns a CheapMetricAccessor for the given dotted metrics path if it is in the supported
 * whitelist, or boost::none otherwise.
 */
inline boost::optional<CheapMetricAccessor> getCheapMetricAccessor(
    std::string_view dottedMetricsPath) {
    for (const auto& entry : kMetricTable) {
        if (dottedMetricsPath == entry.path) {
            return entry.accessor;
        }
    }
    return boost::none;
}

}  // namespace mongo::query_stats
