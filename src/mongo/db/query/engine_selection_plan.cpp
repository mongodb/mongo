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
#include "mongo/db/query/engine_selection_plan.h"

#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

// Test-only failpoint that overrides plan-based engine selection using index names. When active,
// if the winning plan's IXSCAN uses the index named "sbe", SBE is forced; if it uses
// the index named "classic", classic is forced. This overrides the normal rules
// (GROUP/LOOKUP -> SBE, etc.) and exists solely to let JS tests exercise cross-engine replanning
// where a cached plan for one engine is evicted and replaced by one that uses the other engine.
MONGO_FAIL_POINT_DEFINE(engineSelectionOverrideByIndexName);

namespace {

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
void callPreVisit(Rule& rule, RuleEngine& engine, const N& node, size_t index) {
    if constexpr (HasPreVisit<Rule, N>) {
        if (engine.hasMatch())
            return;
        rule.preVisit(engine, node, index);
    }
}

template <class Rule, class N>
void callPostVisit(Rule& rule, RuleEngine& engine, const N& node) {
    if constexpr (HasPostVisit<Rule, N>) {
        if (engine.hasMatch())
            return;
        rule.postVisit(engine, node);
    }
}

template <class Rule>
void callFinish(Rule& rule, RuleEngine& engine) {
    if constexpr (HasFinish<Rule>) {
        if (engine.hasMatch())
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
const QuerySolutionNode* treeSearch(const QuerySolutionNode* root, Rules&&... rules) {
    RuleEngine engine;
    std::function<void(const QuerySolutionNode*, size_t)> walk = [&](const QuerySolutionNode* node,
                                                                     size_t index) {
        visit([&](const auto& node) { (callPreVisit(rules, engine, node, index), ...); }, *node);
        if (engine.hasMatch())
            return;

        for (size_t i = 0; i < node->children.size(); ++i) {
            walk(node->children[i].get(), i);
            if (engine.hasMatch())
                return;
        }

        visit([&](const auto& node) { (callPostVisit(rules, engine, node), ...); }, *node);
    };

    walk(root, 0);
    (callFinish(rules, engine), ...);

    return engine.getMatchedNode();
}

/**
 * Returns 'true' if the query solution tree at 'root' matches any of the rules defined by 'rules'.
 */
template <class... Rules>
bool treeMatchesAny(const QuerySolutionNode* root, Rules&&... rules) {
    return treeSearch(root, std::forward<Rules>(rules)...) != nullptr;
}

struct Range {
    size_t min;
    size_t max;  // Exclusive.

    Range() : Range(0) {}
    Range(size_t index) : min(index), max(index + 1) {}
    explicit Range(size_t amin, size_t amax) : min(amin), max(amax) {}

    bool contains(size_t childIndex) const {
        return childIndex >= min && childIndex < max;
    }
};

/**
 * This is a configurable rule that can match any tree pattern detectable by a state machine.
 *
 * To obtain a match, all the possible paths from root to leaf have to end in a matching state in
 * the state machine (similar to an all-paths NFA). This is a useful property for recognizing nodes
 * that have multiple branches.
 */
class StateMachineRule {
public:
    StateMachineRule() {
        // Create the special states. User-defined states come after them.
        _states.resize(kMaxReservedState);

        // Set start state.
        _stack.push(kStart);
    }

    int getStartState() const {
        return kStart;
    }

    /**
     * Creates a new state that transitions from 'state' when the machine receives the specified
     * node. Depending on how it is used, this can be the building block for both sequences and
     * alternations on the pattern being recognized.
     *
     * The state created is returned as an out parameter in 'state', so that the callers can chain
     * sequences more easily.
     */
    int addState(int state, StageType stage, Range allowedChildren) {
        validateState(state);
        int nextState = allocState();
        validateState(nextState);

        StateSpec& spec = _states[state];
        auto res = spec.edges.emplace(stage, Edge{allowedChildren, nextState});
        // If this triggers, it means we're trying to add two transitions with the same node type to
        // the same state. NFAs allow this, but it'd complicate our implementation and provide worse
        // performance.
        tassert(
            12308402, "Engine selection state machine doesn't have unique transitions", res.second);

        return nextState;
    }

    void addMatch(int state) {
        validateState(state);
        _states[state].isMatch = true;
    }

    void preVisit(RuleEngine& engine, const QuerySolutionNode& node, size_t index) {
        // Consume this QSN node and transition the state of the current root to leaf path.
        step(node.getType(), index);

        const bool isLeaf = node.children.empty();
        // If any root to leaf path ends in a non-matching state, it means the pattern didn't
        // recognize that branch. Once we unwind the traversal, we will reject the candidate tree.
        if (isLeaf && !isMatch()) {
            _allBranchesMatch = false;
        }
    }

    void postVisit(RuleEngine& engine, const QuerySolutionNode& node) {
        // Unwind the state so that we can keep matching other branches.
        _stack.pop();

        // Once we finish traversal, we match if all the branches were recognized.
        if (_stack.size() == 1 && _allBranchesMatch) {
            engine.match(&node);
        }
    }

private:
    enum {
        kNotMatch = 0,
        kStart = 1,
        kMaxReservedState = 2,
    };

    struct Edge {
        Range allowedChildren;
        int nextState;
    };

    struct StateSpec {
        // Transitions allowed from this state.
        std::map<StageType, Edge> edges;
        bool isMatch = false;
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

    void validateStack() const {
        tassert(12308403, "Invalid engine selection stack state", !_stack.empty());
    }

    bool isMatch() const {
        return currentState().isMatch;
    }

    const StateSpec& currentState() const {
        validateStack();
        validateState(_stack.top());
        return _states[_stack.top()];
    }

    void step(StageType stage, size_t index) {
        const StateSpec& state = currentState();

        auto it = state.edges.find(stage);
        if (it != state.edges.end() && it->second.allowedChildren.contains(index)) {
            // We have a partial match, so we transition to the next state.
            _stack.push(it->second.nextState);
        } else {
            // There's no match in this node or its children, so we transition to the not match
            // state. Once we reach the leaf nodes of this subtree, they will set
            // _allBranchesMatch=false.
            _stack.push(kNotMatch);
        }
    }

    void dumpStates() {
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
                std::cout << "  Stage " << stage << " [" << range.min << ", " << range.max
                          << ") -> State " << nextState << std::endl;
            }
        }
    }

    // State of each active frame in the QSN recursion. This is an useful state machine extension
    // for unwinding when exploring several branches.
    std::stack<int> _stack;
    // State machine description (very similar to a graph). The std::vector index provides the state
    // numbers and each state specification (vertex) contains the configured transitions (edges).
    std::vector<StateSpec> _states;
    bool _allBranchesMatch = true;
};

static_assert(HasPreVisit<StateMachineRule, QuerySolutionNode>);
static_assert(HasPostVisit<StateMachineRule, QuerySolutionNode>);

StateMachineRule makeLookupUnwindRule() {
    static constexpr int kMaxBranchesInSbe = 100;

    StateMachineRule sm;
    int state = sm.getStartState();

    // Collscan
    {
        state = sm.addState(sm.getStartState(), STAGE_COLLSCAN, 0);
        sm.addMatch(state);
    }

    const int fetch_state = sm.addState(sm.getStartState(), STAGE_FETCH, 0);

    // Ixscan + Fetch
    {
        state = sm.addState(fetch_state, STAGE_IXSCAN, 0);
        sm.addMatch(state);
    }

    // Ixscan + Sort + Fetch
    {
        state = sm.addState(fetch_state, STAGE_SORT_DEFAULT, 0);
        state = sm.addState(state, STAGE_IXSCAN, 0);
        sm.addMatch(state);
    }

    // Ixscan + Or + Fetch
    {
        state = sm.addState(fetch_state, STAGE_OR, 0);
        state = sm.addState(state, STAGE_IXSCAN, Range(0, kMaxBranchesInSbe));
        sm.addMatch(state);
    }

    // Ixscan + Fetch + Or
    {
        state = sm.addState(sm.getStartState(), STAGE_OR, 0);
        state = sm.addState(state, STAGE_FETCH, Range(0, kMaxBranchesInSbe));
        state = sm.addState(state, STAGE_IXSCAN, 0);
        sm.addMatch(state);
    }

    // Ixscan + SortedMerge + Fetch
    {
        state = sm.addState(fetch_state, STAGE_SORT_MERGE, 0);
        state = sm.addState(state, STAGE_IXSCAN, Range(0, kMaxBranchesInSbe));
        sm.addMatch(state);
    }

    // Ixscan + Fetch + SortedMerge
    {
        state = sm.addState(sm.getStartState(), STAGE_SORT_MERGE, 0);
        state = sm.addState(state, STAGE_FETCH, Range(0, kMaxBranchesInSbe));
        state = sm.addState(state, STAGE_IXSCAN, 0);
        sm.addMatch(state);
    }
    return sm;
}

/**
 * This pattern matches when SBE must be used for the input plan. The matched node points to the
 * top of the section that will be pushed down to SBE.
 *
 * For example, when we receive a $LU query with a disabled data access plan for a LU stage, the
 * top of the SBE section will point to the next SBE-eligible stage. Otherwise, if there are no
 * disabled data access plans, we match the entire tree.
 *
 */
class PlanPushdownSelector {
public:
    PlanPushdownSelector(bool containsLuPattern) : _containsLuPattern(containsLuPattern) {}

    void preVisit(RuleEngine&, const GroupNode& node, size_t) {
        preVisitBase(node);
        _enableSbe = true;
    }

    void preVisit(RuleEngine&, const EqLookupNode& node, size_t) {
        preVisitBase(node);

        if (!node.unwindSpec) {
            // $lookup case.
            _enableSbe = true;
            return;
        }

        // $lookup + $unwind case.
        if (_containsLuPattern) {
            _enableSbe = true;
        } else {
            // Reset the cut point, since this LU node has to be left out from the SBE plan. If we
            // have a non-LU child, it'll become the next cut point. If it's a LU child, it'll also
            // be excluded from the SBE plan.
            //
            // We also disable SBE for now, since this might be the bottom-most node in the QSN,
            // which would make us disable SBE for this QSN.
            _enableSbe = false;
            _cutPoint = nullptr;
        }
    }

    void preVisit(RuleEngine& engine, const QuerySolutionNode& node, size_t) {
        preVisitBase(node);
    }

    void finish(RuleEngine& engine) {
        if (_enableSbe && _cutPoint)
            engine.match(_cutPoint);
    }

private:
    // 'true' when the solution tree contains a LookupUnwind pattern, 'false' otherwise.
    bool _containsLuPattern = false;

    // Indicates whether the query (either as a whole or after a cut) must be executed in SBE or
    // not. It's set to true by nodes that trigger SBE usage (e.g. GroupNode, EqLookupNode, or
    // EqLookupUnwindNode).
    bool _enableSbe = false;

    // Represents the topmost QSN that must run in SBE. This is chosen so as to leave out the nodes
    // with disabled patterns.
    const QuerySolutionNode* _cutPoint = nullptr;

    void preVisitBase(const QuerySolutionNode& node) {
        if (!_cutPoint) {
            _cutPoint = &node;
        }
    }
};

static_assert(HasPreVisit<PlanPushdownSelector, GroupNode>);
static_assert(HasPreVisit<PlanPushdownSelector, EqLookupNode>);
static_assert(HasPreVisit<PlanPushdownSelector, QuerySolutionNode>);
static_assert(HasFinish<PlanPushdownSelector>);

/**
 * This rule matches:
 * 1. A query solution that has at least one DISTINCT_SCAN node.
 */
class DistinctScanRule {
public:
    void preVisit(RuleEngine& engine, const DistinctNode& node, size_t) {
        engine.match(&node);
    }
};
static_assert(HasPreVisit<DistinctScanRule, DistinctNode>);

/**
 * This rule matches:
 * 1. A query solution that has at least one IXSCAN, whose selected key pattern contains both a
 * hashed index and a dotted path for it (SERVER-99889).
 */
class HashedIndexScanPatternRule {
public:
    void preVisit(RuleEngine& engine, const IndexScanNode& node, size_t) {
        if (indexHasHashedPathPrefixOfNonHashedPath(node.index.keyPattern)) {
            engine.match(&node);
        }
    }
};
static_assert(HasPreVisit<HashedIndexScanPatternRule, IndexScanNode>);

/**
 * Test-only rule that matches when the plan contains an IXSCAN whose catalog name equals
 * 'targetIndexName'. Used by the engineSelectionOverrideByIndexName failpoint.
 */
class IndexNameRule_ForTest {
public:
    explicit IndexNameRule_ForTest(StringData targetIndexName)
        : _targetIndexName(targetIndexName) {}

    void preVisit(RuleEngine& engine, const IndexScanNode& node, size_t) {
        if (node.index.identifier.catalogName == _targetIndexName) {
            engine.match(&node);
        }
    }

private:
    StringData _targetIndexName;
};
static_assert(HasPreVisit<IndexNameRule_ForTest, IndexScanNode>);

/**
 * This rule matches:
 * 1. A query solution that has at least one AND_HASH or AND_SORTED node. (SERVER-90818).
 */
class AndHashOrSortedRule {
public:
    void preVisit(RuleEngine& engine, const AndSortedNode& node, size_t) {
        engine.match(&node);
    }
    void preVisit(RuleEngine& engine, const AndHashNode& node, size_t) {
        engine.match(&node);
    }
};
static_assert(HasPreVisit<AndHashOrSortedRule, AndSortedNode>);
static_assert(HasPreVisit<AndHashOrSortedRule, AndHashNode>);
}  // namespace

bool isPlanSbeEligible(const QuerySolution* solution) {
    return !treeMatchesAny(
        solution->root(), DistinctScanRule(), HashedIndexScanPatternRule(), AndHashOrSortedRule());
}

EngineSelectionResult engineSelectionForPlan(const QuerySolution* solution,
                                             const QuerySolutionNode* dataAccessNode) {
    LOGV2_DEBUG(11986305,
                1,
                "Plan-based engine selection logic invoked.",
                "solution"_attr = solution->toString());

    // Test-only: when the failpoint is active, override engine selection based on the index name
    // used by the winning plan's IXSCAN. An IXSCAN named "sbe" forces SBE; an IXSCAN named
    // "classic" forces classic. This takes precedence over all other rules.
    if (auto scoped = engineSelectionOverrideByIndexName.scoped();
        MONGO_unlikely(scoped.isActive())) {
        if (treeMatchesAny(solution->root(), IndexNameRule_ForTest("sbe"))) {
            return {EngineChoice::kSbe, solution->root()};
        }
        if (treeMatchesAny(solution->root(), IndexNameRule_ForTest("classic"))) {
            return {EngineChoice::kClassic, nullptr};
        }
    }

    bool containsLuPattern = treeMatchesAny(dataAccessNode, makeLookupUnwindRule());
    const QuerySolutionNode* planPushdownRoot =
        treeSearch(solution->root(), PlanPushdownSelector(containsLuPattern));

    return {planPushdownRoot ? EngineChoice::kSbe : EngineChoice::kClassic, planPushdownRoot};
}

bool indexHasHashedPathPrefixOfNonHashedPath(const BSONObj& keyPattern) {
    boost::optional<StringData> hashedPath;
    for (const auto& elt : keyPattern) {
        if (elt.valueStringDataSafe() == "hashed") {
            // Indexes may only contain one hashed field.
            hashedPath = elt.fieldNameStringData();
            break;
        }
    }
    if (hashedPath == boost::none) {
        // No hashed fields in the index.
        return false;
    }
    // Check if 'hashedPath' is a path prefix for any field in the index.
    for (const auto& elt : keyPattern) {
        if (expression::isPathPrefixOf(hashedPath.get(), elt.fieldNameStringData())) {
            return true;
        }
    }
    return false;
}

}  // namespace mongo
