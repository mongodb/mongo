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

#include "mongo/db/query/planner_access.h"

#include <vector>

#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"

namespace mongo {

    // static
    QuerySolutionNode* QueryPlannerAccess::makeCollectionScan(const CanonicalQuery& query,
                                                              bool tailable,
                                                              const QueryPlannerParams& params) {
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
        return csn;
    }

    // static
    QuerySolutionNode* QueryPlannerAccess::makeLeafNode(const IndexEntry& index,
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

    void QueryPlannerAccess::mergeWithLeafNode(MatchExpression* expr, const IndexEntry& index,
                                         size_t pos, bool* exactOut, QuerySolutionNode* node,
                                         MatchExpression::MatchType mergeType) {

        const StageType type = node->getType();
        verify(STAGE_GEO_NEAR_2D != type);

        if (STAGE_GEO_2D == type) {
            // XXX: 'expr' is possibly indexed by 'node'.  Right now we don't take advantage
            // of covering for 2d indices.
            *exactOut = false;
            return;
        }

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

    // static
    void QueryPlannerAccess::alignBounds(IndexBounds* bounds, const BSONObj& kp, int scanDir) {
        BSONObjIterator it(kp);
        size_t oilIdx = 0;
        while (it.more()) {
            BSONElement elt = it.next();
            int direction = (elt.numberInt() >= 0) ? 1 : -1;
            direction *= scanDir;
            if (-1 == direction) {
                vector<Interval>& iv = bounds->fields[oilIdx].intervals;
                // Step 1: reverse the list.
                std::reverse(iv.begin(), iv.end());
                // Step 2: reverse each interval.
                for (size_t i = 0; i < iv.size(); ++i) {
                    QLOG() << "reversing " << iv[i].toString() << endl;
                    iv[i].reverse();
                }
            }
            ++oilIdx;
        }

        if (!bounds->isValidFor(kp, scanDir)) {
            QLOG() << "INVALID BOUNDS: " << bounds->toString() << endl;
            QLOG() << "kp = " << kp.toString() << endl;
            QLOG() << "scanDir = " << scanDir << endl;
            verify(0);
        }
    }

    // static
    void QueryPlannerAccess::finishLeafNode(QuerySolutionNode* node, const IndexEntry& index) {
        const StageType type = node->getType();
        verify(STAGE_GEO_NEAR_2D != type);

        if (STAGE_GEO_2D == type || STAGE_TEXT == type) {
            return;
        }

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

    // static
    bool QueryPlannerAccess::processIndexScans(const CanonicalQuery& query,
                                         MatchExpression* root,
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
                QuerySolutionNode* childSolution = buildIndexedDataAccess(query,
                                                                          child,
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
                // In the AND case, the filter can be brought above the AND node.
                // But in the OR case, the filter only applies to one branch, so
                // we must affix curChild's filter now. In order to apply the filter
                // to the proper OR branch, create a FETCH node with the filter whose
                // child is the IXSCAN.
                else if (root->matchType() == MatchExpression::OR) {
                    verify(NULL != currentScan.get());

                    finishLeafNode(currentScan.get(), indices[currentIndexNumber]);
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);

                    FetchNode* fetch = new FetchNode();
                    // takes ownership
                    fetch->filter.reset(child);
                    // takes ownership
                    fetch->children.push_back(currentScan.release());
                    // takes ownership
                    out->push_back(fetch);

                    currentIndexNumber = IndexTag::kNoIndex;
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
                // In the AND case, the filter can be brought above the AND node.
                // But in the OR case, the filter only applies to one branch, so
                // we must affix curChild's filter now. In order to apply the filter
                // to the proper OR branch, create a FETCH node with the filter whose
                // child is the IXSCAN.
                else if (root->matchType() == MatchExpression::OR) {
                    verify(NULL != currentScan.get());

                    finishLeafNode(currentScan.get(), indices[currentIndexNumber]);
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);

                    FetchNode* fetch = new FetchNode();
                    // takes ownership
                    fetch->filter.reset(child);
                    // takes ownership
                    fetch->children.push_back(currentScan.release());
                    // takes ownership
                    out->push_back(fetch);

                    currentIndexNumber = IndexTag::kNoIndex;
                }
                else {
                    // We keep curChild in the AND for affixing later as a filter.
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
    QuerySolutionNode* QueryPlannerAccess::buildIndexedAnd(const CanonicalQuery& query,
                                                     MatchExpression* root,
                                                     bool inArrayOperator,
                                                     const vector<IndexEntry>& indices) {
        auto_ptr<MatchExpression> autoRoot;
        if (!inArrayOperator) {
            autoRoot.reset(root);
        }

        vector<QuerySolutionNode*> ixscanNodes;
        if (!processIndexScans(query, root, inArrayOperator, indices, &ixscanNodes)) {
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
            fetch->children.push_back(andResult);
            andResult = fetch;
        }
        else {
            // root has no children, let autoRoot get rid of it when it goes out of scope.
        }

        return andResult;
    }

    // static
    QuerySolutionNode* QueryPlannerAccess::buildIndexedOr(const CanonicalQuery& query,
                                                    MatchExpression* root,
                                                    bool inArrayOperator,
                                                    const vector<IndexEntry>& indices) {
        auto_ptr<MatchExpression> autoRoot;
        if (!inArrayOperator) {
            autoRoot.reset(root);
        }

        vector<QuerySolutionNode*> ixscanNodes;
        if (!processIndexScans(query, root, inArrayOperator, indices, &ixscanNodes)) {
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
            bool shouldMergeSort = false;

            if (!query.getParsed().getSort().isEmpty()) {
                const BSONObj& desiredSort = query.getParsed().getSort();

                // If there exists a sort order that is present in each child, we can merge them and
                // maintain that sort order / those sort orders.
                ixscanNodes[0]->computeProperties();
                BSONObjSet sharedSortOrders = ixscanNodes[0]->getSort();

                if (!sharedSortOrders.empty()) {
                    for (size_t i = 1; i < ixscanNodes.size(); ++i) {
                        ixscanNodes[i]->computeProperties();
                        BSONObjSet isect;
                        set_intersection(sharedSortOrders.begin(),
                                sharedSortOrders.end(),
                                ixscanNodes[i]->getSort().begin(),
                                ixscanNodes[i]->getSort().end(),
                                std::inserter(isect, isect.end()),
                                BSONObjCmp());
                        sharedSortOrders = isect;
                        if (sharedSortOrders.empty()) {
                            break;
                        }
                    }
                }

                // XXX: consider reversing?
                shouldMergeSort = (sharedSortOrders.end() != sharedSortOrders.find(desiredSort));
            }

            if (shouldMergeSort) {
                MergeSortNode* msn = new MergeSortNode();
                msn->sort = query.getParsed().getSort();
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
    QuerySolutionNode* QueryPlannerAccess::buildIndexedDataAccess(const CanonicalQuery& query,
                                                            MatchExpression* root,
                                                            bool inArrayOperator,
                                                            const vector<IndexEntry>& indices) {
        if (root->isLogical()) {
            if (MatchExpression::AND == root->matchType()) {
                // Takes ownership of root.
                return buildIndexedAnd(query, root, inArrayOperator, indices);
            }
            else if (MatchExpression::OR == root->matchType()) {
                // Takes ownership of root.
                return buildIndexedOr(query, root, inArrayOperator, indices);
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
                fetch->children.push_back(soln);
                return fetch;
            }
            else if (Indexability::arrayUsesIndexOnChildren(root)) {
                QuerySolutionNode* solution = NULL;

                if (MatchExpression::ALL == root->matchType()) {
                    // Here, we formulate an AND of all the sub-clauses.
                    auto_ptr<AndHashNode> ahn(new AndHashNode());

                    for (size_t i = 0; i < root->numChildren(); ++i) {
                        QuerySolutionNode* node = buildIndexedDataAccess(query,
                                                                         root->getChild(i),
                                                                         true,
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
                    solution = buildIndexedDataAccess(query, root->getChild(0), true, indices);
                    if (NULL == solution) { return NULL; }
                }

                // There may be an array operator above us.
                if (inArrayOperator) { return solution; }

                FetchNode* fetch = new FetchNode();
                // Takes ownership of 'root'.
                verify(NULL != autoRoot.get());
                fetch->filter.reset(autoRoot.release());
                fetch->children.push_back(solution);
                return fetch;
            }
        }

        return NULL;
    }

    QuerySolutionNode* QueryPlannerAccess::scanWholeIndex(const IndexEntry& index,
                                                          const CanonicalQuery& query,
                                                          const QueryPlannerParams& params,
                                                          int direction) {
        QuerySolutionNode* solnRoot = NULL;

        // Build an ixscan over the id index, use it, and return it.
        IndexScanNode* isn = new IndexScanNode();
        isn->indexKeyPattern = index.keyPattern;
        isn->indexIsMultiKey = index.multikey;
        isn->bounds.fields.resize(index.keyPattern.nFields());

        // TODO: can we use simple bounds with this compound idx?
        BSONObjIterator it(isn->indexKeyPattern);
        int field = 0;
        while (it.more()) {
            IndexBoundsBuilder::allValuesForField(it.next(), &isn->bounds.fields[field]);
            ++field;
        }
        alignBounds(&isn->bounds, isn->indexKeyPattern);

        if (-1 == direction) {
            QueryPlannerCommon::reverseScans(isn);
            isn->direction = -1;
        }

        MatchExpression* filter = query.root()->shallowClone();

        // If it's find({}) remove the no-op root.
        if (MatchExpression::AND == filter->matchType() && (0 == filter->numChildren())) {
            // XXX wasteful fix
            delete filter;
            solnRoot = isn;
        }
        else {
            // TODO: We may not need to do the fetch if the predicates in root are covered.  But
            // for now it's safe (though *maybe* slower).
            FetchNode* fetch = new FetchNode();
            fetch->filter.reset(filter);
            fetch->children.push_back(isn);
            solnRoot = fetch;
        }

        return solnRoot;
    }

}  // namespace mongo
