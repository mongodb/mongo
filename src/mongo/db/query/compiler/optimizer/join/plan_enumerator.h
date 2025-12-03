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

#pragma once

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/join_plan.h"
#include "mongo/db/query/compiler/optimizer/join/solution_storage.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

/**
 * Describes shape of plan tree.
 */
enum class PlanTreeShape { LEFT_DEEP, RIGHT_DEEP, ZIG_ZAG };

/**
 * Context containing all the state for the bottom-up dynamic programming join plan enumeration
 * algorithm.
 */
class PlanEnumeratorContext {
public:
    PlanEnumeratorContext(const JoinGraph& joinGraph, const QuerySolutionMap& map);

    // Delete copy and move operations to prevent issues with copying '_joinGraph'.
    PlanEnumeratorContext(const PlanEnumeratorContext&) = delete;
    PlanEnumeratorContext& operator=(const PlanEnumeratorContext&) = delete;
    PlanEnumeratorContext(PlanEnumeratorContext&&) = delete;
    PlanEnumeratorContext& operator=(PlanEnumeratorContext&&) = delete;

    /**
     * Return all 'JoinSubsets' of size level+1.
     */
    const std::vector<JoinSubset>& getSubsets(int level);

    /**
     * Enumerates all join subsets in bottom-up fashion.
     */
    void enumerateJoinSubsets(PlanTreeShape type = PlanTreeShape::ZIG_ZAG);

    JoinPlanNodeId getBestFinalPlan() const {
        tassert(11336904,
                "Expected subsets to have already been enumerated",
                _joinSubsets.size() > 0 && _joinSubsets[_joinSubsets.size() - 1].size() == 1);
        const auto& lastSubset = _joinSubsets[_joinSubsets.size() - 1][0];
        return lastSubset.bestPlan();
    }

    const JoinPlanNodeRegistry& registry() const {
        return _registry;
    }

    /**
     * Used for testing & debugging.
     */
    std::string toString() const;

private:
    /**
     * Enumerate plans by constructing possible joins between the 'left' and 'right' subsets and
     * outputting those plans in 'cur'. Note that 'left' and 'right' must be disjoint, and their
     * union must produce 'cur'.
     */
    void enumerateJoinPlans(PlanTreeShape type,
                            const JoinSubset& left,
                            const JoinSubset& right,
                            JoinSubset& cur);

    /**
     * Helper for adding a join plan to subset 'cur', constructed using the specified join 'method'
     * connecting the best plans from the provided subsets.
     */
    void addJoinPlan(PlanTreeShape type,
                     JoinMethod method,
                     const JoinSubset& left,
                     const JoinSubset& right,
                     const std::vector<EdgeId>& edges,
                     JoinSubset& cur);

    /**
     * Determines based on the shape of the tree obtained by joining the best plans on each side if
     * we would retain the tree shape specified by 'type' and the plan is valid for the given join
     * 'method'.
     */
    bool canPlanBeEnumerated(PlanTreeShape type,
                             JoinMethod method,
                             const JoinSubset& left,
                             const JoinSubset& right,
                             const JoinSubset& subset) const;

    const JoinGraph& _joinGraph;

    // Hold intermediate results of the enumeration algorithm. The index into the outer vector
    // represents the "level". The i'th level contains solutions for the optimal way to join all
    // possible subsets of size i+1.
    std::vector<std::vector<JoinSubset>> _joinSubsets;

    // Memory management for trees so we can reuse nodes.
    JoinPlanNodeRegistry _registry;

    // Holds results from CBR.
    const QuerySolutionMap& _cqsToQsns;
};

}  // namespace mongo::join_ordering
