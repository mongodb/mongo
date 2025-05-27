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

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/op_debug.h"
#include "mongo/db/query/util/named_enum.h"

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
