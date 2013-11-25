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
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    /**
     * Methods for creating a QuerySolutionNode tree that accesses the data required by the query.
     */
    class QueryPlannerAccess {
    public:
        /**
         * Return a CollectionScanNode that scans as requested in 'query'.
         */
        static QuerySolutionNode* makeCollectionScan(const CanonicalQuery& query,
                                                     bool tailable,
                                                     const QueryPlannerParams& params);

        /**
         * Return a plan that uses the provided index as a proxy for a collection scan.
         */
        static QuerySolutionNode* scanWholeIndex(const IndexEntry& index,
                                                 const CanonicalQuery& query,
                                                 const QueryPlannerParams& params,
                                                 int direction = 1);

        //
        // Indexed Data Access methods.
        //
        // The inArrayOperator flag deserves some attention.  It is set when we're processing a child of
        // a MatchExpression::ALL or MatchExpression::ELEM_MATCH_OBJECT.
        //
        // When true, the following behavior changes for all methods below that take it as an argument:
        // 0. No deletion of MatchExpression(s).  In fact,
        // 1. No mutation of the MatchExpression at all.  We need the tree as-is in order to perform
        //    a filter on the entire tree.
        // 2. No fetches performed.  There will be a final fetch by the caller of buildIndexedDataAccess
        //    who set the value of inArrayOperator to true.
        // 3. No compound indices are used and no bounds are combined.  These are incorrect in the context
        //    of these operators.
        //

        /**
         * If 'inArrayOperator' is false, takes ownership of 'root'.
         */
        static QuerySolutionNode* buildIndexedDataAccess(const CanonicalQuery& query,
                                                         MatchExpression* root,
                                                         bool inArrayOperator,
                                                         const vector<IndexEntry>& indices);

        /**
         * Takes ownership of 'root'.
         */
        static QuerySolutionNode* buildIndexedAnd(const CanonicalQuery& query,
                                                  MatchExpression* root,
                                                  bool inArrayOperator,
                                                  const vector<IndexEntry>& indices);

        /**
         * Takes ownership of 'root'.
         */
        static QuerySolutionNode* buildIndexedOr(const CanonicalQuery& query,
                                                 MatchExpression* root,
                                                 bool inArrayOperator,
                                                 const vector<IndexEntry>& indices);

        /**
         * Helper used by buildIndexedAnd and buildIndexedOr.
         *
         * The children of AND and OR nodes are sorted by the index that the subtree rooted at
         * that node uses.  Child nodes that use the same index are adjacent to one another to
         * facilitate grouping of index scans.  As such, the processing for AND and OR is
         * almost identical.
         *
         * See tagForSort and sortUsingTags in index_tag.h for details on ordering the children
         * of OR and AND.
         *
         * Does not take ownership of 'root' but may remove children from it.
         */
        static bool processIndexScans(const CanonicalQuery& query,
                                      MatchExpression* root,
                                      bool inArrayOperator,
                                      const vector<IndexEntry>& indices,
                                      vector<QuerySolutionNode*>* out);

        //
        // Helpers for creating an index scan.
        //

        /**
         * Create a new data access node.
         *
         * If the node is an index scan, the bounds for 'expr' are computed and placed into the
         * first field's OIL position.  The rest of the OILs are allocated but uninitialized.
         *
         * If the node is a geo node, grab the geo data from 'expr' and stuff it into the
         * geo solution node of the appropriate type.
         */
        static QuerySolutionNode* makeLeafNode(const IndexEntry& index,
                                               MatchExpression* expr,
                                               IndexBoundsBuilder::BoundsTightness* tightnessOut);

        /**
         * Merge the predicate 'expr' with the leaf node 'node'.
         */
        static void mergeWithLeafNode(MatchExpression* expr,
                                      const IndexEntry& index,
                                      size_t pos,
                                      IndexBoundsBuilder::BoundsTightness* tightnessOut,
                                      QuerySolutionNode* node,
                                      MatchExpression::MatchType mergeType);

        /**
         * If index scan (regular or expression index), fill in any bounds that are missing in
         * 'node' with the "all values for this field" interval.
         *
         * If geo, do nothing.
         */
        static void finishLeafNode(QuerySolutionNode* node, const IndexEntry& index);

        /**
         * Assumes each OIL in bounds is increasing.
         *
         * Aligns OILs (and bounds) according to the kp direction * the scanDir.
         */
        static void alignBounds(IndexBounds* bounds, const BSONObj& kp, int scanDir = 1);

    private:
        /**
         * Add the filter 'match' to the query solution node 'node'. Takes
         * ownership of 'match'.
         *
         * The MatchType, 'type', indicates whether 'match' is a child of an
         * AND or an OR match expression.
         */
        static void _addFilterToSolutionNode(QuerySolutionNode* node, MatchExpression* match,
                                             MatchExpression::MatchType type);
    };

}  // namespace mongo
