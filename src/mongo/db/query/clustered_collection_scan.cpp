// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/clustered_collection_scan.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/record_id_range.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

// Set 'curr' to 'newMin' if 'newMin' < 'curr'
void setLowestRecord(boost::optional<RecordIdBound>& curr, const RecordIdBound& newMin) {
    if (!curr || newMin.recordId() < curr->recordId()) {
        curr = newMin;
    }
}

// Set 'curr' to 'newMax' if 'newMax' > 'curr'
void setHighestRecord(boost::optional<RecordIdBound>& curr, const RecordIdBound& newMax) {
    if (!curr || newMax.recordId() > curr->recordId()) {
        curr = newMax;
    }
}

// Set 'curr' to 'newMin' if 'newMin' < 'curr'
void setLowestRecord(boost::optional<RecordIdBound>& curr, const BSONObj& newMin) {
    setLowestRecord(curr, RecordIdBound(record_id_helpers::keyForObj(newMin), newMin));
}

// Set 'curr' to 'newMax' if 'newMax' > 'curr'
void setHighestRecord(boost::optional<RecordIdBound>& curr, const BSONObj& newMax) {
    setHighestRecord(curr, RecordIdBound(record_id_helpers::keyForObj(newMax), newMax));
}


void simplifyFilterInnerLegacy(std::unique_ptr<MatchExpression>& expr,
                               const std::set<const MatchExpression*>& toRemove) {
    if (toRemove.contains(expr.get())) {
        expr.reset();
        return;
    }
    if (auto conjunct = dynamic_cast<AndMatchExpression*>(expr.get())) {
        auto& childVector = *conjunct->getChildVector();
        for (auto& child : childVector) {
            simplifyFilterInnerLegacy(child, toRemove);
        }
        // The recursive calls may have nulled some children; remove them from the
        // conjunction.
        childVector.erase(std::remove(childVector.begin(), childVector.end(), nullptr),
                          childVector.end());
        if (conjunct->isTriviallyTrue()) {
            // Removing redundant children may have made this conjunct trivially true in turn;
            // reset it.
            expr.reset();
        }
    }
}

}  // namespace

[[nodiscard]] bool ClusteredCollectionScanPlanner::handleRIDRangeScanLegacy(
    const MatchExpression* conjunct,
    const CollatorInterface* queryCollator,
    const CollatorInterface* ccCollator,
    std::string_view clusterKeyFieldName,
    RecordIdRange& recordRange,
    const std::function<void(const MatchExpression*)>& redundant) {
    if (conjunct == nullptr) {
        return false;
    }

    const AndMatchExpression* andMatchPtr = dynamic_cast<const AndMatchExpression*>(conjunct);
    if (andMatchPtr != nullptr) {
        bool atLeastOneConjunctCompatibleCollation = false;
        for (size_t index = 0; index < andMatchPtr->numChildren(); index++) {
            // Recursive call on each branch of 'andMatchPtr'.
            if (handleRIDRangeScanLegacy(andMatchPtr->getChild(index),
                                         queryCollator,
                                         ccCollator,
                                         clusterKeyFieldName,
                                         recordRange,
                                         redundant)) {
                atLeastOneConjunctCompatibleCollation = true;
            }
        }

        // If one of the conjuncts excludes values of the cluster key which are affected by
        // collation, then the entire $and will also exclude those values.
        return atLeastOneConjunctCompatibleCollation;
    }

    // If 'conjunct' does not apply to the cluster key, return early here, as updating bounds based
    // on this conjunct is incorrect and can result in garbage bounds.
    if (conjunct->path() != clusterKeyFieldName) {
        return false;
    }

    // TODO SERVER-62707: Allow $in with regex to use a clustered index.
    const InMatchExpression* inMatch = dynamic_cast<const InMatchExpression*>(conjunct);
    if (inMatch && !inMatch->hasRegex()) {
        // Iterate through the $in equalities to find the min/max values. The min/max bounds for the
        // collscan need to be loose enough to cover all of these values.
        boost::optional<RecordIdBound> minBound;
        boost::optional<RecordIdBound> maxBound;

        bool allEltsCollationCompatible = true;
        for (const BSONElement& element : inMatch->getEqualities()) {
            if (compatibleCollator(ccCollator, queryCollator, element)) {
                const BSONObj collated = IndexBoundsBuilder::objFromElement(element, queryCollator);
                setLowestRecord(minBound, collated);
                setHighestRecord(maxBound, collated);
            } else {
                // Set coarse min/max bounds based on type when we can't set tight bounds.
                allEltsCollationCompatible = false;

                BSONObjBuilder bMin;
                bMin.appendMinForType("", stdx::to_underlying(element.type()));
                setLowestRecord(minBound, bMin.obj());

                BSONObjBuilder bMax;
                bMax.appendMaxForType("", stdx::to_underlying(element.type()));
                setHighestRecord(maxBound, bMax.obj());
            }
        }

        // {min,max}RecordId will bound the range of ids scanned to the highest and lowest present
        // in the InMatchExpression, but the filter is still required to filter to _exactly_ the
        // requested matches.

        // Finally, tighten the collscan bounds with the min/max bounds for the $in.
        recordRange.intersectRange(minBound, maxBound);
        return allEltsCollationCompatible;
    }

    auto match = dynamic_cast<const ComparisonMatchExpressionBase*>(conjunct);
    if (match == nullptr) {
        return false;  // Not a comparison match expression.
    }

    const BSONElement& element = match->getData();

    if (!ComparisonMatchExpressionBase::isInternalExprComparison(match->matchType())) {
        // Internal comparisons e.g., $_internalExprGt do _not_ carry type bracketing
        // semantics (consistent with `$expr{$gt:[a,b]}`).
        // For other comparisons which _do_ perform type bracketing, the RecordId bounds
        // may be tightened here.
        BSONObjBuilder minb;
        minb.appendMinForType("", stdx::to_underlying(element.type()));
        recordRange.maybeNarrowMin(minb.obj(), true /* inclusive */);

        BSONObjBuilder maxb;
        maxb.appendMaxForType("", stdx::to_underlying(element.type()));
        recordRange.maybeNarrowMax(maxb.obj(), true /* inclusive */);
    }

    bool compatible = compatibleCollator(ccCollator, queryCollator, element);
    if (!compatible) {
        // Collator affects probe and it's not compatible with collection's collator.
        return false;
    }

    // Even if the collations don't match at this point, it's fine,
    // because the bounds exclude values that use it.
    const BSONObj collated = IndexBoundsBuilder::objFromElement(element, queryCollator);
    using MType = MatchExpression::MatchType;
    switch (match->matchType()) {
        case MType::EQ:
        case MType::INTERNAL_EXPR_EQ:
            recordRange.maybeNarrowMin(collated, true /* inclusive */);
            recordRange.maybeNarrowMax(collated, true /* inclusive */);
            break;
        case MType::LT:
        case MType::INTERNAL_EXPR_LT:
            recordRange.maybeNarrowMax(collated, false /* EXclusive */);
            break;
        case MType::LTE:
        case MType::INTERNAL_EXPR_LTE:
            recordRange.maybeNarrowMax(collated, true /* inclusive */);
            break;
        case MType::GT:
        case MType::INTERNAL_EXPR_GT:
            recordRange.maybeNarrowMin(collated, false /* EXclusive */);
            break;
        case MType::GTE:
        case MType::INTERNAL_EXPR_GTE:
            recordRange.maybeNarrowMin(collated, true /* inclusive */);
            break;
        default:
            // This expr is _not_ redundant, it could not be re-expressed via {min,max} record
            return true;
    }
    // Report that this expression does not need to be retained in the filter
    // _if_ recordRange is enforced - {min,max}Record will already apply equivalent
    // limits.
    redundant(match);
    return true;
}

void ClusteredCollectionScanPlanner::simplifyFilterLegacy(
    std::unique_ptr<MatchExpression>& expr, const std::set<const MatchExpression*>& toRemove) {
    simplifyFilterInnerLegacy(expr, toRemove);
    if (!expr) {
        // Simplifying the filter might remove everything; filter can't be left
        // null, so populate with a trivially true expression.
        expr = std::make_unique<AndMatchExpression>();
    }
}


bool ClusteredCollectionScanPlanner::affectedByCollator(const BSONElement& element) {
    switch (element.type()) {
        case BSONType::string:
            return true;
        case BSONType::array:
        case BSONType::object:
            for (const auto& sub : element.Obj()) {
                if (affectedByCollator(sub))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

bool ClusteredCollectionScanPlanner::compatibleCollator(const CollatorInterface* collCollator,
                                                        const CollatorInterface* queryCollator,
                                                        const BSONElement& element) {
    bool compatible = CollatorInterface::collatorsMatch(queryCollator, collCollator);
    return compatible || !affectedByCollator(element);
}

void ClusteredCollectionScanPlanner::buildLegacyBounds(CollectionScanNode* csn,
                                                       const CanonicalQuery& query,
                                                       const QueryPlannerParams& params,
                                                       const CollatorInterface* queryCollator,
                                                       const CollatorInterface* collCollator,
                                                       bool canSimplifyFilter) {
    // Seed recordRange from the existing rangeList (e.g. oplog timestamp bounds).
    RecordIdRange recordRange = csn->rangeList.outerBounds();

    std::set<const MatchExpression*> redundantExprs;
    bool compatibleCollation = handleRIDRangeScanLegacy(
        csn->filter.get(),
        queryCollator,
        collCollator,
        clustered_util::getClusterKeyFieldName(params.clusteredInfo->getIndexSpec()),
        recordRange,
        [&](const MatchExpression* expr) { redundantExprs.insert(expr); });
    csn->hasCompatibleCollation |= compatibleCollation;

    QueryPlannerAccess::handleRIDRangeMinMax(
        query, csn->direction, queryCollator, collCollator, recordRange);

    csn->rangeList = RecordIdRangeList(std::move(recordRange));

    if (canSimplifyFilter) {
        simplifyFilterLegacy(csn->filter, redundantExprs);
    }
}

}  // namespace mongo
