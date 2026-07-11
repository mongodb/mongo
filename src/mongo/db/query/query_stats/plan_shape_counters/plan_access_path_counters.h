// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/query_solution_analyzer.h"
#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_counts.h"

#include <bitset>
#include <cstddef>

namespace mongo {
namespace plan_shape_counters {

/**
 * Access path counters. Each counter follows "seen at least once" semantics: they are set either
 * zero times or once, no matter how many nodes in the plan satisfy its condition.
 */
struct AccessPathCounts {
    void set(AccessPathCounter counter) {
        flags.set(static_cast<size_t>(counter));
    }

    bool test(AccessPathCounter counter) const {
        return flags.test(static_cast<size_t>(counter));
    }

    bool operator==(const AccessPathCounts&) const = default;

    std::bitset<kNumAccessPathCounters> flags;
};

/**
 * Visitor class to calculate `AccessPathCounts` for a given QSN.
 * Operates over the find layer of the plan.
 */
class AccessPathAnalyzer {
public:
    void preVisit(query_solution_analyzer::RuleEngine& engine,
                  const QuerySolutionNode& node,
                  size_t index);
    void postVisit(query_solution_analyzer::RuleEngine& engine, const QuerySolutionNode& node);
    void finish(query_solution_analyzer::RuleEngine& engine);

    const AccessPathCounts& counts() const {
        return _counts;
    }

    bool isNodeFetched() const {
        return _fetchDepth > 0;
    }

private:
    AccessPathCounts _counts;
    // The number of fetches above the current node being visited. Useful for
    // answering if an ixscan is covered or not.
    size_t _fetchDepth = 0;
};

}  // namespace plan_shape_counters
}  // namespace mongo
