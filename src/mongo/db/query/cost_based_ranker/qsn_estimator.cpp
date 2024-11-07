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

#include "mongo/db/query/cost_based_ranker/qsn_estimator.h"
#include "mongo/db/query/cost_based_ranker/cardinality_estimator.h"

namespace mongo::cost_based_ranker {

namespace {

void estimateQsnCost(const QuerySolutionNode* node, EstimateMap& estimateMap) {
    // Dummy implementation of estimation as a starting point to test explain.
    for (auto&& child : node->children) {
        estimateQsnCost(child.get(), estimateMap);
    }

    auto res = estimateMap.find(node);
    tassert(958671, "All QSNs must have a CE.", res != estimateMap.end());
    QSNEstimate& est = res->second;
    switch (node->getType()) {
        case STAGE_IXSCAN:
            est.cost = CostEstimate(CostType{10}, EstimationSource::Code);
            break;
        case STAGE_FETCH:
            est.cost = CostEstimate(CostType{20}, EstimationSource::Code);
            break;
        case STAGE_COLLSCAN:
            est.cost = CostEstimate{CostType{50}, EstimationSource::Code};
            break;
        default:
            est.cost = CostEstimate(CostType{100}, EstimationSource::Code);
            break;
    }
}

}  // namespace

void estimatePlanCost(const QuerySolution& plan,
                      CardinalityEstimate collectionCard,
                      EstimateMap& estimateMap) {
    CardinalityEstimator cardEstimator(collectionCard, estimateMap);
    cardEstimator.estimatePlan(plan);
    estimateQsnCost(plan.root(), estimateMap);
}

}  // namespace mongo::cost_based_ranker
