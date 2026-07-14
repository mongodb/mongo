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

        // TODO SERVER-130529: Remove these transitional V3 rows. Until the V3 output format is
        // implemented, a V3 verbosity is normally translated to a legacy verbosity before any
        // content decision. The one exception is DocumentSourceUnionWith::optimizeAt(), which reads
        // the originally requested (possibly-V3) verbosity straight off the ExpressionContext and
        // feeds it here. Mapping every V3 value to the kExecAllPlans policy keeps that site
        // byte-identical: before SERVER-130812 it evaluated "v3Verbosity >= kExecStats", which was
        // true for every V3 value, so its effective content matched kExecAllPlans.
        case V::kPlanSummary:
        case V::kPlannerChoice:
        case V::kPlannerStats:
        case V::kExecStatsV3:
            return ExplainPolicy(execAllPlans);
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
