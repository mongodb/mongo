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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates_storage.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"

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

    CostEstimate filterCost(const MatchExpression* filter, const CardinalityEstimate& ce) const;

    // Cost coefficients based on Bonsai cost calibration
    static const CostCoefficient defaultIncrement;

    static const CostCoefficient filterStartup;
    static const CostCoefficient filterIncrement;

    static const CostCoefficient collScanStartup;
    static const CostCoefficient collScanIncrement;

    static const CostCoefficient virtScanStartup;
    static const CostCoefficient virtScanIncrement;

    static const CostCoefficient indexScanStartup;
    static const CostCoefficient indexScanIncrement;

    static const CostCoefficient fetchStartup;
    static const CostCoefficient fetchIncrement;

    static const CostCoefficient mergeJoinStartup;
    static const CostCoefficient mergeJoinIncrement;

    // TODO SPM-3658: all constants below need calibration
    static const CostCoefficient hashJoinStartup;
    static const CostCoefficient hashJoinBuild;
    static const CostCoefficient hashJoinIncrement;

    static const CostCoefficient sortStartup;
    static const CostCoefficient sortIncrement;
    static const CostCoefficient sortWithLimitIncrement;

    static const CostCoefficient sortedMergeStartup;
    static const CostCoefficient sortedMergeIncrement;

    static const CostCoefficient projectionStartup;
    static const CostCoefficient projectionIncrement;

    static const CostCoefficient limitStartup;
    static const CostCoefficient limitIncrement;

    static const CostCoefficient skipStartup;
    static const CostCoefficient skipIncrement;
    static const CostCoefficient passIncrement;

    EstimateMap& _estimateMap;
};

}  // namespace mongo::cost_based_ranker
