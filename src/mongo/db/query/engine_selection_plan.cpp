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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {

class RuleEngine {
public:
    void match() {
        _match = true;
    }

    bool hasMatch() const {
        return _match;
    }

private:
    bool _match = false;
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
        rule.preVisit(engine, node);
    }
}

template <class Rule, class N>
void callPostVisit(Rule& rule, RuleEngine& engine, const N& node) {
    if constexpr (HasPostVisit<Rule, N>) {
        rule.postVisit(engine, node);
    }
}

template <class Rule>
void callFinish(Rule& rule, RuleEngine& engine) {
    if constexpr (HasFinish<Rule>) {
        rule.finish(engine);
    }
}

template <class F>
void visit(F&& f, const QuerySolutionNode& node) {
    // We'll add specializations as the rules need them.
    switch (node.getType()) {
        case STAGE_EQ_LOOKUP:
            f(static_cast<const EqLookupNode&>(node));
            break;
        case STAGE_GROUP:
            f(static_cast<const GroupNode&>(node));
            break;
        case STAGE_IXSCAN:
            f(static_cast<const IndexScanNode&>(node));
            break;
        case STAGE_EQ_LOOKUP_UNWIND:
            f(static_cast<const EqLookupUnwindNode&>(node));
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
 * Returns 'true' if the query solution tree 'solution' matches any of the rules defined by 'rules'.
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
bool treeMatchesAny(const QuerySolution* solution, Rules&&... rules) {
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

    walk(solution->root());
    if (!engine.hasMatch())
        (callFinish(rules, engine), ...);

    return engine.hasMatch();
}

/**
 * This rule matches when:
 * 1. There is at least one GROUP in the tree.
 */
class GroupRule {
public:
    void preVisit(RuleEngine& engine, const GroupNode& node) {
        engine.match();
    }
};
static_assert(HasPreVisit<GroupRule, GroupNode>);

/**
 * This rule matches when:
 * 1. There is at least one LOOKUP in the tree.
 */
class LookupRule {
public:
    void preVisit(RuleEngine& engine, const EqLookupNode& node) {
        engine.match();
    }
};
static_assert(HasPreVisit<LookupRule, EqLookupNode>);

/**
 * This rule matches when:
 * 1. There is at least one LOOKUP_UNWIND in the tree.
 *
 * TODO SERVER-117922: Implement this rule.
 */
class LookupUnwindRule {
public:
    void preVisit(RuleEngine& engine, const EqLookupUnwindNode& node) {
        engine.match();
    }
};
static_assert(HasPreVisit<LookupUnwindRule, EqLookupUnwindNode>);

/**
 * This rule matches:
 * 1. A query solution that has at least one DISTINCT_SCAN node.
 */
class DistinctScanRule {
public:
    void preVisit(RuleEngine& engine, const DistinctNode& node) {
        engine.match();
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
            engine.match();
        }
    }
};
static_assert(HasPreVisit<HashedIndexScanPatternRule, IndexScanNode>);

/**
 * This rule matches:
 * 1. A query solution that has at least one AND_HASH or AND_SORTED node. (SERVER-90818).
 */
class AndHashOrSortedRule {
public:
    void preVisit(RuleEngine& engine, const AndSortedNode& node) {
        engine.match();
    }
    void preVisit(RuleEngine& engine, const AndHashNode& node) {
        engine.match();
    }
};
static_assert(HasPreVisit<AndHashOrSortedRule, AndSortedNode>);
static_assert(HasPreVisit<AndHashOrSortedRule, AndHashNode>);

}  // namespace

bool isPlanSbeEligible(const QuerySolution* solution) {
    return !treeMatchesAny(
        solution, DistinctScanRule(), HashedIndexScanPatternRule(), AndHashOrSortedRule());
}

EngineChoice engineSelectionForPlan(const QuerySolution* solution) {
    LOGV2_DEBUG(11986305,
                1,
                "Plan-based engine selection logic invoked.",
                "solution"_attr = solution->toString());
    return treeMatchesAny(solution, LookupUnwindRule(), LookupRule(), GroupRule())
        ? EngineChoice::kSbe
        : EngineChoice::kClassic;
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
