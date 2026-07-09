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

#pragma once

#include "mongo/db/exec/container_size_helper.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"

#include <span>

/**
 * Framework for matching patterns in QuerySolutions.
 */
namespace mongo::query_solution_analyzer {

class RuleEngine {
public:
    // 'node': represents the node where the pattern was found.
    void match(const QuerySolutionNode* node) {
        _matchedNode = node;
    }

    bool hasMatch() const {
        return _matchedNode;
    }

    const QuerySolutionNode* getMatchedNode() const {
        return _matchedNode;
    }

private:
    const QuerySolutionNode* _matchedNode = nullptr;
};

template <class Rule, class N>
concept HasPreVisit = requires(Rule& rule, RuleEngine& engine, const N& node, size_t index) {
    rule.preVisit(engine, node, index);
};

template <class Rule, class N>
concept HasPostVisit =
    requires(Rule& rule, RuleEngine& engine, const N& node) { rule.postVisit(engine, node); };

template <class Rule>
concept HasFinish = requires(Rule& rule, RuleEngine& engine) { rule.finish(engine); };

template <class Rule, class N>
void callPreVisit(
    Rule& rule, RuleEngine& engine, const N& node, size_t index, bool allowEarlyExit) {
    if constexpr (HasPreVisit<Rule, N>) {
        if (allowEarlyExit && engine.hasMatch())
            return;
        rule.preVisit(engine, node, index);
    }
}

template <class Rule, class N>
void callPostVisit(Rule& rule, RuleEngine& engine, const N& node, bool allowEarlyExit) {
    if constexpr (HasPostVisit<Rule, N>) {
        if (allowEarlyExit && engine.hasMatch())
            return;
        rule.postVisit(engine, node);
    }
}

template <class Rule>
void callFinish(Rule& rule, RuleEngine& engine, bool allowEarlyExit) {
    if constexpr (HasFinish<Rule>) {
        if (allowEarlyExit && engine.hasMatch())
            return;
        rule.finish(engine);
    }
}

template <class F>
void visit(F&& f, const QuerySolutionNode& node) {
    // We'll add specializations as the rules need them.
    switch (node.getType()) {
        case STAGE_EQ_LOOKUP:
        case STAGE_EQ_LOOKUP_UNWIND:
            f(static_cast<const EqLookupNode&>(node));
            break;
        case STAGE_GROUP:
            f(static_cast<const GroupNode&>(node));
            break;
        case STAGE_COLLSCAN:
            f(static_cast<const CollectionScanNode&>(node));
            break;
        case STAGE_IXSCAN:
            f(static_cast<const IndexScanNode&>(node));
            break;
        case STAGE_DISTINCT_SCAN:
            f(static_cast<const DistinctNode&>(node));
            break;
        case STAGE_AND_HASH:
            f(static_cast<const AndHashNode&>(node));
            break;
        case STAGE_AND_SORTED:
            f(static_cast<const AndSortedNode&>(node));
            break;
        default:
            f(node);
            break;
    }
}

/**
 * Returns a pointer to the node in the query solution tree at 'root' that is returned by the first
 * rule in 'rules' that triggers a match.
 *
 * Each rule is implemented as a separate visitor, and can be conceived as a state machine that
 * receives tree nodes in pre/post order and returns whether they correspond to the pattern the rule
 * identifies.
 *
 * The basic structure of a rule is:
 *
 * class MyRule {
 *    void finish(RuleEngine& engine);
 *    void preVisit(RuleEngine& engine, const QuerySolutionNode& node, size_t index);
 *    void postVisit(RuleEngine& engine, const QuerySolutionNode& node);
 * };
 *
 * All the methods are called via static dispatch and are optional, so rules can opt-in or opt-out
 * of methods as their implementation requires.
 *
 * Since preVisit/postVisit are called like any other method, function overloading resolution rules
 * apply normally, which means that rules can specialize the treatment of specific node types or
 * treat them generically. In the example below, nodes of type 'IndexScanNode' will be passed to (1)
 * and other node types to (2).
 *
 * class MyRule2 {
 * public:
 *    void preVisit(RuleEngine& engine, const IndexScanNode& node, size_t index);     // (1)
 *    void preVisit(RuleEngine& engine, const QuerySolutionNode& node, size_t index); // (2)
 * };
 */
template <class... Rules>
const QuerySolutionNode* _treeSearch(const QuerySolutionNode* root,
                                     bool allowEarlyExit,
                                     Rules&&... rules) {
    RuleEngine engine;
    std::function<void(const QuerySolutionNode*, size_t)> walk = [&](const QuerySolutionNode* node,
                                                                     size_t index) {
        visit(
            [&](const auto& node) {
                (callPreVisit(rules, engine, node, index, allowEarlyExit), ...);
            },
            *node);
        if (allowEarlyExit && engine.hasMatch())
            return;

        for (size_t i = 0; i < node->children.size(); ++i) {
            walk(node->children[i].get(), i);
            if (allowEarlyExit && engine.hasMatch())
                return;
        }

        visit([&](const auto& node) { (callPostVisit(rules, engine, node, allowEarlyExit), ...); },
              *node);
    };

    walk(root, 0);
    (callFinish(rules, engine, allowEarlyExit), ...);

    return engine.getMatchedNode();
}

template <class... Rules>
const QuerySolutionNode* treeSearch(const QuerySolutionNode* root, Rules&&... rules) {
    return _treeSearch(root, true /* allowEarlyExit */, rules...);
}

/**
 * Returns 'true' if the query solution tree at 'root' matches any of the rules defined by 'rules'.
 */
template <class... Rules>
bool treeMatchesAny(const QuerySolutionNode* root, bool allowEarlyExit, Rules&&... rules) {
    return _treeSearch(root, allowEarlyExit, std::forward<Rules>(rules)...) != nullptr;
}

struct Range {
    size_t min;
    size_t max;  // Exclusive.

    Range() : Range(0) {}
    Range(size_t index) : min(index), max(index + 1) {}
    explicit Range(size_t amin, size_t amax) : min(amin), max(amax) {}

    bool operator==(const Range&) const = default;

    bool contains(size_t childIndex) const {
        return childIndex >= min && childIndex < max;
    }
};

// A further predicate applied on top of a stage-type match. kNone means the stage match alone
// is sufficient; other values impose additional conditions on the matched node.
enum class PredicateType {
    kNone,
    kSortWithoutAbsorbedLimit,
};

/**
 * This is a configurable rule that can match any tree pattern detectable by a state machine.
 * Should be immutable after being built.
 */
class StateMachine {
public:
    StateMachine();

    int getStartState() const {
        return kStart;
    }

    /**
     * Creates a new state that transitions from 'state' when the machine receives a node of type
     * 'stage' that also satisfies 'allowedChildren' and the optional predicate 'predicate'.
     * Depending on how it is used, this can be the building block for both sequences and
     * alternations on the pattern being recognized.
     *
     * The return value represents the created state, which can be chained together with other
     * states.
     */
    int addState(int state,
                 StageType stage,
                 Range allowedChildren,
                 PredicateType predicate = PredicateType::kNone);

    /**
     * Like 'addState' above, but if 'state' already has a transition for this stage (because an
     * earlier pattern that shares this prefix added it), returns that existing destination state
     * instead of adding a conflicting duplicate. This lets several patterns be assembled into one
     * shared trie by simply walking each pattern's path step by step.
     */
    int addOrGetState(int state,
                      StageType stage,
                      Range allowedChildren,
                      PredicateType predicate = PredicateType::kNone);

    void addMatch(int state) {
        validateState(state);
        _states[state].isMatch = true;
    }

    void addMatchForCounter(int state, int matchTag) {
        validateState(state);
        _states[state].isMatch = true;
        _states[state].matchTag = matchTag;
    }

    void shrinkToFit() {
        _states.shrink_to_fit();
    }

    /**
     * Estimate memory usage in bytes of this object.
     */
    size_t sizeInBytes() const {
        return sizeof(*this) +
            container_size_helper::estimateObjectSizeInBytes(
                   _states,
                   [](const StateSpec& state) {
                       return container_size_helper::estimateObjectSizeInBytes(state.edges);
                   },
                   true /* includeShallowSize */);
    }

private:
    friend class StateMachineMatcher;

    enum {
        kNotMatch = 0,
        kStart = 1,
        kMaxReservedState = 2,
    };

    struct Edge {
        PredicateType predicate = PredicateType::kNone;
        Range allowedChildren;
        int nextState;
    };

    struct StateSpec {
        // Transitions allowed from this state, keyed by the stage type of the incoming node.
        std::map<StageType, Edge> edges;
        bool isMatch = false;
        // Identifies the pattern that this match state completes. boost::none for
        // machines whose matches are only consumed as booleans.
        boost::optional<int> matchTag = boost::none;
    };

    int allocState() {
        int state = _states.size();
        _states.emplace_back();
        return state;
    }

    void validateState(int state) const {
        tassert(
            12308401, "Invalid tree recognition state", state < static_cast<int>(_states.size()));
    }

    // State machine description (very similar to a graph). The std::vector index provides the state
    // numbers and each state specification (vertex) contains the configured transitions (edges).
    std::vector<StateSpec> _states;
};

/**
 * Matches patterns in a QuerySolution by referencing the given StateMachine rule.
 *
 * To obtain a match, all the possible paths from root to leaf have to end in a matching state in
 * the state machine (similar to an all-paths NFA). This is a useful property for recognizing nodes
 * that have multiple branches.
 */
class StateMachineMatcher {
public:
    explicit StateMachineMatcher(const StateMachine& machine, bool ignoreNonEssentialNodes = false);

    StateMachineMatcher(StateMachine&&) = delete;

    void preVisit(RuleEngine& engine, const QuerySolutionNode& node, size_t index);

    boost::optional<int> getMatchedTag() const {
        return _matchedTag;
    }

    void postVisit(RuleEngine& engine, const QuerySolutionNode& node);

private:
    void validateStack() const {
        tassert(12308403, "Invalid engine selection stack state", !_stack.empty());
    }

    bool isMatch() const {
        return currentState().isMatch;
    }

    const StateMachine::StateSpec& currentState() const {
        validateStack();
        _machine.validateState(_stack.top());
        return _machine._states[_stack.top()];
    }

    bool matchesPredicate(const StateMachine::Edge& edge,
                          const QuerySolutionNode& node,
                          size_t index) const;

    void step(const QuerySolutionNode& node, size_t index);

    bool isStageIgnoredForCounters(const QuerySolutionNode& node) const;

    void dumpStates();

    const StateMachine& _machine;
    // State of each active frame in the QSN recursion. This is an useful state machine extension
    // for unwinding when exploring several branches.
    std::stack<int> _stack;
    bool _allBranchesMatch = true;
    // The tag recorded at the first leaf that ended in a tagged match state; any later leaf ending
    // in a match state with a different tag rejects the tree.
    boost::optional<int> _matchedTag = boost::none;
    // When true, non-plan-shape nodes are ignored during matching.
    bool _ignoreNonEssentialNodes = false;
};
static_assert(HasPreVisit<StateMachineMatcher, QuerySolutionNode>);
static_assert(HasPostVisit<StateMachineMatcher, QuerySolutionNode>);

}  // namespace mongo::query_solution_analyzer
