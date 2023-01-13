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

#include "mongo/db/query/optimizer/utils/memo_utils.h"

#include "mongo/db/query/optimizer/cascades/memo.h"


namespace mongo::optimizer {
using namespace properties;

class MemoLatestPlanExtractor {
public:
    explicit MemoLatestPlanExtractor(const cascades::Memo& memo) : _memo(memo) {}

    /**
     * Logical delegator node.
     */
    void transport(ABT& n,
                   const MemoLogicalDelegatorNode& node,
                   opt::unordered_set<GroupIdType>& visitedGroups) {
        n = extractLatest(node.getGroupId(), visitedGroups);
    }

    /**
     * Other ABT types.
     */
    template <typename T, typename... Ts>
    void transport(ABT& /*n*/,
                   const T& /*node*/,
                   opt::unordered_set<GroupIdType>& visitedGroups,
                   Ts&&...) {
        // noop
    }

    ABT extractLatest(const GroupIdType groupId, opt::unordered_set<GroupIdType>& visitedGroups) {
        if (!visitedGroups.insert(groupId).second) {
            const GroupIdType scanGroupId =
                getPropertyConst<IndexingAvailability>(_memo.getLogicalProps(groupId))
                    .getScanGroupId();
            uassert(
                6624357, "Visited the same non-scan group more than once", groupId == scanGroupId);
        }

        ABT rootNode = _memo.getLogicalNodes(groupId).back();
        algebra::transport<true>(rootNode, *this, visitedGroups);
        return rootNode;
    }

private:
    const cascades::Memo& _memo;
};

ABT extractLatestPlan(const cascades::Memo& memo, const GroupIdType rootGroupId) {
    MemoLatestPlanExtractor extractor(memo);
    opt::unordered_set<GroupIdType> visitedGroups;
    return extractor.extractLatest(rootGroupId, visitedGroups);
}

class MemoPhysicalPlanExtractor {
public:
    explicit MemoPhysicalPlanExtractor(const cascades::Memo& memo,
                                       const Metadata& metadata,
                                       const RIDProjectionsMap& ridProjections,
                                       NodeToGroupPropsMap& nodeToGroupPropsMap)
        : _memo(memo),
          _metadata(metadata),
          _ridProjections(ridProjections),
          _nodeToGroupPropsMap(nodeToGroupPropsMap),
          _planNodeId(0) {}

    /**
     * Physical delegator node.
     */
    void operator()(ABT& n,
                    MemoPhysicalDelegatorNode& node,
                    MemoPhysicalNodeId /*id*/,
                    ProjectionNameOrderPreservingSet /*required*/) {
        n = extract(node.getNodeId());
    }

    void addNodeProps(const Node* node,
                      MemoPhysicalNodeId id,
                      ProjectionNameOrderPreservingSet required) {
        const auto& physicalResult = *_memo.getPhysicalNodes(id._groupId).at(id._index);
        const auto& nodeInfo = *physicalResult._nodeInfo;

        LogicalProps logicalProps = _memo.getLogicalProps(id._groupId);
        PhysProps physProps = physicalResult._physProps;
        if (!_metadata.isParallelExecution()) {
            // Do not display availability and requirement if under centralized setting.
            removeProperty<DistributionAvailability>(logicalProps);
            removeProperty<DistributionRequirement>(physProps);
        }
        setPropertyOverwrite(physProps, ProjectionRequirement{std::move(required)});

        boost::optional<ProjectionName> ridProjName;
        if (hasProperty<IndexingRequirement>(physProps)) {
            const auto& scanDefName =
                getPropertyConst<IndexingAvailability>(logicalProps).getScanDefName();
            ridProjName = _ridProjections.at(scanDefName);
        }

        _nodeToGroupPropsMap.emplace(node,
                                     NodeProps{_planNodeId++,
                                               id,
                                               std::move(logicalProps),
                                               std::move(physProps),
                                               std::move(ridProjName),
                                               nodeInfo._cost,
                                               nodeInfo._localCost,
                                               nodeInfo._adjustedCE});
    }

    void operator()(ABT& n,
                    NestedLoopJoinNode& node,
                    MemoPhysicalNodeId id,
                    ProjectionNameOrderPreservingSet required) {
        addNodeProps(&node, id, required);

        // Obtain correlated projections from the left child, non-correlated from the right child.
        ProjectionNameOrderPreservingSet requiredInner = required;
        ProjectionNameOrderPreservingSet requiredOuter;
        for (const ProjectionName& projectionName : node.getCorrelatedProjectionNames()) {
            requiredInner.erase(projectionName);
            if (required.find(projectionName)) {
                requiredOuter.emplace_back(projectionName);
            }
        }

        node.getLeftChild().visit(*this, id, std::move(requiredOuter));
        node.getRightChild().visit(*this, id, std::move(requiredInner));
    }

    void operator()(ABT& n,
                    GroupByNode& node,
                    MemoPhysicalNodeId id,
                    ProjectionNameOrderPreservingSet required) {
        addNodeProps(&node, id, required);

        // Propagate the input projections only.
        for (const auto& projName : node.getAggregationProjectionNames()) {
            required.erase(projName);
        }
        for (const auto& expr : node.getAggregationExpressions()) {
            if (const auto fnPtr = expr.cast<FunctionCall>();
                fnPtr != nullptr && fnPtr->nodes().size() == 1) {
                if (const auto varPtr = fnPtr->nodes().front().cast<Variable>()) {
                    required.emplace_back(varPtr->name());
                }
            }
        }

        node.getChild().visit(*this, id, std::move(required));
    }

    template <class T>
    void operator()(ABT& n,
                    T& node,
                    MemoPhysicalNodeId id,
                    ProjectionNameOrderPreservingSet required) {
        using namespace algebra::detail;

        if constexpr (is_one_of_v<T,
                                  PhysicalScanNode,
                                  ValueScanNode,
                                  CoScanNode,
                                  IndexScanNode,
                                  SeekNode,
                                  SpoolConsumerNode>) {
            // Nullary nodes.
            addNodeProps(&node, id, std::move(required));
        } else if constexpr (is_one_of_v<T,
                                         FilterNode,
                                         EvaluationNode,
                                         UnwindNode,
                                         UniqueNode,
                                         SpoolProducerNode,
                                         CollationNode,
                                         LimitSkipNode,
                                         ExchangeNode,
                                         RootNode>) {
            // Unary nodes.
            addNodeProps(&node, id, required);
            node.getChild().visit(*this, id, std::move(required));
        } else if constexpr (is_one_of_v<T, SortedMergeNode, UnionNode>) {
            // N-ary nodes.
            addNodeProps(&node, id, required);
            for (auto& child : node.nodes()) {
                child.visit(*this, id, required);
            }
        } else if constexpr (is_one_of_v<T, MergeJoinNode, HashJoinNode>) {
            // HashJoin and MergeJoin.
            addNodeProps(&node, id, required);

            // Do not require RID from the inner child.
            auto requiredInner = required;
            if (const auto& ridProjName = _nodeToGroupPropsMap.at(&node)._ridProjName) {
                requiredInner.erase(*ridProjName);
            }

            node.getLeftChild().visit(*this, id, std::move(required));
            node.getRightChild().visit(*this, id, std::move(requiredInner));
        } else {
            // Other ABT types.
            static_assert(!canBePhysicalNode<T>(), "Physical node must implement its visitor");
        }
    }

    ABT extract(MemoPhysicalNodeId nodeId) {
        const auto& result = *_memo.getPhysicalNodes(nodeId._groupId).at(nodeId._index);
        uassert(6624143,
                "Physical delegator must be pointing to an optimized result.",
                result._nodeInfo.has_value());
        ABT node = result._nodeInfo->_node;

        if (hasProperty<ProjectionRequirement>(result._physProps)) {
            node.visit(*this,
                       nodeId,
                       getPropertyConst<ProjectionRequirement>(result._physProps).getProjections());
        } else {
            node.visit(*this, nodeId, ProjectionNameOrderPreservingSet{});
        }
        return node;
    }

private:
    // We don't own this.
    const cascades::Memo& _memo;
    const Metadata& _metadata;
    const RIDProjectionsMap& _ridProjections;
    NodeToGroupPropsMap& _nodeToGroupPropsMap;

    int32_t _planNodeId;
};

std::pair<ABT, NodeToGroupPropsMap> extractPhysicalPlan(MemoPhysicalNodeId id,
                                                        const Metadata& metadata,
                                                        const RIDProjectionsMap& ridProjections,
                                                        const cascades::Memo& memo) {
    NodeToGroupPropsMap resultMap;
    MemoPhysicalPlanExtractor extractor(memo, metadata, ridProjections, resultMap);
    ABT resultNode = extractor.extract(id);
    return {std::move(resultNode), std::move(resultMap)};
}

}  // namespace mongo::optimizer
