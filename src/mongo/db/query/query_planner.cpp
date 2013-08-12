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

    static void getIndicesStartingWith(const StringData& field, const BSONObjSet& kps,
                                       BSONObjSet* out) {
        for (BSONObjSet::const_iterator i = kps.begin(); i != kps.end(); ++i) {
            const BSONObj& obj = *i;
            if (field == obj.firstElement().fieldName()) {
                out->insert(obj);
            }
        }
    }

    static BSONObj objFromElement(const BSONElement& elt) {
        BSONObjBuilder bob;
        bob.append(elt);
        return bob.obj();
    }

    /**
     * Make a point interval from the provided object.
     * The object must have exactly one field which is the value of the point interval.
     */
    static Interval makePointInterval(const BSONObj& obj) {
        Interval ret;
        ret._intervalData = obj;
        ret.startInclusive = ret.endInclusive = true;
        ret.start = ret.end = obj.firstElement();
        return ret;
    }

    static void reverseInterval(Interval* ival) {
        BSONElement tmp = ival->start;
        ival->start = ival->end;
        ival->end = tmp;

        bool tmpInc = ival->startInclusive;
        ival->startInclusive = ival->endInclusive;
        ival->endInclusive = tmpInc;
    }

    /**
     * Make a range interval from the provided object.
     * The object must have exactly two fields.  The first field is the start, the second the end.
     * The two inclusive flags indicate whether or not the start/end fields are included in the
     * interval (closed interval if included, open if not).
     */
    static Interval makeRangeInterval(const BSONObj& obj, bool startInclusive, bool endInclusive) {
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

    /**
     * Populates 'out' with the full index bounds required to traverse the index described by
     * 'kpObj'.  'first' specifies the allowed interval for the first field.  The remaining fields
     * are allowed to take on any value.
     *
     * kpObj is the full index key pattern.
     *
     * first is the interval for the first field.  It is always oriented such that the start of the
     * interval is less than or equal to the end of the interval.  As such, it may be reversed if
     * the index is backwards.
     */
    static void fillBoundsForRestOfIndex(const BSONObj& kpObj, const Interval& first,
                                         IndexBounds* out) {
        out->fields.clear();

        BSONObjIterator kpIt(kpObj);
        BSONElement elt = kpIt.next();

        // Every index that we're using to satisfy this query has these bounds.
        OrderedIntervalList oil(elt.fieldName());
        oil.intervals.push_back(first);
        // The first interval is always passed in ascending.
        if (-1 == elt.number()) {
            reverseInterval(&oil.intervals.back());
        }
        out->fields.push_back(oil);

        // For every other field in the index, add bounds that say "look at all values for this
        // field."
        while (kpIt.more()) {
            elt = kpIt.next();

            // ARGH, BSONValue would make this shorter.
            BSONObjBuilder bob;
            if (-1 == elt.number()) {
                // Index should go from MaxKey to MinKey as it's descending.
                bob.appendMaxKey("");
                bob.appendMinKey("");
            }
            else {
                // Index goes from MinKey to MaxKey as it's ascending.
                bob.appendMinKey("");
                bob.appendMaxKey("");
            }

            OrderedIntervalList oil(elt.fieldName());
            oil.intervals.push_back(makeRangeInterval(bob.obj(), true, true));
            out->fields.push_back(oil);
        }
    }

    static bool isSimpleComparison(MatchExpression::MatchType type) {
        return MatchExpression::EQ == type
               || MatchExpression::LTE == type
               || MatchExpression::LT == type
               || MatchExpression::GT == type
               || MatchExpression::GTE == type;
    }

    static void handleSimpleComparison(const MatchExpression* root, const BSONObjSet& allIndices,
                                       vector<QuerySolution*>* out) {
        const ComparisonMatchExpression* cme = static_cast<const ComparisonMatchExpression*>(root);

        // The field that we're comparing against in the doc.
        StringData path;
        path = cme->path();

        // TODO: this is going to be cached/precomputed in a smarter way.
        BSONObjSet idxToUse;
        getIndicesStartingWith(path, allIndices, &idxToUse);

        // No indices over the field, can't do anything here.
        if (idxToUse.empty()) { return; }

        Interval firstFieldBounds;

        if (MatchExpression::EQ == root->matchType()) {
            const EqualityMatchExpression* node = static_cast<const EqualityMatchExpression*>(root);

            // We have to copy the data out of the parse tree and stuff it into the index bounds.
            // BSONValue will be useful here.
            BSONObj dataObj = objFromElement(node->getData());
            verify(dataObj.isOwned());
            firstFieldBounds = makePointInterval(dataObj);
        }
        else if (MatchExpression::LTE == root->matchType()) {
            const LTEMatchExpression* node = static_cast<const LTEMatchExpression*>(root);
            BSONObjBuilder bob;
            bob.appendMinKey("");
            bob.append(node->getData());
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            firstFieldBounds = makeRangeInterval(dataObj, true, true);
        }
        else if (MatchExpression::LT == root->matchType()) {
            const LTMatchExpression* node = static_cast<const LTMatchExpression*>(root);
            BSONObjBuilder bob;
            bob.appendMinKey("");
            bob.append(node->getData());
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            firstFieldBounds = makeRangeInterval(dataObj, true, false);
        }
        else if (MatchExpression::GT == root->matchType()) {
            const GTMatchExpression* node = static_cast<const GTMatchExpression*>(root);
            BSONObjBuilder bob;
            bob.append(node->getData());
            bob.appendMaxKey("");
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            firstFieldBounds = makeRangeInterval(dataObj, false, true);
        }
        else if (MatchExpression::GTE == root->matchType()) {
            const GTEMatchExpression* node = static_cast<const GTEMatchExpression*>(root);
            BSONObjBuilder bob;
            bob.append(node->getData());
            bob.appendMaxKey("");
            BSONObj dataObj = bob.obj();
            verify(dataObj.isOwned());
            firstFieldBounds = makeRangeInterval(dataObj, true, true);
        }
        else { verify(0); }

        // For every index that we can use...
        for (BSONObjSet::iterator kpit = idxToUse.begin(); kpit != idxToUse.end(); ++kpit) {
            const BSONObj& indexKeyPattern = *kpit;

            // Create an index scan solution node.
            IndexScanNode* ixscan = new IndexScanNode();
            ixscan->indexKeyPattern = indexKeyPattern;
            fillBoundsForRestOfIndex(indexKeyPattern, firstFieldBounds, &ixscan->bounds);

            // TODO: This can be a debug check eventually.
            // TODO: We might change the direction depending on the requested sort.
            verify(ixscan->bounds.isValidFor(indexKeyPattern, 1));

            // Wrap the ixscan in a solution node.
            QuerySolution* soln = new QuerySolution();
            soln->root.reset(ixscan);
            out->push_back(soln);
        }
    }

    // static
    void QueryPlanner::plan(const CanonicalQuery& query, const BSONObjSet& indexKeyPatterns,
                            vector<QuerySolution*>* out) {
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

        if (isSimpleComparison(root->matchType())) {
            // This builds a simple index scan over the value.
            handleSimpleComparison(root, indexKeyPatterns, out);
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
