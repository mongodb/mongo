// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * This file contains tests for mongo/db/query/query_planner.cpp
 */

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

class QueryPlannerTestLib {
public:
    /**
     * Returns whether the BSON representation of the index bounds in
     * 'testBounds' matches 'trueBounds'.
     *
     * 'testBounds' should be of the following format:
     *    {<field 1>: <oil 1>, <field 2>: <oil 2>, ...}
     * Each ordered interval list (e.g. <oil 1>) is an array of arrays of
     * the format:
     *    [[<low 1>,<high 1>,<lowInclusive 1>,<highInclusive 1>], ...]
     *
     * For example,
     *    {a: [[1,2,true,false], [3,4,false,true]], b: [[-Infinity, Infinity]]}
     * Means that the index bounds on field 'a' consist of the two intervals
     * [1, 2) and (3, 4] and the index bounds on field 'b' are [-Infinity, Infinity].
     */
    static Status boundsMatch(const BSONObj& testBounds,
                              IndexBounds trueBounds,
                              bool relaxBoundsCheck);

    /**
     * @param testSoln -- a BSON representation of a query solution
     * @param trueSoln -- the root node of a query solution tree
     * @param: relaxBoundsCheck -- If 'true', will perform a relaxed "subset" check on index bounds.
     *         Will perform a full check otherwise.
     *
     * Returns Status::OK() if the BSON representation matches the actual tree, otherwise returns
     * a non-OK status indicating what did not match.
     */
    static Status solutionMatches(const BSONObj& testSoln,
                                  const QuerySolutionNode* trueSoln,
                                  bool relaxBoundsCheck = false);

    static Status solutionMatches(const std::string& testSoln,
                                  const QuerySolutionNode* trueSoln,
                                  bool relaxBoundsCheck = false) {
        return solutionMatches(fromjson(testSoln), trueSoln, relaxBoundsCheck);
    }
};

}  // namespace mongo
