/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cardinality_estimator.h"

#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/query/compiler/ce/histogram/histogram_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_rewrites.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/heuristic_estimator.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"

#include <absl/container/flat_hash_map.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::cost_based_ranker {

CardinalityEstimator::CardinalityEstimator(const CollectionInfo& collInfo,
                                           const ce::SamplingEstimator* samplingEstimator,
                                           EstimateMap& qsnEstimates,
                                           QueryPlanRankerModeEnum rankerMode)
    : _collCard{CardinalityEstimate{CardinalityType{collInfo.collStats->getCardinality()},
                                    EstimationSource::Metadata}},
      _inputCard{_collCard},
      _collInfo(collInfo),
      _samplingEstimator(samplingEstimator),
      _qsnEstimates{qsnEstimates},
      _rankerMode(rankerMode) {
    if (_rankerMode == QueryPlanRankerModeEnum::kSamplingCE ||
        _rankerMode == QueryPlanRankerModeEnum::kAutomaticCE) {
        tassert(9746501,
                "samplingEstimator cannot be null when ranker mode is samplingCE or automaticCE",
                _samplingEstimator != nullptr);
    }
    for (auto&& indexEntry : _collInfo.indexes) {
        for (auto&& indexedPath : indexEntry.keyPattern) {
            auto path = indexedPath.fieldNameStringData();
            if (indexEntry.pathHasMultikeyComponent(path)) {
                _multikeyPaths.insert(path);
            } else {
                _nonMultikeyPaths.insert(path);
            }
        }
    }
}

CEResult CardinalityEstimator::estimate(const QuerySolutionNode* node) {
    StageType nodeType = node->getType();
    CEResult ceRes(ErrorCodes::CEFailure, "Unable to estimate expression");
    bool isConjunctionBreaker = false;

    switch (nodeType) {
        case STAGE_COLLSCAN:
            ceRes = estimate(static_cast<const CollectionScanNode*>(node));
            break;
        case STAGE_VIRTUAL_SCAN:
            ceRes = estimate(static_cast<const VirtualScanNode*>(node));
            break;
        case STAGE_IXSCAN:
            ceRes = estimate(static_cast<const IndexScanNode*>(node));
            break;
        case STAGE_FETCH:
            ceRes = estimate(static_cast<const FetchNode*>(node));
            break;
        case STAGE_AND_HASH:
            ceRes = indexIntersectionCard(static_cast<const AndHashNode*>(node));
            break;
        case STAGE_AND_SORTED:
            ceRes = indexIntersectionCard(static_cast<const AndSortedNode*>(node));
            break;
        case STAGE_OR:
            // Notice that his is not a conjunction breaker because the result can be combined with
            // the parent's node estimates. Thus indexUnionCard is responsible for replacing the
            // selectivities of the union's children with the selectivity of the union as a whole.
            ceRes = indexUnionCard(static_cast<const OrNode*>(node));
            break;
        case STAGE_SORT_MERGE:
            // See comment for STAGE_OR.
            ceRes = indexUnionCard(static_cast<const MergeSortNode*>(node));
            break;
        case STAGE_SORT_DEFAULT:
        case STAGE_SORT_SIMPLE: {
            const SortNode* sortNode = static_cast<const SortNode*>(node);
            ceRes = estimate(sortNode);
            if (sortNode->limit > 0) {
                // If there is no limit, this node is just a sort and doesn't affect cardinality.
                isConjunctionBreaker = true;
            }
            break;
        }
        case STAGE_PROJECTION_DEFAULT:
        case STAGE_PROJECTION_COVERED:
        case STAGE_PROJECTION_SIMPLE: {
            ceRes = passThroughNodeCard(node);
            break;
        }
        case STAGE_EOF: {
            _qsnEstimates[node] = QSNEstimate{.inCE = zeroCE, .outCE = zeroCE};
            return zeroCE;
        }
        case STAGE_LIMIT:
            ceRes = estimate(static_cast<const LimitNode*>(node));
            isConjunctionBreaker = true;
            break;
        case STAGE_SKIP:
            ceRes = estimate(static_cast<const SkipNode*>(node));
            isConjunctionBreaker = true;
            break;
        case STAGE_SHARDING_FILTER:  // TODO SERVER-99073: Implement shard filter
        case STAGE_DISTINCT_SCAN:    // TODO SERVER-99075: Implement distinct scan
        case STAGE_TEXT_OR:
        case STAGE_TEXT_MATCH:
        case STAGE_GEO_NEAR_2D:
        case STAGE_GEO_NEAR_2DSPHERE:
        case STAGE_SORT_KEY_GENERATOR:
        case STAGE_RETURN_KEY: {
            // These stages will fallback to multiplanning.
            return Status(ErrorCodes::UnsupportedCbrNode, "encountered unsupported stages");
        }
        case STAGE_BATCHED_DELETE:
        case STAGE_CACHED_PLAN:
        case STAGE_COUNT:
        case STAGE_COUNT_SCAN:
        case STAGE_DELETE:
        case STAGE_IDHACK:
        case STAGE_MATCH:
        case STAGE_MOCK:
        case STAGE_MULTI_ITERATOR:
        case STAGE_MULTI_PLAN:
        case STAGE_QUEUED_DATA:
        case STAGE_RECORD_STORE_FAST_COUNT:
        case STAGE_REPLACE_ROOT:
        case STAGE_SAMPLE_FROM_TIMESERIES_BUCKET:
        case STAGE_SPOOL:
        case STAGE_SUBPLAN:
        case STAGE_TIMESERIES_MODIFY:
        case STAGE_TRIAL:
        case STAGE_UNKNOWN:
        case STAGE_UNPACK_SAMPLED_TS_BUCKET:
        case STAGE_UNWIND:
        case STAGE_UPDATE:
        case STAGE_GROUP:
        case STAGE_EQ_LOOKUP:
        case STAGE_EQ_LOOKUP_UNWIND:
        case STAGE_SEARCH:
        case STAGE_WINDOW:
        case STAGE_SENTINEL:
        case STAGE_UNPACK_TS_BUCKET:
            // These stages should never reach the cardinality estimator.
            tasserted(9902301,
                      str::stream{}
                          << "Encountered " << nodeType
                          << " stage in CardinalityEstimator which should be unreachable");
    }

    if (!ceRes.isOK()) {
        return ceRes;
    }
    if (isConjunctionBreaker) {
        popSelectivities();
        _inputCard = ceRes.getValue();
    }
    return ceRes;
}

/**
 * Check if a leaf expression can possibly be used to generate an interval. This function checks
 * for exactly the same expression types as _translatePredicate which is called from
 * IndexBoundsBuilder::translateAndIntersect via CardinalityEstimator::estimateConjWithHistogram().
 *
 * This check cannot determine if the resulting interval will be exact or not. In order to establish
 * this, one needs to actually create the interval and check the result. Any other solution that
 * repeats the logic of interval generation would be complex, unreliable and error-prone.
 */
bool isSargableLeaf(const MatchExpression* node) {
    switch (node->matchType()) {
        case MatchExpression::EQ:
        case MatchExpression::LT:
        case MatchExpression::LTE:
        case MatchExpression::GT:
        case MatchExpression::GTE:
        case MatchExpression::INTERNAL_EXPR_EQ:
        case MatchExpression::INTERNAL_EXPR_GT:
        case MatchExpression::INTERNAL_EXPR_GTE:
        case MatchExpression::INTERNAL_EXPR_LT:
        case MatchExpression::INTERNAL_EXPR_LTE:
            return static_cast<const ComparisonMatchExpression*>(node)->getData().type() !=
                BSONType::array;
        case MatchExpression::MATCH_IN:
            return !static_cast<const InMatchExpression*>(node)->hasArray();
        case MatchExpression::EXISTS:
        case MatchExpression::MOD:
        case MatchExpression::REGEX:
        case MatchExpression::TYPE_OPERATOR:
        case MatchExpression::INTERNAL_BUCKET_GEO_WITHIN:
            return true;
        default:
            return false;
    }
}

/**
 * Check if an arbitrary expression can possibly be used to generate an interval.
 *
 * This check is cheap but incomplete, and may return false positives. A much more complete check
 * can be done by reusing QueryPlannerIXSelect::rateIndices() or even
 * QueryPlannerIXSelect::logicalNodeMayBeSupportedByAnIndex()
 */
bool isSargableExpr(const MatchExpression* node) {
    if (isSargableLeaf(node)) {
        return true;
    }
    auto nodeType = node->matchType();
    if (nodeType == MatchExpression::NOT) {
        const auto child = node->getChild(0);
        return isSargableLeaf(child);
    }
    if (nodeType == MatchExpression::MatchExpression::ELEM_MATCH_VALUE) {
        bool isSargable = true;
        for (size_t i = 0; i < node->numChildren(); ++i) {
            isSargable &= isSargableLeaf(node->getChild(i));
        }
        return isSargable;
    }
    return false;
}

StringData CardinalityEstimator::getPath(const MatchExpression* node) {
    if (node->matchType() == MatchExpression::NOT) {
        return getPath(node->getChild(0));
    } else if (!_elemMatchPathStack.empty()) {
        return _elemMatchPathStack.top();
    }
    return node->path();
}

CEResult CardinalityEstimator::estimate(const MatchExpression* node, const bool isFilterRoot) {
    if (isFilterRoot && _rankerMode == QueryPlanRankerModeEnum::kSamplingCE) {
        // Sample the entire filter and scale it to the child's input cardinality.
        // The sampling estimator returns cardinality estimates scaled to the collection
        // cardinality, however this MatchExpression maybe appear in the context of a plan fragment
        // which child's cardinality is not that of the original collection (e.g. Fetch above an
        // index union). In this case, the collection cardinality and input cardinality differ and
        // we need to scale our estimate accordingly.
        const auto ce = _ceCache.getOrCompute(
            node, [&] { return _samplingEstimator->estimateCardinality(node); });
        auto sel = ce / _collCard;
        _conjSels.emplace_back(sel);
        return sel * _inputCard;
    }

    CEResult ceRes(ErrorCodes::CEFailure, "Unable to estimate expression");

    bool fallbackToHeuristicCE = true;
    bool strict = _rankerMode == QueryPlanRankerModeEnum::kHistogramCE;
    bool useHistogram = _rankerMode == QueryPlanRankerModeEnum::kHistogramCE ||
        _rankerMode == QueryPlanRankerModeEnum::kAutomaticCE;

    /**
     * Sargable expressions are those that can be transformed into intervals. They can be estimated
     * via histogram-CE if this mode is set, and if there are suitable histograms. Most of these
     * expressions are leaves (have 0-children), but there are few non-leaves that can be estimated
     * as well (e.g. NOT).
     */
    if (useHistogram && isSargableExpr(node)) {
        ceRes = estimateConjWithHistogram(getPath(node), {node});
        if (ceRes.isOK()) {
            fallbackToHeuristicCE = false;
        } else if (strict) {
            return ceRes;
        }
    }

    /**
     * Estimate via heuristic CE any leaf match expression. Notice that there are other such nodes
     * besides LeafMatchExpression subclasses. Heuristic CE doesn't estimate non-leaf nodes. This
     * is done be the switch statment below.
     */
    bool useHeuristic =
        _rankerMode == QueryPlanRankerModeEnum::kHeuristicCE || fallbackToHeuristicCE;
    if (useHeuristic && heuristicIsEstimable(node)) {
        tassert(9902901, "CE reestimation not allowed", !ceRes.isOK());
        const SelectivityEstimate sel = heuristicLeafMatchExpressionSel(node, _inputCard);
        ceRes = CEResult(sel * _inputCard);
    }

    if (ceRes.isOK()) {
        if (isFilterRoot) {
            // Add this node's selectivity to the _conjSels so that it can be combined with parent
            // nodes. For a detailed explanation see the comment to addRootNodeSel().
            _conjSels.emplace_back(ceRes.getValue() / _inputCard);
        }
        return ceRes;
    }

    switch (node->matchType()) {
        case MatchExpression::NOT:
            ceRes = estimate(static_cast<const NotMatchExpression*>(node), isFilterRoot);
            break;
        case MatchExpression::AND:
            ceRes = estimate(static_cast<const AndMatchExpression*>(node));
            break;
        case MatchExpression::OR:
            ceRes = estimate(static_cast<const OrMatchExpression*>(node), isFilterRoot);
            break;
        case MatchExpression::NOR:
            ceRes = estimate(static_cast<const NorMatchExpression*>(node), isFilterRoot);
            break;
        case MatchExpression::ELEM_MATCH_VALUE:
            ceRes = estimate(static_cast<const ElemMatchValueMatchExpression*>(node), isFilterRoot);
            break;
        case MatchExpression::ELEM_MATCH_OBJECT:
            // TODO SERVER-100293
            return Status(ErrorCodes::UnsupportedCbrNode, "elemMatchObject not supported");
        case MatchExpression::INTERNAL_SCHEMA_XOR:
            ceRes =
                estimate(static_cast<const InternalSchemaXorMatchExpression*>(node), isFilterRoot);
            break;
        case MatchExpression::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX:
            ceRes = estimate(
                static_cast<const InternalSchemaAllElemMatchFromIndexMatchExpression*>(node),
                isFilterRoot);
            break;
        case MatchExpression::INTERNAL_SCHEMA_COND:
            ceRes =
                estimate(static_cast<const InternalSchemaCondMatchExpression*>(node), isFilterRoot);
            break;
        case MatchExpression::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX:
            ceRes = estimate(static_cast<const InternalSchemaMatchArrayIndexMatchExpression*>(node),
                             isFilterRoot);
            break;
        case MatchExpression::INTERNAL_SCHEMA_OBJECT_MATCH:
            ceRes = estimate(static_cast<const InternalSchemaObjectMatchExpression*>(node),
                             isFilterRoot);
            break;
        case MatchExpression::INTERNAL_SCHEMA_ALLOWED_PROPERTIES:
            ceRes =
                estimate(static_cast<const InternalSchemaAllowedPropertiesMatchExpression*>(node));
            break;
        default:
            MONGO_UNIMPLEMENTED_TASSERT(9586708);
    }

    return ceRes;
}

/*
 * QuerySolutionNodes
 */
CEResult CardinalityEstimator::estimate(const CollectionScanNode* node) {
    if (node->isClustered) {
        // Fallback to multiplanning
        return Status(ErrorCodes::UnsupportedCbrNode, "clustered scan unsupported");
    }
    return scanCard(node, _inputCard);
}

CEResult CardinalityEstimator::estimate(const VirtualScanNode* node) {
    CardinalityEstimate virtualCollCard{CardinalityType{(double)node->docs.size()},
                                        EstimationSource::Code};
    return scanCard(node, virtualCollCard);
}

CEResult CardinalityEstimator::scanCard(const QuerySolutionNode* node,
                                        const CardinalityEstimate& card) {
    QSNEstimate est;
    est.inCE = card;

    if (_inputCard == zeroCE) {
        est.outCE = _inputCard;
        _qsnEstimates.emplace(node, std::move(est));
        return _inputCard;
    }

    if (const MatchExpression* filter = node->filter.get()) {
        auto ceRes = estimate(filter, true);
        if (!ceRes.isOK()) {
            return ceRes;
        }
        est.outCE = ceRes.getValue();
    } else {
        est.outCE = card;
    }
    CardinalityEstimate outCE{est.outCE};
    _qsnEstimates.emplace(node, std::move(est));

    return outCE;
}

bool isIndexScanSupported(const IndexScanNode& node) {
    const auto& indexEntry = node.index;
    return indexEntry.filterExpr == nullptr &&  // prevent partial index
        indexEntry.type == INDEX_BTREE &&       // prevent hashed, text, wildcard and geo
        !indexEntry.sparse &&                   // prevent sparse indexes
        indexEntry.collator == nullptr &&       // collation unsupported
        indexEntry.version == IndexDescriptor::IndexVersion::kV2;  // prevent old index formats
}

bool hasOnlyPointPredicates(const IndexBounds* node) {
    for (auto&& oil : node->fields) {
        if (!oil.isPoint()) {
            return false;
        }
    }
    return true;
}

CEResult CardinalityEstimator::estimate(const IndexScanNode* node) {
    if (!isIndexScanSupported(*node)) {
        // Fallback to multiplanning
        return Status(ErrorCodes::UnsupportedCbrNode,
                      str::stream{} << "encountered unsupported index scan: "
                                    << node->index.toString());
    }

    QSNEstimate est;

    if (_inputCard == zeroCE) {
        est.inCE = _inputCard;
        est.outCE = _inputCard;
        _qsnEstimates.emplace(node, std::move(est));
        return _inputCard;
    }

    // Ignore selectivities pushed by other operators up to this point
    size_t selOffset = _conjSels.size();

    // Estimate the number of keys in the index scan interval.
    // We can avoid computing input selectivity for sampling CE on point queries over non-multikey
    // fields without residual filter, as input cardinality is equal to output cardinality.
    if (_rankerMode != QueryPlanRankerModeEnum::kSamplingCE || node->index.multikey ||
        !hasOnlyPointPredicates(&node->bounds) || node->filter || !node->bounds.size()) {
        auto ceRes1 = estimate(&node->bounds);
        if (!ceRes1.isOK()) {
            return ceRes1;
        }
        est.inCE = ceRes1.getValue();
    }

    // Estimate the output cardinality of IndexScan + Residual filter.
    if (_rankerMode == QueryPlanRankerModeEnum::kSamplingCE) {
        // Sampling will attempt to get an estimate for the number of RIDs that the scan returns
        // after deduplication and applying the filter. This approach does not combine selectivity
        // computed from the index scan.
        auto ridsEst = [&]() -> CardinalityEstimate {
            // Try to estimate using transformation to match expression.
            auto matchExpr = getMatchExpressionFromBounds(node->bounds, node->filter.get());
            if (matchExpr) {
                const auto matchExprPtr = matchExpr.get();
                return _ceCache.getOrCompute(std::move(matchExpr), [&] {
                    return _samplingEstimator->estimateCardinality(matchExprPtr);
                });
            }
            // Rare case, CE not cached.
            return _samplingEstimator->estimateRIDs(node->bounds, node->filter.get());
        }();

        _conjSels.emplace_back(ridsEst / _inputCard);
        est.outCE = ridsEst;

        if (!est.inCE.has_value()) {
            // Special case for sampling CE for point queries over non-multikey fields without
            // residual filter. The number of keys scanned is equal to the resulting docs.
            est.inCE = est.outCE;
        }

        CardinalityEstimate outCE{est.outCE};
        _qsnEstimates.emplace(node, std::move(est));
        return outCE;
    }

    // We are not sampling, so we need to compute the CE of the filter and then combine that with
    // the selectivity estimated from the index scan.
    if (const MatchExpression* filter = node->filter.get()) {
        // In the OK case the result of this call to estimate() is that the selectivities of all
        // children are added to _conjSels. These selectivities are accounted in the subsequent
        // call to conjCard().
        auto ceRes2 = estimate(filter, true);
        if (!ceRes2.isOK()) {
            return ceRes2;
        }
    }

    // Estimate the cardinality of the combined index scan and filter conditions.
    // TODO: conjCard doesn't account for double-counting because some of the filter conditions
    // may re-evaluate the interval bounds.
    est.outCE = conjCard(selOffset, _inputCard);
    CardinalityEstimate outCE{est.outCE};
    _qsnEstimates.emplace(node, std::move(est));
    return outCE;
}

CEResult CardinalityEstimator::estimate(const FetchNode* node) {
    QSNEstimate est;

    tassert(
        9586704, "There cannot be other sub-plans parallel to a FetchNode", _conjSels.size() == 0);

    // Child's result CE is the input CE of this node, so there is no entry for it for this node.
    auto ceRes1 = estimate(node->children[0].get());
    if (!ceRes1.isOK()) {
        return ceRes1;
    }

    if (ceRes1 == zeroCE) {
        est.outCE = ceRes1.getValue();
        _qsnEstimates.emplace(node, std::move(est));
        return ceRes1.getValue();
    }

    if (_rankerMode == QueryPlanRankerModeEnum::kSamplingCE &&
        node->children[0]->getType() == STAGE_IXSCAN) {
        // If the FetchNode does not have a filter then its output cardinality will be unchanged
        // from its input cardinality.
        if (node->filter == nullptr) {
            est.outCE = ceRes1.getValue();
            _qsnEstimates.emplace(node, std::move(est));
            return ceRes1.getValue();
        }

        // If the child IndexScan stage does not have a filter, then we can estimate the cardinality
        // of the MatchExpression using the bounds of the IndexScan stage and the filter of the
        // FetchNode.
        if (static_cast<const IndexScanNode*>(node->children[0].get())->filter ==
            nullptr  // TODO SERVER-98577: Remove this restriction
        ) {
            auto& bounds = static_cast<const IndexScanNode*>(node->children[0].get())->bounds;
            auto ce = [&]() -> CardinalityEstimate {
                // Try to estimate using transformation to match expression.
                auto matchExpr = getMatchExpressionFromBounds(bounds, node->filter.get());
                if (matchExpr) {
                    const auto matchExprPtr = matchExpr.get();
                    return _ceCache.getOrCompute(std::move(matchExpr), [&] {
                        return _samplingEstimator->estimateCardinality(matchExprPtr);
                    });
                }
                // Rare case, CE not cached.
                return _samplingEstimator->estimateRIDs(bounds, node->filter.get());
            }();

            popSelectivities();
            _conjSels.emplace_back(ce / _inputCard);
            est.outCE = ce;
            _qsnEstimates.emplace(node, std::move(est));
            return ce;
        }
    }

    if (const MatchExpression* filter = node->filter.get()) {
        // In the OK case the result of this call to estimate() is that the selectivities of all
        // children are added to _conjSels. These selectivities are accounted in the subsequent
        // call to conjCard().
        auto ceRes2 = estimate(filter, true);
        if (!ceRes2.isOK()) {
            return ceRes2;
        }
    }

    // Combine the selectivity of this node's filter (if any) with its child selectivities.
    est.outCE = conjCard(0, _inputCard);
    CardinalityEstimate outCE{est.outCE};
    _qsnEstimates.emplace(node, std::move(est));

    return outCE;
}

CEResult CardinalityEstimator::passThroughNodeCard(const QuerySolutionNode* node) {
    tassert(9768401, "Pass-through nodes cannot have filters.", !node->filter);
    tassert(9768402, "Pass-through nodes must have a single child.", node->children.size() == 1);

    auto ceRes = estimate(node->children[0].get());
    if (!ceRes.isOK()) {
        return ceRes;
    }
    _qsnEstimates.emplace(node, QSNEstimate{.outCE = ceRes.getValue()});
    return ceRes.getValue();
}

CEResult CardinalityEstimator::limitNodeCard(const QuerySolutionNode* node, size_t limit) {
    auto ceRes = estimate(node->children[0].get());
    if (!ceRes.isOK()) {
        return ceRes;
    }
    if (ceRes == zeroCE) {
        _qsnEstimates.emplace(node, QSNEstimate{.outCE = ceRes.getValue()});
        return ceRes.getValue();
    }
    auto limitCE = CardinalityEstimate{CardinalityType{static_cast<double>(limit)},
                                       EstimationSource::Metadata};
    auto est = std::min(limitCE, ceRes.getValue());
    _qsnEstimates.emplace(node, QSNEstimate{.outCE = est});
    return est;
}

template <IntersectionType T>
CEResult CardinalityEstimator::indexIntersectionCard(const T* node) {
    tassert(9586703, "Index intersection nodes are not expected to have filters.", !node->filter);

    QSNEstimate est;
    // Ignore selectivities pushed by other operators up to this point.
    size_t selOffset = _conjSels.size();

    bool hasZeroCEChild = false;
    for (auto&& child : node->children) {
        auto ceRes = estimate(child.get());
        if (!ceRes.isOK()) {
            return ceRes;
        }
        if (ceRes == zeroCE) {
            hasZeroCEChild = true;
        }
    }

    if (hasZeroCEChild) {
        est.outCE = zeroCE;
    } else {
        // Combine the selectivities of all child nodes.
        est.outCE = conjCard(selOffset, _inputCard);
    }

    CardinalityEstimate outCE{est.outCE};
    _qsnEstimates.emplace(node, std::move(est));

    return outCE;
}

template <UnionType T>
CEResult CardinalityEstimator::indexUnionCard(const T* node) {
    tassert(9586701, "Index union nodes are not expected to have filters.", !node->filter);

    QSNEstimate est;

    std::vector<SelectivityEstimate> disjSels;
    size_t selOffset = _conjSels.size();
    // We do not support intersections of unions
    tassert(9586702, "Currently index union is a top-level node.", selOffset == 0);
    for (auto&& child : node->children) {
        auto ceRes = estimate(child.get());
        if (!ceRes.isOK()) {
            return ceRes;
        }
        popSelectivities(selOffset);
        if (ceRes.getValue() == zeroCE) {
            continue;
        }
        disjSels.emplace_back(ceRes.getValue() / _inputCard);
    }
    if (!disjSels.empty()) {
        // Combine the selectivities of all child nodes.
        est.outCE = disjCard(_inputCard, disjSels);
    } else {
        est.outCE = zeroCE;
    }
    popSelectivities();
    CardinalityEstimate outCE{est.outCE};
    if (_inputCard != zeroCE) {
        _conjSels.emplace_back(outCE / _inputCard);
    }
    _qsnEstimates.emplace(node, std::move(est));

    return outCE;
}

// Estimate the CE of a conjunction of expressions in 'nodes'. This function assumes that:
// * 'nodes' contains only ComparisonMatchExpressions.
// * 'path' corresponds to the path of all MatchExpressions in 'nodes'.
// It works by building an OrderedIntervalList representing the bounds that would be generated for a
// hypothetical index on 'path' for the conjunction of expressions in 'nodes' and then invoking
// histogram estimation for the resulting OIL.
CEResult CardinalityEstimator::estimateConjWithHistogram(
    StringData path, const std::vector<const MatchExpression*>& nodes) {
    // Bail out of using a histogram for estimation if 'path' is multikey.
    if (_multikeyPaths.contains(path) && nodes.size() > 1) {
        return Status(ErrorCodes::HistogramCEFailure,
                      str::stream{}
                          << "cannot use histogram to estimate conjunction on multikey path: "
                          << path);
    }

    // At this point, we know that 'path' is non-multikey, so we can safely build an interval for
    // each conjunct and intersect them.
    IndexEntry fakeIndex(BSON(path << "1") /* keyPattern */,
                         INDEX_BTREE,
                         IndexDescriptor::IndexVersion::kV2,
                         false /* multikey */,
                         {} /* multikeyPaths */,
                         {} /* multikeyPathSet */,
                         false /* sparse */,
                         false /* unique */,
                         CoreIndexInfo::Identifier("idx"),
                         nullptr /* filterExpression */,
                         BSONObj::kEmptyObject /* infoObj */,
                         nullptr /* collatorInterface */,
                         nullptr /* wildcardProjection */);
    OrderedIntervalList oil;
    IndexBoundsBuilder::allValuesForField(fakeIndex.keyPattern.firstElement(), &oil);
    IndexBoundsBuilder::BoundsTightness tightness;

    // For each node, translate to OIL and accumulate their intersection.
    for (auto&& expr : nodes) {
        IndexBoundsBuilder::translateAndIntersect(
            expr, fakeIndex.keyPattern.firstElement(), fakeIndex, &oil, &tightness, nullptr);
        if (oil.intervals.empty()) {
            return zeroMetadataCE;
        }
    }
    // TODO: SERVER-98094 use tightness depending the context in which a predicate is estimated

    return estimate(&oil, true);
}

CEResult CardinalityEstimator::tryHistogramAnd(const AndMatchExpression* node) {
    // Set of unique paths references under 'node'
    StringDataSet paths;
    // Map from path to set of MatchExpression* referencing that path
    absl::
        flat_hash_map<StringData, std::vector<const MatchExpression*>, StringMapHasher, StringMapEq>
            exprsByPath;
    size_t selOffset = _conjSels.size();

    // Iterate over the children of this AndMatchExpression and perform the following:
    // 1. Verify all children of this AndMatchExpression are either leaves and thus are likely to be
    // converted to index bounds, or are AND/OR expressions whose children can be estimated via
    // histograms.
    // 2. Keep track of unique paths.
    // 3. Group all nodes that reference the same path.
    for (size_t i = 0; i < node->numChildren(); ++i) {
        const auto child = node->getChild(i);
        StringData path;
        if (isSargableExpr(child)) {
            // This node may be estimated via a histogram by converting it to an interval.
            path = getPath(child);
        } else {
            // This is a composite node (usually AND/OR). Check if all its children can be estimated
            // via a histogram.
            // TODO SERVER-99710: the current approach would combine child estimates via a formula,
            // do it instead more accurately via constructing an interval.
            auto ceRes = estimate(node->getChild(i), false);
            if (!ceRes.isOK()) {
                return ceRes;
            }
            if (ceRes.getValue().source() != EstimationSource::Histogram) {
                return Status(ErrorCodes::HistogramCEFailure,
                              str::stream{}
                                  << "Cannot estimate non-leaf predicates via histogram CE: "
                                  << child->toString());
            }
            _conjSels.emplace_back(ceRes.getValue() / _inputCard);
            continue;
        }

        paths.insert(path);
        // Group nodes by the same path
        auto findRes = exprsByPath.find(path);
        if (findRes == exprsByPath.end()) {
            exprsByPath.insert({path, {child}});  // New node with this path
        } else {
            findRes->second.emplace_back(child);  // There were other nodes with this path
        }
    }

    for (auto&& path : paths) {
        // Set of expressions referencing the current path
        auto nodesForPath = exprsByPath.find(path)->second;
        // Estimate conjunction of all expressions for the current path
        auto ceRes = estimateConjWithHistogram(path, nodesForPath);
        if (!ceRes.isOK()) {
            return ceRes;
        }
        _conjSels.emplace_back(ceRes.getValue() / _inputCard);
    }
    // Combine CE's for the estimates of all paths under 'node'.
    return conjCard(selOffset, _inputCard);
}

CEResult CardinalityEstimator::estimate(const SortNode* node) {
    if (node->limit > 0) {
        return limitNodeCard(node, node->limit);
    } else {
        return passThroughNodeCard(node);
    }
}

CEResult CardinalityEstimator::estimate(const LimitNode* node) {
    return limitNodeCard(node, node->limit);
}

CEResult CardinalityEstimator::estimate(const SkipNode* node) {
    auto ceRes = estimate(node->children[0].get());
    if (!ceRes.isOK()) {
        return ceRes;
    }
    if (ceRes == zeroCE) {
        _qsnEstimates.emplace(node, QSNEstimate{.outCE = ceRes.getValue()});
        return ceRes.getValue();
    }
    auto childEst = ceRes.getValue();
    auto skip =
        CardinalityEstimate{CardinalityType{static_cast<double>(node->skip)}, childEst.source()};

    // If the skip node skips more than the estimate of the child, then this node will return no
    // results.
    CardinalityEstimate card{CardinalityType{0}, childEst.source()};
    if (skip <= childEst) {
        card = childEst - skip;
    }
    _qsnEstimates.emplace(node, QSNEstimate{.outCE = card});
    _conjSels.push_back(card / _inputCard);
    return card;
}

/*
 * MatchExpressions
 */
CEResult CardinalityEstimator::estimate(const NotMatchExpression* node, bool isFilterRoot) {
    auto ceRes = estimate(node->getChild(0), false);
    if (ceRes.isOK()) {
        CardinalityEstimate ce = ceRes.getValue();
        // Negation in Mongo is defined wrt the result set, that is the result of negation should
        // consist of the documents/keys that do not satisfy a condition. Therefore the negated CE
        // is computed as the complement to the matching CE - that of the child node.
        // The use of _inputCard is in sync with the fact that all selectivities within a
        // conjunction (whether it is explicit or implicit) are computed wrt _inputCard. Thus
        // subtracting from _inputCard is equivalent to computing the negated selectivity as
        // (1.0 - notChildSelectivity).
        CEResult negatedCE{_inputCard - ce};

        // Estimation of the child will not result in adding child's selectivity to the stack, so
        // add it here.
        if (isFilterRoot &&
            (node->getChild(0)->numChildren() == 0 ||
             node->getChild(0)->matchType() == MatchExpression::OR)) {
            addRootNodeSel(negatedCE);
        }
        return negatedCE;
    }
    return ceRes;
}

CEResult CardinalityEstimator::estimateConjunction(const MatchExpression* conjunction) {
    // Ignore selectivities pushed by other operators up to this point.
    size_t selOffset = _conjSels.size();

    for (size_t i = 0; i < conjunction->numChildren(); i++) {
        auto ceRes = estimate(conjunction->getChild(i), false);
        if (!ceRes.isOK()) {
            return ceRes;
        }
        // Add the child selectivity to the conjunction selectivity stack so that it can be
        // combined with selectivities of conjuncts or leaf nodes from parent QSNs.
        SelectivityEstimate sel = ceRes.getValue() / _inputCard;
        _conjSels.emplace_back(sel);
    }

    return conjCard(selOffset, _inputCard);
}

CEResult CardinalityEstimator::estimate(const AndMatchExpression* node) {
    // Find with an empty query "coll.find({})" generates a AndMatchExpression without children.
    if (node->isTriviallyTrue()) {
        return _inputCard;
    }

    // Try to use histograms to estimate all children of this AndMatchExpression.
    // TODO: Suppose we have an AND with some predicates on 'a' that can answered with a
    // histogram and some predicates on 'b' that can't. Should we still try to use histogram for
    // 'a'? The code as written will not.
    if (_rankerMode == QueryPlanRankerModeEnum::kHistogramCE ||
        _rankerMode == QueryPlanRankerModeEnum::kAutomaticCE) {
        auto ceRes = tryHistogramAnd(node);
        if (ceRes.isOK()) {
            return ceRes.getValue();
        }
        // Fallback to generic AndMatchExpression estimation.
    }

    // Notice that the combined selectivity of the children is not being pushed onto the _conjSels
    // stack because it would otherwise result in double counting when computing the parent
    // selectivity.
    return estimateConjunction(node);
}

CEResult CardinalityEstimator::estimateDisjunction(
    const std::vector<std::unique_ptr<MatchExpression>>& disjuncts) {
    std::vector<SelectivityEstimate> disjSels;
    size_t selOffset = _conjSels.size();
    for (auto&& node : disjuncts) {
        auto ceRes = estimate(node.get(), false);
        if (!ceRes.isOK()) {
            return ceRes;
        }
        disjSels.emplace_back(ceRes.getValue() / _inputCard);
    }
    popSelectivities(selOffset);
    return disjCard(_inputCard, disjSels);
}

CEResult CardinalityEstimator::estimate(const OrMatchExpression* node, bool isFilterRoot) {
    tassert(9586706, "OrMatchExpression must have children.", node->numChildren() > 0);
    CEResult disjRes = estimateDisjunction(node->getChildVector());
    if (!disjRes.isOK()) {
        return disjRes;
    }
    if (isFilterRoot) {
        addRootNodeSel(disjRes);
    }
    return disjRes;
}

CEResult CardinalityEstimator::estimate(const NorMatchExpression* node, bool isFilterRoot) {
    tassert(9903001, "NorMatchExpression must have children.", node->numChildren() > 0);
    // Estimate $nor as a logical negation of OR. First we estimate the selectivity of the children
    // of 'node' as if they were a $or. Then we negate it to get the resuling selectivity of $nor.
    CEResult disjRes = estimateDisjunction(node->getChildVector());
    if (!disjRes.isOK()) {
        return disjRes;
    }
    SelectivityEstimate disjSel = disjRes.getValue() / _inputCard;
    CEResult res = _inputCard * disjSel.negate();
    if (isFilterRoot) {
        addRootNodeSel(res);
    }
    return res;
}

CEResult CardinalityEstimator::estimate(const ElemMatchValueMatchExpression* node,
                                        bool isFilterRoot) {
    // Check if we have metadata about this field from the catalog. If the field is non-multikey, we
    // can immediately return a 0 result.
    if (_nonMultikeyPaths.contains(node->path())) {
        return zeroCE;
    }

    // Sampling and histogram handle this case higher up.
    uassert(9808601,
            "direct estimation of $elemMatch is currently only supported for heuristicCE",
            _rankerMode == QueryPlanRankerModeEnum::kHeuristicCE ||
                _rankerMode == QueryPlanRankerModeEnum::kAutomaticCE);

    size_t selOffset = _conjSels.size();

    // Ensure the original '_inputCard' and '_elemMatchPathStack' are restored at the end of this
    // function.
    auto originalCard = _inputCard;
    ScopeGuard restore([&] {
        _inputCard = originalCard;
        _elemMatchPathStack.pop();
        popSelectivities(selOffset);
    });

    // Change '_inputCard' to represent the number of unwound array elements. All child
    // selectivities should be calculated relative to this cardinality, allowing us to model the
    // semantics of $elemMatch.
    _inputCard = _inputCard * kAverageElementsPerArray;
    _elemMatchPathStack.push(node->path());

    for (size_t i = 0; i < node->numChildren(); i++) {
        auto ceRes = estimate(node->getChild(i), false);
        if (!ceRes.isOK()) {
            return ceRes;
        }
        // Calculate the selectivity relative to number of unwound values.
        SelectivityEstimate sel = ceRes.getValue() / _inputCard;
        _conjSels.emplace_back(sel);
    }

    // Calculate selectivity and result size relative to the original cardinality.
    auto card = conjCard(selOffset, originalCard);
    // The implicit 'isArray' predicate of $elemMatch is independent from the selectivity of its
    // children, so it can be combined by multiplication.
    auto res = card * kIsArraySel;
    if (isFilterRoot) {
        _conjSels.emplace_back(res / _inputCard);
    }
    return res;
}

CEResult CardinalityEstimator::estimate(const InternalSchemaXorMatchExpression* node,
                                        bool isFilterRoot) {
    if (node->numChildren() == 1) {
        return estimate(node->getChild(0), isFilterRoot);
    }
    if (node->numChildren() > 2) {
        // The semantics of XOR with >2 arguments is unclear, and is hard to estimate.
        return Status(ErrorCodes::CEFailure, "Unable to estimate expression");
    }

    std::vector<SelectivityEstimate> childSels;
    for (size_t i = 0; i < node->numChildren(); i++) {
        auto ceRes = estimate(node->getChild(i), false);
        if (!ceRes.isOK()) {
            return ceRes;
        }
        childSels.emplace_back(ceRes.getValue() / _inputCard);
    }

    // XOR is estimated in a way similar to OR. To reflect the difference between OR and XOR,
    // the overlap betwen the two sets is subtracted completely. Unlike OR exponential backoff is
    // not applied because it is not clear how to extend it to XOR.
    // XOR selectivity is thus: childSels[0] + childSels[1] - (2 * childSels[0] * childSels[1]);
    // The expression terms are grouped to avoid intermediate results >1:
    auto overlapSel = childSels[0] * childSels[1];
    auto xorSel = (childSels[0] - overlapSel) + (childSels[1] - overlapSel);

    size_t selOffset = _conjSels.size();
    popSelectivities(selOffset);
    // Add the child selectivity to the conjunction selectivity stack so that it can be
    // combined with selectivities of conjuncts or leaf nodes from parent QSNs.
    if (isFilterRoot) {
        _conjSels.emplace_back(xorSel);
    }
    return xorSel * _inputCard;
}

CEResult CardinalityEstimator::estimate(
    const InternalSchemaAllElemMatchFromIndexMatchExpression* node, bool isFilterRoot) {
    // A match expression similar to $elemMatch, but only matches arrays for which every element
    // matches the sub-expression.
    auto ceRes = estimate(node->getChild(0), false);
    if (!ceRes.isOK()) {
        return ceRes;
    }
    // All array elements must match, therefore combine the individual selectivities via exponential
    // backoff. Since we don't have stats over array contents, assume the same selectivity 's' for
    // all of the array's elements. Using exponential backoff the combined selectivity of all
    // children is: childSel = s * s^(1/2) * s^(1/4) * s^(1/8) = s^(1 + 1/2 + 1/4 + 1/8) = s^(15/8)
    auto childSel = ceRes.getValue() / _inputCard;
    auto sel = childSel.pow(15.0 / 8.0);
    if (isFilterRoot) {
        _conjSels.emplace_back(sel);
    }
    return sel * _inputCard;
}

CEResult CardinalityEstimator::estimate(const InternalSchemaCondMatchExpression* node,
                                        bool isFilterRoot) {
    // Since we don't know any better, assume that the probability of the condition being true is
    // 50%
    auto ceRes1 = estimate(node->thenBranch(), false);
    if (!ceRes1.isOK()) {
        return ceRes1;
    }
    auto ceRes2 = estimate(node->elseBranch(), false);
    if (!ceRes2.isOK()) {
        return ceRes2;
    }
    auto avgCard = (ceRes1.getValue() + ceRes2.getValue()) * 0.5;
    auto avgSel = avgCard / _inputCard;
    if (isFilterRoot) {
        _conjSels.emplace_back(avgSel);
    }
    return avgCard;
}

CEResult CardinalityEstimator::estimate(const InternalSchemaMatchArrayIndexMatchExpression* node,
                                        bool isFilterRoot) {
    // Matches arrays based on whether or not a specific element in the array matches a
    // sub-expression, or if array's size is less than  the element's index.
    if (node->arrayIndex() > kAverageElementsPerArray) {
        return _inputCard;
    }
    auto ceRes = estimate(node->getChild(0), false);
    if (!ceRes.isOK()) {
        return ceRes;
    }
    // Estimate the probability that a specific element matches an expression.
    // Divide by the average number of array values to reflect the fact that there is lower
    // probability to match a specific array element compared to any element.
    double scaledSel = (ceRes.getValue() / _inputCard).toDouble() / kAverageElementsPerArray;
    auto childSel = SelectivityEstimate{SelectivityType{scaledSel}, EstimationSource::Code};
    if (isFilterRoot) {
        _conjSels.emplace_back(childSel);
    }
    return childSel * _inputCard;
}

CEResult CardinalityEstimator::estimate(const InternalSchemaObjectMatchExpression* node,
                                        bool isFilterRoot) {
    // Returns true if the input value is an object, and that object matches the child expression.

    // Currently assume 90% of values are obects. Could be computed via heuristics if available.
    auto objMatchSel = kIsObjectSel;
    auto ceRes = estimate(node->getChild(0), false);
    if (!ceRes.isOK()) {
        return ceRes;
    }
    auto exprSel = ceRes.getValue() / _inputCard;
    auto sel = objMatchSel * exprSel;
    if (isFilterRoot) {
        _conjSels.emplace_back(sel);
    }
    return sel * _inputCard;
}

CEResult CardinalityEstimator::estimate(
    const InternalSchemaAllowedPropertiesMatchExpression* node) {
    return estimateConjunction(node);
}

/*
 * Intervals
 */

OrderedIntervalList openOil(std::string fieldName) {
    OrderedIntervalList out;
    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendMaxKey("");
    out.name = std::move(fieldName);
    out.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        bob.obj(), BoundInclusion::kIncludeBothStartAndEndKeys));
    return out;
}

std::unique_ptr<IndexBounds> equalityPrefix(const IndexBounds* node) {
    auto eqPrefix = std::make_unique<IndexBounds>();
    bool isEqPrefix = true;
    for (auto&& oil : node->fields) {
        if (isEqPrefix) {
            eqPrefix->fields.push_back(oil);
            isEqPrefix = isEqPrefix && oil.isPoint();
        } else {
            eqPrefix->fields.push_back(openOil(oil.name));
        }
    }
    return eqPrefix;
}

CEResult CardinalityEstimator::estimate(const IndexBounds* node) {
    if (node->isSimpleRange) {
        // TODO SERVER-96816: Implement support for estimation of simple ranges
        return Status(ErrorCodes::UnsupportedCbrNode, "simple ranges unsupported");
    }

    if (_rankerMode == QueryPlanRankerModeEnum::kSamplingCE) {
        // TODO: avoid copies to construct the equality prefix. We could do this by teaching
        // SamplingEstimator or IndexBounds about the equality prefix concept.
        auto eqPrefix = equalityPrefix(node);
        const auto eqPrefixPtr = eqPrefix.get();
        return _ceCache.getOrCompute(std::move(eqPrefix), [&] {
            return _samplingEstimator->estimateKeysScanned(*eqPrefixPtr);
        });
    }

    // Iterate over all intervals over individual index fields (OILs). These intervals are
    // partitioned into two groups.
    // - The first group consists of a prefix of point intervals(equalities) possibly followed by a
    // singe range (inequality). These intervals allow to form a pair of start and end keys used to
    // scan the index. The number of keys between the start and end keys define the cost of the
    // index scan itself.
    // - The second group of intervals could be considered to be applied against the matching index
    // keys to filter them out. We call these the "residual" conditions.
    //
    // The combined CE of all index scan intervals are used to estimate the number of matching
    // keys passed to the parent QSN node. This partitioning of intervals provides an upper bound
    // to the number of keys scanned by the "recursive index navigation" strategy.
    // Example - given the index bounds: {[3,3], [42,42], ['a','z'], [13,13], ['df','jz']}
    // The "equality prefix is: {[3,3], [42,42], ['a','z']}
    // The residual intervals (conditions) are: {[13,13], ['df','jz']}
    std::vector<SelectivityEstimate> residualSels;
    bool isEqPrefix = true;  // Tracks if an OIL is part of an equality prefix
    // Ignore selectivities pushed by other operators up to this point.
    size_t selOffset = _conjSels.size();
    for (const auto& field : node->fields) {
        const OrderedIntervalList* oil = &field;
        // Notice that OILs are considered leaves from CE perspective.
        auto ceRes = estimate(oil);
        if (!ceRes.isOK()) {
            return ceRes;
        }
        SelectivityEstimate sel = ceRes.getValue() / _inputCard;
        if (isEqPrefix) {
            _conjSels.emplace_back(sel);
        } else {
            residualSels.emplace_back(sel);
        }
        isEqPrefix = isEqPrefix && oil->isPoint();
    }

    auto res = conjCard(selOffset, _inputCard);
    for (auto& sel : residualSels) {
        _conjSels.emplace_back(std::move(sel));
    }
    return res;
}

CEResult CardinalityEstimator::estimate(const OrderedIntervalList* node, bool forceHistogram) {
    if (node->isMinToMax() || node->isMaxToMin()) {
        return _inputCard;
    }
    if (node->intervals.empty()) {
        return zeroMetadataCE;
    }

    auto localRankerMode = _rankerMode;
    const bool strict = _rankerMode == QueryPlanRankerModeEnum::kHistogramCE || forceHistogram;
    const stats::CEHistogram* histogram = nullptr;

    if (_rankerMode == QueryPlanRankerModeEnum::kHistogramCE ||
        _rankerMode == QueryPlanRankerModeEnum::kAutomaticCE || forceHistogram) {
        histogram = _collInfo.collStats->getHistogram(node->name);
        if (!histogram) {
            if (strict) {
                return CEResult(ErrorCodes::HistogramCEFailure,
                                str::stream{} << "no histogram found for path: " << node->name);
            }
            localRankerMode = QueryPlanRankerModeEnum::kHeuristicCE;
        }
    }

    // The intervals in an OIL are disjunct by definition, therefore the total cardinality is
    // the sum of cardinalities of the intervals. Therefore interval selectivities are summed.
    CardinalityEstimate resultCard = minCE;
    for (const auto& interval : node->intervals) {
        bool fallbackToHeuristicCE = false;
        if (localRankerMode == QueryPlanRankerModeEnum::kHistogramCE ||
            localRankerMode == QueryPlanRankerModeEnum::kAutomaticCE) {
            if (ce::HistogramEstimator::canEstimateInterval(*histogram, interval)) {
                resultCard += ce::HistogramEstimator::estimateCardinality(
                    *histogram,
                    _inputCard,
                    interval,
                    true,
                    ce::ArrayRangeEstimationAlgo::kExactArrayCE);
            } else {
                if (strict) {
                    return CEResult(ErrorCodes::HistogramCEFailure,
                                    str::stream{} << "encountered interval which is unestimatable: "
                                                  << interval.toString(true));
                }
                fallbackToHeuristicCE = true;
            }
        }
        if (localRankerMode == QueryPlanRankerModeEnum::kHeuristicCE || fallbackToHeuristicCE) {
            SelectivityEstimate sel = estimateInterval(interval, _inputCard);
            resultCard += sel * _inputCard;
        }
    }

    resultCard = std::min(resultCard, _inputCard);
    return resultCard;
}
}  // namespace mongo::cost_based_ranker
