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
#include "mongo/db/geo/core.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/plan_enumerator.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/qlog.h"

namespace mongo {

    // static
    void QueryPlanner::getFields(MatchExpression* node, string prefix, unordered_set<string>* out) {
        // Leaf nodes with a path and some array operators.
        if (Indexability::nodeCanUseIndexOnOwnField(node)) {
            out->insert(prefix + node->path().toString());
        }
        else if (Indexability::arrayUsesIndexOnChildren(node)) {
            // If the array uses an index on its children, it's something like
            // {foo : {$elemMatch: { bar: 1}}}, in which case the predicate is really over
            // foo.bar.
            //
            // When we have {foo: {$all: [{$elemMatch: {a:1}}], the path of the embedded elemMatch
            // is empty.  We don't want to append a dot in that case as the field would be foo..a.
            if (!node->path().empty()) {
                prefix += node->path().toString() + ".";
            }

            for (size_t i = 0; i < node->numChildren(); ++i) {
                getFields(node->getChild(i), prefix, out);
            }
        }
        else if (node->isLogical()) {
            for (size_t i = 0; i < node->numChildren(); ++i) {
                getFields(node->getChild(i), prefix, out);
            }
        }
    }

    // static
    void QueryPlanner::findRelevantIndices(const unordered_set<string>& fields,
                                           const vector<IndexEntry>& allIndices,
                                           vector<IndexEntry>* out) {
        for (size_t i = 0; i < allIndices.size(); ++i) {
            BSONObjIterator it(allIndices[i].keyPattern);
            verify(it.more());
            BSONElement elt = it.next();
            if (fields.end() != fields.find(elt.fieldName())) {
                out->push_back(allIndices[i]);
            }
        }
    }

    // static
    bool QueryPlanner::compatible(const BSONElement& elt, const IndexEntry& index,
                                  MatchExpression* node) {
        // XXX: CatalogHack::getAccessMethodName: do we have to worry about this?  when?
        string ixtype;
        if (String != elt.type()) {
            ixtype = "";
        }
        else {
            ixtype = elt.String();
        }

        // we know elt.fieldname() == node->path()

        // XXX: Our predicate may be normal and it may be over a geo index (compounding).  Detect
        // this at some point.

        MatchExpression::MatchType exprtype = node->matchType();

        // TODO: use indexnames
        if ("" == ixtype) {
            if (index.sparse && exprtype == MatchExpression::EQ) {
                // Can't check for null w/a sparse index.
                const EqualityMatchExpression* expr
                    = static_cast<const EqualityMatchExpression*>(node);
                return !expr->getData().isNull();
            }
            return exprtype != MatchExpression::GEO && exprtype != MatchExpression::GEO_NEAR;
        }
        else if ("hashed" == ixtype) {
            return exprtype == MatchExpression::MATCH_IN || exprtype == MatchExpression::EQ;
        }
        else if ("2dsphere" == ixtype) {
            if (exprtype == MatchExpression::GEO) {
                // within or intersect.
                GeoMatchExpression* gme = static_cast<GeoMatchExpression*>(node);
                const GeoQuery& gq = gme->getGeoQuery();
                const GeometryContainer& gc = gq.getGeometry();
                return gc.hasS2Region();
            }
            else if (exprtype == MatchExpression::GEO_NEAR) {
                GeoNearMatchExpression* gnme = static_cast<GeoNearMatchExpression*>(node);
                // Make sure the near query is compatible with 2dsphere.
                if (gnme->getData().centroid.crs == SPHERE || gnme->getData().isNearSphere) {
                    return true;
                }
            }
            return false;
        }
        else if ("2d" == ixtype) {
            if (exprtype == MatchExpression::GEO_NEAR) {
                GeoNearMatchExpression* gnme = static_cast<GeoNearMatchExpression*>(node);
                return gnme->getData().centroid.crs == FLAT;
            }
            else if (exprtype == MatchExpression::GEO) {
                // 2d only supports within.
                GeoMatchExpression* gme = static_cast<GeoMatchExpression*>(node);
                const GeoQuery& gq = gme->getGeoQuery();
                if (GeoQuery::WITHIN != gq.getPred()) {
                    return false;
                }

                const GeometryContainer& gc = gq.getGeometry();

                // 2d indices answer flat queries.
                if (gc.hasFlatRegion()) {
                    return true;
                }

                // 2d indices can answer centerSphere queries.
                if (NULL == gc._cap.get()) {
                    return false;
                }

                verify(SPHERE == gc._cap->crs);
                // No wrapping in 2d centerSphere, don't use 2d index for that.
                const Circle& circle = gc._cap->circle;
                // An overestimate.
                return twoDWontWrap(circle.center.x, circle.center.y, circle.radius);
            }
            return false;
        }
        else if ("text" == ixtype || "fts" == ixtype) {
            return (exprtype == MatchExpression::TEXT);
        }
        else if ("geoHaystack" == ixtype) {
            return false;
        }
        else {
            warning() << "Unknown indexing for node " << node->toString()
                      << " and field " << elt.toString() << endl;
            verify(0);
        }
    }

    // static
    void QueryPlanner::rateIndices(MatchExpression* node, string prefix,
                                   const vector<IndexEntry>& indices) {
        if (Indexability::nodeCanUseIndexOnOwnField(node)) {
            string fullPath = prefix + node->path().toString();
            verify(NULL == node->getTag());
            RelevantTag* rt = new RelevantTag();
            node->setTag(rt);
            rt->path = fullPath;

            // TODO: This is slow, with all the string compares.
            for (size_t i = 0; i < indices.size(); ++i) {
                BSONObjIterator it(indices[i].keyPattern);
                BSONElement elt = it.next();
                if (elt.fieldName() == fullPath && compatible(elt, indices[i], node)) {
                    rt->first.push_back(i);
                }
                while (it.more()) {
                    elt = it.next();
                    if (elt.fieldName() == fullPath && compatible(elt, indices[i], node)) {
                        rt->notFirst.push_back(i);
                    }
                }
            }
        }
        else if (Indexability::arrayUsesIndexOnChildren(node)) {
            // See comment in getFields about all/elemMatch and paths.
            if (!node->path().empty()) {
                prefix += node->path().toString() + ".";
            }
            for (size_t i = 0; i < node->numChildren(); ++i) {
                rateIndices(node->getChild(i), prefix, indices);
            }
        }
        else if (node->isLogical()) {
            for (size_t i = 0; i < node->numChildren(); ++i) {
                rateIndices(node->getChild(i), prefix, indices);
            }
        }
    }

    // static
    QuerySolution* QueryPlanner::makeCollectionScan(const CanonicalQuery& query, bool tailable) {
        // Make the (only) node, a collection scan.
        CollectionScanNode* csn = new CollectionScanNode();
        csn->name = query.ns();
        csn->filter.reset(query.root()->shallowClone());
        csn->tailable = tailable;

        // If the sort is {$natural: +-1} this changes the direction of the collection scan.
        const BSONObj& sortObj = query.getParsed().getSort();
        if (!sortObj.isEmpty()) {
            BSONElement natural = sortObj.getFieldDotted("$natural");
            if (!natural.eoo()) {
                csn->direction = natural.numberInt() >= 0 ? 1 : -1;
            }
        }

        // The hint can specify $natural as well.
        if (!query.getParsed().getHint().isEmpty()) {
            BSONElement natural = query.getParsed().getHint().getFieldDotted("$natural");
            if (!natural.eoo()) {
                csn->direction = natural.numberInt() >= 0 ? 1 : -1;
            }
        }

        // QLOG() << "Outputting collscan " << soln->toString() << endl;
        return analyzeDataAccess(query, csn);
    }

    // static
    QuerySolutionNode* QueryPlanner::makeLeafNode(const IndexEntry& index,
                                                  MatchExpression* expr,
                                                  bool* exact) {
        // QLOG() << "making leaf node for " << expr->toString() << endl;
        // We're guaranteed that all GEO_NEARs are first.  This slightly violates the "sort index
        // predicates by their position in the compound index" rule but GEO_NEAR isn't an ixscan.
        // This saves our bacon when we have {foo: 1, bar: "2dsphere"} and the predicate on bar is a
        // $near.  If we didn't get the GEO_NEAR first we'd create an IndexScanNode and later cast
        // it to a GeoNear2DSphereNode
        //
        // This should gracefully deal with the case where we have a pred over foo but no geo clause
        // over bar.  In that case there is no GEO_NEAR to appear first and it's treated like a
        // straight ixscan.
        BSONElement elt = index.keyPattern.firstElement();
        bool indexIs2D = (String == elt.type() && "2d" == elt.String());

        if (MatchExpression::GEO_NEAR == expr->matchType()) {
            // We must not keep the expression node around.
            *exact = true;
            GeoNearMatchExpression* nearExpr = static_cast<GeoNearMatchExpression*>(expr);
            // 2d geoNear requires a hard limit and as such we take it out before it gets here.  If
            // this happens it's a bug.
            verify(!indexIs2D);
            GeoNear2DSphereNode* ret = new GeoNear2DSphereNode();
            ret->indexKeyPattern = index.keyPattern;
            ret->nq = nearExpr->getData();
            ret->baseBounds.fields.resize(index.keyPattern.nFields());
            return ret;
        }
        else if (indexIs2D) {
            // We must not keep the expression node around.
            *exact = true;
            verify(MatchExpression::GEO == expr->matchType());
            GeoMatchExpression* nearExpr = static_cast<GeoMatchExpression*>(expr);
            verify(indexIs2D);
            Geo2DNode* ret = new Geo2DNode();
            ret->indexKeyPattern = index.keyPattern;
            ret->gq = nearExpr->getGeoQuery();
            return ret;
        }
        else if (MatchExpression::TEXT == expr->matchType()) {
            // We must not keep the expression node around.
            *exact = true;
            TextMatchExpression* textExpr = static_cast<TextMatchExpression*>(expr);
            TextNode* ret = new TextNode();
            ret->_indexKeyPattern = index.keyPattern;
            ret->_query = textExpr->getQuery();
            ret->_language = textExpr->getLanguage();
            return ret;
        }
        else {
            // QLOG() << "making ixscan for " << expr->toString() << endl;

            // Note that indexKeyPattern.firstElement().fieldName() may not equal expr->path()
            // because expr might be inside an array operator that provides a path prefix.
            IndexScanNode* isn = new IndexScanNode();
            isn->indexKeyPattern = index.keyPattern;
            isn->indexIsMultiKey = index.multikey;
            isn->bounds.fields.resize(index.keyPattern.nFields());

            IndexBoundsBuilder::translate(expr, index.keyPattern.firstElement(),
                                          &isn->bounds.fields[0], exact);

            // QLOG() << "bounds are " << isn->bounds.toString() << " exact " << *exact << endl;
            return isn;
        }
    }

    void QueryPlanner::mergeWithLeafNode(MatchExpression* expr, const IndexEntry& index,
                                         size_t pos, bool* exactOut, QuerySolutionNode* node,
                                         MatchExpression::MatchType mergeType) {
        const StageType type = node->getType();

        if (STAGE_GEO_2D == type || STAGE_GEO_NEAR_2D == type) {
            warning() << "About to screw up 2d geo compound";
            // This keeps the pred as a matcher but the limit argument to 2d indices applies
            // post-match so this is wrong.
            *exactOut = false;
        }
        else {
            IndexBounds* boundsToFillOut = NULL;

            if (STAGE_GEO_NEAR_2DSPHERE == type) {
                GeoNear2DSphereNode* gn = static_cast<GeoNear2DSphereNode*>(node);
                boundsToFillOut = &gn->baseBounds;
            }
            else {
                verify(type == STAGE_IXSCAN);
                IndexScanNode* scan = static_cast<IndexScanNode*>(node);
                boundsToFillOut = &scan->bounds;
            }

            // Get the ixtag->pos-th element of the index key pattern.
            // TODO: cache this instead/with ixtag->pos?
            BSONObjIterator it(index.keyPattern);
            BSONElement keyElt = it.next();
            for (size_t i = 0; i < pos; ++i) {
                verify(it.more());
                keyElt = it.next();
            }
            verify(!keyElt.eoo());
            *exactOut = false;

            //QLOG() << "current bounds are " << currentScan->bounds.toString() << endl;
            //QLOG() << "node merging in " << child->toString() << endl;
            //QLOG() << "merging with field " << keyElt.toString(true, true) << endl;
            //QLOG() << "taking advantage of compound index "
                 //<< indices[currentIndexNumber].keyPattern.toString() << endl;

            verify(boundsToFillOut->fields.size() > pos);

            OrderedIntervalList* oil = &boundsToFillOut->fields[pos];

            if (boundsToFillOut->fields[pos].name.empty()) {
                IndexBoundsBuilder::translate(expr, keyElt, oil, exactOut);
            }
            else {
                if (MatchExpression::AND == mergeType) {
                    IndexBoundsBuilder::translateAndIntersect(expr, keyElt, oil, exactOut);
                }
                else {
                    verify(MatchExpression::OR == mergeType);
                    IndexBoundsBuilder::translateAndUnion(expr, keyElt, oil, exactOut);
                }
            }
        }
    }

    /**
     * Assumes each OIL in bounds is increasing.
     * Reverses any OILs that are indexed according to '-1'.
     */
    void alignBounds(IndexBounds* bounds, const BSONObj& kp) {
        BSONObjIterator it(kp);
        size_t oilIdx = 0;
        while (it.more()) {
            BSONElement elt = it.next();
            int direction = (elt.numberInt() >= 0) ? 1 : -1;
            if (-1 == direction) {
                vector<Interval>& iv = bounds->fields[oilIdx].intervals;
                // Step 1: reverse the list.
                std::reverse(iv.begin(), iv.end());
                // Step 2: reverse each interval.
                for (size_t i = 0; i < iv.size(); ++i) {
                    // QLOG() << "reversing " << iv[i].toString() << endl;
                    iv[i].reverse();
                }
            }
            ++oilIdx;
        }

        // XXX: some day we'll maybe go backward to pull a sort out
        if (!bounds->isValidFor(kp, 1)) {
            QLOG() << "INVALID BOUNDS: " << bounds->toString() << endl;
            verify(0);
        }
    }

    // static
    void QueryPlanner::finishLeafNode(QuerySolutionNode* node, const IndexEntry& index) {
        const StageType type = node->getType();

        if (STAGE_GEO_2D == type || STAGE_GEO_NEAR_2D == type || STAGE_TEXT == type) {
            // XXX: do we do anything here?
            return;
        }
        else {
            IndexBounds* bounds = NULL;

            if (STAGE_GEO_NEAR_2DSPHERE == type) {
                GeoNear2DSphereNode* gnode = static_cast<GeoNear2DSphereNode*>(node);
                bounds = &gnode->baseBounds;
            }
            else {
                verify(type == STAGE_IXSCAN);
                IndexScanNode* scan = static_cast<IndexScanNode*>(node);
                bounds = &scan->bounds;
            }

            // XXX: this currently fills out minkey/maxkey bounds for near queries, fix that.  just
            // set the field name of the near query field when starting a near scan.

            // Find the first field in the scan's bounds that was not filled out.
            // TODO: could cache this.
            size_t firstEmptyField = 0;
            for (firstEmptyField = 0; firstEmptyField < bounds->fields.size(); ++firstEmptyField) {
                if ("" == bounds->fields[firstEmptyField].name) {
                    verify(bounds->fields[firstEmptyField].intervals.empty());
                    break;
                }
            }

            // All fields are filled out with bounds, nothing to do.
            if (firstEmptyField == bounds->fields.size()) {
                alignBounds(bounds, index.keyPattern);
                return;
            }

            // Skip ahead to the firstEmptyField-th element, where we begin filling in bounds.
            BSONObjIterator it(index.keyPattern);
            for (size_t i = 0; i < firstEmptyField; ++i) {
                verify(it.more());
                it.next();
            }

            // For each field in the key...
            while (it.more()) {
                BSONElement kpElt = it.next();
                // There may be filled-in fields to the right of the firstEmptyField.
                // Example:
                // The index {loc:"2dsphere", x:1}
                // With a predicate over x and a near search over loc.
                if ("" == bounds->fields[firstEmptyField].name) {
                    verify(bounds->fields[firstEmptyField].intervals.empty());
                    // ...build the "all values" interval.
                    IndexBoundsBuilder::allValuesForField(kpElt,
                                                          &bounds->fields[firstEmptyField]);
                }
                ++firstEmptyField;
            }

            // Make sure that the length of the key is the length of the bounds we started.
            verify(firstEmptyField == bounds->fields.size());

            // We create bounds assuming a forward direction but can easily reverse bounds to align
            // according to our desired direction.
            alignBounds(bounds, index.keyPattern);
        }
    }

    // static
    bool QueryPlanner::processIndexScans(MatchExpression* root,
                                         bool inArrayOperator,
                                         const vector<IndexEntry>& indices,
                                         vector<QuerySolutionNode*>* out) {

        auto_ptr<QuerySolutionNode> currentScan;
        size_t currentIndexNumber = IndexTag::kNoIndex;
        size_t curChild = 0;

        // This 'while' processes all IXSCANs, possibly merging scans by combining the bounds.  We
        // can merge scans in two cases:
        // 1. Filling out subsequent fields in a compound index.
        // 2. Intersecting bounds.  Currently unimplemented.
        while (curChild < root->numChildren()) {
            MatchExpression* child = root->getChild(curChild);

            // If there is no tag, it's not using an index.  We've sorted our children such that the
            // children with tags are first, so we stop now.
            if (NULL == child->getTag()) { break; }

            IndexTag* ixtag = static_cast<IndexTag*>(child->getTag());
            // If there's a tag it must be valid.
            verify(IndexTag::kNoIndex != ixtag->index);

            // If the child can't use an index on its own field, it's indexed by virtue of one of
            // its children having an index.  We don't do anything special here, just add it to
            // the output as-is.
            //
            // NOTE: If the child is logical, it could possibly collapse into a single ixscan.  we
            // ignore this for now.
            if (!Indexability::nodeCanUseIndexOnOwnField(child)) {
                if (!inArrayOperator) {
                    // The logical sub-tree is responsible for fully evaluating itself.  Any
                    // required filters or fetches are already hung on it.  As such, we remove the
                    // filter branch from our tree.  buildIndexedDataAccess takes ownership of the
                    // child.
                    root->getChildVector()->erase(root->getChildVector()->begin() + curChild);
                    // The curChild of today is the curChild+1 of yesterday.
                }
                else {
                    ++curChild;
                }

                // If inArrayOperator: takes ownership of child, which is OK, since we detached
                // child from root.
                QuerySolutionNode* childSolution = buildIndexedDataAccess(child,
                                                                          inArrayOperator,
                                                                          indices);
                if (NULL == childSolution) { return false; }
                out->push_back(childSolution);
                continue;
            }

            // If we're here, we now know that 'child' can use an index directly and the index is
            // over the child's field.

            // If the child we're looking at uses a different index than the current index scan, add
            // the current index scan to the output as we're done with it.  The index scan created
            // by the child then becomes our new current index scan.  Note that the current scan
            // could be NULL, in which case we don't output it.  The rest of the logic is identical.
            //
            // If the child uses the same index as the current index scan, we may be able to merge
            // the bounds for the two scans.
            //
            // Guiding principle: must the values we're testing come from the same array in the
            // document?  If so, we can combine bounds (via intersection or compounding).  If not,
            // we can't.
            //
            // If the index is NOT multikey, it's always semantically correct to combine bounds,
            // as there are no arrays to worry about.
            //
            // If the index is multikey, there are arrays of values.  There are three issues:
            //
            // 1. We can't intersect bounds even if the bounds are not on a compound index.
            //    Example:
            //    Let's say we have the document {a: [5, 7]}.
            //    This document satisfies the query {$and: [ {a: 5}, {a: 7} ] }
            //    For the index {a:1} we have the keys {"": 5} and {"": 7}.
            //    Each child of the AND is tagged with the index {a: 1}
            //    The interval for the {a: 5} branch is [5, 5].  It is exact.
            //    The interval for the {a: 7} branch is [7, 7].  It is exact.
            //    The intersection of the intervals is {}.
            //    If we scan over {}, the intersection of the intervals, we will retrieve nothing.
            //
            // 2. If we're using a compound index, we can only specify bounds for the first field.
            //    Example:
            //    Let's say we have the document {a: [ {b: 3}, {c: 4} ] }
            //    This document satisfies the query {'a.b': 3, 'a.c': 4}.
            //    For the index {'a.b': 1, 'a.c': 1} we have the keys {"": 3, "": null} and
            //                                                        {"": null, "": 4}.
            //    Let's use the aforementioned index to answer the query.
            //    The bounds for 'a.b' are [3,3], and the bounds for 'a.c' are [4,4].
            //    If we combine the bounds, we would only look at keys {"": 3, "":4 }.
            //    Therefore we wouldn't look at the document's keys in the index.
            //    Therefore we don't combine bounds.
            //
            // 3. There is an exception to (2), and that is when we're evaluating an $elemMatch.
            //    Example:
            //    Our query is a: {$elemMatch: {b:3, c:4}}.
            //    Let's say that we have the index {'a.b': 1, 'a.c': 1} as in (2).
            //    $elemMatch requires if a.b==3 and a.c==4, the predicates must be satisfied from
            //    the same array entry.
            //    If those values are both present in the same array, the index key for the
            //    aforementioned index will be {"":3, "":4}
            //    Therefore we can intersect bounds.

            // TODO: we should also merge if we're in an array operator, but only when we figure out index13.js.
            if (NULL != currentScan.get() && (currentIndexNumber == ixtag->index) && !indices[currentIndexNumber].multikey) {
                // The child uses the same index we're currently building a scan for.  Merge
                // the bounds and filters.
                verify(currentIndexNumber == ixtag->index);

                bool exact = false;
                mergeWithLeafNode(child, indices[currentIndexNumber], ixtag->pos, &exact,
                                  currentScan.get(), root->matchType());

                if (exact) {
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);
                    delete child;
                }
                else {
                    // We keep curChild in the AND for affixing later.
                    ++curChild;
                }
            }
            else {
                if (NULL != currentScan.get()) {
                    finishLeafNode(currentScan.get(), indices[currentIndexNumber]);
                    out->push_back(currentScan.release());
                }
                else {
                    verify(IndexTag::kNoIndex == currentIndexNumber);
                }

                currentIndexNumber = ixtag->index;

                bool exact = false;
                currentScan.reset(makeLeafNode(indices[currentIndexNumber],
                                                child, &exact));

                if (exact && !inArrayOperator) {
                    // The bounds answer the predicate, and we can remove the expression from the
                    // root.  NOTE(opt): Erasing entry 0, 1, 2, ... could be kind of n^2, maybe
                    // optimize later.
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);
                    delete child;
                    // Don't increment curChild.
                }
                else {
                    // We keep curChild in the node for affixing later as a filter.
                    ++curChild;
                }
            }
        }

        // Output the scan we're done with, if it exists.
        if (NULL != currentScan.get()) {
            finishLeafNode(currentScan.get(), indices[currentIndexNumber]);
            out->push_back(currentScan.release());
        }

        return true;
    }

    // static
    QuerySolutionNode* QueryPlanner::buildIndexedAnd(MatchExpression* root,
                                                     bool inArrayOperator,
                                                     const vector<IndexEntry>& indices) {
        auto_ptr<MatchExpression> autoRoot;
        if (!inArrayOperator) {
            autoRoot.reset(root);
        }

        vector<QuerySolutionNode*> ixscanNodes;
        if (!processIndexScans(root, inArrayOperator, indices, &ixscanNodes)) {
            return NULL;
        }

        //
        // Process all non-indexed predicates.  We hang these above the AND with a fetch and
        // filter.
        //

        // This is the node we're about to return.
        QuerySolutionNode* andResult;

        // We must use an index for at least one child of the AND.  We shouldn't be here if this
        // isn't the case.
        verify(ixscanNodes.size() >= 1);

        // Short-circuit: an AND of one child is just the child.
        if (ixscanNodes.size() == 1) {
            andResult = ixscanNodes[0];
        }
        else {
            // Figure out if we want AndHashNode or AndSortedNode.
            bool allSortedByDiskLoc = true;
            for (size_t i = 0; i < ixscanNodes.size(); ++i) {
                if (!ixscanNodes[i]->sortedByDiskLoc()) {
                    allSortedByDiskLoc = false;
                    break;
                }
            }
            if (allSortedByDiskLoc) {
                AndSortedNode* asn = new AndSortedNode();
                asn->children.swap(ixscanNodes);
                andResult = asn;
            }
            else {
                AndHashNode* ahn = new AndHashNode();
                ahn->children.swap(ixscanNodes);
                andResult = ahn;
            }
        }

        // Don't bother doing any kind of fetch analysis lite if we're doing it anyway above us.
        if (inArrayOperator) {
            return andResult;
        }

        // If there are any nodes still attached to the AND, we can't answer them using the
        // index, so we put a fetch with filter.
        if (root->numChildren() > 0) {
            FetchNode* fetch = new FetchNode();
            verify(NULL != autoRoot.get());
            // Takes ownership.
            fetch->filter.reset(autoRoot.release());
            // takes ownership
            fetch->child.reset(andResult);
            andResult = fetch;
        }
        else {
            // root has no children, let autoRoot get rid of it when it goes out of scope.
        }

        return andResult;
    }

    // static
    QuerySolutionNode* QueryPlanner::buildIndexedOr(MatchExpression* root,
                                                    bool inArrayOperator,
                                                    const vector<IndexEntry>& indices) {
        auto_ptr<MatchExpression> autoRoot;
        if (!inArrayOperator) {
            autoRoot.reset(root);
        }

        vector<QuerySolutionNode*> ixscanNodes;
        if (!processIndexScans(root, inArrayOperator, indices, &ixscanNodes)) {
            return NULL;
        }

        // Unlike an AND, an OR cannot have filters hanging off of it.  We stop processing
        // when any of our children lack index tags.  If a node lacks an index tag it cannot
        // be answered via an index.
        if (!inArrayOperator && 0 != root->numChildren()) {
            warning() << "planner OR error, non-indexed child of OR.";
            // We won't enumerate an OR without indices for each child, so this isn't an issue, even
            // if we have an AND with an OR child -- we won't get here unless the OR is fully
            // indexed.
            return NULL;
        }

        QuerySolutionNode* orResult = NULL;

        // An OR of one node is just that node.
        if (1 == ixscanNodes.size()) {
            orResult = ixscanNodes[0];
        }
        else {
            // If each child is sorted by the same predicate, we can merge them and maintain
            // sorted order.
            bool haveSameSort;
            if (ixscanNodes[0]->getSort().isEmpty()) {
                haveSameSort = false;
            }
            else {
                haveSameSort = true;
                for (size_t i = 1; i < ixscanNodes.size(); ++i) {
                    if (0 != ixscanNodes[0]->getSort().woCompare(ixscanNodes[i]->getSort())) {
                        haveSameSort = false;
                        break;
                    }
                }
            }

            if (haveSameSort) {
                MergeSortNode* msn = new MergeSortNode();
                msn->sort = ixscanNodes[0]->getSort();
                msn->children.swap(ixscanNodes);
                orResult = msn;
            }
            else {
                OrNode* orn = new OrNode();
                orn->children.swap(ixscanNodes);
                orResult = orn;
            }
        }

        // OR must have an index for each child, so we should have detached all children from
        // 'root', and there's nothing useful to do with an empty or MatchExpression.  We let it die
        // via autoRoot.

        return orResult;
    }

    // static
    QuerySolutionNode* QueryPlanner::buildIndexedDataAccess(MatchExpression* root,
                                                            bool inArrayOperator,
                                                            const vector<IndexEntry>& indices) {
        if (root->isLogical()) {
            if (MatchExpression::AND == root->matchType()) {
                // Takes ownership of root.
                return buildIndexedAnd(root, inArrayOperator, indices);
            }
            else if (MatchExpression::OR == root->matchType()) {
                // Takes ownership of root.
                return buildIndexedOr(root, inArrayOperator, indices);
            }
            else {
                // Can't do anything with negated logical nodes index-wise.
                return NULL;
            }
        }
        else {
            auto_ptr<MatchExpression> autoRoot;
            if (!inArrayOperator) {
                autoRoot.reset(root);
            }

            // isArray or isLeaf is true.  Either way, it's over one field, and the bounds builder
            // deals with it.
            if (NULL == root->getTag()) {
                // No index to use here, not in the context of logical operator, so we're SOL.
                return NULL;
            }
            else if (Indexability::nodeCanUseIndexOnOwnField(root)) {
                // Make an index scan over the tagged index #.
                IndexTag* tag = static_cast<IndexTag*>(root->getTag());

                bool exact = false;
                QuerySolutionNode* soln = makeLeafNode(indices[tag->index], root,
                                                       &exact);
                verify(NULL != soln);
                stringstream ss;
                soln->appendToString(&ss, 0);
                // QLOG() << "about to finish leaf node, soln " << ss.str() << endl;
                finishLeafNode(soln, indices[tag->index]);

                if (inArrayOperator) {
                    return soln;
                }

                // If the bounds are exact, the set of documents that satisfy the predicate is
                // exactly equal to the set of documents that the scan provides.
                //
                // If the bounds are not exact, the set of documents returned from the scan is a
                // superset of documents that satisfy the predicate, and we must check the
                // predicate.

                if (exact) {
                    return soln;
                }

                FetchNode* fetch = new FetchNode();
                verify(NULL != autoRoot.get());
                fetch->filter.reset(autoRoot.release());
                fetch->child.reset(soln);
                return fetch;
            }
            else if (Indexability::arrayUsesIndexOnChildren(root)) {
                QuerySolutionNode* solution = NULL;

                if (MatchExpression::ALL == root->matchType()) {
                    // Here, we formulate an AND of all the sub-clauses.
                    auto_ptr<AndHashNode> ahn(new AndHashNode());

                    for (size_t i = 0; i < root->numChildren(); ++i) {
                        QuerySolutionNode* node = buildIndexedDataAccess(root->getChild(i), true,
                                                                         indices);
                        if (NULL != node) {
                            ahn->children.push_back(node);
                        }
                    }

                    // No children, no point in hashing nothing.
                    if (0 == ahn->children.size()) { return NULL; }

                    // AND of one child is just that child.
                    if (1 == ahn->children.size()) {
                        solution = ahn->children[0];
                        ahn->children.clear();
                        ahn.reset();
                    }
                    else {
                        // More than one child.
                        solution = ahn.release();
                    }
                }
                else {
                    verify(MatchExpression::ELEM_MATCH_OBJECT);
                    // The child is an AND.
                    verify(1 == root->numChildren());
                    solution = buildIndexedDataAccess(root->getChild(0), true, indices);
                    if (NULL == solution) { return NULL; }
                }

                // There may be an array operator above us.
                if (inArrayOperator) { return solution; }

                FetchNode* fetch = new FetchNode();
                // Takes ownership of 'root'.
                verify(NULL != autoRoot.get());
                fetch->filter.reset(autoRoot.release());
                fetch->child.reset(solution);
                return fetch;
            }
        }

        return NULL;
    }

    // static
    QuerySolution* QueryPlanner::analyzeDataAccess(const CanonicalQuery& query,
                                                   QuerySolutionNode* solnRoot) {
        auto_ptr<QuerySolution> soln(new QuerySolution());
        soln->filterData = query.getQueryObj();
        verify(soln->filterData.isOwned());
        soln->ns = query.ns();

        // solnRoot finds all our results.  Let's see what transformations we must perform to the
        // data.

        // Sort the results, if there is a sort specified.
        if (!query.getParsed().getSort().isEmpty()) {
            const BSONObj& sortObj = query.getParsed().getSort();

            // If the sort is $natural, we ignore it, assuming that the caller has detected that and
            // outputted a collscan to satisfy the desired order.
            BSONElement natural = sortObj.getFieldDotted("$natural");
            if (natural.eoo()) {
                // See if solnRoot gives us the sort.  If so, we're done.
                if (0 == sortObj.woCompare(solnRoot->getSort())) {
                    // Sort is already provided!
                }
                else {
                    // If solnRoot isn't already sorted, let's see if it has the fields we're
                    // sorting on.  If it's fetched, it has all the fields by definition.  If it's
                    // not, we check sort field by sort field.
                    if (!solnRoot->fetched()) {
                        bool sortCovered = true;
                        BSONObjIterator it(sortObj);
                        while (it.more()) {
                            if (!solnRoot->hasField(it.next().fieldName())) {
                                sortCovered = false;
                                break;
                            }
                        }

                        if (!sortCovered) {
                            FetchNode* fetch = new FetchNode();
                            fetch->child.reset(solnRoot);
                            solnRoot = fetch;
                        }
                    }

                    soln->hasSortStage = true;
                    SortNode* sort = new SortNode();
                    sort->pattern = sortObj;
                    sort->child.reset(solnRoot);
                    solnRoot = sort;
                }
            }
        }

        // Project the results.
        if (NULL != query.getProj()) {
            QLOG() << "PROJECTION: fetched status: " << solnRoot->fetched() << endl;
            QLOG() << "PROJECTION: Current plan is " << solnRoot->toString() << endl;
            if (query.getProj()->requiresDocument()) {
                QLOG() << "PROJECTION: claims to require doc adding fetch.\n";
                // If the projection requires the entire document, somebody must fetch.
                if (!solnRoot->fetched()) {
                    FetchNode* fetch = new FetchNode();
                    fetch->child.reset(solnRoot);
                    solnRoot = fetch;
                }
            }
            else {
                QLOG() << "PROJECTION: requires fields\n";
                const vector<string>& fields = query.getProj()->requiredFields();
                bool covered = true;
                for (size_t i = 0; i < fields.size(); ++i) {
                    if (!solnRoot->hasField(fields[i])) {
                        QLOG() << "PROJECTION: not covered cuz doesn't have field "
                             << fields[i] << endl;
                        covered = false;
                        break;
                    }
                }
                // If any field is missing from the list of fields the projection wants,
                // a fetch is required.
                if (!covered) {
                    FetchNode* fetch = new FetchNode();
                    fetch->child.reset(solnRoot);
                    solnRoot = fetch;
                }
            }

            // We now know we have whatever data is required for the projection.
            ProjectionNode* projNode = new ProjectionNode();
            projNode->projection = query.getProj();
            projNode->child.reset(solnRoot);
            solnRoot = projNode;
        }
        else {
            // If there's no projection, we must fetch, as the user wants the entire doc.
            if (!solnRoot->fetched()) {
                FetchNode* fetch = new FetchNode();
                fetch->child.reset(solnRoot);
                solnRoot = fetch;
            }
        }

        if (0 != query.getParsed().getSkip()) {
            SkipNode* skip = new SkipNode();
            skip->skip = query.getParsed().getSkip();
            skip->child.reset(solnRoot);
            solnRoot = skip;
        }

        if (0 != query.getParsed().getNumToReturn() && !query.getParsed().wantMore()) {
            LimitNode* limit = new LimitNode();
            limit->limit = query.getParsed().getNumToReturn();
            limit->child.reset(solnRoot);
            solnRoot = limit;
        }

        soln->root.reset(solnRoot);
        return soln.release();
    }

    // Copied verbatim from db/index.h
    static bool isIdIndex( const BSONObj &pattern ) {
        BSONObjIterator i(pattern);
        BSONElement e = i.next();
        //_id index must have form exactly {_id : 1} or {_id : -1}.
        //Allows an index of form {_id : "hashed"} to exist but
        //do not consider it to be the primary _id index
        if(! ( strcmp(e.fieldName(), "_id") == 0
                && (e.numberInt() == 1 || e.numberInt() == -1)))
            return false;
        return i.next().eoo();
    }

    static bool is2DIndex(const BSONObj& pattern) {
        BSONObjIterator it(pattern);
        while (it.more()) {
            BSONElement e = it.next();
            if (String == e.type() && str::equals("2d", e.valuestr())) {
                return true;
            }
        }
        return false;
    }

    // static
    void QueryPlanner::plan(const CanonicalQuery& query, const vector<IndexEntry>& indices,
                            size_t options, vector<QuerySolution*>* out) {
        QLOG() << "============================="
               << "Beginning planning.\n"
               << "query = " << query.toString()
               << "============================="
               << endl;

        for (size_t i = 0; i < indices.size(); ++i) {
            QLOG() << "idx " << i << " is " << indices[i].toString() << endl;
        }

        bool canTableScan = !(options & QueryPlanner::NO_TABLE_SCAN);

        // If the query requests a tailable cursor, the only solution is a collscan + filter with
        // tailable set on the collscan.  TODO: This is a policy departure.  Previously I think you
        // could ask for a tailable cursor and it just tried to give you one.  Now, we fail if we
        // can't provide one.  Is this what we want?
        if (query.getParsed().hasOption(QueryOption_CursorTailable)) {
            if (!QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR)
                && canTableScan) {

                out->push_back(makeCollectionScan(query, true));
            }
            return;
        }

        // The hint can be $natural: 1.  If this happens, output a collscan.  It's a weird way of
        // saying "table scan for two, please."
        if (!query.getParsed().getHint().isEmpty()) {
            BSONElement natural = query.getParsed().getHint().getFieldDotted("$natural");
            if (!natural.eoo()) {
                QLOG() << "forcing a table scan due to hinted $natural\n";
                if (canTableScan) {
                    out->push_back(makeCollectionScan(query, false));
                }
                return;
            }
        }

        // NOR and NOT we can't handle well with indices.  If we see them here, they weren't
        // rewritten to remove the negation.  Just output a collscan for those.
        if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::NOT)
            || QueryPlannerCommon::hasNode(query.root(), MatchExpression::NOR)) {

            // If there's a near predicate, we can't handle this.
            // TODO: Should canonicalized query detect this?
            if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR)) {
                warning() << "Can't handle NOT/NOR with GEO_NEAR";
                return;
            }
            QLOG() << "NOT/NOR in plan, just outtping a collscan\n";
            if (canTableScan) {
                out->push_back(makeCollectionScan(query, false));
            }
            return;
        }

        // Figure out what fields we care about.
        unordered_set<string> fields;
        getFields(query.root(), "", &fields);

        for (unordered_set<string>::const_iterator it = fields.begin(); it != fields.end(); ++it) {
            QLOG() << "predicate over field " << *it << endl;
        }

        // Filter our indices so we only look at indices that are over our predicates.
        vector<IndexEntry> relevantIndices;

        // Hints require us to only consider the hinted index.
        BSONObj hintIndex = query.getParsed().getHint();

        // Snapshot is a form of a hint.  If snapshot is set, try to use _id index to make a real
        // plan.  If that fails, just scan the _id index.
        if (query.getParsed().isSnapshot()) {
            // Find the ID index in indexKeyPatterns.  It's our hint.
            for (size_t i = 0; i < indices.size(); ++i) {
                if (isIdIndex(indices[i].keyPattern)) {
                    hintIndex = indices[i].keyPattern;
                    break;
                }
            }
        }

        size_t hintIndexNumber = numeric_limits<size_t>::max();

        if (!hintIndex.isEmpty()) {
            // Sigh.  If the hint is specified it might be using the index name.
            BSONElement firstHintElt = hintIndex.firstElement();
            if (str::equals("$hint", firstHintElt.fieldName()) && String == firstHintElt.type()) {
                string hintName = firstHintElt.String();
                for (size_t i = 0; i < indices.size(); ++i) {
                    if (indices[i].name == hintName) {
                        QLOG() << "hint by name specified, restricting indices to "
                             << indices[i].keyPattern.toString() << endl;
                        relevantIndices.clear();
                        relevantIndices.push_back(indices[i]);
                        hintIndexNumber = i;
                        hintIndex = indices[i].keyPattern;
                        break;
                    }
                }
            }
            else {
                for (size_t i = 0; i < indices.size(); ++i) {
                    if (0 == indices[i].keyPattern.woCompare(hintIndex)) {
                        relevantIndices.clear();
                        relevantIndices.push_back(indices[i]);
                        QLOG() << "hint specified, restricting indices to " << hintIndex.toString()
                             << endl;
                        hintIndexNumber = i;
                        break;
                    }
                }
            }

            if (hintIndexNumber == numeric_limits<size_t>::max()) {
                // This is supposed to be an error.
                warning() << "Can't find hint for " << hintIndex.toString();
                return;
            }
        }
        else {
            QLOG() << "Finding relevant indices\n";
            findRelevantIndices(fields, indices, &relevantIndices);
        }

        for (size_t i = 0; i < relevantIndices.size(); ++i) {
            QLOG() << "relevant idx " << i << " is " << relevantIndices[i].toString() << endl;
        }

        // Figure out how useful each index is to each predicate.
        // query.root() is now annotated with RelevantTag(s).
        rateIndices(query.root(), "", relevantIndices);

        QLOG() << "rated tree" << endl;
        QLOG() << query.root()->toString() << endl;

        // If there is a GEO_NEAR it must have an index it can use directly.
        MatchExpression* gnNode = NULL;
        if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR, &gnNode)) {
            // No index for GEO_NEAR?  No query.
            RelevantTag* tag = static_cast<RelevantTag*>(gnNode->getTag());
            if (0 == tag->first.size() && 0 == tag->notFirst.size()) {
                return;
            }

            GeoNearMatchExpression* gnme = static_cast<GeoNearMatchExpression*>(gnNode);

            vector<size_t> newFirst;

            // 2d + GEO_NEAR is annoying.  Because 2d's GEO_NEAR isn't streaming we have to embed
            // the full query tree inside it as a matcher.
            for (size_t i = 0; i < tag->first.size(); ++i) {
                // GEO_NEAR has a non-2d index it can use.  We can deal w/that in normal planning.
                if (!is2DIndex(relevantIndices[tag->first[i]].keyPattern)) {
                    newFirst.push_back(i);
                    continue;
                }

                // If we're here, GEO_NEAR has a 2d index.  We create a 2dgeonear plan with the
                // entire tree as a filter, if possible.

                GeoNear2DNode* solnRoot = new GeoNear2DNode();
                solnRoot->nq = gnme->getData();

                if (MatchExpression::GEO_NEAR != query.root()->matchType()) {
                    // root is an AND, clone and delete the GEO_NEAR child.
                    MatchExpression* filterTree = query.root()->shallowClone();
                    verify(MatchExpression::AND == filterTree->matchType());

                    bool foundChild = false;
                    for (size_t i = 0; i < filterTree->numChildren(); ++i) {
                        if (MatchExpression::GEO_NEAR == filterTree->getChild(i)->matchType()) {
                            foundChild = true;
                            filterTree->getChildVector()->erase(filterTree->getChildVector()->begin() + i);
                            break;
                        }
                    }
                    verify(foundChild);
                    solnRoot->filter.reset(filterTree);
                }

                solnRoot->numWanted = query.getParsed().getNumToReturn();
                if (0 == solnRoot->numWanted) {
                    solnRoot->numWanted = 100;
                }
                solnRoot->indexKeyPattern = relevantIndices[tag->first[i]].keyPattern;

                // Remove the 2d index.  2d can only be the first field, and we know there is
                // only one GEO_NEAR, so we don't care if anyone else was assigned it; it'll
                // only be first for gnNode.
                tag->first.erase(tag->first.begin() + i);

                QuerySolution* soln = new QuerySolution();
                soln->root.reset(solnRoot);
                soln->ns = query.ns();
                soln->filterData = query.getQueryObj();
                out->push_back(soln);
            }

            // Continue planning w/non-2d indices tagged for this pred.
            tag->first.swap(newFirst);

            if (0 == tag->first.size() && 0 == tag->notFirst.size()) {
                return;
            }
        }

        // Likewise, if there is a TEXT it must have an index it can use directly.
        MatchExpression* textNode;
        if (QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT, &textNode)) {
            RelevantTag* tag = static_cast<RelevantTag*>(textNode->getTag());
            if (0 == tag->first.size() && 0 == tag->notFirst.size()) {
                return;
            }
        }

        // If we have any relevant indices, we try to create indexed plans.
        if (0 < relevantIndices.size()) {
            // The enumerator spits out trees tagged with IndexTag(s).
            PlanEnumerator isp(query.root(), &relevantIndices);
            isp.init();

            MatchExpression* rawTree;
            while (isp.getNext(&rawTree)) {
                QLOG() << "about to build solntree from tagged tree:\n" << rawTree->toString()
                     << endl;

                // This can fail if enumeration makes a mistake.
                QuerySolutionNode* solnRoot = buildIndexedDataAccess(rawTree, false,
                                                                     relevantIndices);
                if (NULL == solnRoot) { continue; }

                // This shouldn't ever fail.
                QuerySolution* soln = analyzeDataAccess(query, solnRoot);
                verify(NULL != soln);

                QLOG() << "Planner: adding solution:\n" << soln->toString() << endl;
                out->push_back(soln);
            }
        }

        QLOG() << "Planner: outputted " << out->size() << " indexed solutions.\n";

        // An index was hinted.  If there are any solutions, they use the hinted index.  If not, we
        // scan the entire index to provide results and output that as our plan.  This is the
        // desired behavior when an index is hinted that is not relevant to the query.
        if (!hintIndex.isEmpty() && (0 == out->size())) {
            QuerySolutionNode* solnRoot = NULL;

            // Build an ixscan over the id index, use it, and return it.
            IndexScanNode* isn = new IndexScanNode();
            isn->indexKeyPattern = hintIndex;
            isn->indexIsMultiKey = indices[hintIndexNumber].multikey;
            isn->bounds.fields.resize(hintIndex.nFields());

            // TODO: can we use simple bounds with this compound idx?
            BSONObjIterator it(isn->indexKeyPattern);
            int field = 0;
            while (it.more()) {
                IndexBoundsBuilder::allValuesForField(it.next(), &isn->bounds.fields[field]);
                ++field;
            }

            MatchExpression* filter = query.root()->shallowClone();
            // If it's find({}) remove the no-op root.
            if (MatchExpression::AND == filter->matchType() && (0 == filter->numChildren())) {
                delete filter;
                solnRoot = isn;
            }
            else {
                // TODO: We may not need to do the fetch if the predicates in root are covered.  But
                // for now it's safe (though *maybe* slower).
                FetchNode* fetch = new FetchNode();
                fetch->filter.reset(filter);
                fetch->child.reset(isn);
                solnRoot = fetch;
            }

            QuerySolution* soln = analyzeDataAccess(query, solnRoot);
            verify(NULL != soln);
            out->push_back(soln);

            QLOG() << "Planner: using hinted index as scan, soln = " << soln->toString() << endl;
            return;
        }

        // If a sort order is requested, there may be an index that provides it, even if that
        // index is not over any predicates in the query.
        //
        // XXX XXX: Can we do this even if the index is sparse?  Might we miss things?
        if (!query.getParsed().getSort().isEmpty()
            && !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR)) {

            // See if we have a sort provided from an index already.
            bool usingIndexToSort = false;
            for (size_t i = 0; i < out->size(); ++i) {
                QuerySolution* soln = (*out)[i];
                if (!soln->hasSortStage) {
                    usingIndexToSort = true;
                    break;
                }
            }

            if (!usingIndexToSort) {
                for (size_t i = 0; i < indices.size(); ++i) {
                    const BSONObj& kp = indices[i].keyPattern;
                    if (0 == kp.woCompare(query.getParsed().getSort())) {
                        IndexScanNode* isn = new IndexScanNode();
                        isn->indexKeyPattern = kp;
                        isn->indexIsMultiKey = indices[i].multikey;
                        isn->bounds.fields.resize(kp.nFields());

                        // TODO: can we use simple bounds if compound?
                        BSONObjIterator it(isn->indexKeyPattern);
                        size_t field = 0;
                        while (it.more()) {
                            IndexBoundsBuilder::allValuesForField(it.next(),
                                                                  &isn->bounds.fields[field]);
                        }

                        // TODO: We may not need to do the fetch if the predicates in root are
                        // covered.  But for now it's safe (though *maybe* slower).
                        FetchNode* fetch = new FetchNode();
                        fetch->filter.reset(query.root()->shallowClone());
                        fetch->child.reset(isn);

                        QuerySolution* soln = analyzeDataAccess(query, fetch);
                        verify(NULL != soln);
                        out->push_back(soln);
                        QLOG() << "Planner: using index to provide sort, soln = " << soln->toString() << endl;
                        break;
                    }
                }
            }
        }

        // TODO: Do we always want to offer a collscan solution?
        // XXX: currently disabling the always-use-a-collscan in order to find more planner bugs.
        if (    !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR)
             && !QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT)
             && ((options & QueryPlanner::INCLUDE_COLLSCAN) || (0 == out->size() && canTableScan)))
        {
            QuerySolution* collscan = makeCollectionScan(query, false);
            out->push_back(collscan);
            QLOG() << "Planner: outputting a collscan\n";
        }
    }

}  // namespace mongo
