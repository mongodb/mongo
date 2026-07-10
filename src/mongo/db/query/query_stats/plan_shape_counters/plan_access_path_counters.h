/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
