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

namespace mongo::cost_based_ranker {

CardinalityEstimator::CardinalityEstimator(const stats::CollectionStatistics& collStats,
                                           const ce::SamplingEstimator* samplingEstimator,
                                           EstimateMap& qsnEstimates,
                                           QueryPlanRankerModeEnum rankerMode)
    : _collCard{CardinalityEstimate{CardinalityType{collStats.getCardinality()},
                                    EstimationSource::Metadata}},
      _inputCard{_collCard},
      _collStats(collStats),
      _samplingEstimator(samplingEstimator),
      _qsnEstimates{qsnEstimates},
      _rankerMode(rankerMode) {
    if (_rankerMode == QueryPlanRankerModeEnum::kSamplingCE ||
        _rankerMode == QueryPlanRankerModeEnum::kAutomaticCE) {
        tassert(9746501,
                "samplingEstimator cannot be null when ranker mode is samplingCE or automaticCE",
                _samplingEstimator != nullptr);
    }
}

CardinalityEstimate CardinalityEstimator::estimate(const QuerySolutionNode* node) {
    StageType nodeType = node->getType();
    CardinalityEstimate ce{zeroCE};
    bool isConjunctionBreaker = false;

    switch (nodeType) {
        case STAGE_COLLSCAN:
            ce = estimate(static_cast<const CollectionScanNode*>(node));
            break;
        case STAGE_VIRTUAL_SCAN:
            ce = estimate(static_cast<const VirtualScanNode*>(node));
            break;
        case STAGE_IXSCAN:
            ce = estimate(static_cast<const IndexScanNode*>(node));
            break;
        case STAGE_FETCH:
            ce = estimate(static_cast<const FetchNode*>(node));
            break;
        case STAGE_AND_HASH:
            ce = indexIntersectionCard(static_cast<const AndHashNode*>(node));
            break;
        case STAGE_AND_SORTED:
            ce = indexIntersectionCard(static_cast<const AndSortedNode*>(node));
            break;
        case STAGE_OR:
            ce = indexUnionCard(static_cast<const OrNode*>(node));
            isConjunctionBreaker = true;
            break;
        case STAGE_SORT_MERGE:
            ce = indexUnionCard(static_cast<const MergeSortNode*>(node));
            isConjunctionBreaker = true;
            break;
        case STAGE_SORT_DEFAULT:
        case STAGE_SORT_SIMPLE:
            tassert(9768401, "Sort nodes are not expected to have filters.", !node->filter);
            ce = _inputCard;
            isConjunctionBreaker = true;
            break;
        default:
            MONGO_UNIMPLEMENTED_TASSERT(9586709);
    }

    if (isConjunctionBreaker) {
        tassert(9586705,
                "All indirect conjuncts should have been taken into account.",
                _conjSels.empty());
        _inputCard = ce;
    }

    return ce;
}

CardinalityEstimate CardinalityEstimator::estimate(const MatchExpression* node, bool isFilterRoot) {
    if (isFilterRoot && _rankerMode == QueryPlanRankerModeEnum::kSamplingCE) {
        // Sample the entire filter
        return _samplingEstimator->estimateCardinality(node);
    }

    MatchExpression::MatchType nodeType = node->matchType();
    CardinalityEstimate ce{zeroCE};

    switch (nodeType) {
        case MatchExpression::EQ:
        case MatchExpression::LT:
        case MatchExpression::LTE:
        case MatchExpression::GT:
        case MatchExpression::GTE:
            ce = estimate(static_cast<const ComparisonMatchExpression*>(node));
            break;
        case MatchExpression::AND:
            ce = estimate(static_cast<const AndMatchExpression*>(node));
            break;
        case MatchExpression::OR:
            ce = estimate(static_cast<const OrMatchExpression*>(node));
            break;
        default:
            if (node->numChildren() == 0) {
                ce = estimate(static_cast<const LeafMatchExpression*>(node));
            } else {
                MONGO_UNIMPLEMENTED_TASSERT(9586708);
            }
    }

    if (isFilterRoot && (node->numChildren() == 0 || nodeType == MatchExpression::OR)) {
        // Leaf nodes and ORs that are the root of a QSN's filter are atomic from conjunction
        // estimation's perspective, therefore conjunction estimation will not add such nodes
        // to the _conjSels stack. Here we add the selectivity of such root nodes to _conjSels
        // so that they participate in implicit conjunction selectivity calculation. For instance
        // a plan of an IndexScanNode with a filter (a < 5) OR (a > 10), and a subsequent FetchNode
        // with a filter (b > 'abc') express a conjunction ((a < 5) OR (a > 10) AND (b > 'abc')).
        // Both conjuncts are added here when each QSN estimates its filter node.
        // All conjuncts' selectivities are combined when computing the total cardinality of the
        // FetchNode.
        SelectivityEstimate sel = ce / _inputCard;
        _conjSels.emplace_back(sel);
    }

    return ce;
}

/*
 * QuerySolutionNodes
 */
CardinalityEstimate CardinalityEstimator::estimate(const CollectionScanNode* node) {
    return scanCard(node, _inputCard);
}

CardinalityEstimate CardinalityEstimator::estimate(const VirtualScanNode* node) {
    CardinalityEstimate virtualCollCard{CardinalityType{(double)node->docs.size()},
                                        EstimationSource::Code};
    return scanCard(node, virtualCollCard);
}

CardinalityEstimate CardinalityEstimator::scanCard(const QuerySolutionNode* node,
                                                   const CardinalityEstimate& card) {
    QSNEstimate est;
    est.inCE = card;
    if (const MatchExpression* filter = node->filter.get()) {
        est.filterCE = estimate(filter, true);
        est.outCE = *est.filterCE;
    } else {
        est.outCE = card;
    }
    CardinalityEstimate outCE{est.outCE};
    _qsnEstimates.emplace(node, std::move(est));

    return outCE;
}

CardinalityEstimate CardinalityEstimator::estimate(const IndexScanNode* node) {
    QSNEstimate est;
    // Ignore selectivities pushed by other operators up to this point
    size_t selOffset = _conjSels.size();

    // Estimate the number of keys in the scan's interval.
    est.inCE = estimate(&node->bounds);

    if (const MatchExpression* filter = node->filter.get()) {
        // Notice that filterCE is estimated independent of interval CE.
        est.filterCE = estimate(filter, true);
    }

    // Estimate the cardinality of the combined index scan and filter conditions.
    // TODO: conjCard doesn't account for double-counting because some of the filter conditions
    // may re-evaluate the interval bounds.
    est.outCE = conjCard(selOffset, _inputCard);
    CardinalityEstimate outCE{est.outCE};
    _qsnEstimates.emplace(node, std::move(est));

    return outCE;
}

CardinalityEstimate CardinalityEstimator::estimate(const FetchNode* node) {
    QSNEstimate est;

    tassert(
        9586704, "There cannot be other sub-plans parallel to a FetchNode", _conjSels.size() == 0);

    // Child's result CE is the input CE of this node, so there is no entry for it for this node.
    estimate(node->children[0].get());

    if (const MatchExpression* filter = node->filter.get()) {
        est.filterCE = estimate(filter, true);
    }

    // Combine the selectivity of this node's filter with its child selectivities.
    est.outCE = conjCard(0, _inputCard);
    CardinalityEstimate outCE{est.outCE};
    _qsnEstimates.emplace(node, std::move(est));

    return outCE;
}

template <IntersectionType T>
CardinalityEstimate CardinalityEstimator::indexIntersectionCard(const T* node) {
    QSNEstimate est;
    // Ignore selectivities pushed by other operators up to this point.
    size_t selOffset = _conjSels.size();

    for (auto&& child : node->children) {
        estimate(child.get());
    }
    tassert(9586703, "Index intersection nodes are not expected to have filters.", !node->filter);

    // Combine the selectivities of all child nodes.
    est.outCE = conjCard(selOffset, _inputCard);
    CardinalityEstimate outCE{est.outCE};
    _qsnEstimates.emplace(node, std::move(est));

    return outCE;
}

template <UnionType T>
CardinalityEstimate CardinalityEstimator::indexUnionCard(const T* node) {
    QSNEstimate est;

    std::vector<SelectivityEstimate> disjSels;
    size_t selOffset = _conjSels.size();
    // We do not support intersections of unions
    tassert(9586702, "Currently index union is a top-level node.", selOffset == 0);
    for (auto&& child : node->children) {
        auto ce = estimate(child.get());
        trimSels(selOffset);
        disjSels.emplace_back(ce / _inputCard);
    }

    tassert(9586701, "Index union nodes are not expected to have filters.", !node->filter);

    // Combine the selectivities of all child nodes.
    est.outCE = disjCard(_inputCard, disjSels);
    CardinalityEstimate outCE{est.outCE};
    _qsnEstimates.emplace(node, std::move(est));

    return outCE;
}

/*
 * MatchExpressions
 */
CardinalityEstimate CardinalityEstimator::estimate(const ComparisonMatchExpression* node) {
    // We can use a histogram to estimate this MatchExpression by constructing the corresponding
    // interval and estimating that.
    if (_rankerMode == QueryPlanRankerModeEnum::kHistogramCE) {
        auto histogram = _collStats.getHistogram(std::string(node->path()));
        tassert(
            9708801, str::stream{} << "no histogram found for path: " << node->path(), histogram);

        // Generate a fake catalog index object representing a single non-multikey field. We don't
        // care whether the field is actually multikey or not because both cases will generate the
        // same bounds for a ComparisonMatchExpression.
        static IndexEntry fakeIndex(BSONObj::kEmptyObject /* keyPattern */,
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
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(
            node, node->getData(), fakeIndex, &oil, &tightness, nullptr /* ietBuilder */);
        // We expect a simple comparison MatchExpression to generate exact bounds.
        tassert(9708802,
                str::stream{} << "encountered unimplemented case where index bounds are non-exact: "
                              << oil.toString(true),
                tightness == IndexBoundsBuilder::EXACT ||
                    tightness == IndexBoundsBuilder::EXACT_MAYBE_COVERED);
        tassert(9708803,
                str::stream{}
                    << "encountered unimplemented case where index bounds have multiple intervals: "
                    << oil.toString(false),
                oil.intervals.size() == 1);
        const auto& interval = oil.intervals.front();
        tassert(9708804,
                str::stream{} << "encountered interval which is unestimatable: "
                              << interval.toString(true),
                ce::HistogramEstimator::canEstimateInterval(*histogram, interval, true));
        return ce::HistogramEstimator::estimateCardinality(
            *histogram, _inputCard, interval, true, ce::ArrayRangeEstimationAlgo::kExactArrayCE);
    }

    SelectivityEstimate sel = estimateLeafMatchExpression(node, _inputCard);
    return sel * _inputCard;
}

CardinalityEstimate CardinalityEstimator::estimate(const LeafMatchExpression* node) {
    SelectivityEstimate sel = estimateLeafMatchExpression(node, _inputCard);
    return sel * _inputCard;
}

CardinalityEstimate CardinalityEstimator::estimate(const AndMatchExpression* node) {
    // Find with an empty query "coll.find({})" generates a AndMatchExpression without children.
    if (node->numChildren() == 0) {
        return _inputCard;
    }

    // Ignore selectivities pushed by other operators up to this point.
    size_t selOffset = _conjSels.size();

    for (size_t i = 0; i < node->numChildren(); i++) {
        auto ce = estimate(node->getChild(i), false);
        // Add the child selectivity to the conjunction selectivity stack so that it can be
        // combined with selectivities of conjuncts or leaf nodes from parent QSNs.
        SelectivityEstimate sel = ce / _inputCard;
        _conjSels.emplace_back(sel);
    }

    // Notice that the resulting selectivity is not being pushed onto the _conjSels stack
    // because it would otherwise result in double counting when computing the parent selectivity.
    return conjCard(selOffset, _inputCard);
}

CardinalityEstimate CardinalityEstimator::estimate(const OrMatchExpression* node) {
    tassert(9586706, "OrMatchExpression must have children.", node->numChildren() > 0);
    std::vector<SelectivityEstimate> disjSels;
    size_t selOffset = _conjSels.size();
    for (size_t i = 0; i < node->numChildren(); i++) {
        auto ce = estimate(node->getChild(i), false);
        trimSels(selOffset);
        disjSels.emplace_back(ce / _inputCard);
    }
    return disjCard(_inputCard, disjSels);
}

/*
 * Intervals
 */
CardinalityEstimate CardinalityEstimator::estimate(const IndexBounds* node) {
    if (node->isSimpleRange) {
        MONGO_UNIMPLEMENTED_TASSERT(9586707);  // TODO: SERVER-96816
    }
    if (node->isUnbounded()) {
        return _inputCard;
    }

    // Ignore selectivities pushed by other operators up to this point.
    size_t selOffset = _conjSels.size();
    for (const auto& field : node->fields) {
        const OrderedIntervalList* oil = &field;
        // Notice that OILs are considered leaves from CE perspective.
        auto ce = estimate(oil);
        SelectivityEstimate sel = ce / _inputCard;
        _conjSels.emplace_back(sel);
    }

    return conjCard(selOffset, _inputCard);
}

CardinalityEstimate CardinalityEstimator::estimate(const OrderedIntervalList* node) {
    // The intervals in an OIL are disjunct by definition, therefore the total cardinality is
    // the sum of cardinalities of the intervals. Therefore interval selectivities are summed.
    CardinalityEstimate resultCard = minCE;
    for (const auto& interval : node->intervals) {
        SelectivityEstimate sel = [&] {
            if (_rankerMode == QueryPlanRankerModeEnum::kHistogramCE ||
                _rankerMode == QueryPlanRankerModeEnum::kAutomaticCE) {
                auto histogram = _collStats.getHistogram(node->name);
                if (histogram) {
                    bool canEstimate =
                        ce::HistogramEstimator::canEstimateInterval(*histogram, interval, true);
                    if (canEstimate) {
                        return ce::HistogramEstimator::estimateCardinality(
                                   *histogram,
                                   _inputCard,
                                   interval,
                                   true,
                                   ce::ArrayRangeEstimationAlgo::kExactArrayCE) /
                            _inputCard;
                    }
                }
            }
            return estimateInterval(interval, _inputCard);
        }();
        resultCard += sel * _inputCard;
    }
    resultCard = std::min(resultCard, _inputCard);
    return resultCard;
}

}  // namespace mongo::cost_based_ranker
