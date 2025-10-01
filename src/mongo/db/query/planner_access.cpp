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


#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/assert_util.h"
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <s2cellid.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/fts/fts_query.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_text_base.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/record_id_range.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace {

using namespace mongo;

namespace wcp = ::mongo::wildcard_planning;
namespace dps = ::mongo::dotted_path_support;

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
        const QuerySolutionNode* child = node->children[0].get();
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

/**
 * If all nodes can provide the requested sort, returns a vector expressing which nodes must have
 * their index scans reversed to provide the sort. Otherwise, returns an empty vector.
 * 'nodes' must not be empty.
 */
std::vector<bool> canProvideSortWithMergeSort(
    const std::vector<std::unique_ptr<QuerySolutionNode>>& nodes, const BSONObj& requestedSort) {
    invariant(!nodes.empty());
    std::vector<bool> shouldReverseScan;
    const auto reverseSort = QueryPlannerCommon::reverseSortObj(requestedSort);
    for (auto&& node : nodes) {
        node->computeProperties();
        if (node->providedSorts().contains(requestedSort)) {
            shouldReverseScan.push_back(false);
        } else if (node->providedSorts().contains(reverseSort)) {
            shouldReverseScan.push_back(true);
        } else {
            return {};
        }
    }
    return shouldReverseScan;
}

/**
 * Resolves the final direction hint to be used in collection scans, as there multiple mechanisms of
 * overriding the planner's direction decision. It does so by enforcing the following precedence:
 * timeseries traversal preference > query settings '$natural' hint > cursor '$natural' hint.
 */
boost::optional<int> determineCollScanHintedDirection(const CanonicalQuery& query,
                                                      const QueryPlannerParams& params) {
    // If present, let the traversal preference decide what order to scan to avoid a blocking
    // sort.
    if (params.traversalPreference.has_value()) {
        return boost::none;
    }

    // Otherwise use the direction specified by query settings if available.
    if (auto querySettingsDirection = params.mainCollectionInfo.collscanDirection) {
        return static_cast<int>(*querySettingsDirection);
    }

    // Next, try to determine the scan direction using the '$natural' cursor hint.
    const BSONObj& cursorHint = query.getFindCommandRequest().getHint();
    if (cursorHint.isEmpty() || params.querySettingsApplied) {
        return boost::none;
    }
    auto naturalHint = cursorHint[query_request_helper::kNaturalSortField];
    if (!naturalHint) {
        return boost::none;
    }
    return naturalHint.safeNumberInt() >= 0 ? 1 : -1;
}
}  // namespace

namespace mongo {

using std::unique_ptr;
using std::vector;

namespace {
/**
 * Extracts the lower and upper bounds on the "ts" field from 'me'. This only examines comparisons
 * of "ts" against a Timestamp at the top level or inside a top-level $and.
 */
std::pair<boost::optional<Timestamp>, boost::optional<Timestamp>> extractTsRange(
    const MatchExpression* me, bool topLevel = true) {
    boost::optional<Timestamp> min;
    boost::optional<Timestamp> max;

    if (me->matchType() == MatchExpression::AND && topLevel) {
        for (size_t i = 0; i < me->numChildren(); ++i) {
            boost::optional<Timestamp> childMin;
            boost::optional<Timestamp> childMax;
            std::tie(childMin, childMax) = extractTsRange(me->getChild(i), false);
            if (childMin && (!min || childMin.value() > min.value())) {
                min = childMin;
            }
            if (childMax && (!max || childMax.value() < max.value())) {
                max = childMax;
            }
        }
        return {min, max};
    }

    if (!ComparisonMatchExpression::isComparisonMatchExpression(me) ||
        me->path() != repl::OpTime::kTimestampFieldName) {
        return {min, max};
    }

    auto rawElem = static_cast<const ComparisonMatchExpression*>(me)->getData();
    if (rawElem.type() != BSONType::bsonTimestamp) {
        return {min, max};
    }

    switch (me->matchType()) {
        case MatchExpression::EQ:
            min = rawElem.timestamp();
            max = rawElem.timestamp();
            return {min, max};
        case MatchExpression::GT:
        case MatchExpression::GTE:
            min = rawElem.timestamp();
            return {min, max};
        case MatchExpression::LT:
        case MatchExpression::LTE:
            max = rawElem.timestamp();
            return {min, max};
        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Returns true if 'me' is a GTE or GE predicate over the "ts" field.
 */
bool isOplogTsLowerBoundPred(const mongo::MatchExpression* me) {
    if (mongo::MatchExpression::GT != me->matchType() &&
        mongo::MatchExpression::GTE != me->matchType()) {
        return false;
    }

    return me->path() == repl::OpTime::kTimestampFieldName;
}

/**
 * Sets the lowPriority parameter on the given index scan node.
 */
void deprioritizeUnboundedIndexScan(IndexScanNode* solnRoot,
                                    const FindCommandRequest& findCommand) {
    auto sort = findCommand.getSort();
    if (findCommand.getLimit() &&
        (sort.isEmpty() || sort[query_request_helper::kNaturalSortField])) {
        // There is a limit with either no sort or the natural sort.
        return;
    }

    auto indexScan = checked_cast<IndexScanNode*>(solnRoot);
    if (!indexScan->bounds.isUnbounded()) {
        return;
    }

    indexScan->lowPriority = true;
}

// True if the element type is affected by a collator (i.e. it is or contains a String).
bool affectedByCollator(const BSONElement& element) {
    switch (element.type()) {
        case BSONType::String:
            return true;
        case BSONType::Array:
        case BSONType::Object:
            for (const auto& sub : element.Obj()) {
                if (affectedByCollator(sub))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

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

// Returns whether element is not affected by collators or query and collection collators are
// compatible.
bool compatibleCollator(const CollatorInterface* collCollator,
                        const CollatorInterface* queryCollator,
                        const BSONElement& element) {
    bool compatible = CollatorInterface::collatorsMatch(queryCollator, collCollator);
    return compatible || !affectedByCollator(element);
}
}  // namespace

void QueryPlannerAccess::handleRIDRangeMinMax(const CanonicalQuery& query,
                                              const int direction,
                                              const CollatorInterface* queryCollator,
                                              const CollatorInterface* ccCollator,
                                              RecordIdRange& recordRange) {
    BSONObj minObj = query.getFindCommandRequest().getMin();
    BSONObj maxObj = query.getFindCommandRequest().getMax();
    if (minObj.isEmpty() && maxObj.isEmpty()) {
        return;
    }

    // If either min() or max() were provided, we can assume they are on the cluster key due to the
    // following.
    // (1) min() / max() are only legal when they match the pattern in hint()
    // (2) Only hint() on a cluster key will generate collection scan rather than an index scan
    uassert(
        6137402,
        "min() / max() are only supported for forward collection scans on clustered collections",
        direction == 1);

    boost::optional<RecordId> newMinRecord, newMaxRecord;
    if (!maxObj.isEmpty() && compatibleCollator(ccCollator, queryCollator, maxObj.firstElement())) {
        // max() is exclusive.
        // Assumes clustered collection scans are only supported with the forward direction.
        recordRange.maybeNarrowMax(
            IndexBoundsBuilder::objFromElement(maxObj.firstElement(), queryCollator),
            false /* NOT inclusive*/);
    }

    if (!minObj.isEmpty() && compatibleCollator(ccCollator, queryCollator, minObj.firstElement())) {
        // The min() is inclusive as are bounded collection scans by default.
        recordRange.maybeNarrowMin(
            IndexBoundsBuilder::objFromElement(minObj.firstElement(), queryCollator),
            true /* inclusive*/);
    }
}

[[nodiscard]] bool QueryPlannerAccess::handleRIDRangeScan(
    const MatchExpression* conjunct,
    const CollatorInterface* queryCollator,
    const CollatorInterface* ccCollator,
    StringData clusterKeyFieldName,
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
            if (handleRIDRangeScan(andMatchPtr->getChild(index),
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
                bMin.appendMinForType("", element.type());
                setLowestRecord(minBound, bMin.obj());

                BSONObjBuilder bMax;
                bMax.appendMaxForType("", element.type());
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
        minb.appendMinForType("", element.type());
        recordRange.maybeNarrowMin(minb.obj(), true /* inclusive */);

        BSONObjBuilder maxb;
        maxb.appendMaxForType("", element.type());
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

void simplifyFilterInner(std::unique_ptr<MatchExpression>& expr,
                         const std::set<const MatchExpression*>& toRemove) {
    if (toRemove.contains(expr.get())) {
        expr.reset();
        return;
    }
    if (auto conjunct = dynamic_cast<AndMatchExpression*>(expr.get())) {
        auto& childVector = *conjunct->getChildVector();
        for (auto& child : childVector) {
            simplifyFilterInner(child, toRemove);
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

void QueryPlannerAccess::simplifyFilter(std::unique_ptr<MatchExpression>& expr,
                                        const std::set<const MatchExpression*>& toRemove) {
    simplifyFilterInner(expr, toRemove);
    if (!expr) {
        // Simplifying the filter might remove everything; filter can't be left
        // null, so populate with a trivially true expression.
        expr = std::make_unique<AndMatchExpression>();
    }
}

boost::optional<ResumeScanPoint> getResumePoint(const BSONObj& resumeAfterObj,
                                                const BSONObj& startAtObj) {
    tassert(9049501,
            "Cannot set both $_startAt and $_resumeAfter",
            resumeAfterObj.isEmpty() || startAtObj.isEmpty());
    if (!resumeAfterObj.isEmpty()) {
        BSONElement recordIdElem = resumeAfterObj["$recordId"];
        return ResumeScanPoint{RecordId::deserializeToken(recordIdElem),
                               false /* tolerateKeyNotFound */};
    } else if (!startAtObj.isEmpty()) {
        BSONElement recordIdElem = startAtObj["$recordId"];
        return ResumeScanPoint{RecordId::deserializeToken(recordIdElem),
                               true /* tolerateKeyNotFound */};
    }
    return boost::none;
}

std::unique_ptr<QuerySolutionNode> QueryPlannerAccess::makeCollectionScan(
    const CanonicalQuery& query,
    bool tailable,
    const QueryPlannerParams& params,
    int direction,
    const MatchExpression* root) {
    // The following are expensive to look up, so only do it once for each.
    const mongo::NamespaceString nss = query.nss();
    const bool isOplog = nss.isOplog();
    const bool isChangeCollection = nss.isChangeCollection();

    // Make the (only) node, a collection scan.
    auto csn = std::make_unique<CollectionScanNode>();
    csn->nss = nss;
    csn->filter = root->clone();
    csn->tailable = tailable;
    csn->shouldTrackLatestOplogTimestamp =
        params.mainCollectionInfo.options & QueryPlannerParams::TRACK_LATEST_OPLOG_TS;
    csn->shouldWaitForOplogVisibility =
        params.mainCollectionInfo.options & QueryPlannerParams::OPLOG_SCAN_WAIT_FOR_VISIBLE;
    csn->direction = determineCollScanHintedDirection(query, params).value_or(direction);
    csn->isOplog = isOplog;
    csn->isClustered = params.clusteredInfo ? true : false;

    if (params.clusteredInfo) {
        csn->clusteredIndex = params.clusteredInfo->getIndexSpec();
    }

    // If the client requested a resume token and we are scanning the oplog, prepare
    // the collection scan to return timestamp-based tokens. Otherwise, we should
    // return generic RecordId-based tokens.
    if (query.getFindCommandRequest().getRequestResumeToken()) {
        csn->shouldTrackLatestOplogTimestamp = (isOplog || isChangeCollection);
        csn->requestResumeToken = !csn->shouldTrackLatestOplogTimestamp;
    }

    // Extract and set the resumeScanPoint from the 'resumeAfter' or 'startAt' token, if present.
    csn->resumeScanPoint = getResumePoint(query.getFindCommandRequest().getResumeAfter(),
                                          query.getFindCommandRequest().getStartAt());

    const bool assertMinTsHasNotFallenOffOplog = params.mainCollectionInfo.options &
        QueryPlannerParams::ASSERT_MIN_TS_HAS_NOT_FALLEN_OFF_OPLOG;
    if ((isOplog || isChangeCollection) && csn->direction == 1) {
        // Takes Timestamp 'ts' as input, transforms it to the RecordIdBound and assigns it to the
        // output parameter 'recordId'. The RecordId format for the change collection is a string,
        // where as the RecordId format for the oplog is a long integer. The timestamp should be
        // converted to the required format before assigning it to the 'recordId'.
        auto assignRecordIdFromTimestamp = [&](auto& ts, auto* recordId) {
            auto keyFormat = isChangeCollection ? KeyFormat::String : KeyFormat::Long;
            auto status = record_id_helpers::keyForOptime(ts, keyFormat);
            if (status.isOK()) {
                *recordId = RecordIdBound(status.getValue());
            }
        };

        // Optimizes the start and end location parameters for a collection scan for an oplog
        // collection. Not compatible with resumeScanPoint, so we do not optimize in that case.
        if (!csn->resumeScanPoint) {
            auto [minTs, maxTs] = extractTsRange(root);
            if (minTs) {
                assignRecordIdFromTimestamp(*minTs, &csn->minRecord);
                if (assertMinTsHasNotFallenOffOplog) {
                    csn->assertTsHasNotFallenOff = *minTs;
                }
            }
            if (maxTs) {
                assignRecordIdFromTimestamp(*maxTs, &csn->maxRecord);
            }
        }

        // If the query is just a lower bound on "ts" on a forward scan, every document in the
        // collection after the first matching one must also match. To avoid wasting time
        // running the match expression on every document to be returned, we tell the
        // CollectionScan stage to stop applying the filter once it finds the first match.
        if (isOplogTsLowerBoundPred(root)) {
            csn->stopApplyingFilterAfterFirstMatch = true;
        }
    }

    // The user may have requested 'assertMinTsHasNotFallenOffOplog' for a query that does not
    // specify a minimum timestamp. This is not a valid request, so we throw InvalidOptions.
    if (assertMinTsHasNotFallenOffOplog) {
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "assertTsHasNotFallenOff cannot be applied to a query "
                                 "which does not imply a minimum 'ts' value ",
                csn->assertTsHasNotFallenOff);
    }

    auto queryCollator = query.getCollator();
    auto collCollator = params.clusteredCollectionCollator;
    csn->hasCompatibleCollation = CollatorInterface::collatorsMatch(queryCollator, collCollator);

    // If collation _exactly_ matches, it is safe to drop expressions from the filter which can be
    // entirely encoded as a {min,max}Record.
    // handleRIDRangeScan later updates hasCompatibleCollation if the filter restricts the id range
    // to values unaffected by collation e.g., numbers. This is insufficient to allow the filter to
    // be safely modified; a query which has compatible collation _given the current filter args_
    // could be cached and reused for a query with different args which _are_ affected by collation.
    const bool canSimplifyFilter = csn->hasCompatibleCollation;
    std::set<const MatchExpression*> redundantExprs;

    if (csn->isClustered && !csn->resumeScanPoint) {
        // This is a clustered collection. Attempt to perform an efficient, bounded collection scan
        // via minRecord and maxRecord if applicable. During this process, we will check if the
        // query is guaranteed to exclude values of the cluster key which are affected by collation.
        // If so, then even if the query and collection collations differ, the collation difference
        // won't affect the query results. In that case, we can say hasCompatibleCollation is true.

        RecordIdRange recordRange;
        // min/max records may have been set if oplog or change collection.
        recordRange.intersectRange(csn->minRecord, csn->maxRecord);
        bool compatibleCollation = handleRIDRangeScan(
            csn->filter.get(),
            queryCollator,
            collCollator,
            clustered_util::getClusterKeyFieldName(params.clusteredInfo->getIndexSpec()),
            recordRange,
            [&](const auto& expr) { redundantExprs.insert(expr); });
        csn->hasCompatibleCollation |= compatibleCollation;

        handleRIDRangeMinMax(query, csn->direction, queryCollator, collCollator, recordRange);

        csn->minRecord = recordRange.getMin();
        csn->maxRecord = recordRange.getMax();

        csn->boundInclusion = CollectionScanParams::makeInclusion(recordRange.isMinInclusive(),
                                                                  recordRange.isMaxInclusive());
    }

    if (canSimplifyFilter) {
        simplifyFilter(csn->filter, redundantExprs);
    }

    return csn;
}  // makeCollectionScan

std::unique_ptr<QuerySolutionNode> QueryPlannerAccess::makeLeafNode(
    const CanonicalQuery& query,
    const IndexEntry& index,
    size_t pos,
    const MatchExpression* expr,
    IndexBoundsBuilder::BoundsTightness* tightnessOut,
    interval_evaluation_tree::Builder* ietBuilder) {
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
        auto nearExpr = static_cast<const GeoNearMatchExpression*>(expr);

        BSONElement elt = index.keyPattern.firstElement();
        bool indexIs2D = (String == elt.type() && "2d" == elt.String());

        if (indexIs2D) {
            auto ret = std::make_unique<GeoNear2DNode>(index);
            ret->nq = &nearExpr->getData();
            ret->baseBounds.fields.resize(index.keyPattern.nFields());
            ret->addPointMeta = query.metadataDeps()[DocumentMetadataFields::kGeoNearPoint];
            ret->addDistMeta = query.metadataDeps()[DocumentMetadataFields::kGeoNearDist];

            return ret;
        } else {
            auto ret = std::make_unique<GeoNear2DSphereNode>(index);
            ret->nq = &nearExpr->getData();
            ret->baseBounds.fields.resize(index.keyPattern.nFields());
            ret->addPointMeta = query.metadataDeps()[DocumentMetadataFields::kGeoNearPoint];
            ret->addDistMeta = query.metadataDeps()[DocumentMetadataFields::kGeoNearDist];

            return ret;
        }
    } else if (MatchExpression::TEXT == expr->matchType()) {
        // We must not keep the expression node around.
        *tightnessOut = IndexBoundsBuilder::EXACT;
        auto textExpr = static_cast<const TextMatchExpressionBase*>(expr);
        bool wantTextScore = DepsTracker::needsTextScoreMetadata(query.metadataDeps());
        auto ret =
            std::make_unique<TextMatchNode>(index, textExpr->getFTSQuery().clone(), wantTextScore);
        // Count the number of prefix fields before the "text" field.
        for (auto&& keyPatternElt : ret->index.keyPattern) {
            // We know that the only key pattern with a type of String is the _fts field
            // which is immediately after all prefix fields.
            if (BSONType::String == keyPatternElt.type()) {
                break;
            }
            ++(ret->numPrefixFields);
        }

        return ret;
    } else {
        // Note that indexKeyPattern.firstElement().fieldName() may not equal expr->path()
        // because expr might be inside an array operator that provides a path prefix.
        auto isn = std::make_unique<IndexScanNode>(index);
        isn->bounds.fields.resize(index.keyPattern.nFields());
        isn->addKeyMetadata = query.metadataDeps()[DocumentMetadataFields::kIndexKey];
        isn->queryCollator = query.getCollator();

        // Get the ixtag->pos-th element of the index key pattern.
        // TODO: cache this instead/with ixtag->pos?
        BSONObjIterator it(index.keyPattern);
        BSONElement keyElt = it.next();
        for (size_t i = 0; i < pos; ++i) {
            MONGO_verify(it.more());
            keyElt = it.next();
        }
        MONGO_verify(!keyElt.eoo());

        IndexBoundsBuilder::translate(
            expr, keyElt, index, &isn->bounds.fields[pos], tightnessOut, ietBuilder);

        return isn;
    }
}

bool QueryPlannerAccess::shouldMergeWithLeaf(const MatchExpression* expr,
                                             const ScanBuildingState& scanState) {
    const QuerySolutionNode* node = scanState.currentScan.get();
    if (nullptr == node || nullptr == expr) {
        return false;
    }

    if (nullptr == scanState.ixtag) {
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

    if (STAGE_TEXT_MATCH == type) {
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
    invariant(nullptr != node);

    const MatchExpression::MatchType mergeType = scanState->root->matchType();
    const size_t pos = scanState->ixtag->pos;
    const IndexEntry& index = scanState->indices[scanState->currentIndexNumber];

    const StageType type = node->getType();

    if (STAGE_TEXT_MATCH == type) {
        auto textNode = static_cast<TextMatchNode*>(node);

        if (pos < textNode->numPrefixFields) {
            // This predicate is assigned to one of the prefix fields of the text index. Such
            // predicates must always be equalities and must always be attached to the TEXT node. In
            // order to ensure this happens, we assign INEXACT_COVERED tightness.
            scanState->tightness = IndexBoundsBuilder::INEXACT_COVERED;
        } else {
            // The predicate is assigned to one of the trailing fields of the text index. We
            // currently don't generate bounds for predicates assigned to trailing fields of a text
            // index, but rather attempt to attach a covered filter. However, certain predicates can
            // never be correctly covered (e.g. $exists), so we assign the tightness accordingly.
            scanState->tightness = IndexBoundsBuilder::canUseCoveredMatching(expr, index)
                ? IndexBoundsBuilder::INEXACT_COVERED
                : IndexBoundsBuilder::INEXACT_FETCH;
        }

        return;
    }

    IndexBounds* boundsToFillOut = nullptr;

    if (STAGE_GEO_NEAR_2D == type) {
        invariant(INDEX_2D == index.type);

        // 2D indexes have a special format - the "2d" field stores a normally-indexed BinData
        // field, but additional array fields are *not* exploded into multi-keys - they are stored
        // directly as arrays in the index.  Also, no matter what the index expression, the "2d"
        // field is always first.
        //
        // This means that we can only generically accumulate bounds for 2D indexes over the first
        // "2d" field (pos == 0) - MatchExpressions over other fields in the 2D index may be covered
        // (can be evaluated using only the 2D index key).  The additional fields must not affect
        // the index scan bounds, since they are not stored in an IndexScan-compatible format.

        if (pos > 0) {
            // The predicate is over a trailing field of the "2d" index. If possible, we assign it
            // as a covered filter (the INEXACT_COVERED case). Otherwise, the filter must be
            // evaluated after fetching the full documents.
            scanState->tightness = IndexBoundsBuilder::canUseCoveredMatching(expr, index)
                ? IndexBoundsBuilder::INEXACT_COVERED
                : IndexBoundsBuilder::INEXACT_FETCH;
            return;
        }

        // We may have other $geoPredicates on a near index - generate bounds for these
        GeoNear2DNode* gn = static_cast<GeoNear2DNode*>(node);
        boundsToFillOut = &gn->baseBounds;
    } else if (STAGE_GEO_NEAR_2DSPHERE == type) {
        GeoNear2DSphereNode* gn = static_cast<GeoNear2DSphereNode*>(node);
        boundsToFillOut = &gn->baseBounds;
    } else {
        MONGO_verify(type == STAGE_IXSCAN);
        IndexScanNode* scan = static_cast<IndexScanNode*>(node);

        // See STAGE_GEO_NEAR_2D above - 2D indexes can only accumulate scan bounds over the first
        // "2d" field (pos == 0).
        if (INDEX_2D == index.type && pos > 0) {
            // The predicate is over a trailing field of the "2d" index. If possible, we assign it
            // as a covered filter (the INEXACT_COVERED case). Otherwise, the filter must be
            // evaluated after fetching the full documents.
            scanState->tightness = IndexBoundsBuilder::canUseCoveredMatching(expr, index)
                ? IndexBoundsBuilder::INEXACT_COVERED
                : IndexBoundsBuilder::INEXACT_FETCH;
            return;
        }

        boundsToFillOut = &scan->bounds;
    }

    // Get the ixtag->pos-th element of the index key pattern.
    // TODO: cache this instead/with ixtag->pos?
    BSONObjIterator it(index.keyPattern);
    BSONElement keyElt = it.next();
    for (size_t i = 0; i < pos; ++i) {
        MONGO_verify(it.more());
        keyElt = it.next();
    }
    MONGO_verify(!keyElt.eoo());
    scanState->tightness = IndexBoundsBuilder::INEXACT_FETCH;

    MONGO_verify(boundsToFillOut->fields.size() > pos);

    OrderedIntervalList* oil = &boundsToFillOut->fields[pos];

    if (boundsToFillOut->fields[pos].name.empty()) {
        IndexBoundsBuilder::translate(
            expr, keyElt, index, oil, &scanState->tightness, scanState->getCurrentIETBuilder());
    } else {
        if (MatchExpression::AND == mergeType) {
            IndexBoundsBuilder::translateAndIntersect(
                expr, keyElt, index, oil, &scanState->tightness, scanState->getCurrentIETBuilder());
        } else {
            MONGO_verify(MatchExpression::OR == mergeType);
            IndexBoundsBuilder::translateAndUnion(
                expr, keyElt, index, oil, &scanState->tightness, scanState->getCurrentIETBuilder());
        }
    }
}

void buildTextSubPlan(TextMatchNode* tn) {
    tassert(5432205, "text match node is null", tn);
    tassert(5432206, "text match node already has children", tn->children.empty());
    tassert(5432207, "text search query is not provided", tn->ftsQuery.get());

    auto query = dynamic_cast<const fts::FTSQueryImpl*>(tn->ftsQuery.get());
    // If we're unable to cast to FTSQueryImpl, then the given query must be an FTSQueryNoop, which
    // is only used for testing the QueryPlanner and never tries to execute the query, so we don't
    // need to construct an entire text sub-plan. Moreover, to compute index bounds we need a list
    // of terms, which can only be obtain from FTSQueryImpl.
    if (!query) {
        return;
    }

    // If the query requires the "textScore" field or involves multiple search terms, a TEXT_OR or
    // OR stage is needed. Otherwise, we can use a single index scan directly.
    const bool needOrStage = tn->wantTextScore || query->getTermsForBounds().size() > 1;

    tassert(5432208,
            "failed to obtain text index version",
            tn->index.infoObj.hasField("textIndexVersion"));
    const auto textIndexVersion =
        static_cast<fts::TextIndexVersion>(tn->index.infoObj["textIndexVersion"].numberInt());

    // Get all the index scans for each term in our query.
    std::vector<std::unique_ptr<QuerySolutionNode>> indexScanList;
    indexScanList.reserve(query->getTermsForBounds().size());
    for (const auto& term : query->getTermsForBounds()) {
        auto ixscan = std::make_unique<IndexScanNode>(tn->index);
        ixscan->bounds.startKey = fts::FTSIndexFormat::getIndexKey(
            fts::MAX_WEIGHT, term, tn->indexPrefix, textIndexVersion);
        ixscan->bounds.endKey =
            fts::FTSIndexFormat::getIndexKey(0, term, tn->indexPrefix, textIndexVersion);
        ixscan->bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
        ixscan->bounds.isSimpleRange = true;
        ixscan->direction = -1;
        ixscan->shouldDedup = tn->index.multikey;

        // If we will be adding a TEXT_OR or OR stage, then it is responsible for applying the
        // filter. Otherwise, the index scan applies the filter.
        if (!needOrStage && tn->filter) {
            ixscan->filter = tn->filter->clone();
        }

        indexScanList.push_back(std::move(ixscan));
    }

    // In case the query didn't have any search term, we can simply use an EOF sub-plan, as no
    // results can be returned in this case anyway.
    if (indexScanList.empty()) {
        indexScanList.push_back(std::make_unique<EofNode>());
    }

    // Build the union of the index scans as a TEXT_OR or an OR stage, depending on whether the
    // projection requires the "textScore" $meta field.
    if (tn->wantTextScore) {
        // We use a TEXT_OR stage to get the union of the results from the index scans and then
        // compute their text scores. This is a blocking operation.
        auto textScorer = std::make_unique<TextOrNode>();
        textScorer->filter = std::move(tn->filter);
        textScorer->addChildren(std::move(indexScanList));
        tn->children.push_back(std::move(textScorer));
    } else {
        // Because we don't need the text score, we can use a non-blocking OR stage to get the union
        // of the index scans or use the index scan directly if there is only one.
        auto textSearcher = [&]() -> std::unique_ptr<QuerySolutionNode> {
            if (indexScanList.size() == 1) {
                tassert(5397400,
                        "If there is only one index scan and we do not need textScore, needOrStage "
                        "should be false",
                        !needOrStage);
                return std::move(indexScanList[0]);
            } else {
                auto orTextSearcher = std::make_unique<OrNode>();
                orTextSearcher->filter = std::move(tn->filter);
                orTextSearcher->addChildren(std::move(indexScanList));
                return orTextSearcher;
            }
        }();

        // Unlike the TEXT_OR stage, the OR stage does not fetch the documents that it outputs. We
        // add our own FETCH stage to satisfy the requirement of the TEXT_MATCH stage that its
        // WorkingSetMember inputs have fetched data.
        auto fetchNode = std::make_unique<FetchNode>();
        fetchNode->children.push_back(std::move(textSearcher));

        tn->children.push_back(std::move(fetchNode));
    }
}

void QueryPlannerAccess::finishTextNode(QuerySolutionNode* node, const IndexEntry& index) {
    auto tn = static_cast<TextMatchNode*>(node);

    // If there's no prefix, the filter is already on the node and the index prefix is null.
    // We can just return.
    if (!tn->numPrefixFields) {
        buildTextSubPlan(tn);
        return;
    }

    // We can't create a text stage if there aren't EQ predicates on its prefix terms.  So
    // if we've made it this far, we should have collected the prefix predicates in the
    // filter.
    tassert(9751500, "unexpected empty filter in the given text node", tn->filter);
    MatchExpression* textFilterMe = tn->filter.get();

    BSONObjBuilder prefixBob;

    if (MatchExpression::AND != textFilterMe->matchType()) {
        // Only one prefix term.
        tassert(9751501,
                str::stream() << "expected a single prefix term in the given text node, but got "
                              << tn->numPrefixFields,
                1u == tn->numPrefixFields);
        // Sanity check: must be an EQ.
        tassert(9751502,
                str::stream() << "expected 'EQ' match type in the given text node, but got "
                              << textFilterMe->matchType(),
                MatchExpression::EQ == textFilterMe->matchType());

        EqualityMatchExpression* eqExpr = static_cast<EqualityMatchExpression*>(textFilterMe);
        prefixBob.append(eqExpr->getData());
        tn->filter.reset();
    } else {
        tassert(9751503,
                str::stream() << "expected 'AND' match type in the given text node, but got "
                              << textFilterMe->matchType(),
                MatchExpression::AND == textFilterMe->matchType());

        // Indexed by the keyPattern position index assignment.  We want to add
        // prefixes in order but we must order them first.
        vector<std::unique_ptr<MatchExpression>> prefixExprs(tn->numPrefixFields);

        AndMatchExpression* amExpr = static_cast<AndMatchExpression*>(textFilterMe);
        tassert(9751504,
                str::stream() << "'AND' expression children count " << amExpr->numChildren()
                              << " is less than prefix term count " << tn->numPrefixFields,
                amExpr->numChildren() >= tn->numPrefixFields);

        // Look through the AND children.  The prefix children we want to
        // stash in prefixExprs.
        size_t curChild = 0;
        while (curChild < amExpr->numChildren()) {
            IndexTag* ixtag = checked_cast<IndexTag*>(amExpr->getChild(curChild)->getTag());
            tassert(9751505, "expected non-null index tag", nullptr != ixtag);
            // Skip this child if it's not part of a prefix, or if we've already assigned a
            // predicate to this prefix position.
            if (ixtag->pos >= tn->numPrefixFields || prefixExprs[ixtag->pos] != nullptr) {
                ++curChild;
                continue;
            }
            prefixExprs[ixtag->pos] = std::move((*amExpr->getChildVector())[curChild]);
            amExpr->getChildVector()->erase(amExpr->getChildVector()->begin() + curChild);
            // Don't increment curChild.
        }

        // Go through the prefix equalities in order and create an index prefix out of them.
        for (size_t i = 0; i < prefixExprs.size(); ++i) {
            auto prefixMe = prefixExprs[i].get();
            tassert(9751506,
                    "unexpected empty prefix term in the given text node",
                    nullptr != prefixMe);
            tassert(9751507,
                    str::stream() << "expected 'EQ' match type in all prefix terms, but got "
                                  << prefixMe->matchType(),
                    MatchExpression::EQ == prefixMe->matchType());
            EqualityMatchExpression* eqExpr = static_cast<EqualityMatchExpression*>(prefixMe);
            prefixBob.append(eqExpr->getData());
        }

        // Clear out an empty $and.
        if (0 == amExpr->numChildren()) {
            tn->filter.reset();
        } else if (1 == amExpr->numChildren()) {
            // Clear out unsightly only child of $and.
            auto child = std::move((*amExpr->getChildVector())[0]);
            amExpr->getChildVector()->clear();
            // Deletes current filter which is amExpr.
            tn->filter = std::move(child);
        }
    }

    tn->indexPrefix = prefixBob.obj();

    buildTextSubPlan(tn);
}

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

void QueryPlannerAccess::finishAndOutputLeaf(ScanBuildingState* scanState,
                                             vector<std::unique_ptr<QuerySolutionNode>>* out) {
    finishLeafNode(scanState->currentScan.get(),
                   scanState->indices[scanState->currentIndexNumber],
                   std::move(scanState->ietBuilders));

    if (MatchExpression::OR == scanState->root->matchType()) {
        if (orNeedsFetch(scanState)) {
            // In order to correctly evaluate the predicates for this index, we have to
            // fetch the full documents. Add a fetch node above the index scan whose filter
            // includes *all* of the predicates used to generate the ixscan.
            auto fetch = std::make_unique<FetchNode>();
            // Takes ownership.
            fetch->filter = std::move(scanState->curOr);
            // Takes ownership.
            fetch->children.push_back(std::move(scanState->currentScan));

            scanState->currentScan = std::move(fetch);
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
            scanState->currentScan->filter = std::move(scanState->curOr);
        }
    }

    out->push_back(std::move(scanState->currentScan));
}

void QueryPlannerAccess::finishLeafNode(
    QuerySolutionNode* node,
    const IndexEntry& index,
    std::vector<interval_evaluation_tree::Builder> ietBuilders) {
    const StageType type = node->getType();

    if (STAGE_TEXT_MATCH == type) {
        return finishTextNode(node, index);
    }

    IndexEntry* nodeIndex = nullptr;
    IndexBounds* bounds = nullptr;

    if (STAGE_GEO_NEAR_2D == type) {
        GeoNear2DNode* gnode = static_cast<GeoNear2DNode*>(node);
        bounds = &gnode->baseBounds;
        nodeIndex = &gnode->index;
    } else if (STAGE_GEO_NEAR_2DSPHERE == type) {
        GeoNear2DSphereNode* gnode = static_cast<GeoNear2DSphereNode*>(node);
        bounds = &gnode->baseBounds;
        nodeIndex = &gnode->index;
    } else {
        MONGO_verify(type == STAGE_IXSCAN);
        IndexScanNode* scan = static_cast<IndexScanNode*>(node);
        nodeIndex = &scan->index;
        bounds = &scan->bounds;

        // If this is a $** index, update and populate the keyPattern, bounds, and multikeyPaths.
        if (index.type == IndexType::INDEX_WILDCARD) {
            wcp::finalizeWildcardIndexScanConfiguration(scan, &ietBuilders);
        }
    }

    // Find the first field in the scan's bounds that was not filled out.
    // TODO: could cache this.
    size_t firstEmptyField = 0;
    for (firstEmptyField = 0; firstEmptyField < bounds->fields.size(); ++firstEmptyField) {
        if (bounds->fields[firstEmptyField].name.empty()) {
            MONGO_verify(bounds->fields[firstEmptyField].intervals.empty());
            break;
        }
    }

    // Process a case when some fields are not filled out with bounds.
    if (firstEmptyField != bounds->fields.size()) {
        // Skip ahead to the firstEmptyField-th element, where we begin filling in bounds.
        BSONObjIterator it(nodeIndex->keyPattern);
        for (size_t i = 0; i < firstEmptyField; ++i) {
            MONGO_verify(it.more());
            it.next();
        }

        // For each field in the key...
        while (it.more()) {
            BSONElement kpElt = it.next();
            // There may be filled-in fields to the right of the firstEmptyField; for instance, the
            // index {loc:"2dsphere", x:1} with a predicate over x and a near search over loc.
            if (bounds->fields[firstEmptyField].name.empty()) {
                MONGO_verify(bounds->fields[firstEmptyField].intervals.empty());
                IndexBoundsBuilder::allValuesForField(kpElt, &bounds->fields[firstEmptyField]);
            }
            ++firstEmptyField;
        }

        // Make sure that the length of the key is the length of the bounds we started.
        MONGO_verify(firstEmptyField == bounds->fields.size());
    }

    // Build Interval Evaluation Trees used to restore index bounds from cached SBE Plans.
    if (node->getType() == STAGE_IXSCAN && !ietBuilders.empty()) {
        auto ixScan = static_cast<IndexScanNode*>(node);
        ixScan->iets.reserve(ietBuilders.size());
        for (size_t i = 0; i < ietBuilders.size(); ++i) {
            if (auto iet = ietBuilders[i].done()) {
                ixScan->iets.push_back(std::move(*iet));
            } else {
                ixScan->iets.push_back(
                    interval_evaluation_tree::IET::make<interval_evaluation_tree::ConstNode>(
                        bounds->fields[i]));
            }
        }
        LOGV2_DEBUG(
            6334900, 5, "Build IETs", "iets"_attr = ietsToString(ixScan->index, ixScan->iets));
    }

    // We create bounds assuming a forward direction but can easily reverse bounds to align
    // according to our desired direction.
    IndexBoundsBuilder::alignBounds(bounds, nodeIndex->keyPattern, nodeIndex->collator != nullptr);
}

void QueryPlannerAccess::findElemMatchChildren(const MatchExpression* node,
                                               vector<MatchExpression*>* out,
                                               vector<MatchExpression*>* subnodesOut) {
    for (size_t i = 0; i < node->numChildren(); ++i) {
        MatchExpression* child = node->getChild(i);
        if (Indexability::isBoundsGenerating(child) && nullptr != child->getTag()) {
            out->push_back(child);
        } else if (MatchExpression::AND == child->matchType() ||
                   Indexability::arrayUsesIndexOnChildren(child)) {
            findElemMatchChildren(child, out, subnodesOut);
        } else if (nullptr != child->getTag()) {
            subnodesOut->push_back(child);
        }
    }
}

std::vector<std::unique_ptr<QuerySolutionNode>> QueryPlannerAccess::collapseEquivalentScans(
    std::vector<std::unique_ptr<QuerySolutionNode>> scans) {
    invariant(scans.size() > 0);

    // Scans that need to be collapsed will be adjacent to each other in the list due to how we
    // sort the query predicate. We step through the list, either merging the current scan into
    // the last scan in 'collapsedScans', or adding a new entry to 'collapsedScans' if it can't
    // be merged.
    std::vector<std::unique_ptr<QuerySolutionNode>> collapsedScans;

    collapsedScans.push_back(std::move(scans[0]));
    for (size_t i = 1; i < scans.size(); ++i) {
        if (scansAreEquivalent(collapsedScans.back().get(), scans[i].get())) {
            // We collapse the entry from 'ownedScans' into the back of 'collapsedScans'.
            std::unique_ptr<QuerySolutionNode> collapseFrom = std::move(scans[i]);
            FetchNode* collapseFromFetch = getFetchNode(collapseFrom.get());
            FetchNode* collapseIntoFetch = getFetchNode(collapsedScans.back().get());

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
                std::make_unique<OrMatchExpression>();
            collapsedFilter->add(std::move(collapseFromFetch->filter));
            collapsedFilter->add(std::move(collapseIntoFetch->filter));

            // Normalize the filter and add it to 'into'.
            collapseIntoFetch->filter = MatchExpression::optimize(std::move(collapsedFilter),
                                                                  /* enableSimplification */ true);
        } else {
            // Scans are not equivalent and can't be collapsed.
            collapsedScans.push_back(std::move(scans[i]));
        }
    }

    invariant(collapsedScans.size() > 0);
    return collapsedScans;
}

/**
 * This helper determines if a query can be covered depending on the query projection.
 */
bool projNeedsFetch(const CanonicalQuery& query) {
    if (query.isCountLike()) {
        return false;
    }

    // This optimization can only be used for find when the index covers the projection completely.
    // However, if the indexed field is in the projection, the index may return an incorrect value
    // for the field, since it does not distinguish between null and missing. Hence, only find
    // queries projecting _id are covered.
    auto proj = query.getProj();
    if (!proj) {
        return true;
    }

    // We can cover projections on _id and generated fields and expressions depending only on _id.
    // However, if the projection is an exclusion, requires match details, requires the full
    // document, or requires metadata, we will still need a FETCH stage.
    if (proj->type() == projection_ast::ProjectType::kInclusion && !proj->requiresMatchDetails() &&
        proj->metadataDeps().none() && !proj->requiresDocument()) {
        const auto& projFields = proj->getRequiredFields();
        // Note that it is not possible to project onto dotted paths of _id here, since they may be
        // null or missing, and the index cannot differentiate between the two cases, so we would
        // still need a FETCH stage.
        if (projFields.size() == 1 && *projFields.begin() == "_id") {
            return false;
        }
    }

    return true;
}

/**
 * This helper updates a MAYBE_COVERED query tightness to one of EXACT, INEXACT_COVERED, or
 * INEXACT_FETCH, depending on whether we need a FETCH/filter to answer the query projection.
 */
void refineTightnessForMaybeCoveredQuery(const CanonicalQuery& query,
                                         IndexBoundsBuilder::BoundsTightness& tightnessOut) {
    // We need to refine the tightness in case we have a "MAYBE_COVERED" tightness bound which
    // depends on the query's projection. We will not have information about the projection
    // later on in order to make this determination, so we do it here.
    const bool noFetchNeededForProj = !projNeedsFetch(query);
    if (tightnessOut == IndexBoundsBuilder::EXACT_MAYBE_COVERED) {
        if (noFetchNeededForProj) {
            tightnessOut = IndexBoundsBuilder::EXACT;
        } else {
            tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        }
    } else if (tightnessOut == IndexBoundsBuilder::INEXACT_MAYBE_COVERED) {
        if (noFetchNeededForProj) {
            tightnessOut = IndexBoundsBuilder::INEXACT_COVERED;
        } else {
            tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        }
    }
}

bool QueryPlannerAccess::processIndexScans(const CanonicalQuery& query,
                                           MatchExpression* root,
                                           bool inArrayOperator,
                                           const std::vector<IndexEntry>& indices,
                                           const QueryPlannerParams& params,
                                           std::vector<std::unique_ptr<QuerySolutionNode>>* out) {
    // Initialize the ScanBuildingState.
    ScanBuildingState scanState(root, indices, inArrayOperator);

    while (scanState.curChild < root->numChildren()) {
        MatchExpression* child = root->getChild(scanState.curChild);

        // If there is no tag, it's not using an index.  We've sorted our children such that the
        // children with tags are first, so we stop now.
        if (nullptr == child->getTag()) {
            break;
        }

        scanState.ixtag = checked_cast<IndexTag*>(child->getTag());
        // If there's a tag it must be valid.
        MONGO_verify(IndexTag::kNoIndex != scanState.ixtag->index);

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
            scanState.ixtag = checked_cast<IndexTag*>(child->getChild(0)->getTag());
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
            MONGO_verify(scanState.currentIndexNumber == scanState.ixtag->index);
            scanState.tightness = IndexBoundsBuilder::INEXACT_FETCH;
            mergeWithLeafNode(child, &scanState);
            refineTightnessForMaybeCoveredQuery(query, scanState.tightness);
            handleFilter(&scanState);
        } else {
            if (nullptr != scanState.currentScan.get()) {
                // Output the current scan before starting to construct a new out.
                finishAndOutputLeaf(&scanState, out);
            } else {
                MONGO_verify(IndexTag::kNoIndex == scanState.currentIndexNumber);
            }

            // Reset state before producing a new leaf.
            scanState.resetForNextScan(scanState.ixtag, query.isParameterized());

            scanState.currentScan = makeLeafNode(query,
                                                 indices[scanState.currentIndexNumber],
                                                 scanState.ixtag->pos,
                                                 child,
                                                 &scanState.tightness,
                                                 scanState.getCurrentIETBuilder());

            refineTightnessForMaybeCoveredQuery(query, scanState.tightness);
            handleFilter(&scanState);
        }
    }

    // If the index is partial and we have reached children without index tag, check if they are
    // covered by the index' filter expression. In this case the child can be removed. In some cases
    // this enables to remove the fetch stage from the plan.
    // The check could be put inside the 'handleFilterAnd()' function, but if moved then the
    // optimization will not be applied if the predicate contains an $elemMatch expression, since
    // then the 'handleFilterAnd()' is not called.
    if (IndexTag::kNoIndex != scanState.currentIndexNumber) {
        const IndexEntry& index = indices[scanState.currentIndexNumber];
        if (index.filterExpr != nullptr) {
            while (scanState.curChild < root->numChildren()) {
                MatchExpression* child = root->getChild(scanState.curChild);
                if (expression::isSubsetOf(index.filterExpr, child)) {
                    // When the documents satisfying the index filter predicate are a subset of the
                    // documents satisfying the child expression, the child predicate is redundant.
                    // Remove the child from the root's children.
                    // For example: index on 'a' with a filter {$and: [{a: {$gt: 10}}, {b: {$lt:
                    // 100}}]} and a query predicate {$and: [{a: {$gt: 20}}, {b: {$lt: 100}}]}. The
                    // non-indexed child {b: {$lt: 100}} is always satisfied by the index filter and
                    // can be removed.

                    // In case of index filter predicate with $or, this optimization is not
                    // applicable, since the subset relationship doesn't hold.
                    // For example, an index on field 'c' with a filter expression {$or: [{a: {$gt:
                    // 10}}, {b: {$lt: 100}}]} could be applicable for the query with a predicate
                    // {$and: [{c: {$gt: 100}}, {b: {$lt: 100}}]}, but the predicate will not be
                    // removed.
                    scanState.tightness = IndexBoundsBuilder::EXACT;
                    refineTightnessForMaybeCoveredQuery(query, scanState.tightness);
                    handleFilter(&scanState);
                } else {
                    ++scanState.curChild;
                }
            }
        }
    }

    // Output the scan we're done with, if it exists.
    if (nullptr != scanState.currentScan.get()) {
        finishAndOutputLeaf(&scanState, out);
    }

    return true;
}

bool QueryPlannerAccess::processIndexScansElemMatch(
    const CanonicalQuery& query,
    ScanBuildingState* scanState,
    const QueryPlannerParams& params,
    std::vector<std::unique_ptr<QuerySolutionNode>>* out) {
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
            // 'subnode' is beneath an $elemMatch. When planning the children of array operators, we
            // keep ownership of the match expression node. Therefore, we pass nullptr for the
            // 'ownedRoot' argument.
            auto childSolution = _buildIndexedDataAccess(query, subnode, nullptr, indices, params);

            // _buildIndexedDataAccess(...) returns NULL in error conditions, when it is unable to
            // construct a query solution from a tagged match expression tree. If we are unable to
            // construct a solution according to the instructions from the enumerator, then we bail
            // out early (by returning false) rather than continuing on and potentially constructing
            // an invalid solution tree.
            if (!childSolution) {
                return false;
            }

            // Output the resulting solution tree.
            out->push_back(std::move(childSolution));
        }
    }

    // For each predicate in 'emChildren', try to merge it with the current index scan.
    //
    // This loop is similar to that in processIndexScans(...), except it does not call into
    // handleFilters(...). Instead, we leave the entire $elemMatch filter intact. This way,
    // the complete $elemMatch expression will be affixed as a filter later on.
    for (size_t i = 0; i < emChildren.size(); ++i) {
        MatchExpression* emChild = emChildren[i];
        invariant(nullptr != emChild->getTag());
        scanState->ixtag = checked_cast<IndexTag*>(emChild->getTag());

        // If 'emChild' is a NOT, then the tag we're interested in is on the NOT's
        // child node.
        if (MatchExpression::NOT == emChild->matchType()) {
            invariant(nullptr != emChild->getChild(0)->getTag());
            scanState->ixtag = checked_cast<IndexTag*>(emChild->getChild(0)->getTag());
            invariant(IndexTag::kNoIndex != scanState->ixtag->index);
        }

        if (shouldMergeWithLeaf(emChild, *scanState)) {
            // The child uses the same index we're currently building a scan for.  Merge
            // the bounds and filters.
            MONGO_verify(scanState->currentIndexNumber == scanState->ixtag->index);

            scanState->tightness = IndexBoundsBuilder::INEXACT_FETCH;
            mergeWithLeafNode(emChild, scanState);
        } else {
            if (nullptr != scanState->currentScan.get()) {
                finishAndOutputLeaf(scanState, out);
            } else {
                MONGO_verify(IndexTag::kNoIndex == scanState->currentIndexNumber);
            }

            // Reset state before producing a new leaf.
            scanState->resetForNextScan(scanState->ixtag, query.isParameterized());

            scanState->currentScan = makeLeafNode(query,
                                                  indices[scanState->currentIndexNumber],
                                                  scanState->ixtag->pos,
                                                  emChild,
                                                  &scanState->tightness,
                                                  scanState->getCurrentIETBuilder());
        }
    }

    // We're done processing the $elemMatch child. We leave it hanging off
    // it's AND parent so that it will be affixed as a filter later on,
    // and move on to the next child of the AND.
    ++scanState->curChild;
    return true;
}

bool QueryPlannerAccess::processIndexScansSubnode(
    const CanonicalQuery& query,
    ScanBuildingState* scanState,
    const QueryPlannerParams& params,
    std::vector<std::unique_ptr<QuerySolutionNode>>* out) {
    MatchExpression* root = scanState->root;
    MatchExpression* child = root->getChild(scanState->curChild);
    const vector<IndexEntry>& indices = scanState->indices;
    bool inArrayOperator = scanState->inArrayOperator;

    // We may detach the current child from the tree and assume ownership.
    std::unique_ptr<MatchExpression> ownedChild;

    if (MatchExpression::AND == root->matchType() &&
        MatchExpression::ELEM_MATCH_OBJECT == child->matchType()) {
        return processIndexScansElemMatch(query, scanState, params, out);
    } else if (!inArrayOperator) {
        // The logical sub-tree is responsible for fully evaluating itself. Any required filters or
        // fetches are already hung on it. As such, we remove the filter branch from our tree and
        // assume ownership of it.
        ownedChild = std::move((*root->getChildVector())[scanState->curChild]);
        root->getChildVector()->erase(root->getChildVector()->begin() + scanState->curChild);
    } else {
        ++scanState->curChild;
    }

    // If inArrayOperator: takes ownership of child, which is OK, since we detached
    // child from root.
    auto childSolution =
        _buildIndexedDataAccess(query, child, std::move(ownedChild), indices, params);
    if (!childSolution) {
        return false;
    }
    out->push_back(std::move(childSolution));
    return true;
}

std::unique_ptr<QuerySolutionNode> QueryPlannerAccess::buildIndexedAnd(
    const CanonicalQuery& query,
    MatchExpression* root,
    std::unique_ptr<MatchExpression> ownedRoot,
    const vector<IndexEntry>& indices,
    const QueryPlannerParams& params) {
    // Clone the match expression before passing it to processIndexScans(), as it may trim
    // predicates. If we end up with an index intersection solution, then we use our copy of the
    // match expression to be sure that the FETCH stage will recheck the entire predicate. It is not
    // correct to trim predicates for index intersection plans, as this can lead to spurious matches
    // (see SERVER-16750).
    auto clonedRoot = root->clone();

    std::vector<std::unique_ptr<QuerySolutionNode>> ixscanNodes;
    const bool inArrayOperator = !ownedRoot;
    if (!processIndexScans(query, root, inArrayOperator, indices, params, &ixscanNodes)) {
        return nullptr;
    }

    //
    // Process all non-indexed predicates.  We hang these above the AND with a fetch and
    // filter.
    //

    // This is the node we're about to return.
    std::unique_ptr<QuerySolutionNode> andResult;

    // We must use an index for at least one child of the AND.  We shouldn't be here if this
    // isn't the case.
    MONGO_verify(ixscanNodes.size() >= 1);

    // Short-circuit: an AND of one child is just the child.
    if (ixscanNodes.size() == 1) {
        if (root->numChildren() > 0 &&
            wildcard_planning::canOnlyAnswerWildcardPrefixQuery(ixscanNodes)) {
            // If we get here, we have a compound wildcard index which can answer one or more of the
            // predicates in the $and, but we also have at least one additional node attached to the
            // filter. Normally, we would be able to satisfy this case using a FETCH + FILTER +
            // IXSCAN; however, in the case of a $not query which is not supported by the index, the
            // index entry will be expanded in such a way that we won't be able to satisfy the
            // query.
            return nullptr;
        }
        andResult = std::move(ixscanNodes[0]);
    } else {
        if ((params.mainCollectionInfo.options & QueryPlannerParams::TARGET_SBE_STAGE_BUILDER) &&
            !internalQueryForceIntersectionPlans.load()) {
            // When targeting the SBE Stage Builder, we don't allow sort-based intersection or
            // hash-based intersection when 'internalQueryForceIntersectionPlans' is false because
            // SBE's implementation of STAGE_AND_HASH and STAGE_AND_SORTED is not complete.
            LOGV2_DEBUG(9081800,
                        5,
                        "Can't build index intersection solution: AND_SORTED and AND_HASH are not "
                        "possible due to TARGET_SBE_STAGE_BUILDER option being set");
            return nullptr;
        }

        // $** indexes are prohibited from participating in either AND_SORTED or AND_HASH.
        const bool wildcardIndexInvolvedInIntersection =
            std::any_of(ixscanNodes.begin(), ixscanNodes.end(), [](const auto& ixScan) {
                return ixScan->getType() == StageType::STAGE_IXSCAN &&
                    static_cast<IndexScanNode*>(ixScan.get())->index.type == INDEX_WILDCARD;
            });
        if (wildcardIndexInvolvedInIntersection) {
            return nullptr;
        }

        // Figure out if we want AndHashNode or AndSortedNode.
        bool allSortedByDiskLoc = true;
        for (size_t i = 0; i < ixscanNodes.size(); ++i) {
            if (!ixscanNodes[i]->sortedByDiskLoc()) {
                allSortedByDiskLoc = false;
                break;
            }
        }
        if (allSortedByDiskLoc) {
            auto asn = std::make_unique<AndSortedNode>();
            asn->addChildren(std::move(ixscanNodes));
            andResult = std::move(asn);
        } else if (internalQueryPlannerEnableHashIntersection.load()) {
            {
                auto ahn = std::make_unique<AndHashNode>();
                ahn->addChildren(std::move(ixscanNodes));
                andResult = std::move(ahn);
            }

            // The AndHashNode provides the sort order of its last child.  If any of the
            // possible subnodes of AndHashNode provides the sort order we care about, we put
            // that one last.
            for (size_t i = 0; i < andResult->children.size(); ++i) {
                andResult->children[i]->computeProperties();
                if (andResult->children[i]->providedSorts().contains(
                        query.getFindCommandRequest().getSort())) {
                    std::swap(andResult->children[i], andResult->children.back());
                    break;
                }
            }
        } else {
            // We can't use sort-based intersection, and hash-based intersection is disabled.
            // Clean up the index scans and bail out by returning NULL.
            LOGV2_DEBUG(20947,
                        5,
                        "Can't build index intersection solution: AND_SORTED is not possible and "
                        "AND_HASH is disabled");
            return nullptr;
        }
    }

    // Don't bother doing any kind of fetch analysis lite if we're doing it anyway above us.
    if (inArrayOperator) {
        return andResult;
    }

    if (andResult->getType() == STAGE_AND_HASH || andResult->getType() == STAGE_AND_SORTED) {
        // We got an index intersection solution, so we aren't allowed to answer predicates exactly
        // using the index. This is because the index intersection stage finds documents that match
        // each index's predicate, but the document isn't guaranteed to be in a state where it
        // matches all indexed predicates simultaneously. Therefore, it is necessary to add a fetch
        // stage which will explicitly evaluate the entire predicate (see SERVER-16750).
        invariant(clonedRoot);
        auto fetch = std::make_unique<FetchNode>();
        fetch->filter = std::move(clonedRoot);
        fetch->children.push_back(std::move(andResult));
        return fetch;
    }

    // If there are any nodes still attached to the AND, we can't answer them using the
    // index, so we put a fetch with filter.
    if (root->numChildren() > 0) {
        auto fetch = std::make_unique<FetchNode>();
        MONGO_verify(ownedRoot);
        if (ownedRoot->numChildren() == 1) {
            // An $and of one thing is that thing.
            fetch->filter = std::move((*ownedRoot->getChildVector())[0]);
            ownedRoot->getChildVector()->clear();
            // 'autoRoot' will delete the empty $and.
        } else {  // root->numChildren() > 1
            // Takes ownership.
            fetch->filter = std::move(ownedRoot);
        }
        // takes ownership
        fetch->children.push_back(std::move(andResult));
        andResult = std::move(fetch);
    } else {
        // root has no children, let autoRoot get rid of it when it goes out of scope.
    }

    return andResult;
}

std::unique_ptr<QuerySolutionNode> QueryPlannerAccess::buildIndexedOr(
    const CanonicalQuery& query,
    MatchExpression* root,
    std::unique_ptr<MatchExpression> ownedRoot,
    const vector<IndexEntry>& indices,
    const QueryPlannerParams& params) {

    const bool inArrayOperator = !ownedRoot;
    bool usedClusteredCollScan = false;
    std::vector<std::unique_ptr<QuerySolutionNode>> scanNodes;
    if (!processIndexScans(query, root, inArrayOperator, indices, params, &scanNodes)) {
        return nullptr;
    }

    // Check if we can use a CLUSTERED_IXSCAN on the remaining children if they have no plans.
    // Since, the only clustered index currently supported is on '_id' if we are in an array
    // operator, we know we won't make clustered collection scans.
    if (!inArrayOperator && 0 != root->numChildren()) {
        bool clusteredCollection = params.clusteredInfo.has_value();
        const bool isTailable = query.getFindCommandRequest().getTailable();
        if (clusteredCollection) {
            auto clusteredScanDirection =
                QueryPlannerCommon::determineClusteredScanDirection(query, params).value_or(1);
            while (0 < root->numChildren()) {
                usedClusteredCollScan = true;
                MatchExpression* child = root->getChild(0);
                std::unique_ptr<QuerySolutionNode> collScan =
                    makeCollectionScan(query, isTailable, params, clusteredScanDirection, child);
                // Confirm the collection scan node is a clustered collection scan.
                CollectionScanNode* collScanNode = static_cast<CollectionScanNode*>(collScan.get());
                if (!collScanNode->doClusteredCollectionScanClassic()) {
                    return nullptr;
                }

                // Caching OR queries with collection scans is restricted, since it is challenging
                // to determine which match expressions from the input query require a clustered
                // collection scan. Therefore, we cannot correctly calculate the correct bounds for
                // the query using the cached plan.
                collScanNode->markNotEligibleForPlanCache();
                scanNodes.push_back(std::move(collScan));
                // Erase child from root.
                root->getChildVector()->erase(root->getChildVector()->begin());
            }
        }

        // If we have a clustered collection scan, then all index scan stages must be wrapped inside
        // a 'FETCH'. This is to avoid having an unnecessary collection scan node inside a 'FETCH',
        // since the documents will already be fetched.
        // TODO SERVER-77867 investigate when we can avoid adding this 'FETCH' stage.
        if (usedClusteredCollScan) {
            for (size_t i = 0; i < scanNodes.size(); ++i) {
                if (scanNodes[i]->getType() == STAGE_IXSCAN) {
                    scanNodes[i] = std::make_unique<FetchNode>(std::move(scanNodes[i]));
                }
            }
        }
    }

    // Unlike an AND, an OR cannot have filters hanging off of it.  We stop processing
    // when any of our children lack index tags.  If a node lacks an index tag it cannot
    // be answered via an index.
    if (!inArrayOperator && 0 != root->numChildren()) {
        LOGV2_WARNING(20948, "Planner OR error, non-indexed child of OR");
        // We won't enumerate an OR without indices for each child, so this isn't an issue, even
        // if we have an AND with an OR child -- we won't get here unless the OR is fully
        // indexed.
        return nullptr;
    }

    // If all index scans are identical, then we collapse them into a single scan. This prevents
    // us from creating OR plans where the branches of the OR perform duplicate work.
    scanNodes = collapseEquivalentScans(std::move(scanNodes));

    std::unique_ptr<QuerySolutionNode> orResult;

    // An OR of one node is just that node.
    if (1 == scanNodes.size()) {
        orResult = std::move(scanNodes[0]);
    } else {
        std::vector<bool> shouldReverseScan;

        if (query.getSortPattern()) {
            // If all 'scanNodes' can provide the sort, shouldReverseScan is populated with which
            // scans to reverse.
            shouldReverseScan =
                canProvideSortWithMergeSort(scanNodes, query.getFindCommandRequest().getSort());
        }

        if (!shouldReverseScan.empty()) {
            // TODO SERVER-77601 remove this conditional once SBE supports sort keys in collection
            // scans.
            if (usedClusteredCollScan) {
                return nullptr;
            }
            // Each node can provide either the requested sort, or the reverse of the requested
            // sort.
            invariant(scanNodes.size() == shouldReverseScan.size());
            for (size_t i = 0; i < scanNodes.size(); ++i) {
                if (shouldReverseScan[i]) {
                    QueryPlannerCommon::reverseScans(scanNodes[i].get());
                }
            }

            auto msn = std::make_unique<MergeSortNode>();
            msn->sort = query.getFindCommandRequest().getSort();
            msn->addChildren(std::move(scanNodes));
            orResult = std::move(msn);
        } else {
            auto orn = std::make_unique<OrNode>();
            orn->addChildren(std::move(scanNodes));
            orResult = std::move(orn);
        }
    }

    // Evaluate text nodes first to ensure that text scores are available.
    // Move text nodes to front of vector.
    std::stable_partition(orResult->children.begin(), orResult->children.end(), [](auto&& child) {
        return STAGE_TEXT_MATCH == child->getType();
    });

    // OR must have an index for each child, so we should have detached all children from
    // 'root', and there's nothing useful to do with an empty or MatchExpression.  We let it die
    // via autoRoot.

    return orResult;
}

std::unique_ptr<QuerySolutionNode> QueryPlannerAccess::buildIndexedDataAccess(
    const CanonicalQuery& query,
    std::unique_ptr<MatchExpression> root,
    const vector<IndexEntry>& indices,
    const QueryPlannerParams& params) {
    MatchExpression* unownedRoot = root.get();
    return _buildIndexedDataAccess(query, unownedRoot, std::move(root), indices, params);
}

std::unique_ptr<QuerySolutionNode> QueryPlannerAccess::_buildIndexedDataAccess(
    const CanonicalQuery& query,
    MatchExpression* root,
    std::unique_ptr<MatchExpression> ownedRoot,
    const vector<IndexEntry>& indices,
    const QueryPlannerParams& params) {
    if (root->getCategory() == MatchExpression::MatchCategory::kLogical &&
        !Indexability::isBoundsGeneratingNot(root)) {
        if (MatchExpression::AND == root->matchType()) {
            // Takes ownership of root.
            return buildIndexedAnd(query, root, std::move(ownedRoot), indices, params);
        } else if (MatchExpression::OR == root->matchType()) {
            // Takes ownership of root.
            return buildIndexedOr(query, root, std::move(ownedRoot), indices, params);
        } else {
            return nullptr;
        }
    } else {
        if (!root->getTag()) {
            // No index to use here, not in the context of logical operator, so we're SOL.
            return nullptr;
        } else if (Indexability::isBoundsGenerating(root)) {
            // Make an index scan over the tagged index #.
            IndexTag* tag = checked_cast<IndexTag*>(root->getTag());

            IndexBoundsBuilder::BoundsTightness tightness = IndexBoundsBuilder::EXACT;

            const auto& index = indices[tag->index];

            std::vector<interval_evaluation_tree::Builder> ietBuilders{};
            interval_evaluation_tree::Builder* ietBuilder = nullptr;
            if (query.isParameterized()) {
                ietBuilders.resize(index.keyPattern.nFields());
                tassert(6481601,
                        "IET Builder list size must be equal to the number of fields in the key "
                        "pattern",
                        tag->pos < ietBuilders.size());
                ietBuilder = &ietBuilders[tag->pos];
            }

            auto soln = makeLeafNode(query, index, tag->pos, root, &tightness, ietBuilder);
            MONGO_verify(nullptr != soln);
            finishLeafNode(soln.get(), index, std::move(ietBuilders));

            if (!ownedRoot) {
                // We're performing access planning for the child of an array operator such as
                // $elemMatch value.
                return soln;
            }

            // We may be able to avoid adding an extra fetch stage even though the bounds are
            // inexact, for instance if the query is counting null values on an indexed field
            // without projecting that field. We therefore convert "MAYBE_COVERED" bounds into
            // either EXACT or INEXACT, depending on the query projection.
            refineTightnessForMaybeCoveredQuery(query, tightness);

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
                MONGO_verify(nullptr == soln->filter.get());
                soln->filter = std::move(ownedRoot);
                return soln;
            } else {
                auto fetch = std::make_unique<FetchNode>();
                fetch->filter = std::move(ownedRoot);
                fetch->children.push_back(std::move(soln));
                return fetch;
            }
        } else if (Indexability::arrayUsesIndexOnChildren(root)) {
            std::unique_ptr<QuerySolutionNode> solution;

            invariant(root->matchType() == MatchExpression::ELEM_MATCH_OBJECT);
            // The child is an AND.
            invariant(1 == root->numChildren());

            // Recursively build a data access plan for the child of the $elemMatch object. We
            // maintain ownership of 'ownedRoot'.
            solution = _buildIndexedDataAccess(query, root->getChild(0), nullptr, indices, params);
            if (!solution) {
                return nullptr;
            }

            // There may be an array operator above us.
            if (!ownedRoot) {
                return solution;
            }

            auto fetch = std::make_unique<FetchNode>();
            fetch->filter = std::move(ownedRoot);
            fetch->children.push_back(std::move(solution));
            return fetch;
        }
    }

    return nullptr;
}

std::unique_ptr<QuerySolutionNode> QueryPlannerAccess::scanWholeIndex(const IndexEntry& index,
                                                                      const CanonicalQuery& query,
                                                                      int direction) {
    std::unique_ptr<QuerySolutionNode> solnRoot;

    // Build an ixscan over the id index, use it, and return it.
    unique_ptr<IndexScanNode> isn = std::make_unique<IndexScanNode>(index);
    isn->addKeyMetadata = query.metadataDeps()[DocumentMetadataFields::kIndexKey];
    isn->queryCollator = query.getCollator();

    IndexBoundsBuilder::allValuesBounds(index.keyPattern, &isn->bounds, index.collator != nullptr);

    if (-1 == direction) {
        QueryPlannerCommon::reverseScans(isn.get());
        isn->direction = -1;
    }

    deprioritizeUnboundedIndexScan(isn.get(), query.getFindCommandRequest());

    unique_ptr<MatchExpression> filter = query.getPrimaryMatchExpression()->clone();

    // If it's find({}) remove the no-op root.
    if (MatchExpression::AND == filter->matchType() && (0 == filter->numChildren())) {
        solnRoot = std::move(isn);
    } else {
        // TODO: We may not need to do the fetch if the predicates in root are covered.  But
        // for now it's safe (though *maybe* slower).
        unique_ptr<FetchNode> fetch = std::make_unique<FetchNode>();
        fetch->filter = std::move(filter);
        fetch->children.push_back(std::move(isn));
        solnRoot = std::move(fetch);
    }

    return solnRoot;
}

void QueryPlannerAccess::addFilterToSolutionNode(QuerySolutionNode* node,
                                                 std::unique_ptr<MatchExpression> match,
                                                 MatchExpression::MatchType type) {
    if (nullptr == node->filter) {
        node->filter = std::move(match);
    } else if (type == node->filter->matchType()) {
        // The 'node' already has either an AND or OR filter that matches 'type'. Add 'match' as
        // another branch of the filter.
        ListOfMatchExpression* listFilter = static_cast<ListOfMatchExpression*>(node->filter.get());
        listFilter->add(std::move(match));
    } else {
        // The 'node' already has a filter that does not match 'type'. If 'type' is AND, then
        // combine 'match' with the existing filter by adding an AND. If 'type' is OR, combine
        // by adding an OR node.
        unique_ptr<ListOfMatchExpression> listFilter;
        if (MatchExpression::AND == type) {
            listFilter = std::make_unique<AndMatchExpression>();
        } else {
            MONGO_verify(MatchExpression::OR == type);
            listFilter = std::make_unique<OrMatchExpression>();
        }
        unique_ptr<MatchExpression> oldFilter = node->filter->clone();
        listFilter->add(std::move(oldFilter));
        listFilter->add(std::move(match));
        node->filter = std::move(listFilter);
    }
}

void QueryPlannerAccess::handleFilter(ScanBuildingState* scanState) {
    if (MatchExpression::OR == scanState->root->matchType()) {
        handleFilterOr(scanState);
    } else if (MatchExpression::AND == scanState->root->matchType()) {
        handleFilterAnd(scanState);
    } else {
        // We must be building leaves for either and AND or an OR.
        MONGO_UNREACHABLE;
    }
}

void QueryPlannerAccess::handleFilterOr(ScanBuildingState* scanState) {
    MatchExpression* root = scanState->root;

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
        auto child = std::move((*root->getChildVector())[scanState->curChild]);
        root->getChildVector()->erase(root->getChildVector()->begin() + scanState->curChild);
        scanState->curOr->getChildVector()->push_back(std::move(child));
    }
}

void QueryPlannerAccess::handleFilterAnd(ScanBuildingState* scanState) {
    MatchExpression* root = scanState->root;
    const IndexEntry& index = scanState->indices[scanState->currentIndexNumber];

    if (scanState->inArrayOperator) {
        // We're inside an array operator. The entire array operator expression
        // should always be affixed as a filter. We keep 'curChild' in the $and
        // for affixing later.
        ++scanState->curChild;
    } else if (scanState->tightness == IndexBoundsBuilder::EXACT) {
        // The tightness of the bounds is exact. We want to remove this child so that when control
        // returns to handleIndexedAnd we know that we don't need it to create a FETCH stage.
        root->getChildVector()->erase(root->getChildVector()->begin() + scanState->curChild);
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
        auto child = std::move((*root->getChildVector())[scanState->curChild]);
        root->getChildVector()->erase(root->getChildVector()->begin() + scanState->curChild);

        addFilterToSolutionNode(scanState->currentScan.get(), std::move(child), root->matchType());
    } else {
        // We keep curChild in the AND for affixing later.
        ++scanState->curChild;
    }
}

std::unique_ptr<QuerySolutionNode> QueryPlannerAccess::makeIndexScan(
    const IndexEntry& index,
    const CanonicalQuery& query,
    const QueryPlannerParams& params,
    const BSONObj& startKey,
    const BSONObj& endKey) {
    std::unique_ptr<QuerySolutionNode> solnRoot;

    // Build an ixscan over the id index, use it, and return it.
    auto isn = std::make_unique<IndexScanNode>(index);
    isn->direction = 1;
    isn->addKeyMetadata = query.metadataDeps()[DocumentMetadataFields::kIndexKey];
    isn->bounds.isSimpleRange = true;
    isn->bounds.startKey = startKey;
    isn->bounds.endKey = endKey;
    isn->bounds.boundInclusion = BoundInclusion::kIncludeStartKeyOnly;
    isn->queryCollator = query.getCollator();

    unique_ptr<MatchExpression> filter = query.getPrimaryMatchExpression()->clone();

    // If it's find({}) remove the no-op root.
    if (MatchExpression::AND == filter->matchType() && (0 == filter->numChildren())) {
        solnRoot = std::move(isn);
    } else {
        // TODO: We may not need to do the fetch if the predicates in root are covered.  But
        // for now it's safe (though *maybe* slower).
        unique_ptr<FetchNode> fetch = std::make_unique<FetchNode>();
        fetch->filter = std::move(filter);
        fetch->children.push_back(std::move(isn));
        solnRoot = std::move(fetch);
    }

    return solnRoot;
}

void QueryPlannerAccess::ScanBuildingState::resetForNextScan(IndexTag* newTag,
                                                             bool isQueryParameterized) {
    currentScan.reset(nullptr);
    currentIndexNumber = newTag->index;
    tightness = IndexBoundsBuilder::INEXACT_FETCH;
    loosestBounds = IndexBoundsBuilder::EXACT;

    ietBuilders.clear();
    if (isQueryParameterized) {
        const auto& index = indices[newTag->index];
        ietBuilders.resize(index.keyPattern.nFields());
    }

    if (MatchExpression::OR == root->matchType()) {
        curOr = std::make_unique<OrMatchExpression>();
    }
}
}  // namespace mongo
