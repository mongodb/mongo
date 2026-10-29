// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/explain_policy.h"

#include "mongo/util/assert_util.h"

namespace mongo {

ExplainPolicy explainPolicyFor(ExplainOptions::Verbosity v) {
    using V = ExplainOptions::Verbosity;
    using C = ExplainSettings;

    // Explain V1/V2 verbosities are additive. Every legacy verbosity shows the cost-based ranker's
    // estimates, which the legacy node shape has always emitted from its lowest verbosity.
    constexpr auto queryPlanner = C::kPlannerInfo | C::kRejectedPlans | C::kCostBasedStats;
    constexpr auto execStats = queryPlanner | C::kExecStats;
    constexpr auto execAllPlans = execStats | C::kAllPlansExecStats;

    switch (v) {
        case V::kQueryPlanner:
            return ExplainPolicy(queryPlanner);
        case V::kExecStats:
            return ExplainPolicy(execStats);
        case V::kExecAllPlans:
            return ExplainPolicy(execAllPlans);
        case V::kInternal:
            // The internal/test verbosity adds the winning plan's SBE bytecode on top of the
            // allPlansExecution content.
            return ExplainPolicy(execAllPlans | C::kBytecode);

        // Explain V3 verbosities: a separate sequence of verbosities, each one adds contents on
        // top of the previous one: plannerChoice ⊆ plannerStats ⊆ execStats.
        // planSummary is currently legacy-delegated (TODO SERVER-133235) and therefore does not
        // follow this nesting at the policy/output level yet.
        case V::kPlanSummary:
            // The planSummary output is still legacy-delegated (TODO SERVER-133235).
            return ExplainPolicy(queryPlanner);
        case V::kPlannerChoice:
            // Plan structure only: every candidate's stages, with no ranking statistics
            // (multi-planning trial counters or cost-based estimates) and no execution statistics.
            // This is the one policy that excludes kCostBasedStats, which is why the V3
            // planner-only content cannot reuse the legacy 'queryPlanner' baseline - the legacy
            // node shape emits estimates even at its lowest verbosity.
            return ExplainPolicy(C::kPlannerInfo | C::kRejectedPlans);
        case V::kPlannerStats:
            // Trial/per-candidate statistics without winner-execution statistics — a combination
            // no legacy verbosity produces; the V3 plan serializer keys off it. The query is not
            // executed at this verbosity.
            return ExplainPolicy(C::kPlannerInfo | C::kRejectedPlans | C::kCostBasedStats |
                                 C::kAllPlansExecStats);
        case V::kExecStatsV3:
            // plannerStats content plus the retained "executionStats" section (winner executed).
            return ExplainPolicy(C::kPlannerInfo | C::kRejectedPlans | C::kCostBasedStats |
                                 C::kAllPlansExecStats | C::kExecStats);
    }
    MONGO_UNREACHABLE_TASSERT(10812000);
}

ExplainOptions::Verbosity mapV3ToLegacyVerbosity(ExplainOptions::Verbosity v) {
    switch (v) {
        case ExplainOptions::Verbosity::kPlanSummary:
        case ExplainOptions::Verbosity::kPlannerChoice:
            return ExplainOptions::Verbosity::kQueryPlanner;
        case ExplainOptions::Verbosity::kPlannerStats:
            return ExplainOptions::Verbosity::kExecAllPlans;
        case ExplainOptions::Verbosity::kExecStatsV3:
            return ExplainOptions::Verbosity::kExecStats;
        case ExplainOptions::Verbosity::kQueryPlanner:
        case ExplainOptions::Verbosity::kExecStats:
        case ExplainOptions::Verbosity::kExecAllPlans:
        case ExplainOptions::Verbosity::kInternal:
            return v;
    }
    MONGO_UNREACHABLE_TASSERT(13076110);
}

}  // namespace mongo
