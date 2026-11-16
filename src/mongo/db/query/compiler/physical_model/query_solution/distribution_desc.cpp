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
                    case ExchangePartitionType::Hash:
                        return DistributionDesc::hashOn(partition.keys);
                    case ExchangePartitionType::Range:
                        return DistributionDesc::rangeOn(partition.keys);
                    case ExchangePartitionType::RoundRobin:
                        return DistributionDesc::roundRobin();
                }
                MONGO_UNREACHABLE_TASSERT(11700115);
            },
        },
        routing.mode);
}

std::string_view routingModeName(const ExchangeRouting& routing) {
    return std::visit(
        OverloadedVisitor{
            // A gather and a broadcast are whole modes rather than a value of some enumerated type,
            // so these two are the only routing names not already carried by a named enum.
            [](const ExchangeRouting::Gather&) -> std::string_view { return "gather"; },
            [](const ExchangeRouting::Broadcast&) -> std::string_view { return "broadcast"; },
            [](const ExchangeRouting::Partition& partition) -> std::string_view {
                return toStringData(partition.type);
            },
        },
        routing.mode);
}

std::string toString(const ExchangeRouting& routing) {
    const std::string_view name = routingModeName(routing);
    return std::visit(OverloadedVisitor{
                          [&](const ExchangeRouting::Gather& gather) {
                              return fmt::format(
                                  "{}(sortPreserving = {})", name, gather.sortPreserving);
                          },
                          [&](const ExchangeRouting::Broadcast&) { return std::string{name}; },
                          [&](const ExchangeRouting::Partition& partition) {
                              return partition.keys.empty()
                                  ? std::string{name}
                                  : fmt::format("{}{}", name, keysToString(partition.keys));
                          },
                      },
                      routing.mode);
}

std::string toString(const DistributionDesc& d) {
    // 'keys' is non-empty for exactly the partitioned types, so it alone decides the decoration.
    return d.keys.empty() ? std::string{toStringData(d.type)}
                          : fmt::format("{}{}", toStringData(d.type), keysToString(d.keys));
}

}  // namespace mongo
