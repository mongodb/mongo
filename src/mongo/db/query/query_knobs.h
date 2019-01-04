
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/atomic_proxy.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

//
// multi-plan ranking
//

// Max number of times we call work() on plans before comparing them,
// for small collections.
extern AtomicWord<int> internalQueryPlanEvaluationWorks;

// For large collections, the number times we work() candidate plans is
// taken as this fraction of the collection size.
extern AtomicDouble internalQueryPlanEvaluationCollFraction;

// Stop working plans once a plan returns this many results.
extern AtomicWord<int> internalQueryPlanEvaluationMaxResults;

// Do we give a big ranking bonus to intersection plans?
extern AtomicWord<bool> internalQueryForceIntersectionPlans;

// Do we have ixisect on at all?
extern AtomicWord<bool> internalQueryPlannerEnableIndexIntersection;

// Do we use hash-based intersection for rooted $and queries?
extern AtomicWord<bool> internalQueryPlannerEnableHashIntersection;

//
// plan cache
//

// How many entries in the cache?
extern AtomicWord<int> internalQueryCacheSize;

// How many feedback entries do we collect before possibly evicting from the cache based on bad
// performance?
extern AtomicWord<int> internalQueryCacheFeedbacksStored;

// How many times more works must we perform in order to justify plan cache eviction
// and replanning?
extern AtomicDouble internalQueryCacheEvictionRatio;

// How quickly the the 'works' value in an inactive cache entry will grow. It grows
// exponentially. The value of this server parameter is the base.
extern AtomicDouble internalQueryCacheWorksGrowthCoefficient;

// Whether or not cache entries can be marked as "inactive."
extern AtomicWord<bool> internalQueryCacheDisableInactiveEntries;

// Whether or not planCacheListPlans uses the new output format.
extern AtomicWord<bool> internalQueryCacheListPlansNewOutput;

//
// Planning and enumeration.
//

// How many indexed solutions will QueryPlanner::plan output?
extern AtomicWord<int> internalQueryPlannerMaxIndexedSolutions;

// How many solutions will the enumerator consider at each OR?
extern AtomicWord<int> internalQueryEnumerationMaxOrSolutions;

// How many intersections will the enumerator consider at each AND?
extern AtomicWord<int> internalQueryEnumerationMaxIntersectPerAnd;

// Do we want to plan each child of the OR independently?
extern AtomicWord<bool> internalQueryPlanOrChildrenIndependently;

// How many index scans are we willing to produce in order to obtain a sort order
// during explodeForSort?
extern AtomicWord<int> internalQueryMaxScansToExplode;

// Allow the planner to generate covered whole index scans, rather than falling back to a COLLSCAN.
extern AtomicWord<bool> internalQueryPlannerGenerateCoveredWholeIndexScans;

// Ignore unknown JSON Schema keywords.
extern AtomicWord<bool> internalQueryIgnoreUnknownJSONSchemaKeywords;

//
// Query execution.
//

extern AtomicWord<int> internalQueryExecMaxBlockingSortBytes;

// Yield after this many "should yield?" checks.
extern AtomicWord<int> internalQueryExecYieldIterations;

// Yield if it's been at least this many milliseconds since we last yielded.
extern AtomicWord<int> internalQueryExecYieldPeriodMS;

// Limit the size that we write without yielding to 16MB / 64 (max expected number of indexes)
const int64_t insertVectorMaxBytes = 256 * 1024;

// The number of bytes to buffer at once during a $facet stage.
extern AtomicWord<int> internalQueryFacetBufferSizeBytes;

extern AtomicWord<long long> internalDocumentSourceSortMaxBlockingSortBytes;

extern AtomicWord<long long> internalLookupStageIntermediateDocumentMaxSizeBytes;

extern AtomicWord<long long> internalDocumentSourceGroupMaxMemoryBytes;

extern AtomicWord<int> internalInsertMaxBatchSize;

extern AtomicWord<int> internalDocumentSourceCursorBatchSizeBytes;

extern AtomicWord<int> internalDocumentSourceLookupCacheSizeBytes;

extern AtomicWord<bool> internalQueryProhibitBlockingMergeOnMongoS;
}  // namespace mongo
