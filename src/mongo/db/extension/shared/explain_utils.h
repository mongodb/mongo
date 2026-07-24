// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/util/modules.h"

namespace mongo::extension {

inline ::MongoExtensionExplainVerbosity convertHostVerbosityToExtVerbosity(
    boost::optional<mongo::ExplainOptions::Verbosity> hostVerbosity) {
    if (!hostVerbosity.has_value()) {
        return ::MongoExtensionExplainVerbosity::kNotExplain;
    }
    switch (hostVerbosity.value()) {
        case mongo::ExplainOptions::Verbosity::kQueryPlanner:
            return ::MongoExtensionExplainVerbosity::kQueryPlanner;
        case mongo::ExplainOptions::Verbosity::kExecStats:
            return ::MongoExtensionExplainVerbosity::kExecStats;
        case mongo::ExplainOptions::Verbosity::kExecAllPlans:
            return ::MongoExtensionExplainVerbosity::kExecAllPlans;
        // The V3 verbosity modes have no distinct extension-facing representation yet, so map each
        // to the nearest legacy verbosity. Extension stages run on the aggregation explain path,
        // which stays legacy-delegated end-to-end. TODO SERVER-130810 revisit together with the
        // aggregation V3 output format.
        case mongo::ExplainOptions::Verbosity::kPlanSummary:
        case mongo::ExplainOptions::Verbosity::kPlannerChoice:
            return ::MongoExtensionExplainVerbosity::kQueryPlanner;
        case mongo::ExplainOptions::Verbosity::kPlannerStats:
            return ::MongoExtensionExplainVerbosity::kExecAllPlans;
        case mongo::ExplainOptions::Verbosity::kExecStatsV3:
            return ::MongoExtensionExplainVerbosity::kExecStats;
        default:
            MONGO_UNREACHABLE_TASSERT(11239404);
    }
}
}  // namespace mongo::extension
