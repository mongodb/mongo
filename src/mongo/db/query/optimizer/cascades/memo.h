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

#pragma once

#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"
#include "mongo/db/query/optimizer/cascades/interfaces.h"
#include "mongo/db/query/optimizer/cascades/rewrite_queues.h"
#include "mongo/db/query/optimizer/reference_tracker.h"

namespace mongo::optimizer::cascades {

struct MemoNodeRefHash {
    size_t operator()(const ABT::reference_type& nodeRef) const;
};

struct MemoNodeRefCompare {
    bool operator()(const ABT::reference_type& left, const ABT::reference_type& right) const;
};

class OrderPreservingABTSet {
public:
    OrderPreservingABTSet() = default;
    OrderPreservingABTSet(const OrderPreservingABTSet&) = delete;
    OrderPreservingABTSet(OrderPreservingABTSet&&) = default;

    ABT::reference_type at(size_t index) const;
    std::pair<size_t, bool> emplace_back(ABT node);
    std::pair<size_t, bool> find(ABT::reference_type node) const;

    void clear();

    size_t size() const;
    const ABTVector& getVector() const;

private:
    opt::unordered_map<ABT::reference_type, size_t, MemoNodeRefHash, MemoNodeRefCompare> _map;
    ABTVector _vector;
};

struct PhysNodeInfo {
    ABT _node;

    // Total cost for the entire subtree.
    CostType _cost;

    // Operator cost (without including the subtree).
    CostType _localCost;

    // For display purposes, adjusted cardinality based on physical properties (e.g. Repetition and
    // Limit-Skip).
    CEType _adjustedCE;
};

struct PhysOptimizationResult {
    PhysOptimizationResult();
    PhysOptimizationResult(size_t index, properties::PhysProps physProps, CostType costLimit);

    bool isOptimized() const;
    void raiseCostLimit(CostType costLimit);

    const size_t _index;
    const properties::PhysProps _physProps;

    CostType _costLimit;
    // If set, we have successfully optimized.
    boost::optional<PhysNodeInfo> _nodeInfo;
    // Rejected physical plans.
    std::vector<PhysNodeInfo> _rejectedNodeInfo;

    // Index of last logical node in our group we implemented.
    size_t _lastImplementedNodePos;

    PhysRewriteQueue _queue;
};

struct PhysNodes {
    using PhysNodeVector = std::vector<std::unique_ptr<PhysOptimizationResult>>;

    PhysNodes() = default;

    PhysOptimizationResult& addOptimizationResult(properties::PhysProps properties,
                                                  CostType costLimit);

    const PhysOptimizationResult& at(size_t index) const;
    PhysOptimizationResult& at(size_t index);

    std::pair<size_t, bool> find(const properties::PhysProps& props) const;

    const PhysNodeVector& getNodes() const;

private:
    PhysNodeVector _physicalNodes;

    struct PhysPropsHasher {
        size_t operator()(const properties::PhysProps& physProps) const;
    };

    // Used to speed up lookups into the winner's circle using physical properties.
    opt::unordered_map<properties::PhysProps, size_t, PhysPropsHasher> _physPropsToPhysNodeMap;
};

struct Group {
    explicit Group(ProjectionNameSet projections);

    Group(const Group&) = delete;
    Group(Group&&) = default;

    const ExpressionBinder& binder() const;

    // Associated logical nodes.
    OrderPreservingABTSet _logicalNodes;
    // Group logical properties.
    properties::LogicalProps _logicalProperties;
    ABT _binder;

    LogicalRewriteQueue _logicalRewriteQueue;

    // Best physical plan for given physical properties: aka "Winner's circle".
    PhysNodes _physicalNodes;
};

class Memo {
    friend class PhysicalRewriter;

public:
    using GroupIdVector = std::vector<GroupIdType>;

    struct Stats {
        // Number of calls to integrate()
        size_t _numIntegrations = 0;
        // Number of recursive physical optimization calls.
        size_t _physPlanExplorationCount = 0;
        // Number of checks to winner's circle.
        size_t _physMemoCheckCount = 0;
    };

    struct GroupIdVectorHash {
        size_t operator()(const GroupIdVector& v) const;
    };
    using InputGroupsToNodeIdMap = opt::unordered_map<GroupIdVector, NodeIdSet, GroupIdVectorHash>;

    /**
     * Inverse map.
     */
    using NodeIdToInputGroupsMap = opt::unordered_map<MemoLogicalNodeId, GroupIdVector, NodeIdHash>;

    struct NodeTargetGroupHash {
        size_t operator()(const ABT::reference_type& nodeRef) const;
    };
    using NodeTargetGroupMap =
        opt::unordered_map<ABT::reference_type, GroupIdType, NodeTargetGroupHash>;

    Memo(DebugInfo debugInfo,
         const Metadata& metadata,
         std::unique_ptr<LogicalPropsInterface> logicalPropsDerivation,
         std::unique_ptr<CEInterface> ceDerivation);

    const Group& getGroup(GroupIdType groupId) const;
    Group& getGroup(GroupIdType groupId);
    size_t getGroupCount() const;

    std::pair<size_t, bool> findNodeInGroup(GroupIdType groupId, ABT::reference_type node) const;

    ABT::reference_type getNode(MemoLogicalNodeId nodeMemoId) const;

    void estimateCE(GroupIdType groupId, bool useHeuristicCE);

    MemoLogicalNodeId addNode(GroupIdVector groupVector,
                              ProjectionNameSet projections,
                              GroupIdType targetGroupId,
                              NodeIdSet& insertedNodeIds,
                              ABT n,
                              bool useHeuristicCE = false);

    GroupIdType integrate(const ABT& node,
                          NodeTargetGroupMap targetGroupMap,
                          NodeIdSet& insertedNodeIds,
                          bool addExistingNodeWithNewChild = false,
                          bool useHeuristicCE = false);

    void clearLogicalNodes(GroupIdType groupId);

    const InputGroupsToNodeIdMap& getInputGroupsToNodeIdMap() const;

    const DebugInfo& getDebugInfo() const;

    void clear();

    const Stats& getStats() const;
    size_t getLogicalNodeCount() const;
    size_t getPhysicalNodeCount() const;

    const Metadata& getMetadata() const;
    const CEInterface& getCEDerivation() const;

private:
    GroupIdType addGroup(ProjectionNameSet projections);

    std::pair<MemoLogicalNodeId, bool> addNode(GroupIdType groupId, ABT n);

    std::pair<MemoLogicalNodeId, bool> findNode(const GroupIdVector& groups, const ABT& node);

    std::vector<std::unique_ptr<Group>> _groups;

    // Used to find nodes using particular groups as inputs.
    InputGroupsToNodeIdMap _inputGroupsToNodeIdMap;

    NodeIdToInputGroupsMap _nodeIdToInputGroupsMap;

    const Metadata& _metadata;
    std::unique_ptr<LogicalPropsInterface> _logicalPropsDerivation;
    std::unique_ptr<CEInterface> _ceDerivation;
    // Used during the substitution phase, where we don't need to call _ceDerivation.
    HeuristicCE _heuristicCE;

    const DebugInfo _debugInfo;

    Stats _stats;
};

}  // namespace mongo::optimizer::cascades
