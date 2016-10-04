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
     * Building the leaves (i.e. the index scans) is done by looping through
     * predicates one at a time. During the process, there is a fair amount of state
     * information to keep track of, which we consolidate into this data structure.
     */
    struct ScanBuildingState {
        ScanBuildingState(MatchExpression* theRoot,
                          bool inArrayOp,
                          const std::vector<IndexEntry>& indexList)
            : root(theRoot),
              inArrayOperator(inArrayOp),
              indices(indexList),
              currentScan(nullptr),
              curChild(0),
              currentIndexNumber(IndexTag::kNoIndex),
              ixtag(NULL),
              tightness(IndexBoundsBuilder::INEXACT_FETCH),
              curOr(nullptr),
              loosestBounds(IndexBoundsBuilder::EXACT) {}

        /**
         * Reset the scan building state in preparation for building a new scan.
         *
         * This always should be called prior to allocating a new 'currentScan'.
         */
        void resetForNextScan(IndexTag* newTag) {
            currentScan.reset(NULL);
            currentIndexNumber = newTag->index;
            tightness = IndexBoundsBuilder::INEXACT_FETCH;
            loosestBounds = IndexBoundsBuilder::EXACT;

            if (MatchExpression::OR == root->matchType()) {
                curOr.reset(new OrMatchExpression());
            }
        }

        // The root of the MatchExpression tree for which we are currently building index
        // scans. Should be either an AND node or an OR node.
        MatchExpression* root;

        // Are we inside an array operator such as $elemMatch or $all?
        bool inArrayOperator;

        // A list of relevant indices which 'root' may be tagged to use.
        const std::vector<IndexEntry>& indices;

        // The index access node that we are currently constructing. We may merge
        // multiple tagged predicates into a single index scan.
        std::unique_ptr<QuerySolutionNode> currentScan;

        // An index into the child vector of 'root'. Indicates the child MatchExpression
        // for which we are currently either constructing a new scan or which we are about
        // to merge with 'currentScan'.
        size_t curChild;

        // An index into the 'indices', so that 'indices[currentIndexNumber]' gives the
        // index used by 'currentScan'. If there is no currentScan, this should be set
        // to 'IndexTag::kNoIndex'.
        size_t currentIndexNumber;

        // The tag on 'curChild'.
        IndexTag* ixtag;

        // Whether the bounds for predicate 'curChild' are exact, inexact and covered by
        // the index, or inexact with a fetch required.
        IndexBoundsBuilder::BoundsTightness tightness;

        // If 'root' is an $or, the child predicates which are tagged with the same index are
        // detached from the original root and added here. 'curOr' may be attached as a filter
        // later on, or ignored and cleaned up by the unique_ptr.
        std::unique_ptr<MatchExpression> curOr;

        // The values of BoundsTightness range from loosest to tightest in this order:
        //
        //   INEXACT_FETCH < INEXACT_COVERED < EXACT
        //
        // 'loosestBounds' stores the smallest of these three values encountered so far for
        // the current scan. If at least one of the child predicates assigned to the current
        // index is INEXACT_FETCH, then 'loosestBounds' is INEXACT_FETCH. If at least one of
        // the child predicates assigned to the current index is INEXACT_COVERED but none are
        // INEXACT_FETCH, then 'loosestBounds' is INEXACT_COVERED.
        IndexBoundsBuilder::BoundsTightness loosestBounds;

    private:
        // Default constructor is not allowed.
        ScanBuildingState();
    };

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
    // The inArrayOperator flag deserves some attention.  It is set when we're processing a
    // child of an MatchExpression::ELEM_MATCH_OBJECT.
    //
    // When true, the following behavior changes for all methods below that take it as an argument:
    // 0. No deletion of MatchExpression(s).  In fact,
    // 1. No mutation of the MatchExpression at all.  We need the tree as-is in order to perform
    //    a filter on the entire tree.
    // 2. No fetches performed.  There will be a final fetch by the caller of buildIndexedDataAccess
    //    who set the value of inArrayOperator to true.
    // 3. No compound indices are used and no bounds are combined.  These are
    //    incorrect in the context of these operators.
    //

    /**
     * If 'inArrayOperator' is false, takes ownership of 'root'.
     */
    static QuerySolutionNode* buildIndexedDataAccess(const CanonicalQuery& query,
                                                     MatchExpression* root,
                                                     bool inArrayOperator,
                                                     const std::vector<IndexEntry>& indices,
                                                     const QueryPlannerParams& params);

    /**
     * Takes ownership of 'root'.
     */
    static QuerySolutionNode* buildIndexedAnd(const CanonicalQuery& query,
                                              MatchExpression* root,
                                              bool inArrayOperator,
                                              const std::vector<IndexEntry>& indices,
                                              const QueryPlannerParams& params);

    /**
     * Takes ownership of 'root'.
     */
    static QuerySolutionNode* buildIndexedOr(const CanonicalQuery& query,
                                             MatchExpression* root,
                                             bool inArrayOperator,
                                             const std::vector<IndexEntry>& indices,
                                             const QueryPlannerParams& params);

    /**
     * Traverses the tree rooted at the $elemMatch expression 'node',
     * finding all predicates that can use an index directly and returning
     * them in the out-parameter vector 'out'.
     *
     * Traverses only through AND and ELEM_MATCH_OBJECT nodes.
     *
     * Other nodes (i.e. nodes which cannot use an index directly, and which are
     * neither AND nor ELEM_MATCH_OBJECT) are returned in 'subnodesOut' if they are
     * tagged to use an index.
     */
    static void findElemMatchChildren(const MatchExpression* node,
                                      std::vector<MatchExpression*>* out,
                                      std::vector<MatchExpression*>* subnodesOut);

    /**
     * Given a list of OR-related subtrees returned by processIndexScans(), looks for logically
     * equivalent IndexScanNodes and combines them. This is an optimization to avoid creating
     * plans that repeat index access work.
     *
     * Example:
     *  Suppose processIndexScans() returns a list of the following three query solutions:
     *    1) IXSCAN (bounds: {b: [[2,2]]})
     *    2) FETCH (filter: {d:1}) -> IXSCAN (bounds: {c: [[3,3]]})
     *    3) FETCH (filter: {e:1}) -> IXSCAN (bounds: {c: [[3,3]]})
     *  This method would collapse scans #2 and #3, resulting in the following output:
     *    1) IXSCAN (bounds: {b: [[2,2]]})
     *    2) FETCH (filter: {$or:[{d:1}, {e:1}]}) -> IXSCAN (bounds: {c: [[3,3]]})
     *
     * Used as a helper for buildIndexedOr().
     *
     * Takes ownership of 'scans'. The caller assumes ownership of the pointers in the returned
     * list of QuerySolutionNode*.
     */
    static std::vector<QuerySolutionNode*> collapseEquivalentScans(
        const std::vector<QuerySolutionNode*> scans);

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
                                  const std::vector<IndexEntry>& indices,
                                  const QueryPlannerParams& params,
                                  std::vector<QuerySolutionNode*>* out);

    /**
     * Used by processIndexScans(...) in order to recursively build a data access
     * plan for a "subnode", a node in the MatchExpression tree which is indexed by
     * virtue of its children.
     *
     * The resulting scans are outputted in the out-parameter 'out'.
     */
    static bool processIndexScansSubnode(const CanonicalQuery& query,
                                         ScanBuildingState* scanState,
                                         const QueryPlannerParams& params,
                                         std::vector<QuerySolutionNode*>* out);

    /**
     * Used by processIndexScansSubnode(...) to build the leaves of the solution tree for an
     * ELEM_MATCH_OBJECT node beneath an AND.
     *
     * The resulting scans are outputted in the out-parameter 'out'.
     */
    static bool processIndexScansElemMatch(const CanonicalQuery& query,
                                           ScanBuildingState* scanState,
                                           const QueryPlannerParams& params,
                                           std::vector<QuerySolutionNode*>* out);

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
    static void mergeWithLeafNode(MatchExpression* expr, ScanBuildingState* scanState);

    /**
     * Determines whether it is safe to merge the expression 'expr' with
     * the leaf node of the query solution contained in 'scanState'.
     *
     * Does not take ownership of its arguments.
     */
    static bool shouldMergeWithLeaf(const MatchExpression* expr,
                                    const ScanBuildingState& scanState);

    /**
     * If index scan (regular or expression index), fill in any bounds that are missing in
     * 'node' with the "all values for this field" interval.
     *
     * If geo, do nothing.
     * If text, punt to finishTextNode.
     */
    static void finishLeafNode(QuerySolutionNode* node, const IndexEntry& index);

    /**
     * Fills in any missing bounds by calling finishLeafNode(...) for the scan contained in
     * 'scanState'. The resulting scan is outputted in the out-parameter 'out', transferring
     * ownership in the process.
     *
     * If 'scanState' is building an index scan for OR-related predicates, filters
     * may be affixed to the scan as necessary.
     */
    static void finishAndOutputLeaf(ScanBuildingState* scanState,
                                    std::vector<QuerySolutionNode*>* out);

    /**
     * Returns true if the current scan in 'scanState' requires a FetchNode.
     */
    static bool orNeedsFetch(const ScanBuildingState* scanState);

    static void finishTextNode(QuerySolutionNode* node, const IndexEntry& index);

    /**
     * Add the filter 'match' to the query solution node 'node'. Takes
     * ownership of 'match'.
     *
     * The MatchType, 'type', indicates whether 'match' is a child of an
     * AND or an OR match expression.
     */
    static void addFilterToSolutionNode(QuerySolutionNode* node,
                                        MatchExpression* match,
                                        MatchExpression::MatchType type);

    /**
     * Once a predicate is merged into the current scan, there are a few things we might
     * want to do with the filter:
     *   1) Detach the filter from its parent and delete it because the predicate is
     *   answered by exact index bounds.
     *   2) Leave the filter alone so that it can be affixed as part of a fetch node later.
     *   3) Detach the filter from its parent and attach it directly to an index scan node.
     *   We can sometimes due this for INEXACT_COVERED predicates which are not answered exactly
     *   by the bounds, but can be answered by examing the data in the index key.
     *   4) Detach the filter from its parent and attach it as a child of a separate
     *   MatchExpression tree. This is done for proper handling of inexact bounds for $or
     *   queries.
     *
     * This executes one of the four options above, according to the data in 'scanState'.
     */
    static void handleFilter(ScanBuildingState* scanState);

    /**
     * Implements handleFilter(...) for OR queries.
     */
    static void handleFilterAnd(ScanBuildingState* scanState);

    /**
     * Implements handleFilter(...) for AND queries.
     */
    static void handleFilterOr(ScanBuildingState* scanState);
};

}  // namespace mongo
