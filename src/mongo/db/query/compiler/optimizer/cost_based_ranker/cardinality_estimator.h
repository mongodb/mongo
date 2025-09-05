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
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/ce_cache.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/ce_utils.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates_storage.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/query_planner_params.h"

namespace mongo::cost_based_ranker {

using CEResult = StatusWith<CardinalityEstimate>;

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
    CardinalityEstimator(const CollectionInfo& collInfo,
                         const ce::SamplingEstimator* samplingEstimator,
                         EstimateMap& qsnEstimates,
                         QueryPlanRankerModeEnum rankerMode);

    // Delete the copy and move constructors and assignment operator
    CardinalityEstimator(const CardinalityEstimator&) = delete;
    CardinalityEstimator(CardinalityEstimator&&) = delete;
    CardinalityEstimator& operator=(const CardinalityEstimator&) = delete;
    CardinalityEstimator& operator=(CardinalityEstimator&&) = delete;

    CEResult estimatePlan(const QuerySolution& plan) {
        // Restore initial state so that the estimator can be reused for multiple plans.
        _inputCard = _collCard;
        _conjSels.clear();

        return estimate(plan.root());
    }

private:
    // QuerySolutionNodes
    CEResult estimate(const QuerySolutionNode* node);
    CEResult estimate(const CollectionScanNode* node);
    CEResult estimate(const VirtualScanNode* node);
    CEResult estimate(const IndexScanNode* node);
    CEResult estimate(const FetchNode* node);
    CEResult estimate(const AndHashNode* node);
    CEResult estimate(const AndSortedNode* node);
    CEResult estimate(const OrNode* node);
    CEResult estimate(const MergeSortNode* node);
    CEResult estimate(const SortNode* node);
    CEResult estimate(const LimitNode* node);
    CEResult estimate(const SkipNode* node);

    // MatchExpressions
    CEResult estimate(const MatchExpression* node, bool isFilterRoot);
    CEResult estimate(const NotMatchExpression* node, bool isFilterRoot);
    CEResult estimate(const AndMatchExpression* node);
    CEResult estimate(const OrMatchExpression* node, bool isFilterRoot);
    CEResult estimate(const NorMatchExpression* node, bool isFilterRoot);
    CEResult estimate(const ElemMatchValueMatchExpression* node, bool isFilterRoot);
    CEResult estimate(const InternalSchemaXorMatchExpression* node, bool isFilterRoot);
    CEResult estimate(const InternalSchemaAllElemMatchFromIndexMatchExpression* node,
                      bool isFilterRoot);
    CEResult estimate(const InternalSchemaCondMatchExpression* node, bool isFilterRoot);
    CEResult estimate(const InternalSchemaMatchArrayIndexMatchExpression* node, bool isFilterRoot);
    CEResult estimate(const InternalSchemaObjectMatchExpression* node, bool isFilterRoot);
    CEResult estimate(const InternalSchemaAllowedPropertiesMatchExpression* node);

    // Intervals
    CEResult estimate(const IndexBounds* node);
    CEResult estimate(const OrderedIntervalList* node, bool forceHistogram = false);

    // Internal helper functions

    /**
     * Attempt to use histograms to estimate all predicates of the given AndMatchExpression. If
     * estimatation of any of the children is unable to be done with histograms (non-sargable
     * predicate, missing histogram, etc.), returns a non-OK status.
     */
    CEResult tryHistogramAnd(const AndMatchExpression* node);

    /**
     * Estimate the conjunction of MatchExpressions in 'nodes', which are all predicates over
     * 'path', using the histogram over 'path'. This is done by converting each expression in
     * 'nodes' to an Interval, intersecting all the intervals and finally invoking histogram
     * estimation. This function assumes that 'path' is non-multikey.
     */
    CEResult estimateConjWithHistogram(StringData path,
                                       const std::vector<const MatchExpression*>& nodes);

    CEResult scanCard(const QuerySolutionNode* node, const CardinalityEstimate& card);

    template <IntersectionType T>
    CEResult indexIntersectionCard(const T* node);

    template <UnionType T>
    CEResult indexUnionCard(const T* node);

    // Cardinality of nodes that do not affect the number of documents - their output cardinality
    // is the same as their input unless the node has a limit.
    CEResult passThroughNodeCard(const QuerySolutionNode* node);
    CEResult limitNodeCard(const QuerySolutionNode* node, size_t limit);

    CardinalityEstimate conjCard(size_t offset, CardinalityEstimate inputCard) {
        std::span selsToEstimate(std::span(_conjSels.begin() + offset, _conjSels.end()));
        if (selsToEstimate.size() == 0) {
            return inputCard;
        }
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

    /**
     * Estimate the cardinality of the given vector of MatchExpressions as if they were a
     * conjunction.
     */
    CEResult estimateConjunction(const MatchExpression* conjunction);
    /**
     * Estimate the cardinality of the given vector of MatchExpressions as if they were a
     * disjunction. This is a helper for the implementations of estimation of $or and $nor.
     */
    CEResult estimateDisjunction(const std::vector<std::unique_ptr<MatchExpression>>& disjuncts);

    /**
     * Leaf nodes and ORs that are the root of a QSN's filter are atomic from conjunction
     * estimation's perspective, therefore conjunction estimation will not add such nodes
     * to the _conjSels stack. This function adds the selectivity of such root nodes to _conjSels
     * so that they participate in implicit conjunction selectivity calculation.
     * For instance a plan of an IndexScanNode with a filter (a < 5) OR (a > 10), and a subsequent
     * FetchNode with a filter (b > 'abc') express the conjunction
     * ((a < 5) OR (a > 10)) AND (b > 'abc')
     * All conjuncts' selectivities are combined when computing the total cardinality of the
     * FetchNode.
     */
    void addRootNodeSel(const CEResult& ceRes) {
        SelectivityEstimate sel = ceRes.getValue() / _inputCard;
        _conjSels.emplace_back(sel);
    }

    // Pop all selectivities from '_conjSels' after the first 'count' elements.
    void popSelectivities(size_t count = 0) {
        tassert(9586700, "Cannot pop more elements than total size.", count <= _conjSels.size());
        size_t oldSize = _conjSels.size() - count;
        _conjSels.erase(_conjSels.end() - oldSize, _conjSels.end());
    }

    // Get the path of the given node. This function consults the '_elemMatchPathStack' to check if
    // this node is under an $elemMatch, if so it will return that path.
    StringData getPath(const MatchExpression* node);

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

    // Catalog information about the collection including index metadata and cached histograms.
    const CollectionInfo& _collInfo;

    // Sampling estimator used to estimate cardinality using a cache of documents randomly sampled
    // from the collection. We don't own this pointer and it may be null in the case that a sampling
    // method which never uses sampling is requested.
    const ce::SamplingEstimator* _samplingEstimator;

    // A map from QSN to QSNEstimate that stores the final CE result for each QSN node.
    // Not owned by this class - it is passed by the user of this class, and is filled in with
    // entries during the estimation process.
    EstimateMap& _qsnEstimates;

    // The cardinality estimate mode we are using for estimates.
    const QueryPlanRankerModeEnum _rankerMode;

    // Set with the paths we know are multikey which is deduced from the catalog. Note that a field
    // may be multikey but not reflected in this set because there may not be an index over the
    // field.
    StringDataSet _multikeyPaths;

    // Set with the paths we know are non-multikey deduced from the catalog. Like '_multikeyPaths',
    // a field may be non-multikey but not reflected in this set because there are no indexes over
    // the field.
    StringDataSet _nonMultikeyPaths;

    // Keep track of the path associated with the current node in $elemMatch contexts. For example,
    // ElemMatchValueMatchExpression may have a child which looks like GTMatchExpression with an
    // empty path.
    std::stack<StringData> _elemMatchPathStack;

    // Cache cardinality estimates of logically equivalent MatchExpressions and IndexBounds.
    CECache<false /* disable logging */> _ceCache;
};

}  // namespace mongo::cost_based_ranker
