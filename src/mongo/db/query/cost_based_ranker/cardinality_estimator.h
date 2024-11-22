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

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/cost_based_ranker/ce_utils.h"
#include "mongo/db/query/cost_based_ranker/estimates.h"
#include "mongo/db/query/cost_based_ranker/estimates_storage.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/stats/collection_statistics.h"

namespace mongo::cost_based_ranker {

template <typename T>
concept IntersectionType = std::same_as<T, AndHashNode> || std::same_as<T, AndSortedNode>;

template <typename T>
concept UnionType = std::same_as<T, OrNode> || std::same_as<T, MergeSortNode>;

/**
 * This class implements bottom-up cardinality estimation of QuerySolutionNode plans that consist of
 * QSN nodes, MatchExpression filter nodes, and Intervals. This estimator may use different CE
 * methods for different nodes.
 *
 * Estimation is performed recursively bottom-up, where child estimates are combined via
 * exponential backoff. In order to take into account implicit conjunctions where sequences of QSN
 * nodes encode a conjunction, the estimator employs a stack of selectivities that includes the
 * combined selectivities of filter conjunctions and QSN-sequence conjunctions.
 */
class CardinalityEstimator {
public:
    CardinalityEstimator(const stats::CollectionStatistics& collStats,
                         EstimateMap& qsnEstimates,
                         QueryPlanRankerModeEnum rankerMode)
        : _collCard{CardinalityEstimate{CardinalityType{collStats.getCardinality()},
                                        EstimationSource::Metadata}},
          _inputCard{_collCard},
          _collStats(collStats),
          _qsnEstimates{qsnEstimates},
          _rankerMode(rankerMode) {}

    // Delete the copy and move constructors and assignment operator
    CardinalityEstimator(const CardinalityEstimator&) = delete;
    CardinalityEstimator(CardinalityEstimator&&) = delete;
    CardinalityEstimator& operator=(const CardinalityEstimator&) = delete;
    CardinalityEstimator& operator=(CardinalityEstimator&&) = delete;

    void estimatePlan(const QuerySolution& plan) {
        // Restore initial state so that the estimator can be reused for multiple plans.
        _inputCard = _collCard;
        _conjSels.clear();

        estimate(plan.root());
    }

private:
    // QuerySolutionNodes
    CardinalityEstimate estimate(const QuerySolutionNode* node);
    CardinalityEstimate estimate(const CollectionScanNode* node);
    CardinalityEstimate estimate(const VirtualScanNode* node);
    CardinalityEstimate estimate(const IndexScanNode* node);
    CardinalityEstimate estimate(const FetchNode* node);
    CardinalityEstimate estimate(const AndHashNode* node);
    CardinalityEstimate estimate(const AndSortedNode* node);
    CardinalityEstimate estimate(const OrNode* node);
    CardinalityEstimate estimate(const MergeSortNode* node);

    // MatchExpressions
    CardinalityEstimate estimate(const MatchExpression* node, bool isFilterRoot);
    CardinalityEstimate estimate(const ComparisonMatchExpression* node);
    CardinalityEstimate estimate(const LeafMatchExpression* node);
    CardinalityEstimate estimate(const AndMatchExpression* node);
    CardinalityEstimate estimate(const OrMatchExpression* node);
    // Intervals
    CardinalityEstimate estimate(const IndexBounds* node);
    CardinalityEstimate estimate(const OrderedIntervalList* node);

    // Internal helper functions
    CardinalityEstimate scanCard(const QuerySolutionNode* node, const CardinalityEstimate& card);

    template <IntersectionType T>
    CardinalityEstimate indexIntersectionCard(const T* node);

    template <UnionType T>
    CardinalityEstimate indexUnionCard(const T* node);

    CardinalityEstimate conjCard(size_t offset, CardinalityEstimate inputCard) {
        std::span selsToEstimate(std::span(_conjSels.begin() + offset, _conjSels.end()));
        SelectivityEstimate conjSel = conjExponentialBackoff(selsToEstimate);
        CardinalityEstimate resultCard = conjSel * inputCard;
        return resultCard;
    }

    CardinalityEstimate disjCard(CardinalityEstimate inputCard,
                                 std::vector<SelectivityEstimate>& disjSels) {
        SelectivityEstimate disjSel = disjExponentialBackoff(disjSels);
        CardinalityEstimate resultCard = disjSel * inputCard;
        return resultCard;
    }

    // Pop all selectivities from '_conjSels' after the first 'count' elements.
    void trimSels(size_t count) {
        tassert(9586700, "Cannot pop more elements than total size.", count <= _conjSels.size());
        size_t oldSize = _conjSels.size() - count;
        _conjSels.erase(_conjSels.end() - oldSize, _conjSels.end());
    }

    const CardinalityEstimate _collCard;

    // The input cardinality of the last complete conjunction. This conjunction may consist of a
    // chain of QSN nodes (an implicit conjunction) including all intervals and filter expressions
    // in those nodes.
    CardinalityEstimate _inputCard;

    // A stack of the selectivities of all nodes that belong to one conjunction - both from explicit
    // and implicit conjunctions. The stack is cleared by any conjunction-breaker node - that is,
    // any parent node of a conjunction that is not part of the conjunction itself.
    // A subsequent conjunction will push again onto this stack.
    std::vector<SelectivityEstimate> _conjSels;

    // Collection statistics contains cached histograms.
    const stats::CollectionStatistics& _collStats;

    // A map from QSN to QSNEstimate that stores the final CE result for each QSN node.
    // Not owned by this class - it is passed by the user of this class, and is filled in with
    // entries during the estimation process.
    EstimateMap& _qsnEstimates;

    // The cardinality estimate mode we are using for estimates.
    const QueryPlanRankerModeEnum _rankerMode;
};

}  // namespace mongo::cost_based_ranker
