// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/util/named_enum.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>

namespace mongo::plan_shape_counters {

/**
 * Identifies a specific plan shape counter tracked by query stats.
 *
 * The shapes are mutually exclusive: a winning plan matches at most one of them, so a single query
 * execution increments at most one counter (by one).
 */
#define PLAN_SHAPE_COUNTER_TABLE(F)   \
    F(kCollscan)                      \
    F(kCollscanProject)               \
    F(kCollscanProjectSort)           \
    F(kCollscanProjectSortProject)    \
    F(kCollscanSort)                  \
    F(kCollscanSortProject)           \
    F(kCollscanSortProjectSort)       \
    F(kIxscanFetch)                   \
    F(kIxscanFetchOr)                 \
    F(kIxscanFetchOrProject)          \
    F(kIxscanFetchOrProjectSort)      \
    F(kIxscanFetchOrSort)             \
    F(kIxscanFetchOrSortProject)      \
    F(kIxscanFetchProject)            \
    F(kIxscanFetchProjectSort)        \
    F(kIxscanFetchProjectSortProject) \
    F(kIxscanFetchSort)               \
    F(kIxscanFetchSortProject)        \
    F(kIxscanFetchSortProjectSort)    \
    F(kIxscanFetchSortMerge)          \
    F(kIxscanFetchSortMergeProject)   \
    F(kIxscanOrFetch)                 \
    F(kIxscanOrFetchProject)          \
    F(kIxscanOrFetchProjectSort)      \
    F(kIxscanOrFetchSort)             \
    F(kIxscanOrFetchSortProject)      \
    F(kIxscanOrProject)               \
    F(kIxscanOrProjectSort)           \
    F(kIxscanProject)                 \
    F(kIxscanProjectSort)             \
    F(kIxscanProjectSortProject)      \
    F(kIxscanSortFetch)               \
    F(kIxscanSortFetchProject)        \
    F(kIxscanSortMergeFetch)          \
    F(kIxscanSortMergeFetchProject)   \
    F(kIxscanSortMergeProject)        \
    F(kNumCounters)

QUERY_UTIL_NAMED_ENUM_DEFINE(PlanShapeCounter, PLAN_SHAPE_COUNTER_TABLE)
#undef PLAN_SHAPE_COUNTER_TABLE

constexpr size_t kNumPlanShapeCounters = static_cast<size_t>(PlanShapeCounter::kNumCounters);

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
 * Returns the counter name `counter` is reported under.
 */
template <typename CounterEnum>
std::string_view toCounterName(CounterEnum counter);

/**
 * The full set of plan shape counters tracked by query stats: the specific plan shape pattern
 * counters, the QuerySolutionNode counters, and the access path counters. Counts are stored
 * sparsely so that only counters with a non-zero count take up space.
 *
 * Serializes to BSON as '{patterns: {...}, nodes: {...}, accessPaths: {...}}', where each section
 * maps counter names to counts; zero counts and empty sections are omitted. Parsing ignores
 * unrecognized section names, counter names, and non-numeric values, so nodes on different
 * versions can exchange counts safely.
 */
class PlanShapeCounts {
public:
    void increment(PlanShapeCounter counter, int64_t n = 1);
    void increment(QsnNodeCounter counter, int64_t n = 1);
    void increment(AccessPathCounter counter, int64_t n = 1);

    void add(const PlanShapeCounts& other);

    int64_t getCount(PlanShapeCounter counter) const {
        return getCount(_patternCounts, counter);
    }
    int64_t getCount(QsnNodeCounter counter) const {
        return getCount(_nodeCounts, counter);
    }
    int64_t getCount(AccessPathCounter counter) const {
        return getCount(_accessPathCounts, counter);
    }

    bool empty() const {
        return _patternCounts.empty() && _nodeCounts.empty() && _accessPathCounts.empty();
    }

    BSONObj toBSON() const;

    static PlanShapeCounts fromBSON(const BSONObj& obj);
    void parsePatternCounts(const BSONObj& obj);
    void parseNodeCounts(const BSONObj& obj);
    void parseAccessPathCounts(const BSONObj& obj);

private:
    template <typename CounterEnum>
    static int64_t getCount(const std::map<CounterEnum, int64_t>& counts, CounterEnum counter) {
        auto it = counts.find(counter);
        return it == counts.end() ? 0 : it->second;
    }

    std::map<PlanShapeCounter, int64_t> _patternCounts;
    std::map<QsnNodeCounter, int64_t> _nodeCounts;
    std::map<AccessPathCounter, int64_t> _accessPathCounts;
};

}  // namespace mongo::plan_shape_counters
