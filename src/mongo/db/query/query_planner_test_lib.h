/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

/**
 * This file contains tests for mongo/db/query/query_planner.cpp
 */

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include <ostream>

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
    static bool boundsMatch(const BSONObj& testBounds,
                            const IndexBounds trueBounds,
                            bool relaxBoundsCheck);

    /**
     * @param testSoln -- a BSON representation of a query solution
     * @param trueSoln -- the root node of a query solution tree
     * @param: relaxBoundsCheck -- If 'true', will perform a relaxed "subset" check on index bounds.
     *         Will perform a full check otherwise.
     *
     * Returns true if the BSON representation matches the actual
     * tree, otherwise returns false.
     */
    static bool solutionMatches(const BSONObj& testSoln,
                                const QuerySolutionNode* trueSoln,
                                bool relaxBoundsCheck = false);

    static bool solutionMatches(const std::string& testSoln,
                                const QuerySolutionNode* trueSoln,
                                bool relaxBoundsCheck = false) {
        return solutionMatches(fromjson(testSoln), trueSoln, relaxBoundsCheck);
    }
};

}  // namespace mongo
