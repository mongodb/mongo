// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/explain_policy.h"

#include "mongo/db/query/explain_options.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using V = ExplainOptions::Verbosity;
using C = ExplainSettings;

// The compile-guard from explain_options.h, mirrored here for visibility: an ordinal Verbosity
// comparison must not compile, so no explain content decision can depend on the enum's value
// order by construction. See the comment on
// explain::HasOrdinalVerbosityComparison for why this is a trait rather than a requires-expression.
static_assert(!explain::HasOrdinalVerbosityComparison<V>::value,
              "Ordinal Verbosity comparison must not compile - use ExplainPolicy");

// Reproduces-legacy: for every legacy verbosity, explainPolicyFor() yields exactly the expected
// flag set. This is the executable form of the "zero output change" contract at the policy layer.
TEST(ExplainPolicyTest, ReproducesLegacyBehavior) {
    ASSERT_TRUE(explainPolicyFor(V::kQueryPlanner) ==
                ExplainPolicy(C::kPlannerInfo | C::kRejectedPlans));
    ASSERT_TRUE(explainPolicyFor(V::kExecStats) ==
                ExplainPolicy(C::kPlannerInfo | C::kRejectedPlans | C::kExecStats));
    ASSERT_TRUE(
        explainPolicyFor(V::kExecAllPlans) ==
        ExplainPolicy(C::kPlannerInfo | C::kRejectedPlans | C::kExecStats | C::kAllPlansExecStats));
    ASSERT_TRUE(explainPolicyFor(V::kInternal) ==
                ExplainPolicy(C::kPlannerInfo | C::kRejectedPlans | C::kExecStats |
                              C::kAllPlansExecStats | C::kBytecode));
}

// Predicate-level cross-check of the legacy thresholds that content code actually queries.
TEST(ExplainPolicyTest, LegacyPredicates) {
    const auto queryPlanner = explainPolicyFor(V::kQueryPlanner);
    ASSERT_TRUE(queryPlanner.hasPlannerInfo());
    ASSERT_TRUE(queryPlanner.hasRejectedPlans());
    ASSERT_FALSE(queryPlanner.hasExecStats());
    ASSERT_FALSE(queryPlanner.hasAllPlansStats());

    const auto execStats = explainPolicyFor(V::kExecStats);
    ASSERT_TRUE(execStats.hasPlannerInfo());
    ASSERT_TRUE(execStats.hasExecStats());
    ASSERT_FALSE(execStats.hasAllPlansStats());

    const auto execAllPlans = explainPolicyFor(V::kExecAllPlans);
    ASSERT_TRUE(execAllPlans.hasPlannerInfo());
    ASSERT_TRUE(execAllPlans.hasExecStats());
    ASSERT_TRUE(execAllPlans.hasAllPlansStats());
    ASSERT_FALSE(execAllPlans.hasByteCode());

    // The internal verbosity is the only one that carries SBE bytecode.
    const auto internal = explainPolicyFor(V::kInternal);
    ASSERT_TRUE(internal.hasAllPlansStats());
    ASSERT_TRUE(internal.hasByteCode());
}

// For every V3 verbosity, explainPolicyFor() yields exactly the expected flag set.
TEST(ExplainPolicyTest, V3Rows) {
    ASSERT_TRUE(explainPolicyFor(V::kPlanSummary) ==
                ExplainPolicy(C::kPlannerInfo | C::kRejectedPlans));
    ASSERT_TRUE(explainPolicyFor(V::kPlannerChoice) ==
                ExplainPolicy(C::kPlannerInfo | C::kRejectedPlans));
    ASSERT_TRUE(explainPolicyFor(V::kPlannerStats) ==
                ExplainPolicy(C::kPlannerInfo | C::kRejectedPlans | C::kAllPlansExecStats));
    ASSERT_TRUE(
        explainPolicyFor(V::kExecStatsV3) ==
        ExplainPolicy(C::kPlannerInfo | C::kRejectedPlans | C::kAllPlansExecStats | C::kExecStats));
}

// The V3-distinctive predicate combination: plannerStats carries per-candidate trial statistics
// without winner-execution statistics — a combination no legacy verbosity produces (with legacy
// verbosities kAllPlansExecStats implies kExecStats).
TEST(ExplainPolicyTest, PlannerStatsHasTrialStatsWithoutExecStats) {
    const auto plannerStats = explainPolicyFor(V::kPlannerStats);
    ASSERT_TRUE(plannerStats.hasAllPlansStats());
    ASSERT_FALSE(plannerStats.hasExecStats());

    // execStats adds exactly the winner-execution statistics on top.
    const auto execStatsV3 = explainPolicyFor(V::kExecStatsV3);
    ASSERT_TRUE(execStatsV3 == plannerStats.with(C::kExecStats));
}

// Monotonicity: the current two sets of verbosities are additive (each verbosity's content is a
// superset of the previous one's). The V3 verbosities rely on this, while the type still permits
// non-nested subsets for future deltas.
TEST(ExplainPolicyTest, VerbositiesAreMonotone) {
    const auto queryPlanner = explainPolicyFor(V::kQueryPlanner);
    const auto execStats = explainPolicyFor(V::kExecStats);
    const auto execAllPlans = explainPolicyFor(V::kExecAllPlans);

    // queryPlanner ⊆ execStats ⊆ execAllPlans, checked flag by flag.
    for (auto flag : {C::kPlannerInfo, C::kRejectedPlans, C::kExecStats, C::kAllPlansExecStats}) {
        if (queryPlanner.has(flag)) {
            ASSERT_TRUE(execStats.has(flag));
        }
        if (execStats.has(flag)) {
            ASSERT_TRUE(execAllPlans.has(flag));
        }
    }
    ASSERT_TRUE(queryPlanner == queryPlanner.mergedWith(execStats).without(C::kExecStats));

    // The V3 verbosities nest: planSummary = plannerChoice ⊆ plannerStats ⊆ execStats(V3).
    ASSERT_TRUE(explainPolicyFor(V::kPlanSummary) == explainPolicyFor(V::kPlannerChoice));
    const auto plannerChoice = explainPolicyFor(V::kPlannerChoice);
    const auto plannerStats = explainPolicyFor(V::kPlannerStats);
    const auto execStatsV3 = explainPolicyFor(V::kExecStatsV3);
    for (auto flag : {C::kPlannerInfo, C::kRejectedPlans, C::kExecStats, C::kAllPlansExecStats}) {
        if (plannerChoice.has(flag)) {
            ASSERT_TRUE(plannerStats.has(flag));
        }
        if (plannerStats.has(flag)) {
            ASSERT_TRUE(execStatsV3.has(flag));
        }
    }
}

// Set-ops: with/without/mergedWith/has behave as a set.
TEST(ExplainPolicyTest, SetOperations) {
    const ExplainPolicy empty;
    ASSERT_FALSE(empty.hasPlannerInfo());
    ASSERT_FALSE(empty.has(C::kNone));

    const auto withOne = empty.with(C::kExecStats);
    ASSERT_TRUE(withOne.hasExecStats());
    ASSERT_FALSE(withOne.hasPlannerInfo());

    const auto withTwo = withOne.with(C::kPlannerInfo);
    ASSERT_TRUE(withTwo.hasExecStats());
    ASSERT_TRUE(withTwo.hasPlannerInfo());
    ASSERT_TRUE(withTwo.has(C::kExecStats | C::kPlannerInfo));

    ASSERT_TRUE(withTwo.without(C::kExecStats) == ExplainPolicy(C::kPlannerInfo));

    const auto merged = ExplainPolicy(C::kPlannerInfo).mergedWith(ExplainPolicy(C::kExecStats));
    ASSERT_TRUE(merged == withTwo);

    // has() with a multi-bit argument requires ALL bits to be present.
    ASSERT_FALSE(withOne.has(C::kExecStats | C::kPlannerInfo));

    // Non-nested subset is representable (no monotonicity imposed by the type).
    const auto nonNested = ExplainPolicy(C::kPlannerInfo | C::kAllPlansExecStats);
    ASSERT_TRUE(nonNested.hasPlannerInfo());
    ASSERT_TRUE(nonNested.hasAllPlansStats());
    ASSERT_FALSE(nonNested.hasExecStats());
}

}  // namespace
}  // namespace mongo
