// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/parallelism_planner/required_distribution.h"

#include "mongo/util/assert_util.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace mongo::parallelism_planner {

std::string toString(const RequiredDistribution& reqDist) {
    switch (reqDist.type) {
        case RequiredDistribution::Type::Any:
            return "any()";
        case RequiredDistribution::Type::Unpartitioned:
            return "unpartitioned()";
        case RequiredDistribution::Type::Replicated:
            return "replicated()";
        case RequiredDistribution::Type::HashPartitioned: {
            std::vector<std::string> names;
            names.reserve(reqDist.keys.size());
            for (const auto& key : reqDist.keys) {
                names.push_back(key.fullPath());
            }
            return fmt::format("hashOn[{}]", fmt::join(names, ", "));
        }
    }
    MONGO_UNREACHABLE_TASSERT(13623100);
}

bool satisfies(const DistributionDesc& produced, const RequiredDistribution& required) {
    switch (required.type) {
        case RequiredDistribution::Type::Any:
            return true;
        case RequiredDistribution::Type::Unpartitioned:
            return produced.type == DistributionDesc::Type::Unpartitioned;
        case RequiredDistribution::Type::Replicated:
            return produced.type == DistributionDesc::Type::Replicated;
        case RequiredDistribution::Type::HashPartitioned:
            // TODO SERVER-138071 Consider relaxing in some cases.
            return produced.type == DistributionDesc::Type::HashPartitioned &&
                produced.keys == required.keys;
    }
    MONGO_UNREACHABLE_TASSERT(13623101);
}

}  // namespace mongo::parallelism_planner
