// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates_storage.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

namespace mongo::cost_based_ranker {

class CostEstimator {
public:
    CostEstimator(EstimateMap& estimateMap) : _estimateMap{estimateMap} {};

    /**
     * Estimate the cost of a query plan and all its nodes.
     * Return the total cost of the query plan.
     */
    CostEstimate estimatePlan(const QuerySolution& plan) {
        return costTree(plan.root());
    }

private:
    /**
     * Recursively estimate the cost of all nodes of a QSN plan tree, and store those costs
     * in the corresponding QSNEstimate of each node. Return the cost of the root node.
     */
    CostEstimate costTree(const QuerySolutionNode* node);

    /**
     * Given a QSN node, its child estimates, and its own CEs in qsnEst, compute the node's cost,
     * and set 'qsnEst.cost' to this cost. The paramater 'qsnEst' is both input and output.
     */
    void computeAndSetNodeCost(const QuerySolutionNode* node,
                               const std::vector<CostEstimate>& childCosts,
                               const std::vector<CardinalityEstimate>& childCEs,
                               QSNEstimate& qsnEst);

    CostEstimate filterCost(StageType stage,
                            const MatchExpression* filter,
                            const CardinalityEstimate& ce) const;

    // Some nodes may have a filter component that is a MatchExpression with one (or more) leaves.
    // These coefficients represent the cost of evaluating the first leaf for every item that the
    // node processes.
    static const CostCoefficient fetchFilterIncrement;
    static const CostCoefficient indexFilterIncrement;
    static const CostCoefficient collScanFilterIncrement;
    // Incremental cost for evaluating one more leaf in the MatchExpression filter for each node.
    static const CostCoefficient incrementalFetchFilterLeafCost;
    static const CostCoefficient incrementalIndexFilterLeafCost;
    static const CostCoefficient incrementalCollScanFilterLeafCost;

    // Cost for examining one more document in a collection scan.
    static const CostCoefficient collScanForwardIncrement;
    static const CostCoefficient collScanBackwardIncrement;

    // Cost for examining one more index key in an index scan. These coefficients represent the cost
    // of examining a key of an index with one field in the keypattern.
    static const CostCoefficient indexScanForwardExamineKey;
    static const CostCoefficient indexScanBackwardExamineKey;
    // Cost of an index seek.
    static const CostCoefficient indexForwardSeek;
    static const CostCoefficient indexBackwardSeek;
    // Cost of examining a key of an index with one more field in the keypattern.
    static const CostCoefficient incrementalFieldCost;

    // Cost of a FETCH node to process one item.
    static const CostCoefficient fetchIncrement;

    // Coefficients for the AndHash node.
    static const CostCoefficient andHashBuild;
    static const CostCoefficient andHashProbe;
    static const CostCoefficient andHashOutput;

    // SortDefault startup cost and cost to process one more item.
    static const CostCoefficient sortDefaultStartup;
    static const CostCoefficient sortDefaultIncrement;
    // SortDefault's cost to spill one more time.
    static const CostCoefficient sortDefaultSpillIncrement;
    // Coefficients for the SortDefault node with a limit. For details on the reasoning behind the
    // need for 3 other coefficients, see the comment in the function 'updateCutoff' in
    // 'sorter_template_defs.h'.
    static const CostCoefficient sortDefaultLimitStartup;
    static const CostCoefficient sortDefaultLimitC1;
    static const CostCoefficient sortDefaultLimitC2;
    static const CostCoefficient sortDefaultLimitC3;

    // SortSimple startup cost and cost to process one more item.
    static const CostCoefficient sortSimpleStartup;
    static const CostCoefficient sortSimpleIncrement;
    // SortDefault's cost to spill one more time.
    static const CostCoefficient sortSimpleSpillIncrement;
    // Coefficients for the SortSimple node with a limit. For details on the reasoning behind the
    // need for 3 other coefficients, see the comment in the function 'updateCutoff' in
    // 'sorter_template_defs.h'.
    static const CostCoefficient sortSimpleLimitStartup;
    static const CostCoefficient sortSimpleLimitC1;
    static const CostCoefficient sortSimpleLimitC2;
    static const CostCoefficient sortSimpleLimitC3;

    // Cost coefficents for the SortedMerge node.
    static const CostCoefficient sortedMergeInput;
    static const CostCoefficient sortedMergeOutput;

    // Cost of the different PROJECTION nodes to process one item.
    static const CostCoefficient simpleProjectionIncrement;
    static const CostCoefficient coveredProjectionIncrement;
    static const CostCoefficient defaultProjectionIncrement;

    // Cost of a SHARDING_FILTER node to process one item.
    static const CostCoefficient shardingFilterIncrement;

    // Cost of a LIMIT node to process one item.
    static const CostCoefficient limitIncrement;

    // For SKIP: use different coefficients for documents that are skipped and documents that are
    // passed to the parent stage. Skipping documents is slightly more expensive
    // than passing them to the parent stage.
    static const CostCoefficient skipIncrement;
    static const CostCoefficient passIncrement;

    // Cost of an OR node to process one item. Includes deduplication.
    static const CostCoefficient orIncrement;

    // Cost coefficents for the AndSortedNode.
    static const CostCoefficient andSortedInput;
    static const CostCoefficient andSortedOutput;

    // Virtual scan costs are not calibrated.
    static const CostCoefficient virtScanStartup;
    static const CostCoefficient virtScanIncrement;

    EstimateMap& _estimateMap;
};

}  // namespace mongo::cost_based_ranker
