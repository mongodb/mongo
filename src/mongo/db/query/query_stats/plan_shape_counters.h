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

#include <cstddef>

#include <boost/optional.hpp>

namespace mongo {
class QuerySolution;
namespace plan_shape_counters {

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
 * Matches the winning 'solution' against the specific plan shapes tracked by query stats and
 * returns the matched shape's counter, or boost::none if the plan matches no tracked shape.
 * Shapes are matched against the find-layer QuerySolutionNode root. Stages that don't
 * contribute to a plan's shape (skip, limit, sharding filter, sort key generator, return key)
 * are ignored wherever they appear.
 */
boost::optional<PlanShapeCounter> identifyPlanShapeForCounters(const QuerySolution& solution);

}  // namespace plan_shape_counters
}  // namespace mongo
