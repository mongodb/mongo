// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_counts.h"

#include <bitset>
#include <cstddef>

namespace mongo {
struct QuerySolutionNode;
namespace query_solution_analyzer {
class RuleEngine;
}  // namespace query_solution_analyzer
namespace plan_shape_counters {

/**
 * The node counters a single plan contributes to. Each counter follows "seen at least once"
 * semantics: it is set either zero times or once per plan, no matter how many nodes in the plan
 * satisfy its condition.
 */
struct QsnNodeCounts {
    void set(QsnNodeCounter counter) {
        flags.set(static_cast<size_t>(counter));
    }

    bool test(QsnNodeCounter counter) const {
        return flags.test(static_cast<size_t>(counter));
    }

    bool operator==(const QsnNodeCounts&) const = default;

    std::bitset<kNumQsnNodeCounters> flags;
};

/**
 * A query_solution_analyzer rule that accumulates QsnNodeCounts.
 */
class QsnNodeCountAnalyzer {
public:
    void preVisit(const QuerySolutionNode& node);

    // This method must exist because `treeSearch` attempts to call it.
    void preVisit(query_solution_analyzer::RuleEngine& engine,
                  const QuerySolutionNode& node,
                  size_t index);

    const QsnNodeCounts& counts() const {
        return _counts;
    }

private:
    QsnNodeCounts _counts;
};

}  // namespace plan_shape_counters
}  // namespace mongo
