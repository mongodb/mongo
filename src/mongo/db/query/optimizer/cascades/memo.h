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

#include "mongo/db/query/optimizer/cascades/interfaces.h"
#include "mongo/db/query/optimizer/cascades/memo_defs.h"
#include "mongo/db/query/optimizer/cascades/memo_explain_interface.h"
#include "mongo/db/query/optimizer/cascades/memo_group_binder_interface.h"
#include "mongo/db/query/optimizer/cascades/rewrite_queues.h"


namespace mongo::optimizer::cascades {

struct PhysQueueAndImplPos {
    PhysQueueAndImplPos() : _lastImplementedNodePos(0), _queue() {}

    // Index of last logical node in our group we implemented.
    size_t _lastImplementedNodePos;

    PhysRewriteQueue _queue;
};

/**
 * List of physical nodes and associated physical properties for a given group.
 */
struct PhysNodes {
    PhysNodes() = default;

    PhysOptimizationResult& addOptimizationResult(properties::PhysProps properties,
                                                  CostType costLimit);

    const PhysOptimizationResult& at(size_t index) const;
    PhysOptimizationResult& at(size_t index);

    boost::optional<size_t> find(const properties::PhysProps& props) const;

    const PhysNodeVector& getNodes() const;

    const PhysQueueAndImplPos& getQueue(size_t index) const;
    PhysQueueAndImplPos& getQueue(size_t index);

    bool isOptimized(size_t index) const;
    void raiseCostLimit(size_t index, CostType costLimit);

private:
    PhysNodeVector _physicalNodes;

    std::vector<std::unique_ptr<PhysQueueAndImplPos>> _physicalQueues;

    struct PhysPropsHasher {
        size_t operator()(const properties::PhysProps& physProps) const;
    };

    // Used to speed up lookups into the winner's circle using physical properties.
    opt::unordered_map<properties::PhysProps, size_t, PhysPropsHasher> _physPropsToPhysNodeMap;
};

/**
 * Represents a set of equivalent query plans.  See 'class Memo' for more detail.
 */
struct Group {
    explicit Group(ProjectionNameSet projections);

    Group(const Group&) = delete;
    Group(Group&&) = default;

    // Returns the set of bindings that all plans in this group are expected to produce.
    // (Since all plans in a Group are equivalent, they all must produce the same bindings.)
    const ExpressionBinder& binder() const;

    // Contains a set of equivalent logical plans. Each element is a LogicalNode, and its immediate
    // immediate children are MemoLogicalDelegatorNode. This ensures every logical node has an
    // associated group. For example we would never have (Filter B (Filter A (Delegator _))) here
    // because 'Filter A' would have no associated group.
    OrderPreservingABTSet _logicalNodes;
    // Stores, for each logical node, the rewrite rule that first caused that node to be created.
    // '_rules[i]' corresponds to '_logicalNodes[i]'.
    // Used only for explain / debugging.
    std::vector<LogicalRewriteType> _rules;
    // Contains logical properties that are derived bottom-up from the first logical plan in the
    // group. Since all plans in the group are expected to be equivalent, the logical properties are
    // expected to be true for all plans in the group.
    properties::LogicalProps _logicalProperties;

    // Same as 'binder()'.
    ABT _binder;

    // A collection of 'LogicalRewriteEntry', indicating which rewrites we will attempt next, and at
    // which node.
    //
    // Each entry represents a specific rewrite rule, and a specific node. Typically there are many
    // entries pointing to the same node, but each for a different rewrite rule. In
    // 'LogicalRewriter::addNode', for every newly added node we schedule all possible rewrites
    // which transform it or reorder it against other nodes. The goal is to try all possible ways to
    // generate new plans using this new node.
    LogicalRewriteQueue _logicalRewriteQueue;

    // Best physical plan for given physical properties: aka "Winner's circle".
    //
    // Unlike '_logicalNodes', the immediate children of physical nodes are not required to be
    // delegator nodes. Each entry in '_physicalNode' can be a complex tree of nodes, which may or
    // may not end in 'MemoPhysicalDelegatorNode' at the leaves.
    PhysNodes _physicalNodes;
};

/**
 * A Memo holds all the alternative plans for a query, and for all of its subqueries.
 *
 * A Memo is made of 'groups': a group is a set of alternative plans that produce the same result
 * (the same bag of rows). You can think of a group as representing a question: "what is the best
 * plan for this query?". During optimization a group holds several possible answers, and at the end
 * we will choose the best answer based on cost estimates.
 *
 * The logical plans in a group are all interchangeable, since they compute the same bag. Anywhere
 * one logical plan can appear, so can an equivalent one: it doesn't change the overall result.
 * So, the logical plans in a group are all stored together in one ABTVector.
 *
 * By contrast, not all physical plans are interchangeable. For example, the MergeJoin algorithm
 * requires sorted input. So the physical plans in a group are stored separately, to answer separate
 * questions:
 * - "What is the best physical plan whose results are sorted by <x>?"
 * - "What is the best physical plan that uses an index?"
 * - "What is the best physical plan whose results are sorted by (<x>, <y>), and uses an index?"
 * - "What is the best physical plan (with no constraints)?"
 * etc. Each set of physical properties is a different optimization question. So a group has a
 * mapping from set of physical properties, to the best physical plan discovered so far that
 * produces the same logical result and satisfies those properties. For optimization we only need
 * the best plan for each set of properties, but if 'keepRejectedPlans' is enabled then we keep the
 * non-best plans for debugging.
 *
 * Typically a Memo is populated by calling 'integrate()' to add the initial logical plan, and then
 * letting rewrite rules add more plans.
 * - In the substitution phase, 'RewriteContext' uses 'Memo::clearLogicalNodes()' and
 *   'Memo::integrate()' to replace a group with a single logical node.
 * - In the exploration phase, 'RewriteContext' uses 'Memo::integrate()' to add alternative logical
 *   plans to a group.
 * - In the implementation phase, 'PhysicalRewriter' uses 'PhysNodes::addOptimizationResult()' to
 *   update the set of physical plans.
 */
class Memo : public MemoExplainInterface, public MemoGroupBinderInterface {
    // To be able to access _stats field.
    friend class PhysicalRewriter;

public:
    using GroupIdVector = std::vector<GroupIdType>;

    /**
     * This structure is essentially a parameter pack to simplify passing multiple references to
     * external objects to facilitate derivation of the memo group's logical properties.
     */
    struct Context {
        Context(const Metadata* metadata,
                const DebugInfo* debugInfo,
                const LogicalPropsInterface* logicalPropsDerivation,
                const CardinalityEstimator* cardinalityEstimator);

        // None of those should be null.
        const Metadata* _metadata;
        const DebugInfo* _debugInfo;
        const LogicalPropsInterface* _logicalPropsDerivation;
        const CardinalityEstimator* _cardinalityEstimator;
    };

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

    Memo() = default;
    Memo(const Memo& /*other*/) = default;
    Memo(Memo&& /*other*/) = default;
    Memo& operator=(const Memo& /*other*/) = delete;
    Memo& operator=(Memo&& /*other*/) = delete;

    size_t getGroupCount() const final;

    const ExpressionBinder& getBinderForGroup(GroupIdType groupId) const final;

    const properties::LogicalProps& getLogicalProps(GroupIdType groupId) const final;
    const ABTVector& getLogicalNodes(GroupIdType groupId) const final;
    const PhysNodeVector& getPhysicalNodes(GroupIdType groupId) const final;
    const std::vector<LogicalRewriteType>& getRules(GroupIdType groupId) const final;

    LogicalRewriteQueue& getLogicalRewriteQueue(GroupIdType groupId);

    ABT::reference_type getNode(MemoLogicalNodeId nodeMemoId) const;

    /**
     * Update the group's logical properties by looking at its first logical node.
     * This includes the 'CardinalityEstimate' property, which has an overall estimate
     * and a per-PartialSchemaRequirement estimate.
     */
    void estimateCE(const Context& ctx, GroupIdType groupId);

    /**
     * Takes a logical plan, and adds each Node to the appropriate group.
     *
     * Caller can use 'targetGroupMap' to force a node to go into a desired group.
     * The out-param 'insertedNodeIds' tells the caller which nodes were newly inserted.
     * Optional 'rule' is used to annotate any newly inserted nodes, for debugging.
     *
     * See 'class MemoIntegrator' for more details.
     */
    GroupIdType integrate(const Context& ctx,
                          const ABT& node,
                          NodeTargetGroupMap targetGroupMap,
                          NodeIdSet& insertedNodeIds,
                          LogicalRewriteType rule = LogicalRewriteType::Root);

    void clearLogicalNodes(GroupIdType groupId);

    const InputGroupsToNodeIdMap& getInputGroupsToNodeIdMap() const;

    void clear();

    const Stats& getStats() const;
    size_t getLogicalNodeCount() const;
    size_t getPhysicalNodeCount() const;

private:
    // MemoIntegrator is a helper / transport for 'Memo::integrate()'.
    friend class MemoIntegrator;

    /**
     * Ensures the logical node 'n' is present in some Group.
     *
     * 'groupVector' should be the set of group IDs that contain the immediate children of 'n'. This
     * is used to maintain '_inputGroupsToNodeIdMap' and '_nodeIdToInputGroupsMap'.
     *
     * 'projections' should be the set of output bindings of 'n'. It's used to initialize the
     * ProjectionAvailability property in the case where a new Group is created.
     *
     * Optional 'targetGroupId' means force the node to be added to the given group,
     * and raise an error if it's already present in some other group. '-1' means use an existing
     * group if possible or create a new one otherwise.
     *
     * 'rule' is for explain/debugging only: it identifies the rewrite that introduced the node 'n'.
     *
     * The out-param 'insertedNodeIds' is appended to if a new logical node was added to any group
     * (existing or new).
     */
    MemoLogicalNodeId addNode(const Context& ctx,
                              GroupIdVector groupVector,
                              ProjectionNameSet projections,
                              GroupIdType targetGroupId,
                              NodeIdSet& insertedNodeIds,
                              ABT n,
                              LogicalRewriteType rule);

    const Group& getGroup(GroupIdType groupId) const;
    Group& getGroup(GroupIdType groupId);

    GroupIdType addGroup(ProjectionNameSet projections);

    boost::optional<MemoLogicalNodeId> findNode(const GroupIdVector& groups, const ABT& node);

    std::vector<std::unique_ptr<Group>> _groups;

    // Used to find nodes using particular groups as inputs.
    InputGroupsToNodeIdMap _inputGroupsToNodeIdMap;

    NodeIdToInputGroupsMap _nodeIdToInputGroupsMap;

    Stats _stats;
};

}  // namespace mongo::optimizer::cascades
