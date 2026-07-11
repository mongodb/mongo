// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/query_solution_analyzer.h"
#include "mongo/db/query/util/named_enum.h"

#include <bitset>
#include <cstddef>

namespace mongo {
namespace plan_shape_counters {

/**
 * Identifies a data access path counter tracked by query stats. Unlike the specific plan shape
 * counters, these are not mutually exclusive: one plan can set several of them (e.g. an OR over
 * an ixscan-fetch and a covered ixscan).
 * TODO SERVER-131101 split enum into different subtypes.
 */
#define ACCESS_PATH_COUNTER_TABLE(F) \
    F(kCollscan)                     \
    F(kClusteredCollscan)            \
    F(kCoveredIxscan)                \
    F(kCountScan)                    \
    F(kDistinctScan)                 \
    F(kIxscanFetch)                  \
    F(kDistinctScanFetch)            \
    F(kGeoNear2d)                    \
    F(kGeoNear2dSphere)              \
    F(kOtherAccessPath)              \
    F(kTextMatch)                    \
    F(kBtreeIxscan)                  \
    F(kWildcardIxscan)               \
    F(kSparseIxscan)                 \
    F(kUniqueIxscan)                 \
    F(kHashedIxscan)                 \
    F(kMultikeyIxscan)               \
    F(kBoundsFullScan)               \
    F(kBoundsPoint)                  \
    F(kBoundsBoundedRange)           \
    F(kBoundsMinKeyToValue)          \
    F(kBoundsValueToMaxKey)          \
    F(kBoundsMixture)                \
    F(kBoundsUnionedSmall)           \
    F(kBoundsUnionedLarge)           \
    F(kNumCounters)

QUERY_UTIL_NAMED_ENUM_DEFINE(AccessPathCounter, ACCESS_PATH_COUNTER_TABLE)
#undef ACCESS_PATH_COUNTER_TABLE

constexpr size_t kNumAccessPathCounters = static_cast<size_t>(AccessPathCounter::kNumCounters);

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
