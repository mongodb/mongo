// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/op_debug.h"
#include "mongo/db/query/util/named_enum.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::query_stats {

/**
 * Defined an enum class that list all the optional metrics supported.
 */
#define SUPPLEMENTAL_METRIC_STATS_TYPE(X) \
    X(Unknown)                            \
    X(BonsaiM2)                           \
    X(BonsaiM4)                           \
    X(ForceBonsai)                        \
    X(SBE)                                \
    X(Classic)                            \
    X(VectorSearch)

QUERY_UTIL_NAMED_ENUM_DEFINE(SupplementalMetricType, SUPPLEMENTAL_METRIC_STATS_TYPE);
#undef SUPPLEMENTAL_METRIC_STATS_TYPE

/**
 * Supplemental metrics entry base class. All supplemental metrics must derive from this class. This
 * class represents metrics stored in SupplementalMetricsStats map.
 */
class SupplementalStatsEntry {
public:
    SupplementalStatsEntry(const SupplementalMetricType metricType) : metricType(metricType) {}
    /**
     *  The method updates aggregated values with the values provided in the other argument. The
     *  other argument is casted to the appropriate type in the derived class methods.
     */
    virtual void updateStats(const SupplementalStatsEntry* other) = 0;
    /**
     * Append the metrics values to the provided builder.
     */
    virtual void appendTo(BSONObjBuilder& builder) const = 0;
    const SupplementalMetricType metricType;

    virtual std::unique_ptr<SupplementalStatsEntry> clone() const = 0;

    virtual ~SupplementalStatsEntry() {}
};

/**
 * Supplemental metrics storage. The map allocated on demand if there is a metric to be stored and
 * and accessed.
 */
class SupplementalStatsMap {
public:
    /**
     * Prints all optional metrics stored in the map to the BSONObj.
     */
    BSONObj toBSON() const;
    /**
     * Finds the optinal stats entry in the map by the metricType and inserts or updates it.
     */
    void update(std::unique_ptr<SupplementalStatsEntry>);

    std::unique_ptr<SupplementalStatsMap> clone() const;

private:
    mongo::stdx::unordered_map<SupplementalMetricType, clonable_ptr<SupplementalStatsEntry>>
        _metrics;
};

/**
 * Computes supplemental query stats metrics for the operation represented by 'opDebug'.
 */
std::vector<std::unique_ptr<SupplementalStatsEntry>> computeSupplementalQueryStatsMetrics(
    const OpDebug& opDebug);

}  // namespace mongo::query_stats
