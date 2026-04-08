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
concept HasPreVisit =
    requires(Rule& rule, RuleEngine& engine, const N& node) { rule.preVisit(engine, node); };

template <class Rule, class N>
concept HasPostVisit =
    requires(Rule& rule, RuleEngine& engine, const N& node) { rule.postVisit(engine, node); };

template <class Rule>
concept HasFinish = requires(Rule& rule, RuleEngine& engine) { rule.finish(engine); };

template <class Rule, class N>
void callPreVisit(Rule& rule, RuleEngine& engine, const N& node) {
    if constexpr (HasPreVisit<Rule, N>) {
        if (engine.hasMatch())
            return;
        rule.preVisit(engine, node);
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
 *    void preVisit(RuleEngine& engine, const QuerySolutionNode& node);
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
 *    void preVisit(RuleEngine& engine, const IndexScanNode& node);     // (1)
 *    void preVisit(RuleEngine& engine, const QuerySolutionNode& node); // (2)
 * };
 */
template <class... Rules>
const QuerySolutionNode* treeSearch(const QuerySolutionNode* root, Rules&&... rules) {
    RuleEngine engine;
    std::function<void(const QuerySolutionNode*)> walk = [&](const QuerySolutionNode* node) {
        visit([&](const auto& node) { (callPreVisit(rules, engine, node), ...); }, *node);
        if (engine.hasMatch())
            return;

        for (const auto& child : node->children) {
            walk(child.get());
            if (engine.hasMatch())
                return;
        }

        visit([&](const auto& node) { (callPostVisit(rules, engine, node), ...); }, *node);
    };

    walk(root);
    (callFinish(rules, engine), ...);

    return engine.getMatchedNode();
}

/**
 * Returns 'true' if the query solution tree at 'node' matches any of the rules defined by 'rules'.
 */
template <class... Rules>
bool treeMatchesAny(const QuerySolutionNode* root, Rules&&... rules) {
    return treeSearch(root, std::forward<Rules>(rules)...) != nullptr;
}

/**
 * This rule matches when:
 * 1. There is at least one LOOKUP_UNWIND in the tree.
 *
 * TODO SERVER-117922: Implement this rule.
 */
class LookupUnwindRule {
public:
    void preVisit(RuleEngine& engine, const EqLookupNode& node) {
        if (node.unwindSpec) {
            engine.match(&node);
        }
    }
};

/**
 * This pattern matches when SBE must be used for the input plan. The matched node points to the top
 * of the section that will be pushed down to SBE.
 *
 * For example, when we receive a $LU query with a disabled data access plan for a LU stage, the top
 * of the SBE section will point to the next SBE-eligible stage. Otherwise, if there are no disabled
 * data access plans, we match the entire tree.
 *
 */
class PlanPushdownSelector {
public:
    PlanPushdownSelector(bool containsLuPattern) : _containsLuPattern(containsLuPattern) {}

    void preVisit(RuleEngine&, const GroupNode& node) {
        preVisitBase(node);
        _enableSbe = true;
    }

    void preVisit(RuleEngine&, const EqLookupNode& node) {
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

    void preVisit(RuleEngine& engine, const QuerySolutionNode& node) {
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
    void preVisit(RuleEngine& engine, const DistinctNode& node) {
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
    void preVisit(RuleEngine& engine, const IndexScanNode& node) {
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

    void preVisit(RuleEngine& engine, const IndexScanNode& node) {
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
    void preVisit(RuleEngine& engine, const AndSortedNode& node) {
        engine.match(&node);
    }
    void preVisit(RuleEngine& engine, const AndHashNode& node) {
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

EngineSelectionResult engineSelectionForPlan(const QuerySolution* solution) {
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

    bool containsLuPattern = treeMatchesAny(solution->root(), LookupUnwindRule());
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
