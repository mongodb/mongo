/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/plan_enumerator.h"

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/join_plan.h"
#include "mongo/db/query/compiler/optimizer/join/plan_enumerator_helpers.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::join_ordering {
namespace {
static constexpr size_t kBaseLevel = 0;
}  // namespace

const std::vector<JoinSubset>& PlanEnumeratorContext::getSubsets(int level) {
    return _joinSubsets[level];
}

bool PlanEnumeratorContext::canPlanBeEnumerated(JoinMethod method,
                                                const JoinSubset& left,
                                                const JoinSubset& right,
                                                const JoinSubset& subset) {
    if (!_mode.canMethodBe(method)) {
        return false;
    }

    if ((_strategy.planShape == PlanTreeShape::LEFT_DEEP || method == JoinMethod::NLJ ||
         method == JoinMethod::INLJ) &&
        !right.isBaseCollectionAccess()) {
        // Left-deep tree must have a "base" collection and not an intermediate join on the right.
        // NLJ plans perform poorly when the right hand side is not a collection access, while INLJ
        // requires the right side to be a base table access. Don't enumerate this plan.
        return false;
    }

    if (_strategy.planShape == PlanTreeShape::RIGHT_DEEP && !left.isBaseCollectionAccess()) {
        // Right-deep tree must have a "base" collection and not an intermediate join on the left.
        return false;
    }
    if (_strategy.planShape == PlanTreeShape::ZIG_ZAG && !left.isBaseCollectionAccess() &&
        !right.isBaseCollectionAccess()) {
        // Zig-zag is the least strict: at least one of the left or right must be a base collection.
        return false;
    }

    // Pruning heuristic: Disallow plans where the larger CE is on the build side of a HJ. This
    // should kick in only when we know that we will also enumerate the other order (left and right
    // swapped), otherwise it may impact our ability to find a solution. That is, try to prune when
    // we're enumerating:
    // - Zig-zag plans, since these can have intermediate joins on either side of the HJ, OR
    // - A join between two base collections, since these can be reordered regardless of plan shape.
    bool bothBaseColls = left.isBaseCollectionAccess() && right.isBaseCollectionAccess();
    bool eligibleToPrune = _strategy.enableHJOrderPruning && method == JoinMethod::HJ &&
        (_strategy.planShape == PlanTreeShape::ZIG_ZAG || bothBaseColls);
    if (eligibleToPrune &&
        _estimator->getOrEstimateSubsetCardinality(left.subset) >
            _estimator->getOrEstimateSubsetCardinality(right.subset)) {
        return false;
    }

    return true;
}

void PlanEnumeratorContext::addPlanToSubset(JoinMethod method,
                                            JoinPlanNodeId left,
                                            JoinPlanNodeId right,
                                            JoinCostEstimate cost,
                                            JoinSubset& subset,
                                            bool isBestPlan) {
    if (isBestPlan) {
        // Update the index to reflect this is the best plan we have costed so far.
        subset.bestPlanIndex = subset.plans.size();
    }

    subset.plans.push_back(
        _registry.registerJoinNode(subset, method, left, right, std::move(cost)));
    LOGV2_DEBUG(11336912,
                5,
                "Enumerating plan for join subset",
                "plan"_attr =
                    _registry.joinPlanNodeToBSON(subset.plans.back(), _ctx.joinGraph.numNodes()),
                "isBestPlan"_attr = isBestPlan);
}

void PlanEnumeratorContext::enumerateINLJPlan(EdgeId edge,
                                              JoinPlanNodeId leftPlan,
                                              const JoinSubset& right,
                                              JoinSubset& subset) {
    const auto rightNodeId = right.getNodeId();
    // TODO SERVER-117583: Pick index in a cost-based manner.
    auto ie = bestIndexSatisfyingJoinPredicates(_ctx, rightNodeId, _ctx.joinGraph.getEdge(edge));
    if (!ie) {
        // No such index.
        return;
    }

    auto inljCost = _coster
        ? _coster->costINLJFragment(_registry.get(leftPlan), rightNodeId, ie, edge)
        : zeroCost;
    bool isBestPlan = isBestPlanSoFar(subset, inljCost);
    if (_mode.mode() == PlanEnumerationMode::CHEAPEST && !isBestPlan) {
        // Only build this plan if it is better than what we already have.
        return;
    }

    const auto& nss = _ctx.joinGraph.accessPathAt(rightNodeId)->nss();
    auto rhs = _registry.registerINLJRHSNode(rightNodeId, ie, nss);
    addPlanToSubset(JoinMethod::INLJ, leftPlan, rhs, std::move(inljCost), subset, isBestPlan);
}

void PlanEnumeratorContext::enumerateJoinPlan(JoinMethod method,
                                              JoinPlanNodeId leftPlanId,
                                              JoinPlanNodeId rightPlanId,
                                              JoinSubset& subset) {
    JoinCostEstimate joinCost = [this, leftPlanId, rightPlanId, method]() {
        const auto& leftPlan = _registry.get(leftPlanId);
        const auto& rightPlan = _registry.get(rightPlanId);
        if (method == JoinMethod::NLJ) {
            return _coster ? _coster->costNLJFragment(leftPlan, rightPlan) : zeroCost;
        }
        tassert(1748000, "Expected HJ", method == JoinMethod::HJ);
        return _coster ? _coster->costHashJoinFragment(leftPlan, rightPlan) : zeroCost;
    }();

    bool isBestPlan = isBestPlanSoFar(subset, joinCost);
    if (_mode.mode() == PlanEnumerationMode::CHEAPEST && !isBestPlan) {
        // Only build this plan if it is better than what we already have.
        return;
    }

    addPlanToSubset(method, leftPlanId, rightPlanId, std::move(joinCost), subset, isBestPlan);
}

void PlanEnumeratorContext::enumerateAllJoinPlans(JoinMethod method,
                                                  const JoinSubset& left,
                                                  const JoinSubset& right,
                                                  const std::vector<EdgeId>& edges,
                                                  JoinSubset& subset) {
    if (method == JoinMethod::INLJ) {
        tassert(11371701, "Expected at least one edge", edges.size() >= 1);
        // Enumerate an INLJ for every plan we have in the left subset.
        for (auto&& plan : left.plans) {
            if (_registry.isOfType<INLJRHSNode>(plan)) {
                // Index probes are only relevant as the RHS of an INLJ.
                continue;
            }
            enumerateINLJPlan(edges[0], plan, right, subset);
        }
        return;
    }

    // Enumerate a join for every pair of plans.
    for (auto&& leftPlan : left.plans) {
        if (_registry.isOfType<INLJRHSNode>(leftPlan)) {
            // Index probes are only relevant as the RHS of an INLJ.
            continue;
        }
        for (auto&& rightPlan : right.plans) {
            if (_registry.isOfType<INLJRHSNode>(rightPlan)) {
                // Index probes are only relevant as the RHS of an INLJ.
                continue;
            }
            enumerateJoinPlan(method, leftPlan, rightPlan, subset);
        }
    }
}

void PlanEnumeratorContext::enumerateCheapestJoinPlan(JoinMethod method,
                                                      const JoinSubset& left,
                                                      const JoinSubset& right,
                                                      const std::vector<EdgeId>& edges,
                                                      JoinSubset& subset) {
    // Only build a join using the best plans we have for each subset.
    if (method == JoinMethod::INLJ) {
        tassert(11371705, "Expected at least one edge", edges.size() >= 1);
        enumerateINLJPlan(edges[0], left.bestPlan(), right, subset);
        return;
    }
    enumerateJoinPlan(method, left.bestPlan(), right.bestPlan(), subset);
}

void PlanEnumeratorContext::addJoinPlan(JoinMethod method,
                                        const JoinSubset& left,
                                        const JoinSubset& right,
                                        const std::vector<EdgeId>& edges,
                                        JoinSubset& subset) {
    if (!canPlanBeEnumerated(method, left, right, subset)) {
        return;
    }

    if (_mode.specifiesHint()) {
        LOGV2_DEBUG(11458210,
                    5,
                    "Applying hint for subset",
                    "subset"_attr = subset.toString(_ctx.joinGraph.numNodes()),
                    "hint"_attr = _mode.hint().toBSON());
    }

    switch (_mode.mode()) {
        case PlanEnumerationMode::CHEAPEST: {
            enumerateCheapestJoinPlan(method, left, right, edges, subset);
            break;
        }

        case PlanEnumerationMode::ALL: {
            enumerateAllJoinPlans(method, left, right, edges, subset);
            break;
        }
    }
}

void PlanEnumeratorContext::enumerateJoinPlans(const JoinSubset& left,
                                               const JoinSubset& right,
                                               JoinSubset& cur) {
    if (left.plans.empty() || right.plans.empty()) {
        return;
    }

    tassert(11336902,
            "Expected union of subsets to produce output subset",
            (left.subset | right.subset) == cur.subset);

    tassert(11336903,
            "Expected left and right subsets to be disjoint",
            (left.subset & right.subset).none());

    auto joinEdges = _ctx.joinGraph.getJoinEdges(left.subset, right.subset);
    if (joinEdges.empty()) {
        return;
    }

    addJoinPlan(JoinMethod::INLJ, left, right, joinEdges, cur);
    addJoinPlan(JoinMethod::HJ, left, right, joinEdges, cur);
    addJoinPlan(JoinMethod::NLJ, left, right, joinEdges, cur);
}

void PlanEnumeratorContext::enumerateJoinSubsets() {
    size_t numNodes = _ctx.joinGraph.numNodes();
    // Use CombinationSequence to efficiently calculate the final size of each level of the dynamic
    // programming table.
    CombinationSequence cs(numNodes);
    // Skip over C(numNodes, 0). The size of the first level of the table is C(numNodes, 1).
    cs.next();
    _joinSubsets.resize(cs.next());

    auto modeIt = _strategy.mode.begin();
    _mode = modeIt.get();

    // Special case: for the first subset, we still want to enumerate all base nodes, even when
    // hinting. However, we just want to join with one of them.
    NodeId hintedFirstLevelSubset = 0;
    if (_mode.specifiesNode()) {
        hintedFirstLevelSubset = _mode.baseNode();
    }

    modeIt.next();

    // Initialize base level of joinSubsets, representing single collections (no joins).
    for (size_t i = 0; i < numNodes; ++i) {
        const auto* cq = _ctx.joinGraph.getNode((NodeId)i).accessPath.get();
        const auto* qsn = _ctx.singleTableAccess.cbrCqQsns.at(cq).get();
        _joinSubsets[kBaseLevel].push_back(JoinSubset(NodeSet{}.set(i)));
        _joinSubsets[kBaseLevel].back().plans = {_registry.registerBaseNode(
            (NodeId)i,
            qsn,
            cq->nss(),
            _coster ? _coster->costBaseCollectionAccess((NodeId)i) : zeroCost)};
    }

    // Initialize the rest of the joinSubsets.
    for (size_t level = 1; level < numNodes; ++level) {
        // Find the right enumeration mode for the current level. Only need to increment by one
        // because strategy modes change at most as frequently as once per level.
        if (modeIt != _strategy.mode.end() && modeIt.get().level() == level) {
            // Update the mode once we reach the level it refers to.
            _mode = modeIt.get();
            modeIt.next();
        }

        auto& joinSubsetsPrevLevel = _joinSubsets[level - 1];
        auto& joinSubsetsCurrLevel = _joinSubsets[level];
        if (!_mode.specifiesNode()) {
            // Preallocate entries for all subsets in the current level, but not if we're hinting
            // (in which case, we will just enumerate one subset).
            joinSubsetsCurrLevel.reserve(cs.next());
        }

        // Tracks seen subsets along with their indexes in 'joinSubsetsCurrLevel'. This lets us
        // quickly find a subset and update its plans if we see it again.
        stdx::unordered_map<NodeSet, size_t> seenSubsetIndexes;

        // For each join subset of size level-1, iterate through nodes 0 to n-1 and use bitwise-or
        // to enumerate all possible join subsets of size level.
        for (auto&& prevJoinSubset : joinSubsetsPrevLevel) {
            for (size_t i = 0; i < numNodes; ++i) {
                // If the existing join subset contains the current node, avoid generating a new
                // entry.
                if (prevJoinSubset.subset.test(i)) {
                    continue;
                }

                if (!_mode.canBaseNodeBe(i)) {
                    // We should only enumerate plans for the next hinted node.
                    continue;
                } else if (_mode.specifiesNode() && level == 1 &&
                           !prevJoinSubset.subset.test(hintedFirstLevelSubset)) {
                    // Special case for hinting: don't try to join with all subsets in the first
                    // level (since we enumerated all base collection accesses). Only join with the
                    // one that was hinted.
                    continue;
                }

                NodeSet newSubset = NodeSet{prevJoinSubset.subset}.set(i);
                size_t subsetIdx;

                // Ensure we don't generate the same subset twice (for example, AB | C and BC | A
                // both produce ABC).
                if (auto it = seenSubsetIndexes.find(newSubset); it != seenSubsetIndexes.end()) {
                    if (prevJoinSubset.isBaseCollectionAccess()) {
                        // We will have already enumerated all plans for joining these two base
                        // collections (we already tried joining both A | B and B | A). No need
                        // to enumerate more plans. As long as we always join with a base-collection
                        // subset on one side, this is the only case where we could get duplicate
                        // plans.
                        continue;
                    }
                    subsetIdx = it->second;

                } else {
                    subsetIdx = joinSubsetsCurrLevel.size();
                    seenSubsetIndexes.insert({newSubset, subsetIdx});
                    joinSubsetsCurrLevel.push_back(JoinSubset(newSubset));
                }

                auto& cur = joinSubsetsCurrLevel[subsetIdx];
                if (_mode.baseNodeCanBeOnRight()) {
                    // We don't have a hint, or our hint says to enumerate the next base collection
                    // on the right.
                    enumerateJoinPlans(prevJoinSubset, _joinSubsets[kBaseLevel][i], cur);
                }
                if (_mode.baseNodeCanBeOnLeft()) {
                    // We don't have a hint, or our hint says to enumerate the next base collection
                    // on the left.
                    enumerateJoinPlans(_joinSubsets[kBaseLevel][i], prevJoinSubset, cur);
                }
            }
        }
    }
}

std::string PlanEnumeratorContext::toString() const {
    const auto numNodes = _ctx.joinGraph.numNodes();
    std::stringstream ss;
    ss << "HJ order pruning enabled: " << _strategy.enableHJOrderPruning << "\n";
    for (size_t level = 0; level < _joinSubsets.size(); level++) {
        ss << "Level " << level << ":\n";
        const auto n = _joinSubsets[level].size();
        for (size_t i = 0; i < n; i++) {
            ss << _joinSubsets[level][i].toString(numNodes);
            if (i < n - 1) {
                ss << ", ";
            }
        }
        ss << "\n";

        if (level == _joinSubsets.size() - 1) {
            //  Print out only final level of plans.
            tassert(11336907,
                    "Expected a single subset on the final level",
                    _joinSubsets[level].size() == 1);
            ss << "\nOutput plans (best plan " << _joinSubsets[level][kBaseLevel].bestPlanIndex
               << "):\n"
               << _registry.joinPlansToString(_joinSubsets[level][kBaseLevel].plans, numNodes);
        }
    }
    return ss.str();
}

std::vector<JoinPlanNodeId> PlanEnumeratorContext::getRejectedFinalPlans() const {
    std::vector<JoinPlanNodeId> rejected;
    auto bestPlan = getBestFinalPlan();
    const auto& plans = finalSubset().plans;
    rejected.reserve(plans.size() - 1);
    for (auto&& plan : plans) {
        if (plan == bestPlan) {
            continue;
        }
        rejected.push_back(plan);
    }
    return rejected;
}

}  // namespace mongo::join_ordering
