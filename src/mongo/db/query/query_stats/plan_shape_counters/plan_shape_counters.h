// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_stats/plan_shape_counters/plan_access_path_counters.h"
#include "mongo/db/query/query_stats/plan_shape_counters/plan_node_counters.h"
#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_pattern_counters.h"

namespace mongo {
class QuerySolution;
namespace plan_shape_counters {

struct PlanShapeAnalysisResult {
    boost::optional<PlanShapeCounter> pattern;
    AccessPathCounts accessPathCounts;
    QsnNodeCounts qsnNodeCounts;

    /**
     * Increments each counter this plan matched in 'counts' by one.
     */
    void addTo(PlanShapeCounts& counts) const;
};

/*
 * Entry point for computing plan shape counters for query stats.
 * Takes a QuerySolution and computes three kinds of counters in a single traversal of the tree:
 *  - An enum to identify what pattern of query solution this is (see plan_shape_pattern_counters.h)
 *  - Counts of the access paths used in the qsn.
 *  - Counts of the types of nodes that appear in the QSN.
 */
PlanShapeAnalysisResult analyzePlanShapeForCounters(const QuerySolution& solution);

}  // namespace plan_shape_counters
}  // namespace mongo
