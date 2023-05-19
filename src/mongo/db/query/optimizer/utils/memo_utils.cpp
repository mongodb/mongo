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
#include "mongo/db/query/optimizer/reference_tracker.h"


namespace mongo::optimizer {
using namespace properties;

/**
 * Used to extract a logical plan from the memo using the last logical node from each group. It is
 * used for testing.
 */
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

template <class T, class Accessor = DefaultChildAccessor<T>>
static void mergeNodeAndProps(const bool aggregateCost,
                              PlanAndProps& merged,
                              PlanAndProps& incoming,
                              const bool canMove,
                              Accessor instance = Accessor{}) {
    ABT& child = instance(merged._node);

    if (aggregateCost) {
        // Increment cost for current node.
        const auto& childCost = incoming.getRootAnnotation()._cost;
        auto& nodeCost = merged.getRootAnnotation()._cost;
        nodeCost += childCost;
    }

    if (canMove) {
        std::swap(child, incoming._node);
        merged._map.merge(incoming._map);
    } else {
        PlanAndProps copy = incoming;
        std::swap(child, copy._node);
        merged._map.insert(copy._map.cbegin(), copy._map.cend());
    }
}

static PlanAndProps moveOrCopy(PlanAndProps& planAndProps, const bool canMove) {
    if (canMove) {
        return std::move(planAndProps);
    } else {
        return planAndProps;
    }
}

template <class T>
static PlanExtractorResult mergeLeftRightResults(const bool aggregateCost,
                                                 PlanAndProps initial,
                                                 PlanExtractorResult leftResult,
                                                 PlanExtractorResult rightResult) {
    PlanExtractorResult result;
    for (size_t leftIndex = 0; leftIndex < leftResult.size(); leftIndex++) {
        auto& leftEntry = leftResult.at(leftIndex);
        const bool lastOnLeft = leftIndex == leftResult.size() - 1;

        for (size_t rightIndex = 0; rightIndex < rightResult.size(); rightIndex++) {
            auto& rightEntry = rightResult.at(rightIndex);
            const bool lastOnRight = rightIndex == rightResult.size() - 1;
            const bool lastOverall = lastOnLeft && lastOnRight;

            PlanAndProps merged = moveOrCopy(initial, lastOverall);
            mergeNodeAndProps<T, LeftChildAccessor<T>>(
                aggregateCost, merged, leftEntry, lastOnRight);
            mergeNodeAndProps<T, RightChildAccessor<T>>(
                aggregateCost, merged, rightEntry, lastOverall);

            result.push_back(std::move(merged));
        }
    }
    return result;
}

template <class T>
static PlanExtractorResult mergeNaryResults(const bool aggregateCost,
                                            PlanAndProps initial,
                                            std::vector<PlanExtractorResult> childResults) {
    size_t resultCount = 1;
    for (const auto& childResult : childResults) {
        resultCount *= childResult.size();
    }

    PlanExtractorResult result;
    for (size_t resultIndex = 0; resultIndex < resultCount; resultIndex++) {
        const bool lastOverall = resultIndex == resultCount - 1;
        PlanAndProps merged = moveOrCopy(initial, lastOverall);

        size_t v = resultIndex;
        for (size_t childIndex = 0; childIndex < childResults.size(); childIndex++) {
            auto& childResultVector = childResults.at(childIndex);
            const size_t currentSize = childResultVector.size();
            const size_t childResultIndex = v % currentSize;
            v /= currentSize;

            // TODO: should be able to move in more cases besides resultCount == 1.
            mergeNodeAndProps<T, IndexedChildAccessor<T>>(aggregateCost,
                                                          merged,
                                                          childResultVector.at(childResultIndex),
                                                          lastOverall,
                                                          {childIndex});
        }

        result.push_back(std::move(merged));
    }
    return result;
}

/**
 * Used to extract one or many physical plans from the memo.
 */
class MemoPhysicalPlanExtractor {
public:
    explicit MemoPhysicalPlanExtractor(const cascades::Memo& memo,
                                       const Metadata& metadata,
                                       const RIDProjectionsMap& ridProjections,
                                       const cascades::PhysNodeInfo& nodeInfo,
                                       const LogicalProps& logicalProps,
                                       const PhysProps& physProps,
                                       const MemoPhysicalNodeId id,
                                       const bool includeRejected,
                                       int32_t& planNodeId)
        : _memo(memo),
          _metadata(metadata),
          _ridProjections(ridProjections),
          _nodeInfo(nodeInfo),
          _logicalProps(logicalProps),
          _physProps(physProps),
          _id(id),
          _includeRejected(includeRejected),
          _planNodeId(planNodeId) {}

    /**
     * Physical delegator node.
     */
    PlanExtractorResult operator()(const ABT& /*n*/,
                                   const MemoPhysicalDelegatorNode& node,
                                   const bool /*isGroupRoot*/,
                                   ProjectionNameOrderPreservingSet /*required*/) {
        auto result = extract(
            _memo, _metadata, _ridProjections, node.getNodeId(), _includeRejected, _planNodeId);
        return result;
    }

    PlanExtractorResult operator()(const ABT& n,
                                   const NestedLoopJoinNode& node,
                                   const bool isGroupRoot,
                                   ProjectionNameOrderPreservingSet required) {
        // Obtain correlated projections from the left child, non-correlated from the right child.
        ProjectionNameOrderPreservingSet requiredInner = required;
        ProjectionNameOrderPreservingSet requiredOuter;
        for (const ProjectionName& projectionName : node.getCorrelatedProjectionNames()) {
            requiredInner.erase(projectionName);
            if (required.find(projectionName)) {
                requiredOuter.emplace_back(projectionName);
            }
        }

        auto leftResult =
            node.getLeftChild().visit(*this, false /*isGroupRoot*/, std::move(requiredOuter));
        auto rightResult =
            node.getRightChild().visit(*this, false /*isGroupRoot*/, std::move(requiredInner));

        PlanAndProps initial = createInitial(isGroupRoot, n, std::move(required));
        auto result = mergeLeftRightResults<NestedLoopJoinNode>(
            _includeRejected, std::move(initial), std::move(leftResult), std::move(rightResult));
        return result;
    }

    PlanExtractorResult operator()(const ABT& n,
                                   const GroupByNode& node,
                                   const bool isGroupRoot,
                                   ProjectionNameOrderPreservingSet required) {
        ProjectionNameOrderPreservingSet requiredForChild = required;
        // Propagate the input projections only.
        for (const auto& projName : node.getAggregationProjectionNames()) {
            requiredForChild.erase(projName);
        }
        for (const auto& expr : node.getAggregationExpressions()) {
            if (const auto fnPtr = expr.cast<FunctionCall>();
                fnPtr != nullptr && fnPtr->nodes().size() == 1) {
                if (const auto varPtr = fnPtr->nodes().front().cast<Variable>()) {
                    requiredForChild.emplace_back(varPtr->name());
                }
            }
        }

        auto result =
            node.getChild().visit(*this, false /*isGroupRoot*/, std::move(requiredForChild));

        PlanAndProps initial = createInitial(isGroupRoot, n, std::move(required));
        for (size_t index = 0; index < result.size(); index++) {
            auto& entry = result.at(index);
            PlanAndProps merged = moveOrCopy(initial, index == result.size() - 1);
            mergeNodeAndProps<GroupByNode>(_includeRejected, merged, entry, true /*canMove*/);
            std::swap(entry, merged);
        }
        return result;
    }

    PlanExtractorResult operator()(const ABT& n,
                                   const EvaluationNode& node,
                                   const bool isGroupRoot,
                                   ProjectionNameOrderPreservingSet required) {
        ProjectionNameOrderPreservingSet requiredForChild = required;
        requiredForChild.erase(node.getProjectionName());
        auto env = VariableEnvironment::build(node.getProjection());
        for (const auto& proj : env.freeVariableNames()) {
            requiredForChild.emplace_back(proj);
        }

        auto result =
            node.getChild().visit(*this, false /*isGroupRoot*/, std::move(requiredForChild));

        PlanAndProps initial = createInitial(isGroupRoot, n, std::move(required));
        for (size_t index = 0; index < result.size(); index++) {
            auto& entry = result.at(index);
            PlanAndProps merged = moveOrCopy(initial, index == result.size() - 1);
            mergeNodeAndProps<EvaluationNode>(_includeRejected, merged, entry, true /*canMove*/);
            std::swap(entry, merged);
        }
        return result;
    }

    template <class T>
    PlanExtractorResult operator()(const ABT& n,
                                   const T& node,
                                   bool isGroupRoot,
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
            return {createInitial(isGroupRoot, n, std::move(required))};
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
            auto result = node.getChild().visit(*this, false /*isGroupRoot*/, required);

            PlanAndProps initial = createInitial(isGroupRoot, n, std::move(required));
            for (size_t index = 0; index < result.size(); index++) {
                auto& entry = result.at(index);
                PlanAndProps merged = moveOrCopy(initial, index == result.size() - 1);

                mergeNodeAndProps<T>(_includeRejected, merged, entry, true /*canMove*/);
                std::swap(entry, merged);
            }
            return result;
        } else if constexpr (is_one_of_v<T, SortedMergeNode, UnionNode>) {
            // N-ary nodes.

            std::vector<PlanExtractorResult> childResults;
            for (auto& child : node.nodes()) {
                auto childResult = child.visit(*this, false /*isGroupRoot*/, required);
                childResults.push_back(std::move(childResult));
            }

            PlanAndProps initial = createInitial(isGroupRoot, n, std::move(required));
            return mergeNaryResults<T>(
                _includeRejected, std::move(initial), std::move(childResults));
        } else if constexpr (is_one_of_v<T, MergeJoinNode, HashJoinNode>) {
            // HashJoin and MergeJoin.

            // Do not require RID from the inner child.
            auto requiredInner = required;
            if (hasProperty<IndexingRequirement>(_physProps)) {
                const auto& scanDefName =
                    getPropertyConst<IndexingAvailability>(_logicalProps).getScanDefName();
                requiredInner.erase(_ridProjections.at(scanDefName));
            }

            auto leftResult = node.getLeftChild().visit(*this, false /*isGroupRoot*/, required);
            auto rightResult =
                node.getRightChild().visit(*this, false /*isGroupRoot*/, std::move(requiredInner));

            PlanAndProps initial = createInitial(isGroupRoot, n, std::move(required));
            return mergeLeftRightResults<T>(_includeRejected,
                                            std::move(initial),
                                            std::move(leftResult),
                                            std::move(rightResult));
        } else {
            // Other ABT types.
            static_assert(!canBePhysicalNode<T>(), "Physical node must implement its visitor");
            MONGO_UNREACHABLE;
        }
    }

    static PlanExtractorResult extract(const cascades::Memo& memo,
                                       const Metadata& metadata,
                                       const RIDProjectionsMap& ridProjections,
                                       MemoPhysicalNodeId nodeId,
                                       const bool includeRejected,
                                       int32_t& planNodeId) {
        const auto& result = *memo.getPhysicalNodes(nodeId._groupId).at(nodeId._index);
        uassert(6624143,
                "Physical delegator must be pointing to an optimized result.",
                result._nodeInfo.has_value());

        LogicalProps logicalProps = memo.getLogicalProps(nodeId._groupId);
        PhysProps physProps = result._physProps;
        if (!metadata.isParallelExecution()) {
            // Do not display availability and requirement if under centralized setting.
            removeProperty<DistributionAvailability>(logicalProps);
            removeProperty<DistributionRequirement>(physProps);
        }

        ProjectionNameOrderPreservingSet projSet;
        if (hasProperty<ProjectionRequirement>(result._physProps)) {
            projSet = getPropertyConst<ProjectionRequirement>(result._physProps).getProjections();
        }

        PlanExtractorResult results;
        const size_t altCount = includeRejected ? (result._rejectedNodeInfo.size() + 1) : 1;
        for (size_t altIndex = 0; altIndex < altCount; altIndex++) {
            const bool isLastIteration = altIndex == altCount - 1;
            const auto& nodeInfo =
                (altIndex == 0) ? *result._nodeInfo : result._rejectedNodeInfo.at(altIndex - 1);
            const ABT& node = nodeInfo._node;

            MemoPhysicalPlanExtractor instance(memo,
                                               metadata,
                                               ridProjections,
                                               nodeInfo,
                                               logicalProps,
                                               physProps,
                                               nodeId,
                                               includeRejected,
                                               planNodeId);

            PlanExtractorResult altResults;
            if (isLastIteration) {
                altResults = node.visit(instance,
                                        true /*isGroupRoot*/,
                                        std::move(projSet));  // NOLINT(bugprone-use-after-move)
            } else {
                altResults = node.visit(instance, true /*isGroupRoot*/, projSet);
            }
            std::move(altResults.begin(), altResults.end(), std::back_inserter(results));
        }

        invariant(!results.empty() && (includeRejected || results.size() == 1));
        return results;
    }

private:
    PlanAndProps createInitial(const bool isGroupRoot,
                               const ABT& n,
                               ProjectionNameOrderPreservingSet required) {
        PhysProps physProps1 = _physProps;
        // Restrict projections only to currently required ones.
        setPropertyOverwrite(physProps1, ProjectionRequirement{std::move(required)});

        boost::optional<ProjectionName> ridProjName;
        if (hasProperty<IndexingRequirement>(_physProps)) {
            const auto& scanDefName =
                getPropertyConst<IndexingAvailability>(_logicalProps).getScanDefName();
            ridProjName = _ridProjections.at(scanDefName);
        }

        // If we are not returning more than one plan (_includeRejected = false) then we do not
        // aggregate cost, and thus retain the original total cost (_cost). Otherwise, if we are the
        // group root, then we aggregate the cost of the children, otherwise we propagate zero. We
        // keep a single cost for all nodes in the group's subplan (they are all annotated with the
        // group's cost.
        CostType totalCost = _includeRejected
            ? (isGroupRoot ? _nodeInfo._localCost : CostType::kZero)
            : _nodeInfo._cost;
        NodeProps entry{_planNodeId++,
                        _id,
                        _logicalProps,
                        std::move(physProps1),
                        std::move(ridProjName),
                        totalCost,
                        _nodeInfo._localCost,
                        _nodeInfo._adjustedCE};

        PlanAndProps result{n, {}};
        result.setRootAnnotation(std::move(entry));
        return result;
    }

    // We don't own this.
    const cascades::Memo& _memo;
    const Metadata& _metadata;
    const RIDProjectionsMap& _ridProjections;

    const cascades::PhysNodeInfo& _nodeInfo;
    const LogicalProps& _logicalProps;
    const PhysProps& _physProps;
    const MemoPhysicalNodeId _id;
    const bool _includeRejected;

    int32_t& _planNodeId;
};

PlanExtractorResult extractPhysicalPlans(const bool includeRejected,
                                         const MemoPhysicalNodeId id,
                                         const Metadata& metadata,
                                         const RIDProjectionsMap& ridProjections,
                                         const cascades::Memo& memo) {
    int32_t planNodeId = 0;
    auto result = MemoPhysicalPlanExtractor::extract(
        memo, metadata, ridProjections, id, includeRejected, planNodeId);
    return result;
}

}  // namespace mongo::optimizer
