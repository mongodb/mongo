// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_framework_serialization.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>

namespace mongo::query_settings::query_framework {

std::string serialize(QueryFrameworkControlEnum queryFramework) {
    switch (queryFramework) {
        case QueryFrameworkControlEnum::kForceClassicEngine:
            return std::string{kClassic};
        case QueryFrameworkControlEnum::kTrySbeEngine:
            return std::string{kSbe};
        default:
            MONGO_UNREACHABLE;
    }
}

QueryFrameworkControlEnum parse(std::string_view queryFrameworkString) {
    if (queryFrameworkString == kClassic)
        return QueryFrameworkControlEnum::kForceClassicEngine;
    if (queryFrameworkString == kSbe)
        return QueryFrameworkControlEnum::kTrySbeEngine;
    uasserted(ErrorCodes::BadValue,
              str::stream() << "Invalid value for 'queryFramework': expected " << kClassic << " or "
                            << kSbe << ", but got " << queryFrameworkString);
}
}  // namespace mongo::query_settings::query_framework
