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
 * Returns the counter name `shape` is reported under.
 */
std::string_view toCounterName(PlanShapeCounter shape);

/**
 * A set of plan shape counters, stored sparsely so that only counters with a
 * non-zero count take up space.
 *
 * Serializes to BSON as '{[counterName]: [count], ...}' with zero counts omitted.
 * Parsing ignores unrecognized counter names and non-numeric values, so nodes on
 * different versions can exchange counts safely.
 */
class PlanShapeCounts {
public:
    void increment(PlanShapeCounter shape, int64_t n = 1);

    void add(const PlanShapeCounts& other);

    int64_t getCount(PlanShapeCounter shape) const {
        auto it = _counts.find(shape);
        return it == _counts.end() ? 0 : it->second;
    }

    bool empty() const {
        return _counts.empty();
    }

    BSONObj toBSON() const;
    static PlanShapeCounts fromBSON(const BSONObj& obj);

private:
    std::map<PlanShapeCounter, int64_t> _counts;
};

}  // namespace mongo::plan_shape_counters
