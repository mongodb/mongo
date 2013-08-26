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
 */

#include "mongo/db/query/query_planner.h"

// For QueryOption_foobar
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    /**
     * Describes an index that could be used to answer a predicate.
     */
    struct RelevantIndex {
        enum Relevance {
            // Is the index prefixed by the predicate's field?  If so it can be used.
            FIRST,

            // Is the predicate's field in the index but not as a prefix?  If so, the index might be
            // able to be used, if there is another predicate that prefixes the index.
            NOT_FIRST,
        };

        RelevantIndex(int i, Relevance r) : index(i), relevance(r) { }

        // What index is relevant?
        int index;

        // How useful is it?
        Relevance relevance;

        // To allow insertion into a set and sorting by something.
        bool operator<(const RelevantIndex& other) const {
            // We're only ever comparing these inside of a predicate.  A predicate should only be
            // tagged for an index once.  This of course assumes that values are only indexed once
            // in an index.
            verify(other.index != index);
            return index < other.index;
        }
    };

    /**
     * Caches information about the predicates we're trying to plan for.
     */
    struct PredicateInfo {
        PredicateInfo(MatchExpression::MatchType t) : type(t) { }

        // The type of the predicate.  See db/matcher/expression.h
        MatchExpression::MatchType type;

        // Any relevant indices.  Ordered by index no.
        set<RelevantIndex> relevant;
    };

    // This is a multimap because the same field name can easily appear more than once in a query.
    typedef multimap<string, PredicateInfo> PredicateMap;

    /**
     * Scan the parse tree, adding all predicates to the provided map.
     */
    void makePredicateMap(const MatchExpression* node, PredicateMap* out) {
        StringData path = node->path();

        if (!path.empty()) {
            // XXX: make sure the (path, pred) pair isn't in there already?
            out->insert(std::make_pair(path.toString(), PredicateInfo(node->matchType())));
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
        for (PredicateMap::const_iterator i = pm.begin(); i != pm.end(); ++i) {
            if (i->second.type == type) { return true; }
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

    /**
     * Provides elements from the power set of possible indices to use.  Uses the available
     * predicate information to make better decisions about what indices are best.
     *
     * TODO: Use stats about indices.
     * TODO: Use predicate information.
     */
    class PlanEnumerator {
    public:
        class OutputTag : public MatchExpression::TagData {
        public:
            OutputTag(size_t i) : index(i) { }
            virtual ~OutputTag() { }
            virtual void debugString(StringBuilder* builder) const {
                *builder << " || Selected Index #" << index;
            }
            // What index should we try to use for this leaf?
            size_t index;
        };

        /**
         * Internal tag used to explore what indices to use for a leaf node.
         */
        class EnumeratorTag : public MatchExpression::TagData {
        public:
            EnumeratorTag(const PredicateInfo* p) : pred(p), nextIndexToUse(0) { }

            virtual ~EnumeratorTag() { }

            virtual void debugString(StringBuilder* builder) const {
                *builder << " || Relevant Indices:";

                for (set<RelevantIndex>::const_iterator it = pred->relevant.begin();
                     it != pred->relevant.end(); ++it) {

                    *builder << " #" << it->index << " ";
                    if (RelevantIndex::FIRST == it->relevance) {
                        *builder << "[prefix]";
                    }
                    else {
                        verify(RelevantIndex::NOT_FIRST == it->relevance);
                        *builder << "[not-prefix]";
                    }
                }
            }

            // Not owned here.
            const PredicateInfo* pred;
            size_t nextIndexToUse;
        };

        // TODO: This is inefficient.  We could create the tagged tree as part of the PredicateMap
        // construction.
        void tag(MatchExpression* node) {
            StringData path = node->path();

            if (!path.empty()) {

                for (PredicateMap::const_iterator it = _pm.find(path.toString()); _pm.end() != it;
                     ++it) {

                    if (it->second.type == node->matchType()) {
                        EnumeratorTag* td = new EnumeratorTag(&it->second);
                        node->setTag(td);
                        _taggedLeaves.push_back(node);
                        break;
                    }
                }
            }

            // XXX XXX XXX XXX
            // XXX Do we do this if the node is logical, or if it's array, or both?
            // XXX XXX XXX XXX
            for (size_t i = 0; i < node->numChildren(); ++i) {
                tag(const_cast<MatchExpression*>(node->getChild(i)));
            }
        }

        vector<MatchExpression*> _taggedLeaves;

        /**
         * Does not take ownership of any arguments.  They must outlive any calls to getNext(...).
         */
        PlanEnumerator(const CanonicalQuery* cq, const PredicateMap* pm,
                       const vector<BSONObj>* indices)
            : _cq(*cq), _pm(*pm), _indices(*indices) {

            // Copy the query tree.
            // TODO: have a MatchExpression::copy function(?)
            StatusWithMatchExpression swme = MatchExpressionParser::parse(cq->getQueryObj());
            verify(swme.isOK());
            _taggedTree.reset(swme.getValue());

            // Walk the query tree and tag with possible indices
            tag(_taggedTree.get());

            for (size_t i = 0; i < indices->size(); ++i) {
                cout << "Index #" << i << ": " << (*indices)[i].toString() << endl;
            }

            cout << "Tagged tree: " << _taggedTree->toString() << endl;
        }

        /**
         * Outputs a possible plan.  Leaves in the plan are tagged with an index to use.
         * Returns true if a plan was outputted, false if no more plans will be outputted.
         *
         * 'tree' is set to point to the query tree.  A QuerySolution is built from this tree.
         * Caller owns the pointer.  Note that 'tree' itself points into data owned by the provided
         * CanonicalQuery.
         *
         * Nodes in 'tree' are tagged with indices that should be used to answer the tagged nodes.
         * Only nodes that have a field name (isLogical() == false) will be tagged.
         */
        bool getNext(MatchExpression** tree) {
            // TODO: ALBERTO

            // Clone tree
            // MatchExpression* ret = _taggedTree->shallowClone();

            // Walk over cloned tree and tagged tree.  Select indices out of tagged tree and mark
            // the cloned tree.

            // *tree = ret;
            // return true;

            return false;
        }

    private:
        // Not owned by us.
        const CanonicalQuery& _cq;

        // Not owned by us.
        const PredicateMap& _pm;

        // Not owned by us.
        const vector<BSONObj>& _indices;

        scoped_ptr<MatchExpression> _taggedTree;
    };

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

        // No indices, no work to do.
        if (0 == relevantIndices.size()) { return; }

        // Figure out how useful each index is to each predicate.
        rateIndices(relevantIndices, &predicates);

        //
        // Planner Section 2: Use predicate/index data to output sets of indices that we can use.
        //

        PlanEnumerator isp(&query, &predicates, &relevantIndices);

        MatchExpression* rawTree;
        while (isp.getNext(&rawTree)) {
            QuerySolutionNode* solutionRoot = NULL;

            //
            // Planner Section 3: Logical Rewrite.  Use the index selection and the tree structure
            // to try to rewrite the tree.  TODO: Do this for real.  We treat the tree as static.
            //


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
            // Planner Section 5: Sort.  If we're sorting, see if the plan gives us a sort for free.
            // If not, add a sort.
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

            if (NULL != solutionRoot) {
                QuerySolution* qs = new QuerySolution();
                qs->root.reset(solutionRoot);
                out->push_back(qs);
            }
        }

        // TODO: Do we always want to offer a collscan solution?
        if (!hasPredicate(predicates, MatchExpression::GEO_NEAR)) {
            out->push_back(makeCollectionScan(query, false));
        }
    }

}  // namespace mongo
