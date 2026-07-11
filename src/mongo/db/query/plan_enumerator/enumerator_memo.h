// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

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
