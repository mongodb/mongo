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
     * MULTIKEY INDEX BOUNDS RULES
     *
     * 1. In general for a multikey index, we cannot intersect bounds
     * even if the index is not compound.
     *   Example:
     *   Let's say we have the document {a: [5, 7]}.
     *   This document satisfies the query {$and: [ {a: 5}, {a: 7} ] }
     *   For the index {a:1} we have the keys {"": 5} and {"": 7}.
     *   Each child of the AND is tagged with the index {a: 1}
     *   The interval for the {a: 5} branch is [5, 5].  It is exact.
     *   The interval for the {a: 7} branch is [7, 7].  It is exact.
     *   The intersection of the intervals is {}.
     *   If we scan over {}, the intersection of the intervals, we will retrieve nothing.
     *
     * 2. In general for a multikey compound index, we *can* compound the bounds.
     * For example, if we have multikey index {a: 1, b: 1} and query {a: 2, b: 3},
     * we can use the bounds {a: [[2, 2]], b: [[3, 3]]}.
     *
     * 3. Despite rule #2, if fields in the compound index share a prefix, then it
     * is not safe to compound the bounds. We can only specify bounds for the first
     * field.
     *   Example:
     *   Let's say we have the document {a: [ {b: 3}, {c: 4} ] }
     *   This document satisfies the query {'a.b': 3, 'a.c': 4}.
     *   For the index {'a.b': 1, 'a.c': 1} we have the keys {"": 3, "": null} and
     *                                                       {"": null, "": 4}.
     *   Let's use the aforementioned index to answer the query.
     *   The bounds for 'a.b' are [3,3], and the bounds for 'a.c' are [4,4].
     *   If we combine the bounds, we would only look at keys {"": 3, "":4 }.
     *   Therefore we wouldn't look at the document's keys in the index.
     *   Therefore we don't combine bounds.
     *
     * 4. There is an exception to rule #1, and that is when we're evaluating
     * an $elemMatch.
     *   Example:
     *   Let's say that we have the same document from (1), {a: [5, 7]}.
     *   This document satisfies {a: {$lte: 5, $gte: 7}}, but it does not
     *   satisfy {a: {$elemMatch: {$lte: 5, $gte: 7}}}. The $elemMatch indicates
     *   that we are allowed to intersect the bounds, which means that we will
     *   scan over the empty interval {} and retrieve nothing. This is the
     *   expected result because there is no entry in the array "a" that
     *   simultaneously satisfies the predicates a<=5 and a>=7.
     *
     * 5. There is also an exception to rule #3, and that is when we're evaluating
     * an $elemMatch. The bounds can be compounded for predicates that share a prefix
     * so long as the shared prefix is the path for which there is an $elemMatch.
     *   Example:
     *   Suppose we have the same document from (3), {a: [{b: 3}, {c: 4}]}. As discussed
     *   above, we cannot compound the index bounds for query {'a.b': 1, 'a.c': 1}.
     *   However, for the query {a: {$elemMatch: {b: 1, c: 1}} we can compound the
     *   bounds because the $elemMatch is applied to the shared prefix "a".
     */

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

        /**
         * Return a plan that scans the provided index from [startKey to endKey).
         */
        static QuerySolutionNode* makeIndexScan(const IndexEntry& index,
                                                const CanonicalQuery& query,
                                                const QueryPlannerParams& params,
                                                const BSONObj& startKey,
                                                const BSONObj& endKey);

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
         * Traverses the tree rooted at the $elemMatch expression 'node',
         * finding all predicates that can use an index directly and returning
         * them in the out-parameter vector 'out'.
         *
         * Traverses only through $and and array nodes like $all.
         *
         * Other nodes (i.e. nodes which cannot use an index directly, and which are
         * neither $and nor array nodes) are returned in 'subnodesOut' if they are
         * tagged to use an index.
         */
        static void findElemMatchChildren(const MatchExpression* node,
                                          vector<MatchExpression*>* out,
                                          vector<MatchExpression*>* subnodesOut);

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
        static QuerySolutionNode* makeLeafNode(const CanonicalQuery& query,
                                               const IndexEntry& index,
                                               size_t pos,
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
         * Determines whether it is safe to merge the expression 'expr' with
         * the leaf node of the query solution, 'node'.
         *
         * 'index' provides information about the index used by 'node'.
         * 'pos' gives the position in the index (for compound indices) that
         * 'expr' needs to use. Finally, 'mergeType' indicates whether we
         * will try to merge using an AND or OR.
         *
         * Does not take ownership of its arguments.
         */
        static bool shouldMergeWithLeaf(const MatchExpression* expr,
                                        const IndexEntry& index,
                                        size_t pos,
                                        QuerySolutionNode* node,
                                        MatchExpression::MatchType mergeType);

        /**
         * If index scan (regular or expression index), fill in any bounds that are missing in
         * 'node' with the "all values for this field" interval.
         *
         * If geo, do nothing.
         * If text, punt to finishTextNode.
         */
        static void finishLeafNode(QuerySolutionNode* node, const IndexEntry& index);

        static void finishTextNode(QuerySolutionNode* node, const IndexEntry& index);

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
