// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/explain_policy.h"

#include "mongo/util/assert_util.h"

namespace mongo {

ExplainPolicy explainPolicyFor(ExplainOptions::Verbosity v) {
    using V = ExplainOptions::Verbosity;
    using C = ExplainSettings;

    // Explain V1/V2 verbosities are additive.
    constexpr auto queryPlanner = C::kPlannerInfo | C::kRejectedPlans;
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
        // top of the previous one: planSummary = plannerChoice ⊆ plannerStats ⊆ execStats.
        case V::kPlanSummary:
        case V::kPlannerChoice:
            // Planner-only content, no execution statistics. The output of these two reduction
            // modes is still legacy-delegated (TODO SERVER-131451), but the policy is real.
            return ExplainPolicy(queryPlanner);
        case V::kPlannerStats:
            // Trial/per-candidate statistics without winner-execution statistics — a combination
            // no legacy verbosity produces; the V3 plan serializer keys off it. The query is not
            // executed at this verbosity.
            return ExplainPolicy(queryPlanner | C::kAllPlansExecStats);
        case V::kExecStatsV3:
            // plannerStats content plus the retained "executionStats" section (winner executed).
            return ExplainPolicy(queryPlanner | C::kAllPlansExecStats | C::kExecStats);
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
