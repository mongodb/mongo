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

#include "mongo/db/query/cost_based_ranker/cardinality_estimator.h"

#include "mongo/db/query/ce/histogram_estimator.h"
#include "mongo/db/query/cost_based_ranker/heuristic_estimator.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/stage_types.h"
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
        if (!indexEntry.multikey) {
            continue;
        }
        for (auto&& indexedPath : indexEntry.keyPattern) {
            auto path = indexedPath.fieldNameStringData();
            if (indexEntry.pathHasMultikeyComponent(path)) {
                _multikeyPaths.insert(path);
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
        case STAGE_SORT_SIMPLE:
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
        case STAGE_SHARDING_FILTER:
            // TODO SERVER-99073: Implement shard filter
            MONGO_UNIMPLEMENTED_TASSERT(9907301);
        case STAGE_DISTINCT_SCAN: {
            isConjunctionBreaker = true;
            // TODO SERVER-99075: Implement distinct scan
            MONGO_UNIMPLEMENTED_TASSERT(9907501);
        }
        case STAGE_TEXT_MATCH: {
            isConjunctionBreaker = true;
            // TODO SERVER-99278 Estimate the cardinality of TextMatchNode QSNs
            MONGO_UNIMPLEMENTED_TASSERT(9927801);
        }
        case STAGE_BATCHED_DELETE:
        case STAGE_CACHED_PLAN:
        case STAGE_COUNT:
        case STAGE_COUNT_SCAN:
        case STAGE_DELETE:
        case STAGE_GEO_NEAR_2D:
        case STAGE_GEO_NEAR_2DSPHERE:
        case STAGE_IDHACK:
        case STAGE_MATCH:
        case STAGE_MOCK:
        case STAGE_MULTI_ITERATOR:
        case STAGE_MULTI_PLAN:
        case STAGE_QUEUED_DATA:
        case STAGE_RECORD_STORE_FAST_COUNT:
        case STAGE_REPLACE_ROOT:
        case STAGE_RETURN_KEY:
        case STAGE_SAMPLE_FROM_TIMESERIES_BUCKET:
        case STAGE_SORT_KEY_GENERATOR:
        case STAGE_SPOOL:
        case STAGE_SUBPLAN:
        case STAGE_TEXT_OR:
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
            MONGO_UNREACHABLE_TASSERT(9902301);
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
                BSONType::Array;
        case MatchExpression::MATCH_IN:
            return !static_cast<const InMatchExpression*>(node)->hasArray();
        case MatchExpression::EXISTS:
        case MatchExpression::MOD:
        case MatchExpression::REGEX:
        case MatchExpression::TYPE_OPERATOR:
        case MatchExpression::GEO:
        case MatchExpression::INTERNAL_BUCKET_GEO_WITHIN:
        case MatchExpression::INTERNAL_EQ_HASHED_KEY:
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
        bool isSargable = false;
        for (size_t i = 0; i < node->numChildren(); ++i) {
            isSargable &= isSargableLeaf(node->getChild(i));
        }
        return isSargable;
    }
    return false;
}

StringData getPath(const MatchExpression* node) {
    if (node->matchType() == MatchExpression::NOT) {
        return getPath(node->getChild(0));
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
        auto sel = _samplingEstimator->estimateCardinality(node) / _collCard;
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
        default:
            MONGO_UNIMPLEMENTED_TASSERT(9586708);
    }

    return ceRes;
}

/*
 * QuerySolutionNodes
 */
CEResult CardinalityEstimator::estimate(const CollectionScanNode* node) {
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

CEResult CardinalityEstimator::estimate(const IndexScanNode* node) {
    QSNEstimate est;
    // Ignore selectivities pushed by other operators up to this point
    size_t selOffset = _conjSels.size();

    // Estimate the number of keys in the scan's interval.
    auto ceRes1 = estimate(&node->bounds);
    if (!ceRes1.isOK()) {
        return ceRes1;
    }
    est.inCE = ceRes1.getValue();

    // Sampling will attempt to get an estimate for the number of RIDs that the scan returns after
    // dedupication and applying the filter. This approach does not combine selectivity computed
    // from the index scan.
    if (_rankerMode == QueryPlanRankerModeEnum::kSamplingCE) {
        auto ridsEst = _samplingEstimator->estimateRIDs(node->bounds, node->filter.get());
        _conjSels.emplace_back(ridsEst / _inputCard);
        est.outCE = ridsEst;
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

    if (node->children[0]->getType() == STAGE_IXSCAN &&
        static_cast<const IndexScanNode*>(node->children[0].get())->filter ==
            nullptr &&  // TODO SERVER-98577: Remove this restriction
        _rankerMode == QueryPlanRankerModeEnum::kSamplingCE) {
        auto ce = _samplingEstimator->estimateRIDs(
            static_cast<const IndexScanNode*>(node->children[0].get())->bounds, node->filter.get());
        popSelectivities();
        _conjSels.emplace_back(ce / _inputCard);
        est.outCE = ce;
        _qsnEstimates.emplace(node, std::move(est));
        return ce;
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
    QSNEstimate est;
    est.outCE = estimate(node->children[0].get()).getValue();
    CardinalityEstimate outCE{est.outCE};
    _qsnEstimates.emplace(node, std::move(est));
    return outCE;
}

template <IntersectionType T>
CEResult CardinalityEstimator::indexIntersectionCard(const T* node) {
    tassert(9586703, "Index intersection nodes are not expected to have filters.", !node->filter);

    QSNEstimate est;
    // Ignore selectivities pushed by other operators up to this point.
    size_t selOffset = _conjSels.size();

    for (auto&& child : node->children) {
        auto ceRes = estimate(child.get());
        if (!ceRes.isOK()) {
            return ceRes;
        }
    }

    // Combine the selectivities of all child nodes.
    est.outCE = conjCard(selOffset, _inputCard);
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
        disjSels.emplace_back(ceRes.getValue() / _inputCard);
    }

    // Combine the selectivities of all child nodes.
    est.outCE = disjCard(_inputCard, disjSels);
    CardinalityEstimate outCE{est.outCE};
    _conjSels.emplace_back(outCE / _inputCard);
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

CEResult CardinalityEstimator::estimate(const LimitNode* node) {
    auto ceRes = estimate(node->children[0].get());
    if (!ceRes.isOK()) {
        return ceRes;
    }
    auto est = std::min(CardinalityEstimate{CardinalityType{static_cast<double>(node->limit)},
                                            EstimationSource::Metadata},
                        ceRes.getValue());
    _qsnEstimates.emplace(node, QSNEstimate{.outCE = est});
    _conjSels.push_back(est / _inputCard);
    return est;
}

CEResult CardinalityEstimator::estimate(const SkipNode* node) {
    auto ceRes = estimate(node->children[0].get());
    if (!ceRes.isOK()) {
        return ceRes;
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
        // Fallback to alternate AndMatchExpression estimation.
    }

    // Ignore selectivities pushed by other operators up to this point.
    size_t selOffset = _conjSels.size();

    for (size_t i = 0; i < node->numChildren(); i++) {
        auto ceRes = estimate(node->getChild(i), false);
        if (!ceRes.isOK()) {
            return ceRes;
        }
        // Add the child selectivity to the conjunction selectivity stack so that it can be
        // combined with selectivities of conjuncts or leaf nodes from parent QSNs.
        SelectivityEstimate sel = ceRes.getValue() / _inputCard;
        _conjSels.emplace_back(sel);
    }

    // Notice that the resulting selectivity is not being pushed onto the _conjSels stack
    // because it would otherwise result in double counting when computing the parent selectivity.
    return conjCard(selOffset, _inputCard);
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
        popSelectivities(selOffset);
        disjSels.emplace_back(ceRes.getValue() / _inputCard);
    }
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

/*
 * Intervals
 */

OrderedIntervalList openOil(std::string fieldName) {
    OrderedIntervalList out;
    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendMaxKey("");
    out.name = fieldName;
    out.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        bob.obj(), BoundInclusion::kIncludeBothStartAndEndKeys));
    return out;
}

IndexBounds equalityPrefix(const IndexBounds* node) {
    IndexBounds eqPrefix;
    bool isEqPrefix = true;
    for (auto&& oil : node->fields) {
        if (isEqPrefix) {
            eqPrefix.fields.push_back(oil);
            isEqPrefix = isEqPrefix && oil.isPoint();
        } else {
            eqPrefix.fields.push_back(openOil(oil.name));
        }
    }
    return eqPrefix;
}

CEResult CardinalityEstimator::estimate(const IndexBounds* node) {
    if (node->isSimpleRange) {
        MONGO_UNIMPLEMENTED_TASSERT(9586707);  // TODO: SERVER-96816
    }

    if (_rankerMode == QueryPlanRankerModeEnum::kSamplingCE) {
        // TODO: avoid copies to construct the equality prefix. We could do this by teaching
        // SamplingEstimator or IndexBounds about the equality prefix concept.
        return _samplingEstimator->estimateKeysScanned(equalityPrefix(node));
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
    for (const auto& sel : residualSels) {
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
