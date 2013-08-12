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

#include "mongo/db/query/query_solution.h"
#include "mongo/db/matcher/expression_parser.h"

namespace mongo {

    // static
    void QueryPlanner::plan(const CanonicalQuery& query, vector<QuerySolution*> *out) {
        const MatchExpression* root = query.root();

        // TODO: If pq.hasOption(QueryOption_OplogReplay) use FindingStartCursor equivalent which
        // must be translated into stages.

        // The default plan is always a collection scan with a heavy filter.  This is a valid
        // solution for any query that does not require an index.
        if (!requiresIndex(root)) {
            auto_ptr<QuerySolution> soln(new QuerySolution());
            soln->filterData = query.getQueryObj();
            // TODO: have a MatchExpression::copy function(?)
            StatusWithMatchExpression swme = MatchExpressionParser::parse(soln->filterData);
            verify(swme.isOK());
            soln->filter.reset(swme.getValue());

            // Make the (only) node, a collection scan.
            CollectionScanNode* csn = new CollectionScanNode();
            csn->name = query.ns();
            csn->filter = soln->filter.get();

            // Add this solution to the list of solutions.
            soln->root.reset(csn);
            out->push_back(soln.release());
        }
    }

    void getIndicesStartingWith(const StringData& field, const BSONObjSet& kps, BSONObjSet* out) {
        for (BSONObjSet::const_iterator i = kps.begin(); i != kps.end(); ++i) {
            const BSONObj& obj = *i;
            if (field == obj.firstElement().fieldName()) {
                out->insert(obj);
            }
        }
    }

    BSONObj objFromElement(const BSONElement& elt) {
        BSONObjBuilder bob;
        bob.append(elt);
        return bob.obj();
    }

    /**
     * Make a point interval from the provided object.
     * The object must have exactly one field which is the value of the point interval.
     */
    Interval makePointInterval(const BSONObj& obj) {
        Interval ret;
        ret._intervalData = obj;
        ret.startInclusive = ret.endInclusive = true;
        ret.start = ret.end = obj.firstElement();
        return ret;
    }

    /**
     * Make a range interval from the provided object.
     * The object must have exactly two fields.  The first field is the start, the second the end.
     * The two inclusive flags indicate whether or not the start/end fields are included in the
     * interval (closed interval if included, open if not).
     */
    Interval makeRangeInterval(const BSONObj& obj, bool startInclusive, bool endInclusive) {
        Interval ret;
        ret._intervalData = obj;
        ret.startInclusive = startInclusive;
        ret.endInclusive = endInclusive;
        BSONObjIterator it(obj);
        verify(it.more());
        ret.start = it.next();
        verify(it.more());
        ret.end = it.next();
        return ret;
    }

    void QueryPlanner::planWithIndices(const CanonicalQuery& query,
                                       const BSONObjSet& indexKeyPatterns,
                                       vector<QuerySolution*>* out) {
        const MatchExpression* root = query.root();

        if (MatchExpression::EQ == root->matchType()) {
            const EqualityMatchExpression* node = static_cast<const EqualityMatchExpression*>(root);

            // TODO: this is going to be cached/precomputed in a smarter way.
            BSONObjSet overField;
            getIndicesStartingWith(node->path(), indexKeyPatterns, &overField);

            // No indices over the field, can't do anything here.
            if (overField.empty()) { return; }

            // We have to copy the data out of the parse tree and stuff it into the index bounds.
            // BSONValue will be useful here.
            BSONObj dataObj = objFromElement(node->getData());
            verify(dataObj.isOwned());

            // Every index that we're using to satisfy this query has these bounds.  It may also
            // have other fields 
            OrderedIntervalList oil(node->path().toString());
            oil.intervals.push_back(makePointInterval(dataObj));
            IndexBounds commonBounds;
            commonBounds.fields.push_back(oil);

            // For every index that we can use...
            for (BSONObjSet::iterator kpit = overField.begin(); kpit != overField.end(); ++kpit) {
                const BSONObj& indexKeyPattern = *kpit;

                // Create an index scan solution node.
                IndexScanNode* ixscan = new IndexScanNode();
                ixscan->indexKeyPattern = indexKeyPattern;
                // Copy the common bounds.
                ixscan->bounds = commonBounds;
                // And add min/max key bounds to any trailing fields.
                BSONObjIterator it(indexKeyPattern);
                // Skip the field we're indexed on.
                it.next();
                // And for every other field.
                while (it.more()) {
                    BSONElement elt = it.next();

                    // ARGH, BSONValue would make this shorter.
                    BSONObjBuilder bob;
                    if (-1 == elt.number()) {
                        // Index should go from MaxKey to MinKey
                        bob.appendMaxKey("");
                        bob.appendMinKey("");
                    }
                    else {
                        // Index goes from MinKey to MaxKey
                        bob.appendMinKey("");
                        bob.appendMaxKey("");
                    }
                    OrderedIntervalList oil(elt.fieldName());
                    oil.intervals.push_back(makeRangeInterval(bob.obj(), true, true));
                    ixscan->bounds.fields.push_back(oil);
                }

                // TODO: This can be a debug check eventually.
                // TODO: We might change the direction depending on the requested sort.
                verify(ixscan->bounds.isValidFor(indexKeyPattern, 1));

                // Wrap the ixscan in a solution node.
                QuerySolution* soln = new QuerySolution();
                soln->root.reset(ixscan);
                out->push_back(soln);
            }
        }
    }

    // static 
    bool QueryPlanner::requiresIndex(const MatchExpression* node) {
        if (MatchExpression::GEO_NEAR == node->matchType()) {
            return true;
        }

        for (size_t i = 0; i < node->numChildren(); ++i) {
            if (requiresIndex(node->getChild(i))) {
                return true;
            }
        }

        return false;
    }

}  // namespace mongo
