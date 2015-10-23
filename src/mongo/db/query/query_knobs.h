/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <atomic>

#include "mongo/platform/atomic_proxy.h"

namespace mongo {

//
// multi-plan ranking
//

// Max number of times we call work() on plans before comparing them,
// for small collections.
extern std::atomic<int> internalQueryPlanEvaluationWorks;  // NOLINT

// For large collections, the number times we work() candidate plans is
// taken as this fraction of the collection size.
extern AtomicDouble internalQueryPlanEvaluationCollFraction;  // NOLINT

// Stop working plans once a plan returns this many results.
extern std::atomic<int> internalQueryPlanEvaluationMaxResults;  // NOLINT

// Do we give a big ranking bonus to intersection plans?
extern std::atomic<bool> internalQueryForceIntersectionPlans;  // NOLINT

// Do we have ixisect on at all?
extern std::atomic<bool> internalQueryPlannerEnableIndexIntersection;  // NOLINT

// Do we use hash-based intersection for rooted $and queries?
extern std::atomic<bool> internalQueryPlannerEnableHashIntersection;  // NOLINT

//
// plan cache
//

// How many entries in the cache?
extern std::atomic<int> internalQueryCacheSize;  // NOLINT

// How many feedback entries do we collect before possibly evicting from the cache based on bad
// performance?
extern std::atomic<int> internalQueryCacheFeedbacksStored;  // NOLINT

// How many times more works must we perform in order to justify plan cache eviction
// and replanning?
extern AtomicDouble internalQueryCacheEvictionRatio;  // NOLINT

//
// Planning and enumeration.
//

// How many indexed solutions will QueryPlanner::plan output?
extern std::atomic<int> internalQueryPlannerMaxIndexedSolutions;  // NOLINT

// How many solutions will the enumerator consider at each OR?
extern std::atomic<int> internalQueryEnumerationMaxOrSolutions;  // NOLINT

// How many intersections will the enumerator consider at each AND?
extern std::atomic<int> internalQueryEnumerationMaxIntersectPerAnd;  // NOLINT

// Do we want to plan each child of the OR independently?
extern std::atomic<bool> internalQueryPlanOrChildrenIndependently;  // NOLINT

// How many index scans are we willing to produce in order to obtain a sort order
// during explodeForSort?
extern std::atomic<int> internalQueryMaxScansToExplode;  // NOLINT

//
// Query execution.
//

extern std::atomic<int> internalQueryExecMaxBlockingSortBytes;  // NOLINT

// Yield after this many "should yield?" checks.
extern std::atomic<int> internalQueryExecYieldIterations;  // NOLINT

// Yield if it's been at least this many milliseconds since we last yielded.
extern std::atomic<int> internalQueryExecYieldPeriodMS;  // NOLINT

// Limit the size that we write without yielding to 16MB / 64 (max expected number of indexes)
const int64_t insertVectorMaxBytes = 256 * 1024;

}  // namespace mongo
