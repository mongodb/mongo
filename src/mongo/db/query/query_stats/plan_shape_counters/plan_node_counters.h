// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/util/named_enum.h"

#include <bitset>
#include <cstddef>

namespace mongo {
struct QuerySolutionNode;
namespace query_solution_analyzer {
class RuleEngine;
}  // namespace query_solution_analyzer
namespace plan_shape_counters {

/**
 * Identifies a QuerySolutionNode counter tracked by query stats. Unlike the specific plan shape
 * counters, these are not mutually exclusive: one plan can set several of them.
 */
#define QSN_NODE_COUNTER_TABLE(F)         \
    F(kCollscanNoFilter)                  \
    F(kCollscanWithFilter)                \
    F(kIxscanNoFilter)                    \
    F(kIxscanWithFilter)                  \
    F(kFetchNoFilter)                     \
    F(kFetchWithFilter)                   \
    F(kAndHashNoFilter)                   \
    F(kAndHashWithFilter)                 \
    F(kAndSorted)                         \
    F(kOrNoFilterLte100Children)          \
    F(kOrWithFilterLte100Children)        \
    F(kOrNoFilterGt100Children)           \
    F(kOrWithFilterGt100Children)         \
    F(kSortMergeNoFilterLte100Children)   \
    F(kSortMergeWithFilterLte100Children) \
    F(kSortMergeNoFilterGt100Children)    \
    F(kSortMergeWithFilterGt100Children)  \
    F(kReturnKey)                         \
    F(kShardingFilter)                    \
    F(kProjectionDefault)                 \
    F(kProjectionCovered)                 \
    F(kProjectionSimple)                  \
    F(kSortKeyGenerator)                  \
    F(kSortDefaultNoLimit)                \
    F(kSortDefaultWithLimit)              \
    F(kSortSimpleNoLimit)                 \
    F(kSortSimpleWithLimit)               \
    F(kLimitSmall)                        \
    F(kLimitMedium)                       \
    F(kLimitLarge)                        \
    F(kSkipSmall)                         \
    F(kSkipMedium)                        \
    F(kSkipLarge)                         \
    F(kTextOr)                            \
    F(kMatch)                             \
    F(kReplaceRoot)                       \
    F(kGroup)                             \
    F(kEqLookupNoUnwind)                  \
    F(kEqLookupWithUnwind)                \
    F(kUnpackTsBucket)                    \
    F(kHashJoin)                          \
    F(kNlj)                               \
    F(kInlj)                              \
    F(kIndexProbe)                        \
    F(kNumCounters)

QUERY_UTIL_NAMED_ENUM_DEFINE(QsnNodeCounter, QSN_NODE_COUNTER_TABLE)
#undef QSN_NODE_COUNTER_TABLE

constexpr size_t kNumQsnNodeCounters = static_cast<size_t>(QsnNodeCounter::kNumCounters);

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
