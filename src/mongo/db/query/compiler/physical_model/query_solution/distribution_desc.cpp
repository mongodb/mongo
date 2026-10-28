// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/physical_model/query_solution/distribution_desc.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace mongo {
namespace {

std::string keysToString(const std::vector<FieldPath>& keys) {
    std::vector<std::string> names;
    names.reserve(keys.size());
    for (const auto& key : keys) {
        names.push_back(key.fullPath());
    }
    return fmt::format("[{}]", fmt::join(names, ", "));
}

}  // namespace

DistributionDesc toOutputDistribution(const ExchangeRouting& routing) {
    return std::visit(
        OverloadedVisitor{
            [](const ExchangeRouting::Gather&) { return DistributionDesc::unpartitioned(); },
            [](const ExchangeRouting::Broadcast&) { return DistributionDesc::replicated(); },
            [](const ExchangeRouting::Partition& partition) {
                switch (partition.type) {
                    case ExchangeRouting::Partition::Type::Hash:
                        return DistributionDesc::hashOn(partition.keys);
                    case ExchangeRouting::Partition::Type::Range:
                        return DistributionDesc::rangeOn(partition.keys);
                    case ExchangeRouting::Partition::Type::RoundRobin:
                        return DistributionDesc::roundRobin();
                }
                MONGO_UNREACHABLE_TASSERT(11700115);
            },
        },
        routing.mode);
}

std::string toString(const ExchangeRouting& routing) {
    return std::visit(
        OverloadedVisitor{
            [](const ExchangeRouting::Gather& gather) {
                return fmt::format("Gather(sortPreserving = {})", gather.sortPreserving);
            },
            [](const ExchangeRouting::Broadcast&) -> std::string { return "Broadcast"; },
            [](const ExchangeRouting::Partition& partition) -> std::string {
                switch (partition.type) {
                    case ExchangeRouting::Partition::Type::Hash:
                        return fmt::format("Hash{}", keysToString(partition.keys));
                    case ExchangeRouting::Partition::Type::Range:
                        return fmt::format("Range{}", keysToString(partition.keys));
                    case ExchangeRouting::Partition::Type::RoundRobin:
                        return "RoundRobin";
                }
                MONGO_UNREACHABLE_TASSERT(11700116);
            },
        },
        routing.mode);
}

std::string toString(const DistributionDesc& d) {
    switch (d.type) {
        case DistributionDesc::Type::Unpartitioned:
            return "Unpartitioned";
        case DistributionDesc::Type::Replicated:
            return "Replicated";
        case DistributionDesc::Type::RoundRobin:
            return "RoundRobin";
        case DistributionDesc::Type::HashPartitioned:
            return fmt::format("HashPartitioned{}", keysToString(d.keys));
        case DistributionDesc::Type::RangePartitioned:
            return fmt::format("RangePartitioned{}", keysToString(d.keys));
    }
    MONGO_UNREACHABLE_TASSERT(11700100);
}

}  // namespace mongo
