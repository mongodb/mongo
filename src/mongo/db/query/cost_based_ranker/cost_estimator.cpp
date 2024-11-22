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

#include "mongo/db/query/cost_based_ranker/cost_estimator.h"

namespace mongo::cost_based_ranker {

CostEstimate CostEstimator::costTree(const QuerySolutionNode* qsn) {
    std::vector<CostEstimate> childCosts;
    std::vector<CardinalityEstimate> childCEs;

    for (auto&& child : qsn->children) {
        auto childCost = costTree(child.get());
        childCosts.push_back(childCost);
        auto foundChildEst = _estimateMap.find(child.get());
        tassert(9695101, "All QSNs must have a CE.", foundChildEst != _estimateMap.end());
        childCEs.push_back(foundChildEst->second.outCE);
    }

    auto foundQSNEst = _estimateMap.find(qsn);
    tassert(9695100, "All QSNs must have a CE.", foundQSNEst != _estimateMap.end());
    QSNEstimate& qsnEst = foundQSNEst->second;
    qsnEst.cost = costNode(qsn, qsnEst.outCE, childCosts, childCEs);
    return qsnEst.cost;
}

CostEstimate CostEstimator::costNode(const QuerySolutionNode* node,
                                     const CardinalityEstimate& ce,
                                     const std::vector<CostEstimate>& childCosts,
                                     const std::vector<CardinalityEstimate>& childCEs) {
    if (ce == zeroCE) {
        return minCost;
    }

    auto addEstimates = [](const auto& e1, const auto& e2) {
        return e1 + e2;
    };

    CostEstimate nodeCost = zeroCost;
    switch (node->getType()) {
        case STAGE_COLLSCAN: {
            nodeCost += collScanStartup * oneCE + collScanIncrement * ce;
            break;
        }
        case STAGE_VIRTUAL_SCAN: {
            nodeCost += virtScanStartup * oneCE + virtScanIncrement * ce;
            break;
        }
        case STAGE_IXSCAN: {
            nodeCost += indexScanStartup * oneCE + indexScanIncrement * ce;
            break;
        }
        case STAGE_FETCH: {
            nodeCost = childCosts[0];
            nodeCost += fetchStartup * oneCE + fetchIncrement * ce;
            break;
        }
        case STAGE_AND_HASH: {
            nodeCost = hashJoinStartup * oneCE;
            // The cost to read all children
            nodeCost +=
                std::accumulate(childCosts.begin(), childCosts.end(), zeroCost, addEstimates);
            // Take into account the cost to hash the first child (the build collection)
            nodeCost += childCEs[0] * hashJoinBuild;
            // The cost of probing all remaining children
            nodeCost += hashJoinIncrement *
                std::accumulate(childCEs.begin() + 1, childCEs.end(), zeroCE, addEstimates);
            break;
        }
        case STAGE_AND_SORTED: {
            nodeCost = sortedMergeStartup * oneCE;
            nodeCost +=
                std::accumulate(childCosts.begin(), childCosts.end(), zeroCost, addEstimates);
            // The cost of comparing the record IDs
            nodeCost += sortedMergeIncrement *
                std::accumulate(childCEs.begin(), childCEs.end(), zeroCE, addEstimates);
            break;
        }
        case STAGE_OR: {
            // TODO SERVER-97507: reflect the cost of deduplication
            nodeCost =
                std::accumulate(childCosts.begin(), childCosts.end(), zeroCost, addEstimates);
            break;
        }
        case STAGE_SORT_MERGE: {
            // TODO: Make the cost model more realistic
            nodeCost = sortedMergeStartup * oneCE;
            nodeCost = sortedMergeIncrement *
                std::accumulate(childCEs.begin(), childCEs.end(), zeroCE, addEstimates);
            break;
        }
        case STAGE_SORT_DEFAULT:
        case STAGE_SORT_SIMPLE: {
            auto sortNode = static_cast<const SortNode*>(node);
            double logFactor = ce.toDouble();
            CostCoefficient incrCC = sortIncrement;
            if (sortNode->limit > 0) {
                incrCC = sortWithLimitIncrement;
                logFactor = std::min(logFactor, static_cast<double>(sortNode->limit));
            }

            nodeCost = sortStartup * oneCE + childCosts[0];
            if (logFactor > 1.0) {
                CardinalityEstimate sortSteps = ce * std::log2(logFactor);
                nodeCost += incrCC * sortSteps;
            }
            break;
        }
        default:
            MONGO_UNIMPLEMENTED_TASSERT(9695102);
    }
    if (node->filter) {
        // TODO: Take into account the cost of individual filter nodes
        // TODO: Use the ce of the qsn output instead of its input estimate
        nodeCost += filterStartup * oneCE + filterIncrement * ce;
    }

    return (nodeCost < minCost) ? minCost : nodeCost;
}

const CostCoefficient CostEstimator::defaultIncrement =
    CostCoefficient{CostCoefficientType{1000.0_ms}};

const CostCoefficient CostEstimator::filterStartup =
    CostCoefficient{CostCoefficientType{1461.3_ms}};
const CostCoefficient CostEstimator::filterIncrement =
    CostCoefficient{CostCoefficientType{83.7_ms}};

const CostCoefficient CostEstimator::collScanStartup =
    CostCoefficient{CostCoefficientType{6175.5_ms}};
const CostCoefficient CostEstimator::collScanIncrement =
    CostCoefficient{CostCoefficientType{422.3_ms}};

const CostCoefficient CostEstimator::indexScanStartup =
    CostCoefficient{CostCoefficientType{14055.0_ms}};
const CostCoefficient CostEstimator::indexScanIncrement =
    CostCoefficient{CostCoefficientType{403.7_ms}};

const CostCoefficient CostEstimator::fetchStartup = CostCoefficient{CostCoefficientType{7488.7_ms}};
const CostCoefficient CostEstimator::fetchIncrement =
    CostCoefficient{CostCoefficientType{1174.8_ms}};

const CostCoefficient CostEstimator::mergeJoinStartup =
    CostCoefficient{CostCoefficientType{1517.8_ms}};
const CostCoefficient CostEstimator::mergeJoinIncrement =
    CostCoefficient{CostCoefficientType{111.2_ms}};

// TODO SPM-3658: all constants below need calibration
const CostCoefficient CostEstimator::virtScanStartup =
    CostCoefficient{CostCoefficientType{200.0_ms}};
const CostCoefficient CostEstimator::virtScanIncrement =
    CostCoefficient{CostCoefficientType{100.3_ms}};

const CostCoefficient CostEstimator::hashJoinStartup = minCC;
const CostCoefficient CostEstimator::hashJoinBuild = CostCoefficient{CostCoefficientType{100.0_ms}};
const CostCoefficient CostEstimator::hashJoinIncrement =
    CostCoefficient{CostCoefficientType{250.6_ms}};

const CostCoefficient CostEstimator::sortStartup = minCC;
const CostCoefficient CostEstimator::sortIncrement =
    CostCoefficient{CostCoefficientType{2500.0_ms}};
const CostCoefficient CostEstimator::sortWithLimitIncrement =
    CostCoefficient{CostCoefficientType{1000.0_ms}};  // TODO: not yet calibrated

const CostCoefficient CostEstimator::sortedMergeStartup = minCC;
const CostCoefficient CostEstimator::sortedMergeIncrement =
    CostCoefficient{CostCoefficientType{100.0_ms}};

}  // namespace mongo::cost_based_ranker
