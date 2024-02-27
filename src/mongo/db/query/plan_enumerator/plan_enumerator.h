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

#pragma once

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/plan_enumerator/enumerator_memo.h"
#include "mongo/db/query/plan_enumerator/plan_enumerator_explain_info.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace plan_enumerator {

using namespace plan_enumerator;

struct PlanEnumeratorParams {
    PlanEnumeratorParams()
        : maxSolutionsPerOr(internalQueryEnumerationMaxOrSolutions.load()),
          maxIntersectPerAnd(internalQueryEnumerationMaxIntersectPerAnd.load()),
          disableOrPushdown(disableMatchExpressionOptimization.shouldFail()) {}

    // Do we provide solutions that use more indices than the minimum required to provide
    // an indexed solution?
    bool intersect = false;

    // Do we enumerate children of an $or in a special order to prioritize solutions which have the
    // same assignment on each branch?
    bool enumerateOrChildrenLockstep = false;

    // Not owned here.
    MatchExpression* root;

    // Not owned here.
    const std::vector<IndexEntry>* indices;

    // How many plans are we willing to ouput from an OR? We currently consider
    // all possibly OR plans, which means the product of the number of possibilities
    // for each clause of the OR. This could grow disastrously large.
    size_t maxSolutionsPerOr;

    // How many intersect plans are we willing to output from an AND?  Given that we pursue an
    // all-pairs approach, we could wind up creating a lot of enumeration possibilities for
    // certain inputs.
    size_t maxIntersectPerAnd;

    // Whether to disable OR-pushdown optimization. OR-pushdown assumes that the expression has been
    // simplified: for example, that single-child $or nodes are unwrapped. To avoid this, when
    // the 'disableMatchExpressionOptimization' failpoint is set, we also disable OR-pushdown.
    bool disableOrPushdown;

    const projection_ast::Projection* projection;
    const boost::optional<SortPattern>* sort;
    BSONObj shardKey;
};

/**
 * Provides elements from the power set of possible indices to use.  Uses the available
 * predicate information to make better decisions about what indices are best.
 */
class PlanEnumerator {
    PlanEnumerator(const PlanEnumerator&) = delete;
    PlanEnumerator& operator=(const PlanEnumerator&) = delete;

public:
    /**
     * Constructs an enumerator for the query specified in 'root' which is tagged with
     * RelevantTag(s).  The index patterns mentioned in the tags are described by 'indices'.
     *
     * Does not take ownership of any arguments.  They must outlive any calls to getNext(...).
     */
    PlanEnumerator(const PlanEnumeratorParams& params);

    /**
     * Returns OK and performs a sanity check on the input parameters and prepares the
     * internal state so that getNext() can be called. Returns an error status with a
     * description if the sanity check failed.
     */
    Status init();

    /**
     * Outputs a possible plan. Leaves in the plan are tagged with an index to use. Returns a
     * MatchExpression representing a point in the query tree (which can be used to build a
     * QuerySolutionNode tree) or nullptr if no more plans will be outputted. While owned by the
     * caller, the MatchExpression returned points into data that is owned elsewhere.
     *
     * Nodes in 'tree' are tagged with indices that should be used to answer the tagged nodes. Only
     * nodes that have a field name (getCategory() != kLogical) will be tagged.
     *
     * The output tree is a clone identical to that used to initialize the enumerator, with tags
     * added in order to indicate index usage.
     */
    std::unique_ptr<MatchExpression> getNext();

    PlanEnumeratorExplainInfo _explainInfo;

private:
    //
    // Memoization strategy
    //

    /**
     * Represents the route that an outside predicate has taken during the PlanEnumerator's
     * recursive descent of the match expression tree.
     */
    struct OutsidePredRoute {
        /**
         * Whether or not the route has traversed through an $elemMatch object node. This is needed
         * because it is not correct to push down a predicate through an $elemMatch object.
         */
        bool traversedThroughElemMatchObj = false;

        /**
         * The route of the outside predicate. This starts at the indexed OR sibling of the
         * predicate.  Each value in 'route' is the index of a child in an indexed OR.
         *
         * For example, if the MatchExpression tree is:
         *         AND
         *        /    \
         *   {a: 5}    OR
         *           /    \
         *         AND    {e: 9}
         *       /     \
         *    {b: 6}   OR
         *           /    \
         *       {c: 7}  {d: 8}
         *
         * and the predicate is {a: 5}, then the route will be {0, 1} when the recursive descent
         * reaches {d: 8}.
         */
        std::deque<size_t> route;
    };

    struct PrepMemoContext {
        PrepMemoContext() : elemMatchExpr(nullptr) {}
        MatchExpression* elemMatchExpr;

        // Maps from indexable predicates that can be pushed into the current node to the route
        // through ORs that they have taken to get to this node.
        stdx::unordered_map<MatchExpression*, OutsidePredRoute> outsidePreds;
    };

    /**
     * Traverses the match expression and generates the memo structure from it.
     * Returns true if the provided node uses an index, false otherwise.
     */
    bool prepMemo(MatchExpression* node, const PrepMemoContext& context);

    /**
     * Traverses the memo structure and annotates the tree with IndexTags for the chosen
     * indices.
     */
    void tagMemo(MemoID id);

    /**
     * Move to the next enumeration state.  Each assignment stores its own enumeration state.
     * See the various ____Assignment classes below for details on enumeration state.
     *
     * Returns true if the memo subtree with root 'node' has no further enumeration states.  In
     * this case, that subtree restarts its enumeration at the beginning state.  This implies
     * that the parent of node should move to the next state.  If 'node' is the root of the
     * tree, we are done with enumeration.
     *
     * The return of this function can be thought of like a 'carry' in addition.
     *
     * Returns false if the memo subtree has moved to the next state.
     */
    bool nextMemo(MemoID id);

    /**
     * A short word on the memo structure.
     *
     * The PlanEnumerator is interested in matching predicates and indices.  Predicates
     * are leaf nodes in the parse tree.  {x:5}, {x: {$geoWithin:...}} are both predicates.
     *
     * Since the output of an OR is the union of its results, each of its children must be
     * indexed for the entire OR to be indexed.  If each subtree of an OR is indexable, the
     * OR is as well.
     *
     * For an AND to be indexed, only one of its children must be indexed.  AND is an
     * intersection of its children, so each of its children describes a superset of the
     * produced results.
     *
     * Leaf predicates are also given AND assignments, since they may index outside predicates that
     * have been pushed down through OR nodes.
     */

    /**
     * Allocates a NodeAssignment and associates it with the provided 'expr'.
     *
     * The unique MemoID of the new assignment is outputted in '*id'.
     * The out parameter '*slot' points to the newly allocated NodeAssignment.
     */
    std::pair<MemoID, NodeAssignment*> allocateAssignment(MatchExpression* expr);

    /**
     * Predicates inside $elemMatch's that are semantically "$and of $and"
     * predicates are not rewritten to the top-level during normalization.
     * However, we would like to make predicates inside $elemMatch available
     * for combining index bounds with the top-level $and predicates.
     *
     * This function deeply traverses $and and $elemMatch expressions of
     * the tree rooted at 'node', adding all preds that can use an index
     * to the output vector 'indexOut'. At the same time, $elemMatch
     * context information is stashed in the tags so that we don't lose
     * information due to flattening.
     *
     * Does not take ownership of arguments.
     *
     * Returns false if the AND cannot be indexed. Otherwise returns true.
     */
    void getIndexedPreds(MatchExpression* node,
                         const PrepMemoContext& context,
                         std::vector<MatchExpression*>* indexOut);

    /**
     * Recursively traverse 'node', with OR nodes as the base case. The OR nodes are not
     * explored--instead we call prepMemo() on the OR subnode, and add its assignment to the output.
     * Subnodes are "mandatory" if they *must* use an index (TEXT and GEO).
     * Returns a boolean indicating whether all mandatory subnodes can be indexed and returns false
     * to stop enumerating alternatives for an indexed OR.
     */
    bool prepSubNodes(MatchExpression* node,
                      const PrepMemoContext& context,
                      std::vector<MemoID>* subnodesOut,
                      std::vector<MemoID>* mandatorySubnodes);

    /**
     * Finds a set of predicates that can be safely compounded with the set
     * of predicates in 'assigned', under the assumption that we are assigning
     * predicates to a compound, multikey index.
     *
     * The list of candidate predicates that we could compound is passed
     * in 'couldCompound'. A subset of these predicates that is safe to
     * combine by compounding is returned in the out-parameter 'out'.
     *
     * Does not take ownership of its arguments.
     *
     * The rules for when to compound for multikey indices are reasonably
     * complex, and are dependent on the structure of $elemMatch's used
     * in the query. Ignoring $elemMatch for the time being, the rule is this:
     *
     *   "Any set of predicates for which no two predicates share a path
     *    prefix can be compounded."
     *
     * Suppose we have predicates over paths 'a.b' and 'a.c'. These cannot
     * be compounded because they share the prefix 'a'. Similarly, the bounds
     * for 'a' and 'a.b' cannot be compounded (in the case of multikey index
     * {a: 1, 'a.b': 1}). You *can* compound predicates over the paths 'a.b.c',
     * 'd', and 'e.b.c', because there is no shared prefix.
     *
     * The rules are different in the presence of $elemMatch. For $elemMatch
     * {a: {$elemMatch: {<pred1>, ..., <predN>}}}, we are allowed to compound
     * bounds for pred1 through predN, even though these predicates share the
     * path prefix 'a'. However, we still cannot compound in the case of
     * {a: {$elemMatch: {'b.c': {$gt: 1}, 'b.d': 5}}} because 'b.c' and 'b.d'
     * share a prefix. In other words, what matters inside an $elemMatch is not
     * the absolute prefix, but rather the "relative prefix" after the shared
     * $elemMatch part of the path.
     *
     * A few more examples:
     *    1) {'a.b': {$elemMatch: {c: {$gt: 1}, d: 5}}}. In this case, we can
     *    compound, because the $elemMatch is applied to the shared part of
     *    the path 'a.b'.
     *
     *    2) {'a.b': 1, a: {$elemMatch: {b: {$gt: 0}}}}. We cannot combine the
     *    bounds here because the prefix 'a' is shared by two predicates which
     *    are not joined together by an $elemMatch.
     *
     * NOTE:
     *   Usually 'assigned' has just one predicate. However, in order to support
     *   mandatory predicate assignment (TEXT and GEO_NEAR), we allow multiple
     *   already-assigned predicates to be passed. If a mandatory predicate is over
     *   a trailing field in a multikey compound index, then we assign both a predicate
     *   over the leading field as well as the mandatory predicate prior to calling
     *   this function.
     *
     *   Ex:
     *      Say we have index {a: 1, b: 1, c: "2dsphere", d: 1} as well as a $near
     *      predicate and a $within predicate over "c". The $near predicate is mandatory
     *      and must be assigned. The $within predicate is not mandatory. Furthermore,
     *      it cannot be assigned in addition to the $near predicate because the index
     *      is multikey.
     *
     *      In this case the enumerator must assign the $near predicate, and pass it in
     *      in 'assigned'. Otherwise it would be possible to assign the $within predicate,
     *      and then not assign the $near because the $within is already assigned (and
     *      has the same path).
     */
    void getMultikeyCompoundablePreds(const std::vector<MatchExpression*>& assigned,
                                      const std::vector<MatchExpression*>& couldCompound,
                                      std::vector<MatchExpression*>* out);

    /**
     * Assigns predicates from 'couldAssign' to 'indexAssignment' that can safely be assigned
     * according to the intersecting and compounding rules for multikey indexes. The rules can
     * loosely be stated as follows:
     *
     *   - It is always safe to assign a predicate on path Y to the index when no prefix of the path
     *     Y causes the index to be multikey.
     *
     *   - For any non-$elemMatch predicate on path X already assigned to the index, it isn't safe
     *     to assign a predicate on path Y (possibly equal to X) to the index when a shared prefix
     *     of the paths X and Y causes the index to be multikey.
     *
     *   - For any $elemMatch predicate on path X already assigned to the index, it isn't safe to
     *     assign a predicate on path Y (possibly equal to X) to the index when
     *       (a) a shared prefix of the paths X and Y causes the index to be multikey and the
     *           predicates aren't joined by the same $elemMatch context, or
     *       (b) a shared prefix of the paths X and Y inside the innermost $elemMatch causes the
     *           index to be multikey.
     *
     * If a predicate in 'couldAssign' is also in 'outsidePreds', then it is assumed to be an
     * outside predicate that was pushed down through an OR. Instead of adding it to
     * indexAssignment->preds, we add it to indexAssignment->orPushdowns. We create its
     * OrPushdownTag using the route specified in 'outsidePreds'.
     *
     * This function should only be called if the index has path-level multikey information.
     * Otherwise, getMultikeyCompoundablePreds() and compound() should be used instead.
     */
    void assignMultikeySafePredicates(
        const std::vector<MatchExpression*>& couldAssign,
        const stdx::unordered_map<MatchExpression*, OutsidePredRoute>& outsidePreds,
        OneIndexAssignment* indexAssignment);

    /**
     * 'andAssignment' contains assignments that we've already committed to outputting,
     * including both single index assignments and ixisect assignments.
     *
     * 'ixisectAssigned' is a set of predicates that we are about to add to 'andAssignment'
     * as an index intersection assignment.
     *
     * Returns true if an single index assignment which is already in 'andAssignment'
     * contains a superset of the predicates in 'ixisectAssigned'. This means that we
     * can assign the same preds to a compound index rather than using index intersection.
     *
     * Ex.
     *   Suppose we have indices {a: 1}, {b: 1}, and {a: 1, b: 1} with query
     *   {a: 2, b: 2}. When we try to intersect {a: 1} and {b: 1} the predicates
     *   a==2 and b==2 will get assigned to respective indices. But then we will
     *   call this function with ixisectAssigned equal to the set {'a==2', 'b==2'},
     *   and notice that we have already assigned this same set of predicates to
     *   the single index {a: 1, b: 1} via compounding.
     */
    bool alreadyCompounded(const std::set<MatchExpression*>& ixisectAssigned,
                           const AndAssignment* andAssignment) const;

    struct CmpByIndexID {
        bool operator()(IndexID a, IndexID b) const {
            return a < b;
        }
    };
    /**
     * Maps from index id to the list of predicates assigned to that index.
     */
    typedef std::map<IndexID, std::vector<MatchExpression*>, CmpByIndexID> IndexToPredMap;

    /**
     * Generate index intersection assignments given the predicate/index structure in idxToFirst
     * and idxToNotFirst (and the sub-trees in 'subnodes').  Outputs the assignments in
     * 'andAssignment'.
     */
    void enumerateAndIntersect(const IndexToPredMap& idxToFirst,
                               const IndexToPredMap& idxToNotFirst,
                               const std::vector<MemoID>& subnodes,
                               AndAssignment* andAssignment);

    /**
     * Generate one-index-at-once assignments given the predicate/index structure in idxToFirst
     * and idxToNotFirst (and the sub-trees in 'subnodes').  Outputs the assignments into
     * 'andAssignment'. The predicates in 'outsidePreds' are considered for OrPushdownTags.
     */
    void enumerateOneIndex(
        IndexToPredMap idxToFirst,
        IndexToPredMap idxToNotFirst,
        const std::vector<MemoID>& subnodes,
        const stdx::unordered_map<MatchExpression*, OutsidePredRoute>& outsidePreds,
        AndAssignment* andAssignment);

    /**
     * Generate single-index assignments for queries which contain mandatory
     * predicates (TEXT and GEO_NEAR, which are required to use a compatible index).
     * Outputs these assignments into 'andAssignment'.
     *
     * Returns true if it generated at least one assignment, and false if no assignment
     * of 'mandatoryPred' is possible.
     */
    bool enumerateMandatoryIndex(const IndexToPredMap& idxToFirst,
                                 const IndexToPredMap& idxToNotFirst,
                                 MatchExpression* mandatoryPred,
                                 const std::set<IndexID>& mandatoryIndices,
                                 AndAssignment* andAssignment);

    /**
     * Assigns predicates in 'predsOverLeadingField' and 'idxToNotFirst' to 'indexAssign'. Assumes
     * that the index is not multikey. Also assumes that that the index is of a type used to answer
     * "mandatory predicates" such as text or geoNear.
     */
    void assignToNonMultikeyMandatoryIndex(
        const IndexEntry& index,
        const std::vector<MatchExpression*>& predsOverLeadingField,
        const IndexToPredMap& idxToNotFirst,
        OneIndexAssignment* indexAssign);

    /**
     * Try to assign predicates in 'tryCompound' to 'thisIndex' as compound assignments.
     * Output the assignments in 'assign'.
     */
    void compound(const std::vector<MatchExpression*>& tryCompound,
                  const IndexEntry& thisIndex,
                  OneIndexAssignment* assign);

    /**
     * Returns the position that 'predicate' can use in the key pattern for 'indexEntry'. It is
     * illegal to call this if 'predicate' does not have a RelevantTag, or it cannot use the index.
     */
    size_t getPosition(const IndexEntry& indexEntry, MatchExpression* predicate) const;

    /**
     * Adds 'pred' to 'indexAssignment', using 'position' as its position in the index. If 'pred' is
     * in 'outsidePreds', then it is assumed to be an outside predicate that was pushed down through
     * an OR. Instead of adding it to indexAssignment->preds, we add it to
     * indexAssignment->orPushdowns. We create its OrPushdownTag using the route specified in
     * 'outsidePreds'. 'pred' must be able to use the index and be multikey-safe to add to
     * 'indexAssignment'.
     */
    void assignPredicate(
        const stdx::unordered_map<MatchExpression*, OutsidePredRoute>& outsidePreds,
        MatchExpression* pred,
        size_t position,
        OneIndexAssignment* indexAssignment);

    /**
     * Sets a flag on all outside pred routes that descend through an $elemMatch object node.
     */
    void markTraversedThroughElemMatchObj(PrepMemoContext* context);

    /**
     * Return the memo entry for 'node'.  Does some sanity checking to ensure that a memo entry
     * actually exists.
     */
    MemoID memoIDForNode(MatchExpression* node) const;

    /**
     * Helper for advancing the enumeration for a LockstepOrAssignment node.
     */
    bool _nextMemoForLockstepOrAssignment(LockstepOrAssignment*);

    std::string dumpMemo() const;

    // Map from expression to its MemoID.
    stdx::unordered_map<MatchExpression*, MemoID> _nodeToId;

    // Map from MemoID to its precomputed solution info.
    stdx::unordered_map<MemoID, std::unique_ptr<NodeAssignment>> _memo;

    // If true, there are no further enumeration states, and getNext should return false.
    // We could be _done immediately after init if we're unable to output an indexed plan.
    bool _done;

    //
    // Data used by all enumeration strategies
    //

    // Match expression we're planning for.  Not owned by us.
    MatchExpression* _root;

    // Indices we're allowed to enumerate with.  Not owned here.
    const std::vector<IndexEntry>* _indices;

    // Do we output >1 index per AND (index intersection)?
    bool _ixisect;

    // Do we enumerate children of an $or in a special order to prioritize solutions which have the
    // same assignment on each branch?
    bool _enumerateOrChildrenLockstep;

    // How many enumerations are we willing to produce from each OR?
    size_t _orLimit;

    // How many things do we want from each AND?
    size_t _intersectLimit;

    // Whether we should disable OR-pushdown optimization.
    const bool _disableOrPushdown;

    // Additional information about the query used to prune the memo.
    const projection_ast::Projection* _projection;
    boost::optional<stdx::unordered_set<std::string>> _sortPatFields;
    BSONObj _shardKey;
};

}  // namespace plan_enumerator
}  // namespace mongo
