/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/index_tag.h"

namespace mongo {
namespace plan_enumerator {

// Everything is really a size_t but it's far more readable to impose a type via typedef.
// An ID we use to index into _memo.  An entry in _memo is a NodeAssignment.
typedef size_t MemoID;

// An index in _indices.
typedef size_t IndexID;

// The position of a field in a possibly compound index.
typedef size_t IndexPosition;

struct OrAssignment {
    OrAssignment() : counter(0) {}

    // Each child of an OR must be indexed for the OR to be indexed. When an OR moves to a
    // subsequent state it just asks all its children to move their states forward.

    // Must use all of subnodes.
    std::vector<MemoID> subnodes;

    // The number of OR states that we've enumerated so far.
    size_t counter;
};

struct LockstepOrAssignment {
    struct PreferFirstSubNode {
        MemoID memoId;
        size_t iterationCount;
        boost::optional<size_t> maxIterCount;
    };
    std::vector<PreferFirstSubNode> subnodes;

    bool exhaustedLockstepIteration = false;
    size_t totalEnumerated = 0;

    /**
     * Returns true if 'totalEnumerated' matches the total number of expected plans for this
     * assignment.
     */
    bool shouldResetBeforeProceeding(size_t totalEnumerated, size_t orLimit) const;

    /**
     * Returns true if each sub node is at the same iterationCount.
     */
    bool allIdentical() const;
};

// This is used by AndAssignment and is not an actual assignment.
struct OneIndexAssignment {
    // 'preds[i]' is uses index 'index' at position 'positions[i]'
    std::vector<MatchExpression*> preds;
    std::vector<IndexPosition> positions;
    IndexID index;

    // True if the bounds on 'index' for the leaf expressions in 'preds' can be intersected
    // and/or compounded, and false otherwise. If 'canCombineBounds' is set to false and
    // multiple predicates are assigned to the same position of a multikey index, then the
    // access planner should generate a self-intersection plan.
    bool canCombineBounds = true;

    // The expressions that should receive an OrPushdownTag when this assignment is made.
    std::vector<std::pair<MatchExpression*, OrPushdownTag::Destination>> orPushdowns;
};

template <typename H>
H AbslHashValue(H h, const OneIndexAssignment& assignment) {
    for (const auto& pred : assignment.preds) {
        h = H::combine(std::move(h), reinterpret_cast<size_t>(pred));
    }
    return H::combine_contiguous(
        std::move(h), assignment.positions.data(), assignment.positions.size());
}

struct AndEnumerableState {
    std::vector<OneIndexAssignment> assignments;
    std::vector<MemoID> subnodesToIndex;

    // Creates an AndEnumerableState with no subnodes and a single index assignment.
    static AndEnumerableState makeSingleton(OneIndexAssignment oneIndexAssignment);
};

struct AndAssignment {
    AndAssignment() : counter(0) {}

    std::vector<AndEnumerableState> choices;

    // We're on the counter-th member of state.
    size_t counter;
};

struct ArrayAssignment {
    ArrayAssignment() : counter(0) {}
    std::vector<MemoID> subnodes;
    size_t counter;
};

/**
 * Associates indices with predicates. This is the common node type for our tree.
 */
struct NodeAssignment {
    std::variant<OrAssignment, LockstepOrAssignment, AndAssignment, ArrayAssignment> assignment;
    std::string toString() const;
};

}  // namespace plan_enumerator
}  // namespace mongo
