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

#include "mongo/db/query/query_planner.h"

#include <map>
#include <set>
#include <stack>
#include <vector>

// For QueryOption_foobar
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/plan_enumerator.h"
#include "mongo/db/query/predicate_map.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    /**
     * Scan the parse tree, adding all predicates to the provided map.
     */
    void makePredicateMap(MatchExpression* node, PredicateMap* out) {
        StringData path = node->path();

        // If we've seen this path before, link 'node' into that bunch.
        // Otherwise, create a new entry <path, node> in the predicate map.
        if (!path.empty()) {
            PredicateMap::iterator it = out->lower_bound(path.toString());
            if (it != out->end()) {
                vector<MatchExpression*>& nodes = it->second.nodes;
                nodes.push_back(node);
            }
            else {
                out->insert(std::make_pair(path.toString(), node));
            }
        }

        // XXX XXX XXX XXX
        // XXX Do we do this if the node is logical, or if it's array, or both?
        // XXX XXX XXX XXX
        for (size_t i = 0; i < node->numChildren(); ++i) {
            makePredicateMap(node->getChild(i), out);
        }
    }

    /**
     * Find all indices prefixed by fields we have predicates over.  Only these indices are useful
     * in answering the query.
     */
    void findRelevantIndices(const PredicateMap& pm, const vector<BSONObj>& allIndices,
                             vector<BSONObj>* out) {
        for (size_t i = 0; i < allIndices.size(); ++i) {
            BSONObjIterator it(allIndices[i]);
            verify(it.more());
            BSONElement elt = it.next();
            if (pm.end() != pm.find(elt.fieldName())) {
                out->push_back(allIndices[i]);
            }
        }
    }

    /**
     * Given the set of relevant indices, annotate predicates with any applicable indices.  Also
     * mark how applicable the indices are (see RelevantIndex::Relevance).
     */
    void rateIndices(const vector<BSONObj>& indices, PredicateMap* predicates) {
        for (size_t i = 0; i < indices.size(); ++i) {
            BSONObjIterator kpIt(indices[i]);
            BSONElement elt = kpIt.next();

            // We're looking at the first element in the index.  We can definitely use any index
            // prefixed by the predicate's field to answer that predicate.
            for (PredicateMap::iterator it = predicates->find(elt.fieldName());
                 it != predicates->end(); ++it) {
                it->second.relevant.insert(RelevantIndex(i, RelevantIndex::FIRST));
            }

            // We're now looking at the subsequent elements of the index.  We can only use these if
            // we have a restriction of all the previous fields.  We won't figure that out until
            // later.
            while (kpIt.more()) {
                elt = kpIt.next();
                for (PredicateMap::iterator it = predicates->find(elt.fieldName());
                     it != predicates->end(); ++it) {
                    it->second.relevant.insert(RelevantIndex(i, RelevantIndex::NOT_FIRST));
                }
            }
        }
    }

    bool hasPredicate(const PredicateMap& pm, MatchExpression::MatchType type) {
        for (PredicateMap::const_iterator itMap = pm.begin(); itMap != pm.end(); ++itMap) {
            const vector<MatchExpression*>& nodes = itMap->second.nodes;
            for (vector<MatchExpression*>::const_iterator itVec = nodes.begin();
                 itVec != nodes.end();
                 ++itVec) {
                if ((*itVec)->matchType() == type) {
                    return true;
                }
            }
        }
        return false;
    }

    QuerySolution* makeCollectionScan(const CanonicalQuery& query, bool tailable) {
        auto_ptr<QuerySolution> soln(new QuerySolution());
        soln->filter.reset(query.root()->shallowClone());
        // BSONValue, where are you?
        soln->filterData = query.getQueryObj();

        // Make the (only) node, a collection scan.
        CollectionScanNode* csn = new CollectionScanNode();
        csn->name = query.ns();
        csn->filter = soln->filter.get();
        csn->tailable = tailable;

        const BSONObj& sortObj = query.getParsed().getSort();
        // TODO: We need better checking for this.  Should be done in CanonicalQuery and we should
        // be able to assume it's correct.
        if (!sortObj.isEmpty()) {
            BSONElement natural = sortObj.getFieldDotted("$natural");
            csn->direction = natural.numberInt() >= 0 ? 1 : -1;
        }

        // Add this solution to the list of solutions.
        soln->root.reset(csn);
        return soln.release();
    }

    // TODO: Document when this settles
    struct FilterInfo {
        MatchExpression* filterNode;
        size_t currChild;
        FilterInfo(MatchExpression* f, int c) : filterNode(f), currChild(c) {}
    };

    // TODO: Document when this settles
    QuerySolution* makeIndexedPath(const CanonicalQuery& query,
                                   const vector<BSONObj>& indexKeyPatterns,
                                   MatchExpression* filter) {

        auto_ptr<QuerySolution> soln(new QuerySolution());

        // We'll build a tree of solutions nodes as we traverse the filter. For
        // now, we're not generating multi-index solutions, so we can hold on to a
        // single node here.
        IndexScanNode* isn = new IndexScanNode();

        // We'll do a post order traversal of the filter, which is a n-ary tree. We descend the
        // tree keeping track of which child is next to visit.
        std::stack<FilterInfo> filterNodes;
        FilterInfo rootFilter(filter, 0);
        filterNodes.push(rootFilter);

        while (!filterNodes.empty()) {

            FilterInfo& fi = filterNodes.top();
            MatchExpression* filterNode = fi.filterNode;
            size_t& currChild = fi.currChild;
            if (filterNode->numChildren() == currChild) {

                // Visit leaf or node. If we find an index tag, we compute the bounds and
                // fill up the index information on the current IXScan node.
                IndexTag* indexTag = static_cast<IndexTag*>(filterNode->getTag());
                bool exactBounds = false;
                if (indexTag != NULL) {

                    isn->indexKeyPattern = indexKeyPatterns[indexTag->index];

                    // We assume that this is in the same direction of the index. We may later
                    // change our minds if the query requires sorting in the opposite
                    // direction. But, right now, the direction of traversal is not in
                    // question.
                    isn->direction = 1;

                    // TODO: handle combining oils if this is not the first predicate we'eve
                    // seen for this field.
                    // TODO: handle compound indexes
                    OrderedIntervalList oil(filterNode->path().toString());
                    IndexBoundsBuilder::translate(
                        static_cast<LeafMatchExpression*>(filterNode),
                        isn->indexKeyPattern.firstElement(),
                        &oil,
                        &exactBounds);

                    // TODO: union or intersect oils
                    // It can be the case that this is not the first "oil" for a given
                    // field. That is, there are several predicates agains a same field which
                    // happens to have a useful index. In that case, we may need to combine
                    // oils in a "larger" bound to accomodate all the predicates.
                    vector<OrderedIntervalList>& fields = isn->bounds.fields;
                    fields.push_back(oil);
                }

                // TODO: possibly trim redundant MatchExpressions nodes
                // It can be the case that the bounds for a query will only return exact
                // results (as opposed to just limiting the index range in which the results
                // lie). When results are exact, we don't need to retest them and thus such
                // nodes can be eliminated from the filter.  Note that some non-leaf nodes
                // (e.g. and's, or's) may be left with single children and can therefore be
                // eliminated as well.
                isn->filter = filterNode;

                // TODO: Multiple IXScans nodes in a solution
                // If this is not the first index tag found, then we add another IXScan node to
                // the queryNodes stack and start using $or and $ands in the filter expession
                // to tie the IXScan together into a tree of QuerySolutionNode's.

                filterNodes.pop();
            }
            else {
                // Continue the traversal.
                FilterInfo fiChild(filterNode->getChild(currChild), 0);
                filterNodes.push(fiChild);
                currChild++;
            }
        }

        soln->root.reset(isn);
        return soln.release();
    }

    // static
    void QueryPlanner::plan(const CanonicalQuery& query, const vector<BSONObj>& indexKeyPatterns,
                            vector<QuerySolution*>* out) {
        // XXX: If pq.hasOption(QueryOption_OplogReplay) use FindingStartCursor equivalent which
        // must be translated into stages.

        //
        // Planner Section 1: Calculate predicate/index data.
        //

        // Get all the predicates (and their fields).
        PredicateMap predicates;
        makePredicateMap(query.root(), &predicates);

        // If the query requests a tailable cursor, the only solution is a collscan + filter with
        // tailable set on the collscan.  TODO: This is a policy departure.  Previously I think you
        // could ask for a tailable cursor and it just tried to give you one.  Now, we fail if we
        // can't provide one.  Is this what we want?
        if (query.getParsed().hasOption(QueryOption_CursorTailable)) {
            if (!hasPredicate(predicates, MatchExpression::GEO_NEAR)) {
                out->push_back(makeCollectionScan(query, true));
            }
            return;
        }

        // NOR and NOT we can't handle well with indices.  If we see them here, they weren't
        // rewritten.  Just output a collscan for those.
        if (hasPredicate(predicates, MatchExpression::NOT)
            || hasPredicate(predicates, MatchExpression::NOR)) {

            // If there's a near predicate, we can't handle this.
            // TODO: Should canonicalized query detect this?
            if (hasPredicate(predicates, MatchExpression::GEO_NEAR)) {
                warning() << "Can't handle NOT/NOR with GEO_NEAR";
                return;
            }
            out->push_back(makeCollectionScan(query, false));
            return;
        }

        // Filter our indices so we only look at indices that are over our predicates.
        vector<BSONObj> relevantIndices;
        findRelevantIndices(predicates, indexKeyPatterns, &relevantIndices);

        // No indices, no work to do other than emitting the collection scan plan.
        if (0 == relevantIndices.size()) {
            out->push_back(makeCollectionScan(query, false));
            return;
        }

        // Figure out how useful each index is to each predicate.
        rateIndices(relevantIndices, &predicates);

        //
        // Planner Section 2: Use predicate/index data to output sets of indices that we can use.
        //

        PlanEnumerator isp(query.root(), &predicates, &relevantIndices);
        isp.init();

        MatchExpression* rawTree;
        while (isp.getNext(&rawTree)) {

            //
            // Planner Section 3: Logical Rewrite.  Use the index selection and the tree structure
            // to try to rewrite the tree.  TODO: Do this for real.  We treat the tree as static.
            //

            QuerySolution* soln = makeIndexedPath(query, indexKeyPatterns, rawTree);

            //
            // Planner Section 4: Covering.  If we're projecting, See if we get any covering from
            // this plan.  If not, add a fetch.
            //

            if (!query.getParsed().getProj().isEmpty()) {
                warning() << "Can't deal with proj yet" << endl;
            }
            else {
                // Note that we need a fetch, possibly tack on to end?
            }

            //
            // Planner Section 5: Sort.  If we're sorting, see if the plan gives us a sort for
            // free.  If not, add a sort.
            //

            if (!query.getParsed().getSort().isEmpty()) {
            }
            else {
                // Note that we need a sort, possibly tack on to end?  may want to see if sort is
                // covered and then tack fetch on after the covered sort...
            }

            //
            // Planner Section 6: Final check.  Make sure that we build a valid solution.
            // TODO: Validate.
            //

            if (NULL != soln) {
                out->push_back(soln);
            }
        }

        // TODO: Do we always want to offer a collscan solution?
        if (!hasPredicate(predicates, MatchExpression::GEO_NEAR)) {
            out->push_back(makeCollectionScan(query, false));
        }
    }

}  // namespace mongo
