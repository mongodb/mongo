/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_solution_analyzer.h"

namespace mongo::query_solution_analyzer {

StateMachineRule::StateMachineRule(bool ignoreNonEssentialNodes)
    : _ignoreNonEssentialNodes(ignoreNonEssentialNodes) {
    // Create the special states. User-defined states come after them.
    _states.resize(kMaxReservedState);

    // Set start state.
    _stack.push(kStart);
}

int StateMachineRule::addState(int state,
                               StageType stage,
                               Range allowedChildren,
                               PredicateType predicate) {
    validateState(state);
    int nextState = allocState();
    validateState(nextState);

    StateSpec& spec = _states[state];
    auto res = spec.edges.emplace(stage, Edge{predicate, allowedChildren, nextState});
    // If this triggers, it means we're trying to add two transitions with the same stage type
    // to the same state. NFAs allow this, but it'd complicate our implementation and provide
    // worse performance.
    tassert(12308402, "Engine selection state machine doesn't have unique transitions", res.second);

    return nextState;
}

int StateMachineRule::addState(int state,
                               std::initializer_list<StageType> stages,
                               Range allowedChildren,
                               PredicateType predicate) {
    validateState(state);
    int nextState = allocState();
    validateState(nextState);

    StateSpec& spec = _states[state];
    for (const StageType stage : stages) {
        auto res = spec.edges.emplace(stage, Edge{predicate, allowedChildren, nextState});
        tassert(
            11907600, "Engine selection state machine doesn't have unique transitions", res.second);
    }

    return nextState;
}

void StateMachineRule::preVisit(RuleEngine& engine, const QuerySolutionNode& node, size_t index) {
    if (isStageIgnoredForCounters(node)) {
        // Don't consume a state transition or push a stack frame, so this node's child is
        // matched exactly as if the node weren't present.
    } else {
        // Consume this QSN node and transition the state of the current root to leaf path.
        step(node, index);
    }

    const bool isLeaf = node.children.empty();
    if (isLeaf) {
        if (!isMatch()) {
            // If any root to leaf path ends in a non-matching state, it means the pattern
            // didn't recognize that branch. Once we unwind the traversal, we will reject the
            // candidate tree.
            _allBranchesMatch = false;
        } else if (const auto& tag = currentState().matchTag; tag) {
            if (!_matchedTag) {
                _matchedTag = tag;
            } else if (_matchedTag != tag) {
                // The leaves ended in match states of two different patterns, so the tree as a
                // whole follows no single pattern.
                _allBranchesMatch = false;
            }
        }
    }
}

void StateMachineRule::postVisit(RuleEngine& engine, const QuerySolutionNode& node) {
    if (isStageIgnoredForCounters(node)) {
        // preVisit() pushed no frame for this node, so there is nothing to unwind.
        return;
    }

    // Unwind the state so that we can keep matching other branches.
    _stack.pop();

    // Once we finish traversal, we match if all the branches were recognized.
    if (_stack.size() == 1 && _allBranchesMatch) {
        engine.match(&node);
    }
}

bool StateMachineRule::matchesPredicate(const Edge& edge,
                                        const QuerySolutionNode& node,
                                        size_t index) const {
    if (!edge.allowedChildren.contains(index)) {
        return false;
    }

    switch (edge.predicate) {
        case PredicateType::kNone:
            return true;
        case PredicateType::kSortWithoutAbsorbedLimit:
            tassert(12594101,
                    "The SortWithoutAbsorbedLimit predicate can only be applied to SORT nodes",
                    node.getType() == STAGE_SORT_DEFAULT);
            return static_cast<const SortNode&>(node).limit == 0;
    }

    MONGO_UNREACHABLE;
}

void StateMachineRule::step(const QuerySolutionNode& node, size_t index) {
    const StateSpec& state = currentState();

    auto it = state.edges.find(node.getType());
    if (it != state.edges.end() && matchesPredicate(it->second, node, index)) {
        // We have a partial match, so we transition to the next state.
        _stack.push(it->second.nextState);
    } else {
        // There's no match in this node or its children, so we transition to the not match
        // state. Once we reach the leaf nodes of this subtree, they will set
        // _allBranchesMatch=false.
        _stack.push(kNotMatch);
    }
}

bool StateMachineRule::isStageIgnoredForCounters(const QuerySolutionNode& node) const {
    if (!_ignoreNonEssentialNodes) {
        return false;
    }
    switch (node.getType()) {
        case STAGE_SKIP:
        case STAGE_LIMIT:
        case STAGE_SHARDING_FILTER:
        case STAGE_SORT_KEY_GENERATOR:
        case STAGE_RETURN_KEY:
            return true;
        default:
            return false;
    }
}

void StateMachineRule::dumpStates() {
    for (size_t i = 0; i < _states.size(); ++i) {
        std::cout << "State " << i << ":";
        if (i == kStart)
            std::cout << " [START]";
        if (i == kNotMatch)
            std::cout << " [NOT_MATCH]";
        if (_states[i].isMatch)
            std::cout << " [MATCH]";
        std::cout << std::endl;

        for (const auto& [stage, edge] : _states[i].edges) {
            const auto& range = edge.allowedChildren;
            const auto& nextState = edge.nextState;

            std::cout << "  Stage(" << stage << ")";
            if (edge.predicate == PredicateType::kSortWithoutAbsorbedLimit) {
                std::cout << " + SortWithoutAbsorbedLimit";
            }

            std::cout << " [" << range.min << ", " << range.max << ") -> State " << nextState
                      << std::endl;
        }
    }
}
}  // namespace mongo::query_solution_analyzer
