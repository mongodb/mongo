// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/parallelism_planner/required_distribution.h"

#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

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

std::vector<RequiredDistribution> childRequirements(const QuerySolutionNode& parent) {
    switch (parent.getType()) {
        // Nodes that impose nothing on how their input is distributed.
        case STAGE_COLLSCAN:
        case STAGE_MATCH:
        case STAGE_SHARDING_FILTER:
        case STAGE_EXCHANGE:
            return std::vector<RequiredDistribution>(parent.children.size(),
                                                     RequiredDistribution::any());

        // Every node type that opts in to isParallelEligible() needs a case above.
        default:
            MONGO_UNREACHABLE_TASSERT(13623106);
    }
}

std::unique_ptr<QuerySolutionNode> insertExchangeFor(std::unique_ptr<QuerySolutionNode> child,
                                                     const RequiredDistribution& required) {
    ExchangeRouting routing = [&] {
        switch (required.type) {
            case RequiredDistribution::Type::HashPartitioned:
                return ExchangeRouting::hashOn(required.keys);
            case RequiredDistribution::Type::Replicated:
                return ExchangeRouting::broadcast();
            case RequiredDistribution::Type::Unpartitioned:
                // No eligible node provides a sort order yet, so there is nothing for a gather to
                // preserve.
                tassert(13623107,
                        "gather over an ordered input is not supported",
                        child->providedSorts().getBaseSortPattern().isEmpty());
                return ExchangeRouting::gather(false);
            case RequiredDistribution::Type::Any:
                MONGO_UNREACHABLE_TASSERT(11700106);
        }
        MONGO_UNREACHABLE_TASSERT(13623110);
    }();

    auto exchange = std::make_unique<ExchangeNode>(std::move(child), std::move(routing));
    exchange->physicalProps.distribution = toOutputDistribution(exchange->routing);
    return exchange;
}

void planDistribution(QuerySolutionNode& node, const ParallelismPlannerContext& ctx) {
    // The planner is only ever handed a subtree made entirely of eligible nodes.
    tassert(13623108,
            str::stream() << "parallelism planner reached a node that is not parallel eligible: "
                          << nodeStageTypeToString(&node),
            node.isParallelEligible());

    const auto reqs = childRequirements(node);
    invariant(reqs.size() == node.children.size());

    for (size_t i = 0; i < node.children.size(); ++i) {
        auto& child = node.children[i];
        // Settle the child's subtree first: whether it satisfies the requirement depends on what
        // it ends up producing, and an exchange inserted below it can change that.
        planDistribution(*child, ctx);
        if (!satisfies(child->physicalProps.distribution, reqs[i])) {
            child = insertExchangeFor(std::move(child), reqs[i]);
        }
    }

    // Base case: all children have their distributions settled or this is a leaf node.
    switch (node.getType()) {
        case STAGE_COLLSCAN:
            node.physicalProps.distribution = DistributionDesc::roundRobin();
            break;

        // An exchange sets its own distribution when it is created, from its own routing. Do not
        // overwrite it.
        case STAGE_EXCHANGE:
            break;

        // Single-child nodes that never move a row to a different worker, so they produce what
        // their child produces
        case STAGE_MATCH:
        case STAGE_SHARDING_FILTER:
            node.physicalProps.distribution = node.children[0]->physicalProps.distribution;
            break;

        // Every node type that opts in to isParallelEligible() needs a case above.
        default:
            MONGO_UNREACHABLE_TASSERT(13623109);
    }
}

}  // namespace mongo::parallelism_planner
