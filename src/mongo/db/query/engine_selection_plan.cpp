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

#include "mongo/db/feature_flag.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_solution_analyzer.h"
#include "mongo/util/fail_point.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using namespace query_solution_analyzer;

// Test-only failpoint that overrides plan-based engine selection using index names. When active,
// if the winning plan's IXSCAN uses the index named "sbe", SBE is forced; if it uses
// the index named "classic", classic is forced. This overrides the normal rules
// (GROUP/LOOKUP -> SBE, etc.) and exists solely to let JS tests exercise cross-engine replanning
// where a cached plan for one engine is evicted and replaced by one that uses the other engine.
MONGO_FAIL_POINT_DEFINE(engineSelectionOverrideByIndexName);

namespace {

// Builds a state machine rule that matches the enabled local-side data access patterns for
// $lookup-$unwind (LU), gated by the three local access-plan IFR flags.
StateMachineRule makeLookupUnwindRule(IncrementalFeatureRolloutContext& ifrContext) {
    const bool collscanEnabled =
        ifrContext.getSavedFlagValue(feature_flags::gFeatureFlagSbeEqLookupUnwindLocalCollscan);
    const bool ixscanFetchEnabled =
        ifrContext.getSavedFlagValue(feature_flags::gFeatureFlagSbeEqLookupUnwindLocalIxscanFetch);
    const bool complexEnabled = ifrContext.getSavedFlagValue(
        feature_flags::gFeatureFlagSbeEqLookupUnwindLocalComplexDataAccessPlans);
    static constexpr int kMaxBranchesInSbe = 100;

    StateMachineRule sm;
    int state = sm.getStartState();

    if (collscanEnabled) {
        state = sm.addState(sm.getStartState(), STAGE_COLLSCAN, 0);
        sm.addMatch(state);
    }

    // Both ixscanFetch and complex patterns share a FETCH transition from the start state, so they
    // are grouped here to ensure that transition is added only once.
    if (ixscanFetchEnabled || complexEnabled) {
        const int fetch_state = sm.addState(sm.getStartState(), STAGE_FETCH, 0);

        // Ixscan + Fetch
        if (ixscanFetchEnabled) {
            state = sm.addState(fetch_state, STAGE_IXSCAN, 0);
            sm.addMatch(state);
        }

        if (complexEnabled) {
            // Ixscan + Sort (SortNodeDefault) + Fetch
            {
                state = sm.addState(
                    fetch_state, STAGE_SORT_DEFAULT, 0, PredicateType::kSortWithoutAbsorbedLimit);
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
        }
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
    PlanPushdownSelector(bool containsLuPattern, IncrementalFeatureRolloutContext& ifrContext)
        : _containsLuPattern(containsLuPattern), _ifrContext(ifrContext) {}

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

        // $lookup + $unwind case: both the local-side access plan flag and the strategy flag must
        // be enabled for this LU node to be pushed into SBE.
        if (_containsLuPattern && _isStrategyEnabled(node.lookupStrategy)) {
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
    // Returns true if the IFR flag for the given LU join strategy is enabled, meaning the strategy
    // is eligible for SBE pushdown.
    bool _isStrategyEnabled(EqLookupNode::LookupStrategy strategy) const {
        switch (strategy) {
            case EqLookupNode::LookupStrategy::kHashJoin:
                return _ifrContext.getSavedFlagValue(
                    feature_flags::gFeatureFlagSbeEqLookupUnwindHashJoin);
            case EqLookupNode::LookupStrategy::kIndexedLoopJoin:
                return _ifrContext.getSavedFlagValue(
                    feature_flags::gFeatureFlagSbeEqLookupUnwindIndexedLoopJoin);
            case EqLookupNode::LookupStrategy::kNestedLoopJoin:
                return _ifrContext.getSavedFlagValue(
                    feature_flags::gFeatureFlagSbeEqLookupUnwindNestedLoopJoin);
            case EqLookupNode::LookupStrategy::kDynamicIndexedLoopJoin:
                return _ifrContext.getSavedFlagValue(
                    feature_flags::gFeatureFlagSbeEqLookupUnwindDynamicIndexedLoopJoin);
            // The foreign collection doesn't exist, so the join produces no rows — semantically
            // equivalent to NLJ with an empty inner side. Gate it by the same flag so operators
            // can disable both failure modes together.
            case EqLookupNode::LookupStrategy::kNonExistentForeignCollection:
                return _ifrContext.getSavedFlagValue(
                    feature_flags::gFeatureFlagSbeEqLookupUnwindNestedLoopJoin);
        }
        MONGO_UNREACHABLE;
    }

    // 'true' when the solution tree contains a LookupUnwind access plan pattern, 'false' otherwise.
    bool _containsLuPattern = false;

    // Used to read strategy IFR flags with per-operation snapshot semantics.
    IncrementalFeatureRolloutContext& _ifrContext;

    // Indicates whether the query (either as a whole or after a cut) must be executed in SBE or
    // not. It's set to true by nodes that trigger SBE usage (e.g. GroupNode or EqLookupNode;
    // both $lookup and $lookup+$unwind are represented as EqLookupNode).
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
    explicit IndexNameRule_ForTest(std::string_view targetIndexName)
        : _targetIndexName(targetIndexName) {}

    void preVisit(RuleEngine& engine, const IndexScanNode& node, size_t) {
        if (node.index.identifier.catalogName == _targetIndexName) {
            engine.match(&node);
        }
    }

private:
    std::string_view _targetIndexName;
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

bool isPlanSbeCompatible(const QuerySolution* solution) {
    return !treeMatchesAny(
        solution->root(), DistinctScanRule(), HashedIndexScanPatternRule(), AndHashOrSortedRule());
}

EngineSelectionResult engineSelectionForPlan(const QuerySolution* solution,
                                             const QuerySolutionNode* dataAccessNode,
                                             IncrementalFeatureRolloutContext& ifrContext) {
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

    const bool containsLuPattern = treeMatchesAny(dataAccessNode, makeLookupUnwindRule(ifrContext));

    const QuerySolutionNode* planPushdownRoot =
        treeSearch(solution->root(), PlanPushdownSelector(containsLuPattern, ifrContext));

    return {planPushdownRoot ? EngineChoice::kSbe : EngineChoice::kClassic, planPushdownRoot};
}

bool indexHasHashedPathPrefixOfNonHashedPath(const BSONObj& keyPattern) {
    boost::optional<std::string_view> hashedPath;
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
