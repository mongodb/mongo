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
}

const std::vector<JoinSubset>& PlanEnumeratorContext::getSubsets(int level) {
    return _joinSubsets[level];
}

bool PlanEnumeratorContext::canPlanBeEnumerated(PlanTreeShape type,
                                                JoinMethod method,
                                                const JoinSubset& left,
                                                const JoinSubset& right,
                                                const JoinSubset& subset) {
    if ((method == JoinMethod::NLJ || method == JoinMethod::INLJ) &&
        !right.isBaseCollectionAccess()) {
        // NLJ plans perform poorly when the right hand side is not a collection access, while INLJ
        // requires the right side to be a base table access. Don't enumerate this plan.
        return false;
    }

    if (type == PlanTreeShape::LEFT_DEEP && !right.isBaseCollectionAccess()) {
        // Left-deep tree must have a "base" collection and not an intermediate join on the right.
        return false;
    }
    if (type == PlanTreeShape::RIGHT_DEEP && !left.isBaseCollectionAccess()) {
        // Right-deep tree must have a "base" collection and not an intermediate join on the left.
        return false;
    }
    if (type == PlanTreeShape::ZIG_ZAG && !left.isBaseCollectionAccess() &&
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
    bool eligibleToPrune = _enableHJOrderPruning && method == JoinMethod::HJ &&
        (type == PlanTreeShape::ZIG_ZAG || bothBaseColls);
    if (eligibleToPrune &&
        _estimator->getOrEstimateSubsetCardinality(left.subset) >
            _estimator->getOrEstimateSubsetCardinality(right.subset)) {
        return false;
    }

    return true;
}

void PlanEnumeratorContext::addJoinPlan(PlanTreeShape type,
                                        JoinMethod method,
                                        const JoinSubset& left,
                                        const JoinSubset& right,
                                        const std::vector<EdgeId>& edges,
                                        JoinSubset& subset) {
    if (!canPlanBeEnumerated(type, method, left, right, subset)) {
        return;
    }

    if (method == JoinMethod::INLJ) {
        tassert(11371701, "Expected at least one edge", edges.size() >= 1);
        auto edge = edges[0];
        auto ie = bestIndexSatisfyingJoinPredicates(
            _ctx, (NodeId)*begin(right.subset), _ctx.joinGraph.getEdge(edge));
        if (ie) {
            const auto nodeId = right.getNodeId();
            const auto& nss = _ctx.joinGraph.accessPathAt(nodeId)->nss();
            auto rhs = _registry.registerINLJRHSNode(nodeId, ie, nss);
            subset.plans.push_back(
                _registry.registerJoinNode(subset, method, left.bestPlan(), rhs));
        } else {
            return;
        }
    } else {
        subset.plans.push_back(
            _registry.registerJoinNode(subset, method, left.bestPlan(), right.bestPlan()));
    }

    LOGV2_DEBUG(11336912,
                5,
                "Enumerating plan for join subset",
                "plan"_attr =
                    _registry.joinPlanNodeToBSON(subset.plans.back(), _ctx.joinGraph.numNodes()));
}

void PlanEnumeratorContext::enumerateJoinPlans(PlanTreeShape type,
                                               const JoinSubset& left,
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

    addJoinPlan(type, JoinMethod::INLJ, left, right, joinEdges, cur);
    addJoinPlan(type, JoinMethod::HJ, left, right, joinEdges, cur);
    addJoinPlan(type, JoinMethod::NLJ, left, right, joinEdges, cur);
}

void PlanEnumeratorContext::enumerateJoinSubsets(PlanTreeShape type) {
    int numNodes = _ctx.joinGraph.numNodes();
    // Use CombinationSequence to efficiently calculate the final size of each level of the dynamic
    // programming table.
    CombinationSequence cs(numNodes);
    // Skip over C(numNodes, 0). The size of the first level of the table is C(numNodes, 1).
    cs.next();
    _joinSubsets.resize(cs.next());

    // Initialize base level of joinSubsets, representing single collections (no joins).
    for (int i = 0; i < numNodes; ++i) {
        const auto* cq = _ctx.joinGraph.getNode((NodeId)i).accessPath.get();
        const auto* qsn = _ctx.cbrCqQsns.at(cq).get();
        _joinSubsets[kBaseLevel].push_back(JoinSubset(NodeSet{}.set(i)));
        _joinSubsets[kBaseLevel].back().plans = {
            _registry.registerBaseNode((NodeId)i, qsn, cq->nss())};
    }

    // Initialize the rest of the joinSubsets.
    for (int level = 1; level < numNodes; ++level) {
        auto& joinSubsetsPrevLevel = _joinSubsets[level - 1];
        auto& joinSubsetsCurrLevel = _joinSubsets[level];
        // Preallocate entries for all subsets in the current level.
        joinSubsetsCurrLevel.reserve(cs.next());

        // Tracks seen subsets along with their indexes in 'joinSubsetsCurrLevel'. This lets us
        // quickly find a subset and update its plans if we see it again.
        stdx::unordered_map<NodeSet, size_t> seenSubsetIndexes;

        // For each join subset of size level-1, iterate through nodes 0 to n-1 and use bitwise-or
        // to enumerate all possible join subsets of size level.
        for (auto&& prevJoinSubset : joinSubsetsPrevLevel) {
            for (int i = 0; i < numNodes; ++i) {
                // If the existing join subset contains the current node, avoid generating a new
                // entry.
                if (prevJoinSubset.subset.test(i)) {
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

                enumerateJoinPlans(type, prevJoinSubset, _joinSubsets[kBaseLevel][i], cur);
                enumerateJoinPlans(type, _joinSubsets[kBaseLevel][i], prevJoinSubset, cur);
            }
        }
    }
}

std::string PlanEnumeratorContext::toString() const {
    const auto numNodes = _ctx.joinGraph.numNodes();
    std::stringstream ss;
    ss << "HJ order pruning enabled: " << _enableHJOrderPruning << "\n";
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

}  // namespace mongo::join_ordering
