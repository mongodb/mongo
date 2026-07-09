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
