// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/explain_options.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

constexpr std::string_view ExplainOptions::kVerbosityName;

bool ExplainOptions::isV3Verbosity(ExplainOptions::Verbosity verbosity) {
    switch (verbosity) {
        case Verbosity::kPlanSummary:
        case Verbosity::kPlannerChoice:
        case Verbosity::kPlannerStats:
        case Verbosity::kExecStatsV3:
            return true;
        case Verbosity::kQueryPlanner:
        case Verbosity::kExecStats:
        case Verbosity::kExecAllPlans:
        case Verbosity::kInternal:
            return false;
    }
    MONGO_UNREACHABLE_TASSERT(10905000);
}

std::string_view ExplainOptions::verbosityString(ExplainOptions::Verbosity verbosity) {
    return idl::serialize(verbosity);
}

BSONObj ExplainOptions::toBSON(ExplainOptions::Verbosity verbosity) {
    BSONObjBuilder explainOptionsBuilder;
    explainOptionsBuilder.append(kVerbosityName, std::string_view(verbosityString(verbosity)));
    return explainOptionsBuilder.obj();
}

}  // namespace mongo
