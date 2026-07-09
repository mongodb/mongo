/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
