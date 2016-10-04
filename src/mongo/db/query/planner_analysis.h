/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

class Collection;

class QueryPlannerAnalysis {
public:
    /**
     * Checks solution nodes for geo match expressions to apply an optimization.
     *
     * If a geo match expression is on a field with a corresponding 2dsphere index we can
     * skip expensive validation of geometries in the matcher, since those geometries were
     * validated when they were indexed.
     */
    static void analyzeGeo(const QueryPlannerParams& params, QuerySolutionNode* solnRoot);

    /**
     * Takes an index key pattern and returns an object describing the "maximal sort" that this
     * index can provide.  Returned object is in normalized sort form (all elements have value 1
     * or -1).
     *
     * Examples:
     * - {a: 1, b: -1} => {a: 1, b: -1}
     * - {a: true} => {a: 1}
     * - {a: "hashed"} => {}
     * - {a: 1, b: "text", c: 1} => {a: 1}
     */
    static BSONObj getSortPattern(const BSONObj& indexKeyPattern);

    /**
     * In brief: performs sort and covering analysis.
     *
     * The solution rooted at 'solnRoot' provides data for the query, whether through some
     * configuration of indices or through a collection scan.  Additional stages may be required
     * to perform sorting, projection, or other operations that are independent of the source
     * of the data.  These stages are added atop 'solnRoot'.
     *
     * 'taggedRoot' is a copy of the parse tree.  Nodes in 'solnRoot' may point into it.
     *
     * Takes ownership of 'solnRoot' and 'taggedRoot'.
     *
     * Returns NULL if a solution cannot be constructed given the requirements in 'params'.
     *
     * Caller owns the returned QuerySolution.
     */
    static QuerySolution* analyzeDataAccess(const CanonicalQuery& query,
                                            const QueryPlannerParams& params,
                                            QuerySolutionNode* solnRoot);

    /**
     * Sort the results, if there is a sort required.
     */
    static QuerySolutionNode* analyzeSort(const CanonicalQuery& query,
                                          const QueryPlannerParams& params,
                                          QuerySolutionNode* solnRoot,
                                          bool* blockingSortOut);

    /**
     * Internal helper function used by analyzeSort.
     *
     * Rewrites an index scan over many point intervals as an OR of many index scans in order to
     * obtain an indexed sort.  For full details, see SERVER-1205.
     *
     * Here is an example:
     *
     * Consider the query find({a: {$in: [1,2]}}).sort({b: 1}) with using the index {a:1, b:1}.
     *
     * Our default solution will be to construct one index scan with the bounds a:[[1,1],[2,2]]
     * and b: [MinKey, MaxKey].
     *
     * However, this is logically equivalent to the union of the following scans:
     * a:[1,1], b:[MinKey, MaxKey]
     * a:[2,2], b:[MinKey, MaxKey]
     *
     * Since the bounds on 'a' are a point, each scan provides the sort order {b:1} in addition
     * to {a:1, b:1}.
     *
     * If we union these scans with a merge sort instead of a normal hashing OR, we can preserve
     * the sort order that each scan provides.
     */
    static bool explodeForSort(const CanonicalQuery& query,
                               const QueryPlannerParams& params,
                               QuerySolutionNode** solnRoot);
};

}  // namespace mongo
