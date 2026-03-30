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
        childCEs.push_back(foundChildEst->second->outCE);
    }

    auto foundQSNEst = _estimateMap.find(qsn);
    tassert(9695100, "All QSNs must have a CE.", foundQSNEst != _estimateMap.end());
    QSNEstimate& qsnEstimate = *foundQSNEst->second;
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
            auto collScanNode = static_cast<const CollectionScanNode*>(node);
            if (collScanNode->direction == 1) {
                nodeCost += collScanForwardIncrement * inCE;
            } else {
                nodeCost += collScanBackwardIncrement * inCE;
            }

            if (filter) {
                nodeCost += filterCost(node->getType(), filter, inCE);
            }
            break;
        }
        case STAGE_VIRTUAL_SCAN: {
            const auto& inCE = *qsnEst.inCE;
            nodeCost += virtScanStartup * oneCE + virtScanIncrement * inCE;
            if (filter) {
                nodeCost += filterCost(node->getType(), filter, inCE);
            }
            break;
        }
        case STAGE_IXSCAN: {
            const auto& inCE = *qsnEst.inCE;
            auto ixscanNode = static_cast<const IndexScanNode*>(node);
            const double numFields = static_cast<double>(ixscanNode->index.keyPattern.nFields());

            // The 'indexScanForwardExamineKey' cost represents the cost examining an index key with
            // one field. We need to adjust the cost of the index scan as a whole to reflect the
            // number of fields present in the index spec. The assumption is that scanning an index
            // where each key has more fields than another will be more expensive since it processes
            // more data per key. Since we already account for the cost of the first field, we
            // multiply 'incrementalFieldCost' by the number of remaining fields.
            const double numIncrementalFields = std::max(0.0, numFields - 1.0);
            const auto totalIncrementalFieldCost =
                incrementalFieldCost * (numIncrementalFields * inCE);

            // TODO SERVER-100611: Incorporate index seek cost (coefficient 'indexSeek') by adding
            // 'indexSeek * estimatedNumSeeks' to 'nodeCost'. In the meantime, we know there will
            // always be at least one seek.
            if (ixscanNode->direction == 1) {
                nodeCost += indexScanForwardExamineKey * inCE + totalIncrementalFieldCost +
                    indexForwardSeek * oneCE;
            } else {
                nodeCost += indexScanBackwardExamineKey * inCE + totalIncrementalFieldCost +
                    indexBackwardSeek * oneCE;
            }

            if (filter) {
                nodeCost += filterCost(node->getType(), filter, inCE);
            }
            break;
        }
        case STAGE_FETCH: {
            nodeCost = childCosts[0];
            const auto& inCE = childCEs[0];
            auto fetchNode = static_cast<const FetchNode*>(node);

            // TODO SERVER-101473: Remove this case from the cost model.
            if (auto isChildAlsoFetch =
                    dynamic_cast<const FetchNode*>(fetchNode->children[0].get());
                !isChildAlsoFetch) {
                nodeCost += fetchIncrement * inCE;
            }

            if (filter) {
                nodeCost += filterCost(node->getType(), filter, inCE);
            }
            break;
        }
        case STAGE_AND_HASH: {
            tassert(11028600,
                    str::stream() << "Expected AND_HASH to have 2 children, instead it had "
                                  << childCEs.size(),
                    childCEs.size() == 2);
            // The cost to read all children
            nodeCost +=
                std::accumulate(childCosts.begin(), childCosts.end(), zeroCost, addEstimates);

            const auto& numReturned = qsnEst.outCE;
            nodeCost += andHashBuild * childCEs[0] + andHashProbe * childCEs[1] +
                andHashOutput * numReturned;

            break;
        }
        case STAGE_AND_SORTED: {
            // Intersects streams of sorted RIDs
            nodeCost +=
                std::accumulate(childCosts.begin(), childCosts.end(), zeroCost, addEstimates);

            const auto& numProcessed =
                std::accumulate(childCEs.begin(), childCEs.end(), zeroCE, addEstimates);
            const auto& numReturned = qsnEst.outCE;

            nodeCost += andSortedInput * numProcessed + andSortedOutput * numReturned;
            break;
        }
        case STAGE_OR: {
            // Union of its children.
            auto orNode = static_cast<const OrNode*>(node);

            // TODO SERVER-112697: Remove these tasserts.
            tassert(11028601, "Encountered an OR stage with a filter", !orNode->filter);
            tassert(11028605, "Encountered an OR stage with dedup = false", orNode->dedup);

            nodeCost =
                std::accumulate(childCosts.begin(), childCosts.end(), zeroCost, addEstimates);

            const auto& numProcessed =
                std::accumulate(childCEs.begin(), childCEs.end(), zeroCE, addEstimates);

            nodeCost += orIncrement * numProcessed;
            break;
        }
        case STAGE_SORT_MERGE: {
            // Merges the outputs of N children, each of which is sorted in the order specified by
            // some pattern.
            nodeCost +=
                std::accumulate(childCosts.begin(), childCosts.end(), zeroCost, addEstimates);

            const auto& numProcessed =
                std::accumulate(childCEs.begin(), childCEs.end(), zeroCE, addEstimates);
            const auto& numReturned = qsnEst.outCE;

            tassert(11028602,
                    "Expected SORT_MERGE stage to have at least one child.",
                    childCEs.size() >= 1);

            // The sorted merge is implemented with the k-way merge algorithm using a priority
            // queue, which leads to a time complexity of O(n * log k) where n is the size of the
            // output array and k is the number of input arrays
            nodeCost += sortedMergeInput * numProcessed +
                sortedMergeOutput * (numReturned * std::log2(childCEs.size()));

            break;
        }
        case STAGE_SORT_DEFAULT:
        case STAGE_SORT_SIMPLE: {
            nodeCost = childCosts[0];

            // The 'numProcessedLogFactor' is used to estimate the number of sort steps. Even for
            // very small estimates make sure to count at least one sort step per input document.
            const auto& numProcessed = childCEs[0];
            double numProcessedLogFactor = numProcessed.toDouble();
            numProcessedLogFactor += 1;

            auto sortNode = static_cast<const SortNode*>(node);
            if (sortNode->limit > 0) {
                nodeCost += sortDefaultLimitStartup * oneCE;

                auto sortLimitCE =
                    CardinalityEstimate{CardinalityType{static_cast<double>(sortNode->limit)},
                                        EstimationSource::Metadata};

                // The 'numReturnedLogFactor' is used to estimate the number of sort steps. Even
                // for very small estimates make sure to count at least one sort step per input
                // document.
                const auto& numReturned = std::min(numProcessed, sortLimitCE);
                double numReturnedLogFactor = numReturned.toDouble();
                numReturnedLogFactor += 1;

                // For details on the reasoning behind this cost formula, see the comment in the
                // function 'updateCutoff' in 'sorter_template_defs.h'.
                auto sortLimitC1 = (node->getType() == STAGE_SORT_DEFAULT) ? sortDefaultLimitC1
                                                                           : sortSimpleLimitC1;
                auto sortLimitC2 = (node->getType() == STAGE_SORT_DEFAULT) ? sortDefaultLimitC2
                                                                           : sortSimpleLimitC2;
                auto sortLimitC3 = (node->getType() == STAGE_SORT_DEFAULT) ? sortDefaultLimitC3
                                                                           : sortSimpleLimitC3;

                nodeCost += sortLimitC1 * numProcessed +
                    sortLimitC2 * (numProcessed * std::log2(numReturnedLogFactor)) +
                    sortLimitC3 * (numReturned * std::log2(numReturnedLogFactor));

            } else {
                auto sortStartup = (node->getType() == STAGE_SORT_DEFAULT) ? sortDefaultStartup
                                                                           : sortSimpleStartup;
                nodeCost += sortStartup * oneCE;

                // The SORT stage might spill to disk in case it runs out of its allowed memory
                // usage, which significantly increases execution time. We estimate to spill when
                // the stage input size is greater than 'maxMemoryUsageBytes' (which is 100MB by
                // default). We don't estimate average document or key size yet, so we need to use
                // hardcoded values. Concretely, that means we currently estimate to spill for more
                // than 102400 (= 100MB/1KB) documents or 409600 (= 100MB/256) index entries.
                const auto processedItemInputSize =
                    sortNode->fetched() ? kAverageDocumentSizeBytes : kAverageIndexEntrySizeBytes;
                const bool mightRequireSpilling =
                    (processedItemInputSize * numProcessed.toDouble()) >
                    sortNode->maxMemoryUsageBytes;

                auto sortIncrement = (node->getType() == STAGE_SORT_DEFAULT)
                    ? (mightRequireSpilling ? sortDefaultSpillIncrement : sortDefaultIncrement)
                    : (mightRequireSpilling ? sortSimpleSpillIncrement : sortSimpleIncrement);

                nodeCost += sortIncrement * (std::log2(numProcessedLogFactor) * numProcessed);
            }

            break;
        }
        case STAGE_PROJECTION_DEFAULT: {
            const auto& inCE = childCEs[0];
            nodeCost = defaultProjectionIncrement * inCE + childCosts[0];
            break;
        }
        case STAGE_PROJECTION_COVERED: {
            const auto& inCE = childCEs[0];
            nodeCost = coveredProjectionIncrement * inCE + childCosts[0];
            break;
        }
        case STAGE_PROJECTION_SIMPLE: {
            const auto& inCE = childCEs[0];
            nodeCost = simpleProjectionIncrement * inCE + childCosts[0];
            break;
        }
        case STAGE_LIMIT: {
            nodeCost = childCosts[0];
            const auto& inCE = childCEs[0];
            auto limitNode = static_cast<const LimitNode*>(node);
            auto limitCE = CardinalityEstimate{
                CardinalityType{static_cast<double>(limitNode->limit)}, EstimationSource::Metadata};
            auto adjLimitCE = std::min(limitCE, inCE);
            nodeCost += limitIncrement * adjLimitCE;
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
            nodeCost += skipIncrement * adjSkipCE + passIncrement * passCE;
            break;
        }
        default:
            MONGO_UNIMPLEMENTED_TASSERT(9695102);
    }

    qsnEst.cost = (nodeCost < minCost) ? minCost : nodeCost;
}

/**
 * Returns the number of leaves in the expression tree (notably returns 1 for empty
 * expressions).
 * Note: Top-level empty filters '{}' are handled specially in 'computeAndSetNodeCost()' where they
 * are treated as if there was no filter. So in practice, this function's "returns 1 for empty"
 * behavior only affects nested empty expressions like '$elemMatch: {}'.
 */
size_t countLeaves(const MatchExpression* root) {
    if (root->numChildren() == 0) {
        // This could represent a case like '{$alwaysTrue: 1}' or '{a: {$size: 5}}' or even the
        // empty MatchExpression case: '{}' or '$elemMatch: {}'. We return 1 for empty
        // expressions since we will still do work when evaluating.
        return 1;
    }

    size_t sum = 0;
    for (size_t i = 0; i < root->numChildren(); ++i) {
        sum += countLeaves(root->getChild(i));
    }

    return sum;
}

// TODO SERVER-110320: Take into account the cost of different types of predicates.
CostEstimate CostEstimator::filterCost(StageType stage,
                                       const MatchExpression* filter,
                                       const CardinalityEstimate& ce) const {
    // The normal filter increment represents the cost of a filter with 1 leaf. For every additional
    // leaf, we charge an incremental filter leaf cost.
    const auto numFilterLeaves = countLeaves(filter);
    const auto numIncrementalFilterLeaves = numFilterLeaves - 1;
    tassert(11028603, "Expected at least 1 filter leaf", numFilterLeaves >= 1);

    CostCoefficient filterIncrement = minCC;
    CostCoefficient incrementalFilterLeafCost = minCC;

    switch (stage) {
        case STAGE_IXSCAN:
            filterIncrement = indexFilterIncrement;
            incrementalFilterLeafCost = incrementalIndexFilterLeafCost;
            break;
        case STAGE_COLLSCAN:
        case STAGE_VIRTUAL_SCAN:
            filterIncrement = collScanFilterIncrement;
            incrementalFilterLeafCost = incrementalCollScanFilterLeafCost;
            break;
        case STAGE_FETCH:
            filterIncrement = fetchFilterIncrement;
            incrementalFilterLeafCost = incrementalFetchFilterLeafCost;
            break;
        default:
            tasserted(11028604, str::stream() << "Unexpected stage with filter: " << stage);
    }

    return filterIncrement * ce + incrementalFilterLeafCost * (numIncrementalFilterLeaves * ce);
}

const CostCoefficient CostEstimator::fetchFilterIncrement = makeCostCoefficient(nsec(435.38));
const CostCoefficient CostEstimator::indexFilterIncrement = makeCostCoefficient(nsec(132.60));
const CostCoefficient CostEstimator::collScanFilterIncrement = makeCostCoefficient(nsec(202.32));

// TODO(SERVER-114546): Use the calibrated coefficients once we estimate the number of
// filter leaf evaluations.
const CostCoefficient CostEstimator::incrementalFetchFilterLeafCost = minCC;
const CostCoefficient CostEstimator::incrementalIndexFilterLeafCost = minCC;
const CostCoefficient CostEstimator::incrementalCollScanFilterLeafCost = minCC;

const CostCoefficient CostEstimator::collScanForwardIncrement = makeCostCoefficient(nsec(399.23));
const CostCoefficient CostEstimator::collScanBackwardIncrement = makeCostCoefficient(nsec(404.0));

const CostCoefficient CostEstimator::indexScanForwardExamineKey = makeCostCoefficient(nsec(435.55));
const CostCoefficient CostEstimator::indexScanBackwardExamineKey =
    makeCostCoefficient(nsec(459.68));
const CostCoefficient CostEstimator::incrementalFieldCost = makeCostCoefficient(nsec(51.25));
const CostCoefficient CostEstimator::indexForwardSeek = makeCostCoefficient(nsec(1134.71));
const CostCoefficient CostEstimator::indexBackwardSeek = makeCostCoefficient(nsec(1211.58));

const CostCoefficient CostEstimator::fetchIncrement = makeCostCoefficient(nsec(1232.20));

const CostCoefficient CostEstimator::andHashBuild = makeCostCoefficient(nsec(216.05));
const CostCoefficient CostEstimator::andHashProbe = makeCostCoefficient(nsec(180.35));
const CostCoefficient CostEstimator::andHashOutput = makeCostCoefficient(nsec(281.46));

const CostCoefficient CostEstimator::sortDefaultStartup = makeCostCoefficient(nsec(24067.01));
const CostCoefficient CostEstimator::sortDefaultIncrement = makeCostCoefficient(nsec(216.73));
const CostCoefficient CostEstimator::sortDefaultSpillIncrement = makeCostCoefficient(nsec(653.83));
const CostCoefficient CostEstimator::sortDefaultLimitStartup = makeCostCoefficient(nsec(23761.57));
const CostCoefficient CostEstimator::sortDefaultLimitC1 = makeCostCoefficient(nsec(764.58));
const CostCoefficient CostEstimator::sortDefaultLimitC2 = makeCostCoefficient(nsec(15.81));
const CostCoefficient CostEstimator::sortDefaultLimitC3 = makeCostCoefficient(nsec(233.36));

const CostCoefficient CostEstimator::sortSimpleStartup = makeCostCoefficient(nsec(22324.54));
const CostCoefficient CostEstimator::sortSimpleIncrement = makeCostCoefficient(nsec(163.56));
const CostCoefficient CostEstimator::sortSimpleSpillIncrement = makeCostCoefficient(nsec(297.03));
const CostCoefficient CostEstimator::sortSimpleLimitStartup = makeCostCoefficient(nsec(21813.35));
const CostCoefficient CostEstimator::sortSimpleLimitC1 = makeCostCoefficient(nsec(739.81));
const CostCoefficient CostEstimator::sortSimpleLimitC2 = makeCostCoefficient(nsec(11.67));
const CostCoefficient CostEstimator::sortSimpleLimitC3 = makeCostCoefficient(nsec(164.50));

const CostCoefficient CostEstimator::sortedMergeInput = makeCostCoefficient(nsec(481.48));
const CostCoefficient CostEstimator::sortedMergeOutput = makeCostCoefficient(nsec(254.03));

const CostCoefficient CostEstimator::simpleProjectionIncrement = makeCostCoefficient(nsec(317.60));
const CostCoefficient CostEstimator::coveredProjectionIncrement = makeCostCoefficient(nsec(216.77));
const CostCoefficient CostEstimator::defaultProjectionIncrement = makeCostCoefficient(nsec(768.21));

const CostCoefficient CostEstimator::limitIncrement = makeCostCoefficient(nsec(109.50));

const CostCoefficient CostEstimator::skipIncrement = makeCostCoefficient(nsec(127.41));
const CostCoefficient CostEstimator::passIncrement = makeCostCoefficient(nsec(113.94));

const CostCoefficient CostEstimator::orIncrement = makeCostCoefficient(nsec(211.52));

const CostCoefficient CostEstimator::andSortedInput = makeCostCoefficient(nsec(183.34));
const CostCoefficient CostEstimator::andSortedOutput = makeCostCoefficient(nsec(362.22));

// Virtual scan costs are not calibrated.
const CostCoefficient CostEstimator::virtScanStartup = makeCostCoefficient(nsec(200.0));
const CostCoefficient CostEstimator::virtScanIncrement = makeCostCoefficient(nsec(100.3));

}  // namespace mongo::cost_based_ranker
