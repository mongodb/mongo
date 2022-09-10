/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/cascades/physical_rewriter.h"

#include "mongo/db/query/optimizer/cascades/enforcers.h"
#include "mongo/db/query/optimizer/cascades/implementers.h"
#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/utils/rewriter_utils.h"

namespace mongo::optimizer::cascades {

using namespace properties;

/**
 * Helper class used to check if two physical property sets are compatible by testing each
 * constituent property for compatibility. This is used to check if a winner's circle entry can be
 * reused.
 */
class PropCompatibleVisitor {
public:
    bool operator()(const PhysProperty&, const CollationRequirement& requiredProp) {
        return collationsCompatible(
            getPropertyConst<CollationRequirement>(_availableProps).getCollationSpec(),
            requiredProp.getCollationSpec());
    }

    bool operator()(const PhysProperty&, const LimitSkipRequirement& requiredProp) {
        const auto& available = getPropertyConst<LimitSkipRequirement>(_availableProps);
        return available.getSkip() >= requiredProp.getSkip() &&
            available.getAbsoluteLimit() <= requiredProp.getAbsoluteLimit();
    }

    bool operator()(const PhysProperty&, const ProjectionRequirement& requiredProp) {
        const auto& availableProjections =
            getPropertyConst<ProjectionRequirement>(_availableProps).getProjections();
        // Do we have a projection superset (not necessarily strict superset)?
        for (const ProjectionName& projectionName : requiredProp.getProjections().getVector()) {
            if (!availableProjections.find(projectionName).second) {
                return false;
            }
        }
        return true;
    }

    bool operator()(const PhysProperty&, const IndexingRequirement& requiredProp) {
        const auto& available = getPropertyConst<IndexingRequirement>(_availableProps);
        return available.getIndexReqTarget() == requiredProp.getIndexReqTarget() &&
            (available.getDedupRID() || !requiredProp.getDedupRID()) &&
            available.getSatisfiedPartialIndexesGroupId() ==
            requiredProp.getSatisfiedPartialIndexesGroupId();
    }

    template <class T>
    bool operator()(const PhysProperty&, const T& requiredProp) {
        return getPropertyConst<T>(_availableProps) == requiredProp;
    }

    static bool propertiesCompatible(const PhysProps& requiredProps,
                                     const PhysProps& availableProps) {
        if (requiredProps.size() != availableProps.size()) {
            return false;
        }

        PropCompatibleVisitor visitor(availableProps);
        for (const auto& [key, prop] : requiredProps) {
            if (availableProps.find(key) == availableProps.cend() || !prop.visit(visitor)) {
                return false;
            }
        }
        return true;
    }

private:
    PropCompatibleVisitor(const PhysProps& availableProp) : _availableProps(availableProp) {}

    // We don't own this.
    const PhysProps& _availableProps;
};

PhysicalRewriter::PhysicalRewriter(Memo& memo,
                                   const GroupIdType rootGroupId,
                                   const QueryHints& hints,
                                   const RIDProjectionsMap& ridProjections,
                                   const CostingInterface& costDerivation,
                                   const PathToIntervalFn& pathToInterval,
                                   std::unique_ptr<LogicalRewriter>& logicalRewriter)
    : _memo(memo),
      _rootGroupId(rootGroupId),
      _costDerivation(costDerivation),
      _hints(hints),
      _ridProjections(ridProjections),
      _pathToInterval(pathToInterval),
      _logicalRewriter(logicalRewriter) {}

static void printCandidateInfo(const ABT& node,
                               const GroupIdType groupId,
                               const CostType nodeCost,
                               const ChildPropsType& childProps,
                               const PhysOptimizationResult& bestResult) {
    std::cout
        << "group: " << groupId << ", id: " << bestResult._index
        << ", nodeCost: " << nodeCost.toString() << ", best cost: "
        << (bestResult._nodeInfo ? bestResult._nodeInfo->_cost : CostType::kInfinity).toString()
        << "\n";
    std::cout << ExplainGenerator::explainPhysProps("Physical properties", bestResult._physProps)
              << "\n";
    std::cout << "Node: \n" << ExplainGenerator::explainV2(node) << "\n";

    for (const auto& childProp : childProps) {
        std::cout << ExplainGenerator::explainPhysProps("Child properties", childProp.second);
    }
}

void PhysicalRewriter::costAndRetainBestNode(ABT node,
                                             ChildPropsType childProps,
                                             NodeCEMap nodeCEMap,
                                             const PhysicalRewriteType rule,
                                             const GroupIdType groupId,
                                             PrefixId& prefixId,
                                             PhysOptimizationResult& bestResult) {
    const CostAndCE nodeCostAndCE =
        _costDerivation.deriveCost(_memo, bestResult._physProps, node.ref(), childProps, nodeCEMap);
    const CostType nodeCost = nodeCostAndCE._cost;
    uassert(6624056, "Must get non-infinity cost for physical node.", !nodeCost.isInfinite());

    if (_memo.getDebugInfo().hasDebugLevel(3)) {
        std::cout << "Requesting optimization\n";
        printCandidateInfo(node, groupId, nodeCost, childProps, bestResult);
    }

    const CostType childCostLimit =
        bestResult._nodeInfo ? bestResult._nodeInfo->_cost : bestResult._costLimit;
    const auto [success, cost] = optimizeChildren(nodeCost, childProps, prefixId, childCostLimit);
    const bool improvement =
        success && (!bestResult._nodeInfo || cost < bestResult._nodeInfo->_cost);

    if (_memo.getDebugInfo().hasDebugLevel(3)) {
        std::cout << (success ? (improvement ? "Improved" : "Did not improve")
                              : "Failed optimizing")
                  << "\n";
        printCandidateInfo(node, groupId, nodeCost, childProps, bestResult);
    }

    tassert(6678300,
            "Retaining node with uninitialized rewrite rule",
            rule != cascades::PhysicalRewriteType::Uninitialized);
    PhysNodeInfo candidateNodeInfo{
        unwrapConstFilter(std::move(node)), cost, nodeCost, nodeCostAndCE._ce, rule};
    const bool keepRejectedPlans = _hints._keepRejectedPlans;
    if (improvement) {
        if (keepRejectedPlans && bestResult._nodeInfo) {
            bestResult._rejectedNodeInfo.push_back(std::move(*bestResult._nodeInfo));
        }
        bestResult._nodeInfo = std::move(candidateNodeInfo);
    } else if (keepRejectedPlans) {
        bestResult._rejectedNodeInfo.push_back(std::move(candidateNodeInfo));
    }
}

/**
 * Convert nodes from logical to physical memo delegators.
 * Performs branch-and-bound search.
 */
std::pair<bool, CostType> PhysicalRewriter::optimizeChildren(const CostType nodeCost,
                                                             ChildPropsType childProps,
                                                             PrefixId& prefixId,
                                                             const CostType costLimit) {
    const bool disableBranchAndBound = _hints._disableBranchAndBound;

    CostType totalCost = nodeCost;
    if (costLimit < totalCost && !disableBranchAndBound) {
        return {false, CostType::kInfinity};
    }

    for (auto& [node, props] : childProps) {
        const GroupIdType groupId = node->cast<MemoLogicalDelegatorNode>()->getGroupId();

        const CostType childCostLimit =
            disableBranchAndBound ? CostType::kInfinity : (costLimit - totalCost);
        auto optGroupResult = optimizeGroup(groupId, std::move(props), prefixId, childCostLimit);
        if (!optGroupResult._success) {
            return {false, CostType::kInfinity};
        }

        totalCost += optGroupResult._cost;
        if (costLimit < totalCost && !disableBranchAndBound) {
            return {false, CostType::kInfinity};
        }

        ABT optimizedChild =
            make<MemoPhysicalDelegatorNode>(MemoPhysicalNodeId{groupId, optGroupResult._index});
        std::swap(*node, optimizedChild);
    }

    return {true, totalCost};
}

PhysicalRewriter::OptimizeGroupResult::OptimizeGroupResult()
    : _success(false), _index(0), _cost(CostType::kInfinity) {}

PhysicalRewriter::OptimizeGroupResult::OptimizeGroupResult(const size_t index, const CostType cost)
    : _success(true), _index(index), _cost(std::move(cost)) {
    uassert(6624347,
            "Cannot have successful optimization with infinite cost",
            _cost < CostType::kInfinity);
}

PhysicalRewriter::OptimizeGroupResult PhysicalRewriter::optimizeGroup(const GroupIdType groupId,
                                                                      PhysProps physProps,
                                                                      PrefixId& prefixId,
                                                                      CostType costLimit) {
    const size_t localPlanExplorationCount = ++_memo._stats._physPlanExplorationCount;
    if (_memo.getDebugInfo().hasDebugLevel(2)) {
        std::cout << "#" << localPlanExplorationCount << " Optimizing group " << groupId
                  << ", cost limit: " << costLimit.toString() << "\n";
        std::cout << ExplainGenerator::explainPhysProps("Physical properties", physProps) << "\n";
    }

    Group& group = _memo.getGroup(groupId);
    const LogicalProps& logicalProps = group._logicalProperties;
    if (hasProperty<IndexingAvailability>(logicalProps) &&
        !hasProperty<IndexingRequirement>(physProps)) {
        // Re-optimize under complete scan indexing requirements.
        setPropertyOverwrite(
            physProps, IndexingRequirement{IndexReqTarget::Complete, true /*dedupRID*/, groupId});
    }

    auto& physicalNodes = group._physicalNodes;
    // Establish if we have found exact match of the physical properties in the winner's circle.
    const auto [exactPropsIndex, hasExactProps] = physicalNodes.find(physProps);
    // If true, we have found compatible (but not equal) props with cost under our cost limit.
    bool hasCompatibleProps = false;

    if (hasExactProps) {
        PhysOptimizationResult& physNode = physicalNodes.at(exactPropsIndex);
        if (!physNode.isOptimized()) {
            // Currently optimizing under the same properties higher up the stack (recursive loop).
            return {};
        }
        // At this point we have an optimized entry.

        if (!physNode._nodeInfo) {
            if (physNode._costLimit < costLimit) {
                physNode.raiseCostLimit(costLimit);
                // Fall through and continue optimizing.
            } else {
                // Previously failed to optimize under less strict cost limit.
                return {};
            }
        } else if (costLimit < physNode._nodeInfo->_cost) {
            // We have a stricter limit than our previous optimization's cost.
            return {};
        } else {
            // Reuse result under identical properties.
            if (_memo.getDebugInfo().hasDebugLevel(3)) {
                std::cout << "Reusing winner's circle entry: group: " << groupId
                          << ", id: " << physNode._index
                          << ", cost: " << physNode._nodeInfo->_cost.toString()
                          << ", limit: " << costLimit.toString() << "\n";
                std::cout << "Existing props: "
                          << ExplainGenerator::explainPhysProps("existing", physNode._physProps)
                          << "\n";
                std::cout << "New props: " << ExplainGenerator::explainPhysProps("new", physProps)
                          << "\n";
                std::cout << "Reused plan: "
                          << ExplainGenerator::explainV2(physNode._nodeInfo->_node) << "\n";
            }
            return {physNode._index, physNode._nodeInfo->_cost};
        }
    } else {
        // Check winner's circle for compatible properties.
        for (const auto& physNode : physicalNodes.getNodes()) {
            _memo._stats._physMemoCheckCount++;

            if (!physNode->_nodeInfo) {
                continue;
            }
            // At this point we have an optimized entry.

            if (costLimit < physNode->_nodeInfo->_cost) {
                // Properties are not identical. Continue exploring even if limit was stricter.
                continue;
            }

            if (!PropCompatibleVisitor::propertiesCompatible(physProps, physNode->_physProps)) {
                // We are stricter that what is available.
                continue;
            }

            if (physNode->_nodeInfo->_cost < costLimit) {
                if (_memo.getDebugInfo().hasDebugLevel(3)) {
                    std::cout << "Reducing cost limit: group: " << groupId
                              << ", id: " << physNode->_index
                              << ", cost: " << physNode->_nodeInfo->_cost.toString()
                              << ", limit: " << costLimit.toString() << "\n";
                    std::cout << ExplainGenerator::explainPhysProps("Existing props",
                                                                    physNode->_physProps)
                              << "\n";
                    std::cout << ExplainGenerator::explainPhysProps("New props", physProps) << "\n";
                }

                // Reduce cost limit result under compatible properties.
                hasCompatibleProps = true;
                costLimit = physNode->_nodeInfo->_cost;
            }
        }
    }

    // If found an exact match for properties, re-use entry and continue optimizing under higher
    // cost limit. Otherwise create with a new entry for the current properties.
    PhysOptimizationResult& bestResult = hasExactProps
        ? physicalNodes.at(exactPropsIndex)
        : physicalNodes.addOptimizationResult(physProps, costLimit);

    // Enforcement rewrites run just once, and are independent of the logical nodes.
    if (groupId != _rootGroupId) {
        // Verify properties can be enforced and add enforcers if necessary.
        addEnforcers(groupId,
                     _memo.getMetadata(),
                     _ridProjections,
                     prefixId,
                     bestResult._queue,
                     bestResult._physProps,
                     logicalProps);
    }

    // Iterate until we perform all logical for the group and physical rewrites for our best plan.
    const OrderPreservingABTSet& logicalNodes = group._logicalNodes;
    while (bestResult._lastImplementedNodePos < logicalNodes.size() || !bestResult._queue.empty()) {
        if (_logicalRewriter) {
            // Attempt to perform logical rewrites.
            _logicalRewriter->rewriteGroup(groupId);
        }

        // Add rewrites to convert logical into physical nodes. Only add rewrites for newly added
        // logical nodes.
        addImplementers(_memo,
                        _hints,
                        _ridProjections,
                        prefixId,
                        bestResult,
                        logicalProps,
                        logicalNodes,
                        _pathToInterval);

        // Perform physical rewrites, use branch-and-bound.
        while (!bestResult._queue.empty()) {
            PhysRewriteEntry rewrite = std::move(*bestResult._queue.top());
            bestResult._queue.pop();

            NodeCEMap nodeCEMap = std::move(rewrite._nodeCEMap);
            if (nodeCEMap.empty()) {
                nodeCEMap.emplace(
                    rewrite._node.cast<Node>(),
                    getPropertyConst<CardinalityEstimate>(logicalProps).getEstimate());
            }

            costAndRetainBestNode(std::move(rewrite._node),
                                  std::move(rewrite._childProps),
                                  std::move(nodeCEMap),
                                  rewrite._rule,
                                  groupId,
                                  prefixId,
                                  bestResult);
        }
    }

    uassert(6624128, "Result is not optimized!", bestResult.isOptimized());
    if (!bestResult._nodeInfo) {
        uassert(6624348,
                "Must optimize successfully if found compatible properties!",
                !hasCompatibleProps);
        return {};
    }

    // We have a successful rewrite.
    if (_memo.getDebugInfo().hasDebugLevel(2)) {
        std::cout << "#" << localPlanExplorationCount << " Optimized group: " << groupId
                  << ", id: " << bestResult._index
                  << ", cost: " << bestResult._nodeInfo->_cost.toString() << "\n";
        std::cout << ExplainGenerator::explainPhysProps("Physical properties",
                                                        bestResult._physProps)
                  << "\n";
        std::cout << "Node: \n"
                  << ExplainGenerator::explainV2(
                         bestResult._nodeInfo->_node, false /*displayProperties*/, &_memo);
    }

    return {bestResult._index, bestResult._nodeInfo->_cost};
}

}  // namespace mongo::optimizer::cascades
