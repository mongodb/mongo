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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cost_estimator.h"

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
    QSNEstimate& qsnEstimate = foundQSNEst->second;
    computeAndSetNodeCost(qsn, childCosts, childCEs, qsnEstimate);
    return qsnEstimate.cost;
}

void CostEstimator::computeAndSetNodeCost(const QuerySolutionNode* node,
                                          const std::vector<CostEstimate>& childCosts,
                                          const std::vector<CardinalityEstimate>& childCEs,
                                          QSNEstimate& qsnEst) {
    if (qsnEst.inCE && *qsnEst.inCE == zeroCE) {
        qsnEst.cost = minCost;
        return;
    }

    auto addEstimates = [](const auto& e1, const auto& e2) {
        return e1 + e2;
    };

    CostEstimate nodeCost = zeroCost;
    // Empty predicates may be represented as an empty AND.
    const MatchExpression* filter = node->filter
        ? (node->filter->matchType() == MatchExpression::AND && node->filter->numChildren() == 0)
            ? nullptr
            : node->filter.get()
        : nullptr;

    switch (node->getType()) {
        case STAGE_COLLSCAN: {
            const auto& inCE = *qsnEst.inCE;
            nodeCost += collScanStartup * oneCE + collScanIncrement * inCE;
            if (filter) {
                nodeCost += filterCost(filter, inCE);
            }
            break;
        }
        case STAGE_VIRTUAL_SCAN: {
            const auto& inCE = *qsnEst.inCE;
            nodeCost += virtScanStartup * oneCE + virtScanIncrement * inCE;
            if (filter) {
                nodeCost += filterCost(filter, inCE);
            }
            break;
        }
        case STAGE_IXSCAN: {
            const auto& inCE = *qsnEst.inCE;
            nodeCost += indexScanStartup * oneCE + indexScanIncrement * inCE;
            if (filter) {
                nodeCost += filterCost(filter, inCE);
            }
            break;
        }
        case STAGE_FETCH: {
            nodeCost = childCosts[0];
            const auto& inCE = childCEs[0];
            nodeCost += fetchStartup * oneCE + fetchIncrement * inCE;
            if (filter) {
                nodeCost += filterCost(filter, inCE);
            }
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
            // Intersects streams of sorted RIDs
            nodeCost = sortedMergeStartup * oneCE;
            nodeCost +=
                std::accumulate(childCosts.begin(), childCosts.end(), zeroCost, addEstimates);
            // The cost of comparing the RIDs
            nodeCost += sortedMergeIncrement *
                std::accumulate(childCEs.begin(), childCEs.end(), zeroCE, addEstimates);
            break;
        }
        case STAGE_OR: {
            // Union of its children. Optionally deduplicates on RecordId.
            // TODO SERVER-97507: reflect the cost of deduplication
            nodeCost =
                std::accumulate(childCosts.begin(), childCosts.end(), zeroCost, addEstimates);
            break;
        }
        case STAGE_SORT_MERGE: {
            // Merges the outputs of N children, each of which is sorted in the order specified by
            // some pattern.
            // TODO: Make the cost model more realistic
            nodeCost = sortedMergeStartup * oneCE;
            nodeCost +=
                std::accumulate(childCosts.begin(), childCosts.end(), zeroCost, addEstimates);
            nodeCost += sortedMergeIncrement *
                std::accumulate(childCEs.begin(), childCEs.end(), zeroCE, addEstimates);
            break;
        }
        case STAGE_SORT_DEFAULT:
        case STAGE_SORT_SIMPLE: {
            auto sortNode = static_cast<const SortNode*>(node);
            const auto& inCE = childCEs[0];
            double logFactor = inCE.toDouble();
            CostCoefficient incrCC = sortIncrement;
            if (sortNode->limit > 0) {
                incrCC = sortWithLimitIncrement;
                logFactor = std::min(logFactor, static_cast<double>(sortNode->limit));
            }

            nodeCost = sortStartup * oneCE + childCosts[0];
            if (logFactor > 1.0) {
                CardinalityEstimate sortSteps = inCE * std::log2(logFactor);
                nodeCost += incrCC * sortSteps;
            }
            break;
        }
        case STAGE_PROJECTION_DEFAULT:
        case STAGE_PROJECTION_COVERED:
        case STAGE_PROJECTION_SIMPLE: {
            const auto& inCE = childCEs[0];
            nodeCost = projectionStartup * oneCE + projectionIncrement * inCE + childCosts[0];
            break;
        }
        case STAGE_LIMIT: {
            nodeCost = childCosts[0];
            const auto& inCE = childCEs[0];
            auto limitNode = static_cast<const LimitNode*>(node);
            auto limitCE = CardinalityEstimate{
                CardinalityType{static_cast<double>(limitNode->limit)}, EstimationSource::Metadata};
            auto adjLimitCE = std::min(limitCE, inCE);
            nodeCost += limitStartup * oneCE + limitIncrement * adjLimitCE;
            break;
        }
        case STAGE_SKIP: {
            nodeCost = childCosts[0];
            const auto& inCE = childCEs[0];
            auto skipNode = static_cast<const SkipNode*>(node);
            auto skipCE = CardinalityEstimate{CardinalityType{static_cast<double>(skipNode->skip)},
                                              EstimationSource::Metadata};
            auto adjSkipCE = std::min(skipCE, inCE);
            auto passCE = inCE - adjSkipCE;
            // Use different coefficients for documents that are skipped and documents that are
            // passed to the parent stage. Skipping documents seems to be slightly more expensive
            // than passing them to the parent stage.
            // TODO: SERVER-101425 Calibrate cost model for SKIP.
            nodeCost += skipStartup * oneCE + skipIncrement * adjSkipCE + passIncrement * passCE;
            break;
        }
        default:
            MONGO_UNIMPLEMENTED_TASSERT(9695102);
    }

    qsnEst.cost = (nodeCost < minCost) ? minCost : nodeCost;
}

CostEstimate CostEstimator::filterCost(const MatchExpression* filter,
                                       const CardinalityEstimate& ce) const {
    if (ce == zeroCE) {
        return zeroCost;
    }

    CostEstimate res = zeroCost;
    // TODO SERVER-110320: Take into account the cost of different types of predicates.
    for (size_t i = 0; i < filter->numChildren(); i++) {
        res += filterCost(filter->getChild(i), ce);
    }
    res += filterStartup * oneCE + filterIncrement * ce;
    return res;
}


const CostCoefficient CostEstimator::defaultIncrement =
    CostCoefficient{CostCoefficientType{1000.0_ms}};

const CostCoefficient CostEstimator::filterStartup = minCC;
const CostCoefficient CostEstimator::filterIncrement = minCC;

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
    CostCoefficient{CostCoefficientType{1500.0_ms}};

const CostCoefficient CostEstimator::mergeJoinStartup =
    CostCoefficient{CostCoefficientType{1517.8_ms}};
const CostCoefficient CostEstimator::mergeJoinIncrement =
    CostCoefficient{CostCoefficientType{111.2_ms}};

// TODO SPM-3658: all constants below need calibration
const CostCoefficient CostEstimator::virtScanStartup =
    CostCoefficient{CostCoefficientType{200.0_ms}};
const CostCoefficient CostEstimator::virtScanIncrement =
    CostCoefficient{CostCoefficientType{100.3_ms}};

const CostCoefficient CostEstimator::hashJoinStartup =
    CostCoefficient{CostCoefficientType{100.0_ms}};
const CostCoefficient CostEstimator::hashJoinBuild = CostCoefficient{CostCoefficientType{100.0_ms}};
const CostCoefficient CostEstimator::hashJoinIncrement =
    CostCoefficient{CostCoefficientType{250.6_ms}};

const CostCoefficient CostEstimator::sortStartup = CostCoefficient{CostCoefficientType{100.0_ms}};
const CostCoefficient CostEstimator::sortIncrement = CostCoefficient{CostCoefficientType{210.0_ms}};
const CostCoefficient CostEstimator::sortWithLimitIncrement =
    CostCoefficient{CostCoefficientType{100.0_ms}};

const CostCoefficient CostEstimator::sortedMergeStartup =
    CostCoefficient{CostCoefficientType{100.0_ms}};
const CostCoefficient CostEstimator::sortedMergeIncrement =
    CostCoefficient{CostCoefficientType{200.0_ms}};

const CostCoefficient CostEstimator::projectionStartup =
    CostCoefficient{CostCoefficientType{1103.4_ms}};
const CostCoefficient CostEstimator::projectionIncrement =
    CostCoefficient{CostCoefficientType{430.6_ms}};

const CostCoefficient CostEstimator::limitStartup = CostCoefficient{CostCoefficientType{655.1_ms}};
const CostCoefficient CostEstimator::limitIncrement = CostCoefficient{CostCoefficientType{62.4_ms}};

const CostCoefficient CostEstimator::skipStartup = CostCoefficient{CostCoefficientType{655.1_ms}};
const CostCoefficient CostEstimator::skipIncrement = CostCoefficient{CostCoefficientType{62.4_ms}};
const CostCoefficient CostEstimator::passIncrement = CostCoefficient{CostCoefficientType{41.2_ms}};

}  // namespace mongo::cost_based_ranker
