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

namespace mongo::join_ordering {

PlanEnumeratorContext::PlanEnumeratorContext(const JoinGraph& joinGraph) : _joinGraph(joinGraph) {}

const std::vector<JoinSubset>& PlanEnumeratorContext::getSubsets(int level) {
    return _joinSubsets[level];
}

void PlanEnumeratorContext::enumerateJoinSubsets() {
    int numNodes = _joinGraph.numNodes();
    _joinSubsets.resize(numNodes);

    // Initialize base level of joinSubsets, representing single collections (no joins).
    for (int i = 0; i < numNodes; ++i) {
        JoinSubset s{.subset = NodeSet{}.set(i)};
        _joinSubsets[0].push_back(s);
    }

    // Initialize the rest of the joinSubsets.
    for (int level = 1; level < numNodes; ++level) {
        const std::vector<JoinSubset>& joinSubsetsPrevLevel = _joinSubsets[level - 1];
        auto& joinSubsetsCurrLevel = _joinSubsets[level];
        stdx::unordered_set<NodeSet> seenSubsets;

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
                // Ensure we don't generate the same subset twice (for example, AB | C and BC | A
                // both produce ABC).
                if (seenSubsets.contains(newSubset)) {
                    // TODO SERVER-113369: Reuse this entry in the table to perform plan
                    // enumeration, rather than continuing.
                    continue;
                }
                seenSubsets.insert(newSubset);
                JoinSubset joinSubset{.subset = newSubset};
                joinSubsetsCurrLevel.push_back(std::move(joinSubset));
            }
        }
    }
}

}  // namespace mongo::join_ordering
