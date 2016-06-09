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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/planner_access.h"

#include <algorithm>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace {

using namespace mongo;

namespace dps = ::mongo::dotted_path_support;

/**
 * Text node functors.
 */
bool isTextNode(const QuerySolutionNode* node) {
    return STAGE_TEXT == node->getType();
}

/**
 * Casts 'node' to a FetchNode* if it is a FetchNode, otherwise returns null.
 */
FetchNode* getFetchNode(QuerySolutionNode* node) {
    if (STAGE_FETCH != node->getType()) {
        return nullptr;
    }

    return static_cast<FetchNode*>(node);
}

/**
 * If 'node' is an index scan node, casts it to IndexScanNode*. If 'node' is a FetchNode with an
 * IndexScanNode child, then returns a pointer to the child index scan node. Otherwise returns
 * null.
 */
const IndexScanNode* getIndexScanNode(const QuerySolutionNode* node) {
    if (STAGE_IXSCAN == node->getType()) {
        return static_cast<const IndexScanNode*>(node);
    } else if (STAGE_FETCH == node->getType()) {
        invariant(1U == node->children.size());
        const QuerySolutionNode* child = node->children[0];
        if (STAGE_IXSCAN == child->getType()) {
            return static_cast<const IndexScanNode*>(child);
        }
    }

    return nullptr;
}

/**
 * Takes as input two query solution nodes returned by processIndexScans(). If both are
 * IndexScanNode or FetchNode with an IndexScanNode child and the index scan nodes are identical
 * (same bounds, same filter, same direction, etc.), then returns true. Otherwise returns false.
 */
bool scansAreEquivalent(const QuerySolutionNode* lhs, const QuerySolutionNode* rhs) {
    const IndexScanNode* leftIxscan = getIndexScanNode(lhs);
    const IndexScanNode* rightIxscan = getIndexScanNode(rhs);
    if (!leftIxscan || !rightIxscan) {
        return false;
    }

    return *leftIxscan == *rightIxscan;
}

}  // namespace

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

// static
QuerySolutionNode* QueryPlannerAccess::makeCollectionScan(const CanonicalQuery& query,
                                                          bool tailable,
                                                          const QueryPlannerParams& params) {
    // Make the (only) node, a collection scan.
    CollectionScanNode* csn = new CollectionScanNode();
    csn->name = query.ns();
    csn->filter = query.root()->shallowClone();
    csn->tailable = tailable;
    csn->maxScan = query.getQueryRequest().getMaxScan();

    // If the hint is {$natural: +-1} this changes the direction of the collection scan.
    if (!query.getQueryRequest().getHint().isEmpty()) {
        BSONElement natural =
            dps::extractElementAtPath(query.getQueryRequest().getHint(), "$natural");
        if (!natural.eoo()) {
            csn->direction = natural.numberInt() >= 0 ? 1 : -1;
        }
    }

    // The sort can specify $natural as well. The sort direction should override the hint
    // direction if both are specified.
    const BSONObj& sortObj = query.getQueryRequest().getSort();
    if (!sortObj.isEmpty()) {
        BSONElement natural = dps::extractElementAtPath(sortObj, "$natural");
        if (!natural.eoo()) {
            csn->direction = natural.numberInt() >= 0 ? 1 : -1;
        }
    }

    return csn;
}

// static
QuerySolutionNode* QueryPlannerAccess::makeLeafNode(
    const CanonicalQuery& query,
    const IndexEntry& index,
    size_t pos,
    MatchExpression* expr,
    IndexBoundsBuilder::BoundsTightness* tightnessOut) {
    // We're guaranteed that all GEO_NEARs are first.  This slightly violates the "sort index
    // predicates by their position in the compound index" rule but GEO_NEAR isn't an ixscan.
    // This saves our bacon when we have {foo: 1, bar: "2dsphere"} and the predicate on bar is a
    // $near.  If we didn't get the GEO_NEAR first we'd create an IndexScanNode and later cast
    // it to a GeoNear2DSphereNode
    //
    // This should gracefully deal with the case where we have a pred over foo but no geo clause
    // over bar.  In that case there is no GEO_NEAR to appear first and it's treated like a
    // straight ixscan.

    if (MatchExpression::GEO_NEAR == expr->matchType()) {
        // We must not keep the expression node around.
        *tightnessOut = IndexBoundsBuilder::EXACT;
        GeoNearMatchExpression* nearExpr = static_cast<GeoNearMatchExpression*>(expr);

        BSONElement elt = index.keyPattern.firstElement();
        bool indexIs2D = (String == elt.type() && "2d" == elt.String());

        if (indexIs2D) {
            GeoNear2DNode* ret = new GeoNear2DNode();
            ret->indexKeyPattern = index.keyPattern;
            ret->nq = &nearExpr->getData();
            ret->baseBounds.fields.resize(index.keyPattern.nFields());
            if (NULL != query.getProj()) {
                ret->addPointMeta = query.getProj()->wantGeoNearPoint();
                ret->addDistMeta = query.getProj()->wantGeoNearDistance();
            }

            return ret;
        } else {
            GeoNear2DSphereNode* ret = new GeoNear2DSphereNode();
            ret->indexKeyPattern = index.keyPattern;
            ret->nq = &nearExpr->getData();
            ret->baseBounds.fields.resize(index.keyPattern.nFields());
            if (NULL != query.getProj()) {
                ret->addPointMeta = query.getProj()->wantGeoNearPoint();
                ret->addDistMeta = query.getProj()->wantGeoNearDistance();
            }
            return ret;
        }
    } else if (MatchExpression::TEXT == expr->matchType()) {
        // We must not keep the expression node around.
        *tightnessOut = IndexBoundsBuilder::EXACT;
        TextMatchExpressionBase* textExpr = static_cast<TextMatchExpressionBase*>(expr);
        TextNode* ret = new TextNode();
        ret->indexKeyPattern = index.keyPattern;
        ret->ftsQuery = textExpr->getFTSQuery().clone();
        return ret;
    } else {
        // Note that indexKeyPattern.firstElement().fieldName() may not equal expr->path()
        // because expr might be inside an array operator that provides a path prefix.
        IndexScanNode* isn = new IndexScanNode();
        isn->indexKeyPattern = index.keyPattern;
        isn->indexIsMultiKey = index.multikey;
        isn->bounds.fields.resize(index.keyPattern.nFields());
        isn->maxScan = query.getQueryRequest().getMaxScan();
        isn->addKeyMetadata = query.getQueryRequest().returnKey();
        isn->indexCollator = index.collator;
        isn->queryCollator = query.getCollator();

        // Get the ixtag->pos-th element of the index key pattern.
        // TODO: cache this instead/with ixtag->pos?
        BSONObjIterator it(index.keyPattern);
        BSONElement keyElt = it.next();
        for (size_t i = 0; i < pos; ++i) {
            verify(it.more());
            keyElt = it.next();
        }
        verify(!keyElt.eoo());

        IndexBoundsBuilder::translate(expr, keyElt, index, &isn->bounds.fields[pos], tightnessOut);

        return isn;
    }
}

bool QueryPlannerAccess::shouldMergeWithLeaf(const MatchExpression* expr,
                                             const ScanBuildingState& scanState) {
    const QuerySolutionNode* node = scanState.currentScan.get();
    if (NULL == node || NULL == expr) {
        return false;
    }

    if (NULL == scanState.ixtag) {
        return false;
    }

    if (scanState.currentIndexNumber != scanState.ixtag->index) {
        return false;
    }

    size_t pos = scanState.ixtag->pos;
    const IndexEntry& index = scanState.indices[scanState.currentIndexNumber];
    const MatchExpression::MatchType mergeType = scanState.root->matchType();

    const StageType type = node->getType();
    const MatchExpression::MatchType exprType = expr->matchType();

    //
    // First handle special solution tree leaf types. In general, normal index bounds
    // building is not used for special leaf types, and hence we cannot merge leaves.
    //
    // This rule is always true for OR, but there are exceptions for AND.
    // Specifically, we can often merge a predicate with a special leaf type
    // by adding a filter to the special leaf type.
    //

    if (STAGE_TEXT == type) {
        // Currently only one text predicate is allowed, but to be safe, make sure that we
        // do not try to merge two text predicates.
        return MatchExpression::AND == mergeType && MatchExpression::TEXT != exprType;
    }

    if (STAGE_GEO_NEAR_2D == type || STAGE_GEO_NEAR_2DSPHERE == type) {
        // Currently only one GEO_NEAR is allowed, but to be safe, make sure that we
        // do not try to merge two GEO_NEAR predicates.
        return MatchExpression::AND == mergeType && MatchExpression::GEO_NEAR != exprType;
    }

    //
    // If we're here, then we're done checking for special leaf nodes, and the leaf
    // must be a regular index scan.
    //

    invariant(type == STAGE_IXSCAN);
    const IndexScanNode* scan = static_cast<const IndexScanNode*>(node);
    const IndexBounds* boundsToFillOut = &scan->bounds;

    if (boundsToFillOut->fields[pos].name.empty()) {
        // Bounds have yet to be assigned for the 'pos' position in the index. The plan enumerator
        // should have told us that it is safe to compound bounds in this case.
        invariant(scanState.ixtag->canCombineBounds);
        return true;
    } else {
        // Bounds have already been assigned for the 'pos' position in the index.
        if (MatchExpression::AND == mergeType) {
            // The bounds on the 'pos' position in the index would be intersected if we merged these
            // two leaf expressions.
            if (!scanState.ixtag->canCombineBounds) {
                // If the plan enumerator told us that it isn't safe to intersect bounds in this
                // case, then it must be because we're using a multikey index.
                invariant(index.multikey);
            }
            return scanState.ixtag->canCombineBounds;
        } else {
            // The bounds will be unionized.
            return true;
        }
    }
}

void QueryPlannerAccess::mergeWithLeafNode(MatchExpression* expr, ScanBuildingState* scanState) {
    QuerySolutionNode* node = scanState->currentScan.get();
    invariant(NULL != node);

    const MatchExpression::MatchType mergeType = scanState->root->matchType();
    size_t pos = scanState->ixtag->pos;
    const IndexEntry& index = scanState->indices[scanState->currentIndexNumber];

    const StageType type = node->getType();

    // Text data is covered, but not exactly.  Text covering is unlike any other covering
    // so we deal with it in addFilterToSolutionNode.
    if (STAGE_TEXT == type) {
        scanState->tightness = IndexBoundsBuilder::INEXACT_COVERED;
        return;
    }

    IndexBounds* boundsToFillOut = NULL;

    if (STAGE_GEO_NEAR_2D == type) {
        invariant(INDEX_2D == index.type);

        // 2D indexes are weird - the "2d" field stores a normally-indexed BinData field, but
        // additional array fields are *not* exploded into multi-keys - they are stored directly
        // as arrays in the index.  Also, no matter what the index expression, the "2d" field is
        // always first.
        // This means that we can only generically accumulate bounds for 2D indexes over the
        // first "2d" field (pos == 0) - MatchExpressions over other fields in the 2D index may
        // be covered (can be evaluated using only the 2D index key).  The additional fields
        // must not affect the index scan bounds, since they are not stored in an
        // IndexScan-compatible format.

        if (pos > 0) {
            // Marking this field as covered allows the planner to accumulate a MatchExpression
            // over the returned 2D index keys instead of adding to the index bounds.
            scanState->tightness = IndexBoundsBuilder::INEXACT_COVERED;
            return;
        }

        // We may have other $geoPredicates on a near index - generate bounds for these
        GeoNear2DNode* gn = static_cast<GeoNear2DNode*>(node);
        boundsToFillOut = &gn->baseBounds;
    } else if (STAGE_GEO_NEAR_2DSPHERE == type) {
        GeoNear2DSphereNode* gn = static_cast<GeoNear2DSphereNode*>(node);
        boundsToFillOut = &gn->baseBounds;
    } else {
        verify(type == STAGE_IXSCAN);
        IndexScanNode* scan = static_cast<IndexScanNode*>(node);

        // See STAGE_GEO_NEAR_2D above - 2D indexes can only accumulate scan bounds over the
        // first "2d" field (pos == 0)
        if (INDEX_2D == index.type && pos > 0) {
            scanState->tightness = IndexBoundsBuilder::INEXACT_COVERED;
            return;
        }

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
    scanState->tightness = IndexBoundsBuilder::INEXACT_FETCH;

    verify(boundsToFillOut->fields.size() > pos);

    OrderedIntervalList* oil = &boundsToFillOut->fields[pos];

    if (boundsToFillOut->fields[pos].name.empty()) {
        IndexBoundsBuilder::translate(expr, keyElt, index, oil, &scanState->tightness);
    } else {
        if (MatchExpression::AND == mergeType) {
            IndexBoundsBuilder::translateAndIntersect(
                expr, keyElt, index, oil, &scanState->tightness);
        } else {
            verify(MatchExpression::OR == mergeType);
            IndexBoundsBuilder::translateAndUnion(expr, keyElt, index, oil, &scanState->tightness);
        }
    }
}

// static
void QueryPlannerAccess::finishTextNode(QuerySolutionNode* node, const IndexEntry& index) {
    TextNode* tn = static_cast<TextNode*>(node);

    // Figure out what positions are prefix positions.  We build an index key prefix from
    // the predicates over the text index prefix keys.
    // For example, say keyPattern = { a: 1, _fts: "text", _ftsx: 1, b: 1 }
    // prefixEnd should be 1.
    size_t prefixEnd = 0;
    BSONObjIterator it(tn->indexKeyPattern);
    // Count how many prefix terms we have.
    while (it.more()) {
        // We know that the only key pattern with a type of String is the _fts field
        // which is immediately after all prefix fields.
        if (String == it.next().type()) {
            break;
        }
        ++prefixEnd;
    }

    // If there's no prefix, the filter is already on the node and the index prefix is null.
    // We can just return.
    if (!prefixEnd) {
        return;
    }

    // We can't create a text stage if there aren't EQ predicates on its prefix terms.  So
    // if we've made it this far, we should have collected the prefix predicates in the
    // filter.
    invariant(NULL != tn->filter.get());
    MatchExpression* textFilterMe = tn->filter.get();

    BSONObjBuilder prefixBob;

    if (MatchExpression::AND != textFilterMe->matchType()) {
        // Only one prefix term.
        invariant(1 == prefixEnd);
        // Sanity check: must be an EQ.
        invariant(MatchExpression::EQ == textFilterMe->matchType());

        EqualityMatchExpression* eqExpr = static_cast<EqualityMatchExpression*>(textFilterMe);
        prefixBob.append(eqExpr->getData());
        tn->filter.reset();
    } else {
        invariant(MatchExpression::AND == textFilterMe->matchType());

        // Indexed by the keyPattern position index assignment.  We want to add
        // prefixes in order but we must order them first.
        vector<MatchExpression*> prefixExprs(prefixEnd, NULL);

        AndMatchExpression* amExpr = static_cast<AndMatchExpression*>(textFilterMe);
        invariant(amExpr->numChildren() >= prefixEnd);

        // Look through the AND children.  The prefix children we want to
        // stash in prefixExprs.
        size_t curChild = 0;
        while (curChild < amExpr->numChildren()) {
            MatchExpression* child = amExpr->getChild(curChild);
            IndexTag* ixtag = static_cast<IndexTag*>(child->getTag());
            invariant(NULL != ixtag);
            // Skip this child if it's not part of a prefix, or if we've already assigned a
            // predicate to this prefix position.
            if (ixtag->pos >= prefixEnd || prefixExprs[ixtag->pos] != NULL) {
                ++curChild;
                continue;
            }
            // prefixExprs takes ownership of 'child'.
            prefixExprs[ixtag->pos] = child;
            amExpr->getChildVector()->erase(amExpr->getChildVector()->begin() + curChild);
            // Don't increment curChild.
        }

        // Go through the prefix equalities in order and create an index prefix out of them.
        for (size_t i = 0; i < prefixExprs.size(); ++i) {
            MatchExpression* prefixMe = prefixExprs[i];
            invariant(NULL != prefixMe);
            invariant(MatchExpression::EQ == prefixMe->matchType());
            EqualityMatchExpression* eqExpr = static_cast<EqualityMatchExpression*>(prefixMe);
            prefixBob.append(eqExpr->getData());
            // We removed this from the AND expression that owned it, so we must clean it
            // up ourselves.
            delete prefixMe;
        }

        // Clear out an empty $and.
        if (0 == amExpr->numChildren()) {
            tn->filter.reset();
        } else if (1 == amExpr->numChildren()) {
            // Clear out unsightly only child of $and
            MatchExpression* child = amExpr->getChild(0);
            amExpr->getChildVector()->clear();
            // Deletes current filter which is amExpr.
            tn->filter.reset(child);
        }
    }

    tn->indexPrefix = prefixBob.obj();
}

// static
bool QueryPlannerAccess::orNeedsFetch(const ScanBuildingState* scanState) {
    if (scanState->loosestBounds == IndexBoundsBuilder::EXACT) {
        return false;
    } else if (scanState->loosestBounds == IndexBoundsBuilder::INEXACT_FETCH) {
        return true;
    } else {
        invariant(scanState->loosestBounds == IndexBoundsBuilder::INEXACT_COVERED);
        const IndexEntry& index = scanState->indices[scanState->currentIndexNumber];
        return index.multikey;
    }
}

// static
void QueryPlannerAccess::finishAndOutputLeaf(ScanBuildingState* scanState,
                                             vector<QuerySolutionNode*>* out) {
    finishLeafNode(scanState->currentScan.get(), scanState->indices[scanState->currentIndexNumber]);

    if (MatchExpression::OR == scanState->root->matchType()) {
        if (orNeedsFetch(scanState)) {
            // In order to correctly evaluate the predicates for this index, we have to
            // fetch the full documents. Add a fetch node above the index scan whose filter
            // includes *all* of the predicates used to generate the ixscan.
            FetchNode* fetch = new FetchNode();
            // Takes ownership.
            fetch->filter.reset(scanState->curOr.release());
            // Takes ownership.
            fetch->children.push_back(scanState->currentScan.release());

            scanState->currentScan.reset(fetch);
        } else if (scanState->loosestBounds == IndexBoundsBuilder::INEXACT_COVERED) {
            // This an OR, at least one of the predicates used to generate 'currentScan'
            // is inexact covered, but none is inexact fetch. This means that we can put
            // these predicates, joined by an $or, as filters on the index scan. This avoids
            // a fetch and allows the predicates to be covered by the index.
            //
            // Ex.
            //   Say we have index {a: 1} and query {$or: [{a: /foo/}, {a: /bar/}]}.
            //   The entire query, {$or: [{a: /foo/}, {a: /bar/}]}, should be a filter
            //   in the index scan stage itself.
            scanState->currentScan->filter.reset(scanState->curOr.release());
        }
    }

    out->push_back(scanState->currentScan.release());
}

// static
void QueryPlannerAccess::finishLeafNode(QuerySolutionNode* node, const IndexEntry& index) {
    const StageType type = node->getType();

    if (STAGE_TEXT == type) {
        finishTextNode(node, index);
        return;
    }

    IndexBounds* bounds = NULL;

    if (STAGE_GEO_NEAR_2D == type) {
        GeoNear2DNode* gnode = static_cast<GeoNear2DNode*>(node);
        bounds = &gnode->baseBounds;
    } else if (STAGE_GEO_NEAR_2DSPHERE == type) {
        GeoNear2DSphereNode* gnode = static_cast<GeoNear2DSphereNode*>(node);
        bounds = &gnode->baseBounds;
    } else {
        verify(type == STAGE_IXSCAN);
        IndexScanNode* scan = static_cast<IndexScanNode*>(node);
        bounds = &scan->bounds;
    }

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
        IndexBoundsBuilder::alignBounds(bounds, index.keyPattern);
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
            IndexBoundsBuilder::allValuesForField(kpElt, &bounds->fields[firstEmptyField]);
        }
        ++firstEmptyField;
    }

    // Make sure that the length of the key is the length of the bounds we started.
    verify(firstEmptyField == bounds->fields.size());

    // We create bounds assuming a forward direction but can easily reverse bounds to align
    // according to our desired direction.
    IndexBoundsBuilder::alignBounds(bounds, index.keyPattern);
}

// static
void QueryPlannerAccess::findElemMatchChildren(const MatchExpression* node,
                                               vector<MatchExpression*>* out,
                                               vector<MatchExpression*>* subnodesOut) {
    for (size_t i = 0; i < node->numChildren(); ++i) {
        MatchExpression* child = node->getChild(i);
        if (Indexability::isBoundsGenerating(child) && NULL != child->getTag()) {
            out->push_back(child);
        } else if (MatchExpression::AND == child->matchType() ||
                   Indexability::arrayUsesIndexOnChildren(child)) {
            findElemMatchChildren(child, out, subnodesOut);
        } else if (NULL != child->getTag()) {
            subnodesOut->push_back(child);
        }
    }
}

// static
std::vector<QuerySolutionNode*> QueryPlannerAccess::collapseEquivalentScans(
    const std::vector<QuerySolutionNode*> scans) {
    OwnedPointerVector<QuerySolutionNode> ownedScans(scans);
    invariant(ownedScans.size() > 0);

    // Scans that need to be collapsed will be adjacent to each other in the list due to how we
    // sort the query predicate. We step through the list, either merging the current scan into
    // the last scan in 'collapsedScans', or adding a new entry to 'collapsedScans' if it can't
    // be merged.
    OwnedPointerVector<QuerySolutionNode> collapsedScans;

    collapsedScans.push_back(ownedScans.releaseAt(0));
    for (size_t i = 1; i < ownedScans.size(); ++i) {
        if (scansAreEquivalent(collapsedScans.back(), ownedScans[i])) {
            // We collapse the entry from 'ownedScans' into the back of 'collapsedScans'.
            std::unique_ptr<QuerySolutionNode> collapseFrom(ownedScans.releaseAt(i));
            FetchNode* collapseFromFetch = getFetchNode(collapseFrom.get());
            FetchNode* collapseIntoFetch = getFetchNode(collapsedScans.back());

            // If there's no filter associated with a fetch node on 'collapseFrom', all we have to
            // do is clear the filter on the node that we are collapsing into.
            if (!collapseFromFetch || !collapseFromFetch->filter.get()) {
                if (collapseIntoFetch) {
                    collapseIntoFetch->filter.reset();
                }
                continue;
            }

            // If there's no filter associated with a fetch node on the back of the 'collapsedScans'
            // list, then there's nothing more to do.
            if (!collapseIntoFetch || !collapseIntoFetch->filter.get()) {
                continue;
            }

            // Both the 'from' and 'into' nodes have filters. We join them with an
            // OrMatchExpression.
            std::unique_ptr<OrMatchExpression> collapsedFilter =
                stdx::make_unique<OrMatchExpression>();
            collapsedFilter->add(collapseFromFetch->filter.release());
            collapsedFilter->add(collapseIntoFetch->filter.release());

            // Normalize the filter and add it to 'into'.
            collapseIntoFetch->filter.reset(
                CanonicalQuery::normalizeTree(collapsedFilter.release()));
        } else {
            // Scans are not equivalent and can't be collapsed.
            collapsedScans.push_back(ownedScans.releaseAt(i));
        }
    }

    invariant(collapsedScans.size() > 0);
    return collapsedScans.release();
}

// static
bool QueryPlannerAccess::processIndexScans(const CanonicalQuery& query,
                                           MatchExpression* root,
                                           bool inArrayOperator,
                                           const std::vector<IndexEntry>& indices,
                                           const QueryPlannerParams& params,
                                           std::vector<QuerySolutionNode*>* out) {
    // Initialize the ScanBuildingState.
    ScanBuildingState scanState(root, inArrayOperator, indices);

    while (scanState.curChild < root->numChildren()) {
        MatchExpression* child = root->getChild(scanState.curChild);

        // If there is no tag, it's not using an index.  We've sorted our children such that the
        // children with tags are first, so we stop now.
        if (NULL == child->getTag()) {
            break;
        }

        scanState.ixtag = static_cast<IndexTag*>(child->getTag());
        // If there's a tag it must be valid.
        verify(IndexTag::kNoIndex != scanState.ixtag->index);

        // If the child can't use an index on its own field (and the child is not a negation
        // of a bounds-generating expression), then it's indexed by virtue of one of
        // its children having an index.
        //
        // NOTE: If the child is logical, it could possibly collapse into a single ixscan.  we
        // ignore this for now.
        if (!Indexability::isBoundsGenerating(child)) {
            // If we're here, then the child is indexed by virtue of its children.
            // In most cases this means that we recursively build indexed data
            // access on 'child'.
            if (!processIndexScansSubnode(query, &scanState, params, out)) {
                return false;
            }
            continue;
        }

        // If we're here, we now know that 'child' can use an index directly and the index is
        // over the child's field.

        // If 'child' is a NOT, then the tag we're interested in is on the NOT's
        // child node.
        if (MatchExpression::NOT == child->matchType()) {
            scanState.ixtag = static_cast<IndexTag*>(child->getChild(0)->getTag());
            invariant(IndexTag::kNoIndex != scanState.ixtag->index);
        }

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
        // If the index is multikey, there are arrays of values.  There are several
        // complications in the multikey case that have to be obeyed both by the enumerator
        // and here as we try to merge predicates into query solution leaves. The hairy
        // details of these rules are documented near the top of planner_access.h.
        if (shouldMergeWithLeaf(child, scanState)) {
            // The child uses the same index we're currently building a scan for.  Merge
            // the bounds and filters.
            verify(scanState.currentIndexNumber == scanState.ixtag->index);
            scanState.tightness = IndexBoundsBuilder::INEXACT_FETCH;
            mergeWithLeafNode(child, &scanState);
            handleFilter(&scanState);
        } else {
            if (NULL != scanState.currentScan.get()) {
                // Output the current scan before starting to construct a new out.
                finishAndOutputLeaf(&scanState, out);
            } else {
                verify(IndexTag::kNoIndex == scanState.currentIndexNumber);
            }

            // Reset state before producing a new leaf.
            scanState.resetForNextScan(scanState.ixtag);

            scanState.currentScan.reset(makeLeafNode(query,
                                                     indices[scanState.currentIndexNumber],
                                                     scanState.ixtag->pos,
                                                     child,
                                                     &scanState.tightness));

            handleFilter(&scanState);
        }
    }

    // Output the scan we're done with, if it exists.
    if (NULL != scanState.currentScan.get()) {
        finishAndOutputLeaf(&scanState, out);
    }

    return true;
}

// static
bool QueryPlannerAccess::processIndexScansElemMatch(const CanonicalQuery& query,
                                                    ScanBuildingState* scanState,
                                                    const QueryPlannerParams& params,
                                                    std::vector<QuerySolutionNode*>* out) {
    MatchExpression* root = scanState->root;
    MatchExpression* child = root->getChild(scanState->curChild);
    const vector<IndexEntry>& indices = scanState->indices;

    // We have an AND with an ELEM_MATCH_OBJECT child. The plan enumerator produces
    // index taggings which indicate that we should try to compound with
    // predicates retrieved from inside the subtree rooted at the ELEM_MATCH.
    // In order to obey the enumerator's tagging, we need to retrieve these
    // predicates from inside the $elemMatch, and try to merge them with
    // the current index scan.

    // Contains tagged predicates from inside the tree rooted at 'child'
    // which are logically part of the AND.
    vector<MatchExpression*> emChildren;

    // Contains tagged nodes that are not logically part of the AND and
    // cannot use the index directly (e.g. OR nodes which are tagged to
    // be indexed).
    vector<MatchExpression*> emSubnodes;

    // Populate 'emChildren' and 'emSubnodes'.
    findElemMatchChildren(child, &emChildren, &emSubnodes);

    // Recursively build data access for the nodes inside 'emSubnodes'.
    for (size_t i = 0; i < emSubnodes.size(); ++i) {
        MatchExpression* subnode = emSubnodes[i];

        if (!Indexability::isBoundsGenerating(subnode)) {
            // Must pass true for 'inArrayOperator' because the subnode is
            // beneath an ELEM_MATCH_OBJECT.
            QuerySolutionNode* childSolution =
                buildIndexedDataAccess(query, subnode, true, indices, params);

            // buildIndexedDataAccess(...) returns NULL in error conditions, when
            // it is unable to construct a query solution from a tagged match
            // expression tree. If we are unable to construct a solution according
            // to the instructions from the enumerator, then we bail out early
            // (by returning false) rather than continuing on and potentially
            // constructing an invalid solution tree.
            if (NULL == childSolution) {
                return false;
            }

            // Output the resulting solution tree.
            out->push_back(childSolution);
        }
    }

    // For each predicate in 'emChildren', try to merge it with the current index scan.
    //
    // This loop is similar to that in processIndexScans(...), except it does not call into
    // handleFilters(...). Instead, we leave the entire $elemMatch filter intact. This way,
    // the complete $elemMatch expression will be affixed as a filter later on.
    for (size_t i = 0; i < emChildren.size(); ++i) {
        MatchExpression* emChild = emChildren[i];
        invariant(NULL != emChild->getTag());
        scanState->ixtag = static_cast<IndexTag*>(emChild->getTag());

        // If 'emChild' is a NOT, then the tag we're interested in is on the NOT's
        // child node.
        if (MatchExpression::NOT == emChild->matchType()) {
            invariant(NULL != emChild->getChild(0)->getTag());
            scanState->ixtag = static_cast<IndexTag*>(emChild->getChild(0)->getTag());
            invariant(IndexTag::kNoIndex != scanState->ixtag->index);
        }

        if (shouldMergeWithLeaf(emChild, *scanState)) {
            // The child uses the same index we're currently building a scan for.  Merge
            // the bounds and filters.
            verify(scanState->currentIndexNumber == scanState->ixtag->index);

            scanState->tightness = IndexBoundsBuilder::INEXACT_FETCH;
            mergeWithLeafNode(emChild, scanState);
        } else {
            if (NULL != scanState->currentScan.get()) {
                finishAndOutputLeaf(scanState, out);
            } else {
                verify(IndexTag::kNoIndex == scanState->currentIndexNumber);
            }

            scanState->currentIndexNumber = scanState->ixtag->index;

            scanState->tightness = IndexBoundsBuilder::INEXACT_FETCH;
            scanState->currentScan.reset(makeLeafNode(query,
                                                      indices[scanState->currentIndexNumber],
                                                      scanState->ixtag->pos,
                                                      emChild,
                                                      &scanState->tightness));
        }
    }

    // We're done processing the $elemMatch child. We leave it hanging off
    // it's AND parent so that it will be affixed as a filter later on,
    // and move on to the next child of the AND.
    ++scanState->curChild;
    return true;
}

// static
bool QueryPlannerAccess::processIndexScansSubnode(const CanonicalQuery& query,
                                                  ScanBuildingState* scanState,
                                                  const QueryPlannerParams& params,
                                                  std::vector<QuerySolutionNode*>* out) {
    MatchExpression* root = scanState->root;
    MatchExpression* child = root->getChild(scanState->curChild);
    const vector<IndexEntry>& indices = scanState->indices;
    bool inArrayOperator = scanState->inArrayOperator;

    if (MatchExpression::AND == root->matchType() &&
        MatchExpression::ELEM_MATCH_OBJECT == child->matchType()) {
        return processIndexScansElemMatch(query, scanState, params, out);
    } else if (!inArrayOperator) {
        // The logical sub-tree is responsible for fully evaluating itself.  Any
        // required filters or fetches are already hung on it.  As such, we remove the
        // filter branch from our tree.  buildIndexedDataAccess takes ownership of the
        // child.
        root->getChildVector()->erase(root->getChildVector()->begin() + scanState->curChild);
        // The curChild of today is the curChild+1 of yesterday.
    } else {
        ++scanState->curChild;
    }

    // If inArrayOperator: takes ownership of child, which is OK, since we detached
    // child from root.
    QuerySolutionNode* childSolution =
        buildIndexedDataAccess(query, child, inArrayOperator, indices, params);
    if (NULL == childSolution) {
        return false;
    }
    out->push_back(childSolution);
    return true;
}

// static
QuerySolutionNode* QueryPlannerAccess::buildIndexedAnd(const CanonicalQuery& query,
                                                       MatchExpression* root,
                                                       bool inArrayOperator,
                                                       const vector<IndexEntry>& indices,
                                                       const QueryPlannerParams& params) {
    unique_ptr<MatchExpression> autoRoot;
    if (!inArrayOperator) {
        autoRoot.reset(root);
    }

    // If we are not allowed to trim for ixisect, then clone the match expression before
    // passing it to processIndexScans(), which may do the trimming. If we end up with
    // an index intersection solution, then we use our copy of the match expression to be
    // sure that the FETCH stage will recheck the entire predicate.
    //
    // XXX: This block is a hack to accommodate the storage layer concurrency model.
    std::unique_ptr<MatchExpression> clonedRoot;
    if (params.options & QueryPlannerParams::CANNOT_TRIM_IXISECT) {
        clonedRoot = root->shallowClone();
    }

    vector<QuerySolutionNode*> ixscanNodes;
    if (!processIndexScans(query, root, inArrayOperator, indices, params, &ixscanNodes)) {
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
    } else {
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
        } else if (internalQueryPlannerEnableHashIntersection) {
            AndHashNode* ahn = new AndHashNode();
            ahn->children.swap(ixscanNodes);
            andResult = ahn;
            // The AndHashNode provides the sort order of its last child.  If any of the
            // possible subnodes of AndHashNode provides the sort order we care about, we put
            // that one last.
            for (size_t i = 0; i < ahn->children.size(); ++i) {
                ahn->children[i]->computeProperties();
                const BSONObjSet& sorts = ahn->children[i]->getSort();
                if (sorts.end() != sorts.find(query.getQueryRequest().getSort())) {
                    std::swap(ahn->children[i], ahn->children.back());
                    break;
                }
            }
        } else {
            // We can't use sort-based intersection, and hash-based intersection is disabled.
            // Clean up the index scans and bail out by returning NULL.
            LOG(5) << "Can't build index intersection solution: "
                   << "AND_SORTED is not possible and AND_HASH is disabled.";

            for (size_t i = 0; i < ixscanNodes.size(); i++) {
                delete ixscanNodes[i];
            }
            return NULL;
        }
    }

    // Don't bother doing any kind of fetch analysis lite if we're doing it anyway above us.
    if (inArrayOperator) {
        return andResult;
    }

    // XXX: This block is a hack to accommodate the storage layer concurrency model.
    if ((params.options & QueryPlannerParams::CANNOT_TRIM_IXISECT) &&
        (andResult->getType() == STAGE_AND_HASH || andResult->getType() == STAGE_AND_SORTED)) {
        // We got an index intersection solution, and we aren't allowed to answer predicates
        // using the index. We add a fetch with the entire filter.
        invariant(clonedRoot.get());
        FetchNode* fetch = new FetchNode();
        fetch->filter.reset(clonedRoot.release());
        // Takes ownership of 'andResult'.
        fetch->children.push_back(andResult);
        return fetch;
    }

    // If there are any nodes still attached to the AND, we can't answer them using the
    // index, so we put a fetch with filter.
    if (root->numChildren() > 0) {
        FetchNode* fetch = new FetchNode();
        verify(NULL != autoRoot.get());
        if (autoRoot->numChildren() == 1) {
            // An $and of one thing is that thing.
            MatchExpression* child = autoRoot->getChild(0);
            autoRoot->getChildVector()->clear();
            // Takes ownership.
            fetch->filter.reset(child);
            // 'autoRoot' will delete the empty $and.
        } else {  // root->numChildren() > 1
            // Takes ownership.
            fetch->filter.reset(autoRoot.release());
        }
        // takes ownership
        fetch->children.push_back(andResult);
        andResult = fetch;
    } else {
        // root has no children, let autoRoot get rid of it when it goes out of scope.
    }

    return andResult;
}

// static
QuerySolutionNode* QueryPlannerAccess::buildIndexedOr(const CanonicalQuery& query,
                                                      MatchExpression* root,
                                                      bool inArrayOperator,
                                                      const vector<IndexEntry>& indices,
                                                      const QueryPlannerParams& params) {
    unique_ptr<MatchExpression> autoRoot;
    if (!inArrayOperator) {
        autoRoot.reset(root);
    }

    vector<QuerySolutionNode*> ixscanNodes;
    if (!processIndexScans(query, root, inArrayOperator, indices, params, &ixscanNodes)) {
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

    // If all index scans are identical, then we collapse them into a single scan. This prevents
    // us from creating OR plans where the branches of the OR perform duplicate work.
    ixscanNodes = collapseEquivalentScans(ixscanNodes);

    QuerySolutionNode* orResult = NULL;

    // An OR of one node is just that node.
    if (1 == ixscanNodes.size()) {
        orResult = ixscanNodes[0];
    } else {
        bool shouldMergeSort = false;

        if (!query.getQueryRequest().getSort().isEmpty()) {
            const BSONObj& desiredSort = query.getQueryRequest().getSort();

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

            // TODO: If we're looking for the reverse of one of these sort orders we could
            // possibly reverse the ixscan nodes.
            shouldMergeSort = (sharedSortOrders.end() != sharedSortOrders.find(desiredSort));
        }

        if (shouldMergeSort) {
            MergeSortNode* msn = new MergeSortNode();
            msn->sort = query.getQueryRequest().getSort();
            msn->children.swap(ixscanNodes);
            orResult = msn;
        } else {
            OrNode* orn = new OrNode();
            orn->children.swap(ixscanNodes);
            orResult = orn;
        }
    }

    // Evaluate text nodes first to ensure that text scores are available.
    // Move text nodes to front of vector.
    std::stable_partition(orResult->children.begin(), orResult->children.end(), isTextNode);

    // OR must have an index for each child, so we should have detached all children from
    // 'root', and there's nothing useful to do with an empty or MatchExpression.  We let it die
    // via autoRoot.

    return orResult;
}

// static
QuerySolutionNode* QueryPlannerAccess::buildIndexedDataAccess(const CanonicalQuery& query,
                                                              MatchExpression* root,
                                                              bool inArrayOperator,
                                                              const vector<IndexEntry>& indices,
                                                              const QueryPlannerParams& params) {
    if (root->isLogical() && !Indexability::isBoundsGeneratingNot(root)) {
        if (MatchExpression::AND == root->matchType()) {
            // Takes ownership of root.
            return buildIndexedAnd(query, root, inArrayOperator, indices, params);
        } else if (MatchExpression::OR == root->matchType()) {
            // Takes ownership of root.
            return buildIndexedOr(query, root, inArrayOperator, indices, params);
        } else {
            // Can't do anything with negated logical nodes index-wise.
            if (!inArrayOperator) {
                delete root;
            }
            return NULL;
        }
    } else {
        unique_ptr<MatchExpression> autoRoot;
        if (!inArrayOperator) {
            autoRoot.reset(root);
        }

        // isArray or isLeaf is true.  Either way, it's over one field, and the bounds builder
        // deals with it.
        if (NULL == root->getTag()) {
            // No index to use here, not in the context of logical operator, so we're SOL.
            return NULL;
        } else if (Indexability::isBoundsGenerating(root)) {
            // Make an index scan over the tagged index #.
            IndexTag* tag = static_cast<IndexTag*>(root->getTag());

            IndexBoundsBuilder::BoundsTightness tightness = IndexBoundsBuilder::EXACT;
            QuerySolutionNode* soln =
                makeLeafNode(query, indices[tag->index], tag->pos, root, &tightness);
            verify(NULL != soln);
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

            if (tightness == IndexBoundsBuilder::EXACT) {
                return soln;
            } else if (tightness == IndexBoundsBuilder::INEXACT_COVERED &&
                       !indices[tag->index].multikey) {
                verify(NULL == soln->filter.get());
                soln->filter.reset(autoRoot.release());
                return soln;
            } else {
                FetchNode* fetch = new FetchNode();
                verify(NULL != autoRoot.get());
                fetch->filter.reset(autoRoot.release());
                fetch->children.push_back(soln);
                return fetch;
            }
        } else if (Indexability::arrayUsesIndexOnChildren(root)) {
            QuerySolutionNode* solution = NULL;

            invariant(MatchExpression::ELEM_MATCH_OBJECT);
            // The child is an AND.
            invariant(1 == root->numChildren());
            solution = buildIndexedDataAccess(query, root->getChild(0), true, indices, params);
            if (NULL == solution) {
                return NULL;
            }

            // There may be an array operator above us.
            if (inArrayOperator) {
                return solution;
            }

            FetchNode* fetch = new FetchNode();
            // Takes ownership of 'root'.
            verify(NULL != autoRoot.get());
            fetch->filter.reset(autoRoot.release());
            fetch->children.push_back(solution);
            return fetch;
        }
    }

    if (!inArrayOperator) {
        delete root;
    }

    return NULL;
}

QuerySolutionNode* QueryPlannerAccess::scanWholeIndex(const IndexEntry& index,
                                                      const CanonicalQuery& query,
                                                      const QueryPlannerParams& params,
                                                      int direction) {
    QuerySolutionNode* solnRoot = NULL;

    // Build an ixscan over the id index, use it, and return it.
    unique_ptr<IndexScanNode> isn = make_unique<IndexScanNode>();
    isn->indexKeyPattern = index.keyPattern;
    isn->indexIsMultiKey = index.multikey;
    isn->maxScan = query.getQueryRequest().getMaxScan();
    isn->addKeyMetadata = query.getQueryRequest().returnKey();
    isn->indexCollator = index.collator;
    isn->queryCollator = query.getCollator();

    IndexBoundsBuilder::allValuesBounds(index.keyPattern, &isn->bounds);

    if (-1 == direction) {
        QueryPlannerCommon::reverseScans(isn.get());
        isn->direction = -1;
    }

    unique_ptr<MatchExpression> filter = query.root()->shallowClone();

    // If it's find({}) remove the no-op root.
    if (MatchExpression::AND == filter->matchType() && (0 == filter->numChildren())) {
        solnRoot = isn.release();
    } else {
        // TODO: We may not need to do the fetch if the predicates in root are covered.  But
        // for now it's safe (though *maybe* slower).
        unique_ptr<FetchNode> fetch = make_unique<FetchNode>();
        fetch->filter = std::move(filter);
        fetch->children.push_back(isn.release());
        solnRoot = fetch.release();
    }

    return solnRoot;
}

// static
void QueryPlannerAccess::addFilterToSolutionNode(QuerySolutionNode* node,
                                                 MatchExpression* match,
                                                 MatchExpression::MatchType type) {
    if (NULL == node->filter) {
        node->filter.reset(match);
    } else if (type == node->filter->matchType()) {
        // The 'node' already has either an AND or OR filter that matches 'type'. Add 'match' as
        // another branch of the filter.
        ListOfMatchExpression* listFilter = static_cast<ListOfMatchExpression*>(node->filter.get());
        listFilter->add(match);
    } else {
        // The 'node' already has a filter that does not match 'type'. If 'type' is AND, then
        // combine 'match' with the existing filter by adding an AND. If 'type' is OR, combine
        // by adding an OR node.
        unique_ptr<ListOfMatchExpression> listFilter;
        if (MatchExpression::AND == type) {
            listFilter = make_unique<AndMatchExpression>();
        } else {
            verify(MatchExpression::OR == type);
            listFilter = make_unique<OrMatchExpression>();
        }
        unique_ptr<MatchExpression> oldFilter = node->filter->shallowClone();
        listFilter->add(oldFilter.release());
        listFilter->add(match);
        node->filter = std::move(listFilter);
    }
}

// static
void QueryPlannerAccess::handleFilter(ScanBuildingState* scanState) {
    if (MatchExpression::OR == scanState->root->matchType()) {
        handleFilterOr(scanState);
    } else if (MatchExpression::AND == scanState->root->matchType()) {
        handleFilterAnd(scanState);
    } else {
        // We must be building leaves for either and AND or an OR.
        invariant(0);
    }
}

// static
void QueryPlannerAccess::handleFilterOr(ScanBuildingState* scanState) {
    MatchExpression* root = scanState->root;
    MatchExpression* child = root->getChild(scanState->curChild);

    if (scanState->inArrayOperator) {
        // We're inside an array operator. The entire array operator expression
        // should always be affixed as a filter. We keep 'curChild' in the $and
        // for affixing later.
        ++scanState->curChild;
    } else {
        if (scanState->tightness < scanState->loosestBounds) {
            scanState->loosestBounds = scanState->tightness;
        }

        // Detach 'child' and add it to 'curOr'.
        root->getChildVector()->erase(root->getChildVector()->begin() + scanState->curChild);
        scanState->curOr->getChildVector()->push_back(child);
    }
}

// static
void QueryPlannerAccess::handleFilterAnd(ScanBuildingState* scanState) {
    MatchExpression* root = scanState->root;
    MatchExpression* child = root->getChild(scanState->curChild);
    const IndexEntry& index = scanState->indices[scanState->currentIndexNumber];

    if (scanState->inArrayOperator) {
        // We're inside an array operator. The entire array operator expression
        // should always be affixed as a filter. We keep 'curChild' in the $and
        // for affixing later.
        ++scanState->curChild;
    } else if (scanState->tightness == IndexBoundsBuilder::EXACT) {
        root->getChildVector()->erase(root->getChildVector()->begin() + scanState->curChild);
        delete child;
    } else if (scanState->tightness == IndexBoundsBuilder::INEXACT_COVERED &&
               (INDEX_TEXT == index.type || !index.multikey)) {
        // The bounds are not exact, but the information needed to
        // evaluate the predicate is in the index key. Remove the
        // MatchExpression from its parent and attach it to the filter
        // of the index scan we're building.
        //
        // We can only use this optimization if the index is NOT multikey.
        // Suppose that we had the multikey index {x: 1} and a document
        // {x: ["a", "b"]}. Now if we query for {x: /b/} the filter might
        // ever only be applied to the index key "a". We'd incorrectly
        // conclude that the document does not match the query :( so we
        // gotta stick to non-multikey indices.
        root->getChildVector()->erase(root->getChildVector()->begin() + scanState->curChild);

        addFilterToSolutionNode(scanState->currentScan.get(), child, root->matchType());
    } else {
        // We keep curChild in the AND for affixing later.
        ++scanState->curChild;
    }
}

QuerySolutionNode* QueryPlannerAccess::makeIndexScan(const IndexEntry& index,
                                                     const CanonicalQuery& query,
                                                     const QueryPlannerParams& params,
                                                     const BSONObj& startKey,
                                                     const BSONObj& endKey) {
    QuerySolutionNode* solnRoot = NULL;

    // Build an ixscan over the id index, use it, and return it.
    IndexScanNode* isn = new IndexScanNode();
    isn->indexKeyPattern = index.keyPattern;
    isn->indexIsMultiKey = index.multikey;
    isn->direction = 1;
    isn->maxScan = query.getQueryRequest().getMaxScan();
    isn->addKeyMetadata = query.getQueryRequest().returnKey();
    isn->bounds.isSimpleRange = true;
    isn->bounds.startKey = startKey;
    isn->bounds.endKey = endKey;
    isn->bounds.endKeyInclusive = false;
    isn->indexCollator = index.collator;
    isn->queryCollator = query.getCollator();

    unique_ptr<MatchExpression> filter = query.root()->shallowClone();

    // If it's find({}) remove the no-op root.
    if (MatchExpression::AND == filter->matchType() && (0 == filter->numChildren())) {
        solnRoot = isn;
    } else {
        // TODO: We may not need to do the fetch if the predicates in root are covered.  But
        // for now it's safe (though *maybe* slower).
        unique_ptr<FetchNode> fetch = make_unique<FetchNode>();
        fetch->filter = std::move(filter);
        fetch->children.push_back(isn);
        solnRoot = fetch.release();
    }

    return solnRoot;
}

}  // namespace mongo
