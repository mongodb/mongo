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
        const cascades::Group& group = _memo.getGroup(groupId);
        if (!visitedGroups.insert(groupId).second) {
            const GroupIdType scanGroupId =
                properties::getPropertyConst<properties::IndexingAvailability>(
                    group._logicalProperties)
                    .getScanGroupId();
            uassert(
                6624357, "Visited the same non-scan group more than once", groupId == scanGroupId);
        }

        ABT rootNode = group._logicalNodes.getVector().back();
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
                                       NodeToGroupPropsMap& nodeToGroupPropsMap)
        : _memo(memo),
          _metadata(metadata),
          _nodeToGroupPropsMap(nodeToGroupPropsMap),
          _planNodeId(0) {}

    /**
     * Physical delegator node.
     */
    void transport(ABT& n, const MemoPhysicalDelegatorNode& node, const MemoPhysicalNodeId /*id*/) {
        n = extract(node.getNodeId());
    }

    /**
     * Other ABT types.
     */
    template <typename T, typename... Ts>
    void transport(ABT& /*n*/, const T& node, MemoPhysicalNodeId id, Ts&&...) {
        if constexpr (std::is_base_of_v<Node, T>) {
            using namespace properties;

            const cascades::Group& group = _memo.getGroup(id._groupId);
            const auto& physicalResult = group._physicalNodes.at(id._index);
            const auto& nodeInfo = *physicalResult._nodeInfo;

            LogicalProps logicalProps = group._logicalProperties;
            PhysProps physProps = physicalResult._physProps;
            if (!_metadata.isParallelExecution()) {
                // Do not display availability and requirement if under centralized setting.
                removeProperty<DistributionAvailability>(logicalProps);
                removeProperty<DistributionRequirement>(physProps);
            }

            _nodeToGroupPropsMap.emplace(&node,
                                         NodeProps{_planNodeId++,
                                                   id,
                                                   std::move(logicalProps),
                                                   std::move(physProps),
                                                   nodeInfo._cost,
                                                   nodeInfo._localCost,
                                                   nodeInfo._adjustedCE});
        }
    }

    ABT extract(const MemoPhysicalNodeId nodeId) {
        const auto& result = _memo.getGroup(nodeId._groupId)._physicalNodes.at(nodeId._index);
        uassert(6624143,
                "Physical delegator must be pointing to an optimized result.",
                result._nodeInfo.has_value());
        ABT node = result._nodeInfo->_node;

        algebra::transport<true>(node, *this, nodeId);
        return node;
    }

private:
    // We don't own this.
    const cascades::Memo& _memo;
    const Metadata& _metadata;
    NodeToGroupPropsMap& _nodeToGroupPropsMap;

    int32_t _planNodeId;
};

std::pair<ABT, NodeToGroupPropsMap> extractPhysicalPlan(const MemoPhysicalNodeId id,
                                                        const Metadata& metadata,
                                                        const cascades::Memo& memo) {
    NodeToGroupPropsMap resultMap;
    MemoPhysicalPlanExtractor extractor(memo, metadata, resultMap);
    ABT resultNode = extractor.extract(id);
    return {std::move(resultNode), std::move(resultMap)};
}

}  // namespace mongo::optimizer
