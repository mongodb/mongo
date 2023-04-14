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

#include "mongo/db/query/optimizer/cascades/memo.h"

#include <set>

#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/utils/abt_hash.h"
#include "mongo/db/query/optimizer/utils/utils.h"


namespace mongo::optimizer::cascades {

PhysOptimizationResult& PhysNodes::addOptimizationResult(properties::PhysProps properties,
                                                         CostType costLimit) {
    const size_t index = _physicalNodes.size();
    _physPropsToPhysNodeMap.emplace(properties, index);
    _physicalQueues.emplace_back(std::make_unique<PhysQueueAndImplPos>());
    return *_physicalNodes.emplace_back(std::make_unique<PhysOptimizationResult>(
        index, std::move(properties), std::move(costLimit)));
}

const PhysOptimizationResult& PhysNodes::at(const size_t index) const {
    return *_physicalNodes.at(index);
}

PhysOptimizationResult& PhysNodes::at(const size_t index) {
    return *_physicalNodes.at(index);
}

boost::optional<size_t> PhysNodes::find(const properties::PhysProps& props) const {
    auto it = _physPropsToPhysNodeMap.find(props);
    if (it == _physPropsToPhysNodeMap.cend()) {
        return boost::none;
    }
    return it->second;
}

const PhysNodeVector& PhysNodes::getNodes() const {
    return _physicalNodes;
}

const PhysQueueAndImplPos& PhysNodes::getQueue(size_t index) const {
    return *_physicalQueues.at(index);
}

PhysQueueAndImplPos& PhysNodes::getQueue(const size_t index) {
    return *_physicalQueues.at(index);
}

bool PhysNodes::isOptimized(const size_t index) const {
    return getQueue(index)._queue.empty();
}

void PhysNodes::raiseCostLimit(const size_t index, CostType costLimit) {
    at(index)._costLimit = costLimit;
    // Allow for re-optimization under the higher cost limit.
    getQueue(index)._lastImplementedNodePos = 0;
}

size_t PhysNodes::PhysPropsHasher::operator()(const properties::PhysProps& physProps) const {
    return ABTHashGenerator::generateForPhysProps(physProps);
}

static ABT createBinderMap(const properties::LogicalProps& logicalProperties) {
    ProjectionNameVector projectionVector;
    ABTVector expressions;

    const auto& projSet =
        properties::getPropertyConst<properties::ProjectionAvailability>(logicalProperties)
            .getProjections();
    ProjectionNameOrderedSet ordered{projSet.cbegin(), projSet.cend()};

    for (const ProjectionName& projection : ordered) {
        projectionVector.push_back(projection);
        expressions.emplace_back(make<Source>());
    }

    return make<ExpressionBinder>(std::move(projectionVector), std::move(expressions));
}

Group::Group(ProjectionNameSet projections)
    : _logicalNodes(),
      _logicalProperties(
          properties::makeLogicalProps(properties::ProjectionAvailability(std::move(projections)))),
      _binder(createBinderMap(_logicalProperties)),
      _logicalRewriteQueue(),
      _physicalNodes() {}

const ExpressionBinder& Group::binder() const {
    auto pointer = _binder.cast<ExpressionBinder>();
    uassert(6624048, "Invalid binder type", pointer);

    return *pointer;
}

/**
 * MemoIntegrator adds a logical plan to a Memo, by putting each node (stage) in the appropriate
 * group, and returning the group ID where the root node can be found.
 *
 * It works recursively: to add a node to a Memo, first add each child, and replace each child with
 * a MemoLogicalDelegator that refers to the group where that child was put.
 *
 * After stubbing out the children it relies on 'Memo::addNode' to ensure that if a syntactically
 * equal node is already in some group, we reuse it.
 */
class MemoIntegrator {
public:
    explicit MemoIntegrator(Memo::Context ctx,
                            Memo& memo,
                            Memo::NodeTargetGroupMap targetGroupMap,
                            NodeIdSet& insertedNodeIds,
                            const LogicalRewriteType rule)
        : _ctx(std::move(ctx)),
          _memo(memo),
          _insertedNodeIds(insertedNodeIds),
          _targetGroupMap(std::move(targetGroupMap)),
          _rule(rule) {}

    // This is a transient structure. We do not allow copying or moving.
    MemoIntegrator(const MemoIntegrator& /*other*/) = delete;
    MemoIntegrator(MemoIntegrator&& /*other*/) = delete;
    MemoIntegrator& operator=(const MemoIntegrator& /*other*/) = delete;
    MemoIntegrator& operator=(MemoIntegrator&& /*other*/) = delete;

    /**
     * Nodes
     */

    GroupIdType transport(const ABT& n,
                          const ScanNode& node,
                          const VariableEnvironment& env,
                          GroupIdType /*binder*/) {
        return addNodes(n, node, n, env, {});
    }

    GroupIdType transport(const ABT& n,
                          const ValueScanNode& node,
                          const VariableEnvironment& env,
                          GroupIdType /*binder*/) {
        return addNodes(n, node, n, env, {});
    }

    GroupIdType transport(const ABT& n,
                          const MemoLogicalDelegatorNode& node,
                          const VariableEnvironment& env) {
        if (_targetGroupMap.count(n.ref()) == 0) {
            return node.getGroupId();
        }

        return addNodes(n, node, n, env, {});
    }

    GroupIdType transport(const ABT& n,
                          const FilterNode& node,
                          const VariableEnvironment& env,
                          GroupIdType child,
                          GroupIdType /*binder*/) {
        return addNode(n, node, env, child);
    }

    GroupIdType transport(const ABT& n,
                          const EvaluationNode& node,
                          const VariableEnvironment& env,
                          GroupIdType child,
                          GroupIdType /*binder*/) {
        return addNode(n, node, env, child);
    }

    GroupIdType transport(const ABT& n,
                          const SargableNode& node,
                          const VariableEnvironment& env,
                          GroupIdType child,
                          GroupIdType /*binder*/,
                          GroupIdType /*references*/) {
        return addNode(n, node, env, child);
    }

    GroupIdType transport(const ABT& n,
                          const RIDIntersectNode& node,
                          const VariableEnvironment& env,
                          GroupIdType leftChild,
                          GroupIdType rightChild) {
        return addNodes(n, node, env, leftChild, rightChild);
    }

    GroupIdType transport(const ABT& n,
                          const RIDUnionNode& node,
                          const VariableEnvironment& env,
                          GroupIdType leftChild,
                          GroupIdType rightChild) {
        return addNodes(n, node, env, leftChild, rightChild);
    }

    GroupIdType transport(const ABT& n,
                          const BinaryJoinNode& node,
                          const VariableEnvironment& env,
                          GroupIdType leftChild,
                          GroupIdType rightChild,
                          GroupIdType /*filter*/) {
        return addNodes(n, node, env, leftChild, rightChild);
    }

    GroupIdType transport(const ABT& n,
                          const UnionNode& node,
                          const VariableEnvironment& env,
                          Memo::GroupIdVector children,
                          GroupIdType /*binder*/,
                          GroupIdType /*refs*/) {
        return addNodes(n, node, env, std::move(children));
    }

    GroupIdType transport(const ABT& n,
                          const GroupByNode& node,
                          const VariableEnvironment& env,
                          GroupIdType child,
                          GroupIdType /*binderAgg*/,
                          GroupIdType /*refsAgg*/,
                          GroupIdType /*binderGb*/,
                          GroupIdType /*refsGb*/) {
        return addNode(n, node, env, child);
    }

    GroupIdType transport(const ABT& n,
                          const UnwindNode& node,
                          const VariableEnvironment& env,
                          GroupIdType child,
                          GroupIdType /*binder*/,
                          GroupIdType /*refs*/) {
        return addNode(n, node, env, child);
    }

    GroupIdType transport(const ABT& n,
                          const CollationNode& node,
                          const VariableEnvironment& env,
                          GroupIdType child,
                          GroupIdType /*refs*/) {
        return addNode(n, node, env, child);
    }

    GroupIdType transport(const ABT& n,
                          const LimitSkipNode& node,
                          const VariableEnvironment& env,
                          GroupIdType child) {
        return addNode(n, node, env, child);
    }

    GroupIdType transport(const ABT& n,
                          const ExchangeNode& node,
                          const VariableEnvironment& env,
                          GroupIdType child,
                          GroupIdType /*refs*/) {
        return addNode(n, node, env, child);
    }

    GroupIdType transport(const ABT& n,
                          const RootNode& node,
                          const VariableEnvironment& env,
                          GroupIdType child,
                          GroupIdType /*refs*/) {
        return addNode(n, node, env, child);
    }

    /**
     * Other ABT types.
     */
    template <typename T, typename... Ts>
    GroupIdType transport(const ABT& /*n*/,
                          const T& /*node*/,
                          const VariableEnvironment& /*env*/,
                          Ts&&...) {
        static_assert(!canBeLogicalNode<T>(), "Logical node must implement its transport.");
        return -1;
    }

    GroupIdType integrate(const ABT& n) {
        return algebra::transport<true>(n, *this, VariableEnvironment::build(n, &_memo));
    }

private:
    GroupIdType addNodes(const ABT& n,
                         const Node& node,
                         ABT forMemo,
                         const VariableEnvironment& env,
                         Memo::GroupIdVector childGroupIds) {
        auto it = _targetGroupMap.find(n.ref());
        const GroupIdType targetGroupId = (it == _targetGroupMap.cend()) ? -1 : it->second;
        const auto result = _memo.addNode(_ctx,
                                          std::move(childGroupIds),
                                          env.getProjections(node),
                                          targetGroupId,
                                          _insertedNodeIds,
                                          std::move(forMemo),
                                          _rule);
        return result._groupId;
    }

    template <class T, typename... Args>
    GroupIdType addNodes(const ABT& n,
                         const T& node,
                         const VariableEnvironment& env,
                         Memo::GroupIdVector childGroupIds) {
        ABT forMemo = n;
        auto& childNodes = forMemo.template cast<T>()->nodes();
        for (size_t i = 0; i < childNodes.size(); i++) {
            const GroupIdType childGroupId = childGroupIds.at(i);
            uassert(6624121, "Invalid child group", childGroupId >= 0);
            childNodes.at(i) = make<MemoLogicalDelegatorNode>(childGroupId);
        }

        return addNodes(n, node, std::move(forMemo), env, std::move(childGroupIds));
    }

    template <class T>
    GroupIdType addNode(const ABT& n,
                        const T& node,
                        const VariableEnvironment& env,
                        GroupIdType childGroupId) {
        ABT forMemo = n;
        uassert(6624122, "Invalid child group", childGroupId >= 0);
        forMemo.cast<T>()->getChild() = make<MemoLogicalDelegatorNode>(childGroupId);
        return addNodes(n, node, std::move(forMemo), env, {childGroupId});
    }

    template <class T>
    GroupIdType addNodes(const ABT& n,
                         const T& node,
                         const VariableEnvironment& env,
                         GroupIdType leftGroupId,
                         GroupIdType rightGroupId) {
        ABT forMemo = n;
        uassert(6624123, "Invalid left child group", leftGroupId >= 0);
        uassert(6624124, "Invalid right child group", rightGroupId >= 0);

        forMemo.cast<T>()->getLeftChild() = make<MemoLogicalDelegatorNode>(leftGroupId);
        forMemo.cast<T>()->getRightChild() = make<MemoLogicalDelegatorNode>(rightGroupId);
        return addNodes(n, node, std::move(forMemo), env, {leftGroupId, rightGroupId});
    }

    // Contains several resources required for calling Memo::addNode.
    Memo::Context _ctx;
    // The memo to be updated.
    Memo& _memo;
    // An out param, for reporting nodes we insert to the memo.
    NodeIdSet& _insertedNodeIds;

    // Each [node, groupId] entry means force the given node to go into the given group.
    // This is only used for the root node passed in to Memo::integrate(), so there should be
    // at most 1 entry.
    Memo::NodeTargetGroupMap _targetGroupMap;

    // Rewrite rule that triggered this node to be created.
    const LogicalRewriteType _rule;
};

Memo::Context::Context(const Metadata* metadata,
                       const DebugInfo* debugInfo,
                       const LogicalPropsInterface* logicalPropsDerivation,
                       const CardinalityEstimator* cardinalityEstimator)
    : _metadata(metadata),
      _debugInfo(debugInfo),
      _logicalPropsDerivation(logicalPropsDerivation),
      _cardinalityEstimator(cardinalityEstimator) {
    invariant(_metadata != nullptr);
    invariant(_debugInfo != nullptr);
    invariant(_logicalPropsDerivation != nullptr);
    invariant(_cardinalityEstimator != nullptr);
}

size_t Memo::GroupIdVectorHash::operator()(const Memo::GroupIdVector& v) const {
    size_t result = 17;
    for (const GroupIdType id : v) {
        updateHash(result, std::hash<GroupIdType>()(id));
    }
    return result;
}

size_t Memo::NodeTargetGroupHash::operator()(const ABT::reference_type& nodeRef) const {
    return std::hash<const Node*>()(nodeRef.cast<Node>());
}

const Group& Memo::getGroup(const GroupIdType groupId) const {
    return *_groups.at(groupId);
}

Group& Memo::getGroup(const GroupIdType groupId) {
    return *_groups.at(groupId);
}

GroupIdType Memo::addGroup(ProjectionNameSet projections) {
    _groups.emplace_back(std::make_unique<Group>(std::move(projections)));
    return _groups.size() - 1;
}

ABT::reference_type Memo::getNode(const MemoLogicalNodeId nodeMemoId) const {
    return getGroup(nodeMemoId._groupId)._logicalNodes.at(nodeMemoId._index);
}

boost::optional<MemoLogicalNodeId> Memo::findNode(const GroupIdVector& groups, const ABT& node) {
    const auto it = _inputGroupsToNodeIdMap.find(groups);
    if (it != _inputGroupsToNodeIdMap.cend()) {
        for (const MemoLogicalNodeId& nodeMemoId : it->second) {
            if (getNode(nodeMemoId) == node) {
                return nodeMemoId;
            }
        }
    }
    return boost::none;
}

// Returns true if n is a sargable node with exactly one predicate on _id.
static bool isSimpleIdLookup(ABT::reference_type n) {
    const SargableNode* node = n.cast<SargableNode>();
    if (!node || PSRExpr::numLeaves(node->getReqMap().getRoot()) != 1) {
        return false;
    }

    bool isIdLookup = false;
    PSRExpr::visitAnyShape(node->getReqMap().getRoot(), [&](const PartialSchemaEntry& entry) {
        if (const auto interval = IntervalReqExpr::getSingularDNF(entry.second.getIntervals());
            !interval || !interval->isEquality()) {
            return;
        }
        if (const PathGet* getPtr = entry.first._path.cast<PathGet>();
            getPtr && getPtr->name() == "_id") {
            if (getPtr->getPath().is<PathIdentity>()) {
                isIdLookup = true;
            } else if (const PathTraverse* traversePtr = getPtr->getPath().cast<PathTraverse>()) {
                isIdLookup = traversePtr->getPath().is<PathIdentity>();
            }
        }
    });
    return isIdLookup;
}

void Memo::estimateCE(const Context& ctx, const GroupIdType groupId) {
    // If inserted into a new group, derive logical properties, and cardinality estimation
    // for the new group.
    Group& group = getGroup(groupId);
    properties::LogicalProps& props = group._logicalProperties;

    const ABT::reference_type nodeRef = group._logicalNodes.at(0);
    properties::LogicalProps logicalProps =
        ctx._logicalPropsDerivation->deriveProps(*ctx._metadata, nodeRef, nullptr, this, groupId);
    props.merge(logicalProps);

    const bool simpleIdLookup = isSimpleIdLookup(nodeRef);
    const CEType estimate = simpleIdLookup
        ? CEType{1.0}
        : ctx._cardinalityEstimator->deriveCE(*ctx._metadata, *this, props, nodeRef);

    auto ceProp = properties::CardinalityEstimate(estimate);

    if (auto sargablePtr = nodeRef.cast<SargableNode>(); sargablePtr != nullptr) {
        auto& partialSchemaKeyCE = ceProp.getPartialSchemaKeyCE();
        invariant(partialSchemaKeyCE.empty());

        // Cache estimation for each individual requirement.
        PSRExpr::visitDNF(sargablePtr->getReqMap().getRoot(), [&](const PartialSchemaEntry& e) {
            ABT singularReq =
                make<SargableNode>(PartialSchemaRequirements{PSRExpr::makeSingularDNF(e)},
                                   CandidateIndexes{},
                                   ScanParams{},
                                   sargablePtr->getTarget(),
                                   sargablePtr->getChild());
            const CEType singularEst = simpleIdLookup
                ? CEType{1.0}
                : ctx._cardinalityEstimator->deriveCE(
                      *ctx._metadata, *this, props, singularReq.ref());
            partialSchemaKeyCE.emplace_back(e.first, singularEst);
        });
    }

    properties::setPropertyOverwrite(props, std::move(ceProp));
    if (ctx._debugInfo->hasDebugLevel(2)) {
        std::cout << "Group " << groupId << ": "
                  << ExplainGenerator::explainLogicalProps("Logical properties", props);
    }
}

MemoLogicalNodeId Memo::addNode(const Context& ctx,
                                GroupIdVector groupVector,
                                ProjectionNameSet projections,
                                const GroupIdType targetGroupId,
                                NodeIdSet& insertedNodeIds,
                                ABT n,
                                const LogicalRewriteType rule) {
    uassert(6624052, "Attempting to insert a physical node", !n.is<ExclusivelyPhysicalNode>());
    for (const GroupIdType groupId : groupVector) {
        // Invalid tree: node is its own child.
        uassert(6624127, "Target group appears inside group vector", groupId != targetGroupId);
    }

    if (const auto existingId = findNode(groupVector, n)) {
        uassert(6624054,
                "Found node outside target group",
                targetGroupId < 0 || targetGroupId == existingId->_groupId);
        return *existingId;
    }

    const bool noTargetGroup = targetGroupId < 0;
    // Only for debugging.
    ProjectionNameSet projectionsCopy;
    if (!noTargetGroup && ctx._debugInfo->isDebugMode()) {
        projectionsCopy = projections;
    }

    // Current node is not in the memo. Insert unchanged.
    const GroupIdType groupId = noTargetGroup ? addGroup(std::move(projections)) : targetGroupId;
    Group& group = *_groups.at(groupId);
    OrderPreservingABTSet& nodes = group._logicalNodes;
    const auto [index, inserted] = nodes.emplace_back(std::move(n));
    if (inserted) {
        group._rules.push_back(rule);
    }
    auto newId = MemoLogicalNodeId{groupId, index};

    if (inserted || noTargetGroup) {
        insertedNodeIds.insert(newId);
        _inputGroupsToNodeIdMap[groupVector].insert(newId);
        _nodeIdToInputGroupsMap[newId] = groupVector;

        if (noTargetGroup) {
            estimateCE(ctx, groupId);
        } else if (ctx._debugInfo->isDebugMode()) {
            const Group& group = getGroup(groupId);
            // If inserted into an existing group, verify we deliver all expected projections.
            for (const ProjectionName& groupProjection : group.binder().names()) {
                uassert(6624055,
                        "Node does not project all specified group projections",
                        projectionsCopy.find(groupProjection) != projectionsCopy.cend());
            }

            // TODO: possibly verify cardinality estimation
        }
    }

    return newId;
}

GroupIdType Memo::integrate(const Memo::Context& ctx,
                            const ABT& node,
                            NodeTargetGroupMap targetGroupMap,
                            NodeIdSet& insertedNodeIds,
                            const LogicalRewriteType rule) {
    _stats._numIntegrations++;
    MemoIntegrator integrator(ctx, *this, std::move(targetGroupMap), insertedNodeIds, rule);
    return integrator.integrate(node);
}

size_t Memo::getGroupCount() const {
    return _groups.size();
}

const ExpressionBinder& Memo::getBinderForGroup(const GroupIdType groupId) const {
    return getGroup(groupId).binder();
}

const properties::LogicalProps& Memo::getLogicalProps(GroupIdType groupId) const {
    return getGroup(groupId)._logicalProperties;
}

const ABTVector& Memo::getLogicalNodes(GroupIdType groupId) const {
    return getGroup(groupId)._logicalNodes.getVector();
}

const PhysNodeVector& Memo::getPhysicalNodes(GroupIdType groupId) const {
    return getGroup(groupId)._physicalNodes.getNodes();
}

const std::vector<LogicalRewriteType>& Memo::getRules(GroupIdType groupId) const {
    return getGroup(groupId)._rules;
}

LogicalRewriteQueue& Memo::getLogicalRewriteQueue(GroupIdType groupId) {
    return getGroup(groupId)._logicalRewriteQueue;
}

void Memo::clearLogicalNodes(const GroupIdType groupId) {
    auto& group = getGroup(groupId);
    auto& logicalNodes = group._logicalNodes;

    for (size_t index = 0; index < logicalNodes.size(); index++) {
        const MemoLogicalNodeId nodeId{groupId, index};
        const auto& groupVector = _nodeIdToInputGroupsMap.at(nodeId);
        _inputGroupsToNodeIdMap.at(groupVector).erase(nodeId);
        _nodeIdToInputGroupsMap.erase(nodeId);
    }

    logicalNodes.clear();
    group._logicalRewriteQueue = {};
    group._rules.clear();
}

const Memo::InputGroupsToNodeIdMap& Memo::getInputGroupsToNodeIdMap() const {
    return _inputGroupsToNodeIdMap;
}

void Memo::clear() {
    _stats = {};
    _groups.clear();
    _inputGroupsToNodeIdMap.clear();
    _nodeIdToInputGroupsMap.clear();
}

const Memo::Stats& Memo::getStats() const {
    return _stats;
}

size_t Memo::getLogicalNodeCount() const {
    size_t result = 0;
    for (const auto& group : _groups) {
        result += group->_logicalNodes.size();
    }
    return result;
}

size_t Memo::getPhysicalNodeCount() const {
    size_t result = 0;
    for (const auto& group : _groups) {
        result += group->_physicalNodes.getNodes().size();
    }
    return result;
}

}  // namespace mongo::optimizer::cascades
