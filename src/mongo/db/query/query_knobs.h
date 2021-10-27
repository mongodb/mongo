
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
extern AtomicInt32 internalQueryPlanEvaluationWorks;

// For large collections, the number times we work() candidate plans is
// taken as this fraction of the collection size.
extern AtomicDouble internalQueryPlanEvaluationCollFraction;

// Stop working plans once a plan returns this many results.
extern AtomicInt32 internalQueryPlanEvaluationMaxResults;

// Do we give a big ranking bonus to intersection plans?
extern AtomicBool internalQueryForceIntersectionPlans;

// Do we have ixisect on at all?
extern AtomicBool internalQueryPlannerEnableIndexIntersection;

// Do we use hash-based intersection for rooted $and queries?
extern AtomicBool internalQueryPlannerEnableHashIntersection;

//
// plan cache
//

// The maximum number of entries allowed in a given collection's plan cache.
extern AtomicInt32 internalQueryCacheSize;

// How many feedback entries do we collect before possibly evicting from the cache based on bad
// performance?
extern AtomicInt32 internalQueryCacheFeedbacksStored;

// Limits the amount of debug info stored across all plan caches in the system. Once the estimate of
// the number of bytes used across all plan caches exceeds this threshold, then debug info is not
// stored alongside new cache entries, in order to limit plan cache memory consumption. If plan
// cache entries are freed and the estimate once again dips below this threshold, then new cache
// entries will once again have debug info associated with them.
extern AtomicInt64 internalQueryCacheMaxSizeBytesBeforeStripDebugInfo;

// How many times more works must we perform in order to justify plan cache eviction
// and replanning?
extern AtomicDouble internalQueryCacheEvictionRatio;

//
// Planning and enumeration.
//

// How many indexed solutions will QueryPlanner::plan output?
extern AtomicInt32 internalQueryPlannerMaxIndexedSolutions;

// If set to true, instructs the plan enumerator to enumerate contained $ors in a special order. $or
// enumeration can generate an exponential number of plans, and is therefore limited at some
// arbitrary cutoff controlled by a parameter. When this limit is hit, the order of enumeration is
// important. For example, a query like the following has a 'contained $or' (within an $and): {a: 1,
// $or: [{b: 1, c: 1}, {b: 2, c: 2}]} For this query if there are indexes a_b={a: 1, b: 1} and
// a_c={a: 1, c: 1}, the normal enumeration order would output assignments [a_b, a_b], [a_c, a_b],
// [a_b, a_c], then [a_c, a_c]. This flag will instruct the enumerator to instead prefer a different
// order. It's hard to summarize, but perhaps the phrases 'lockstep enumeration', 'simultaneous
// advancement', or 'parallel iteration' will help the reader. The effect is to give earlier
// enumeration to plans which use the same choice across all branches. In this order, we would get
// assignments [a_b, a_b], [a_c, a_c], [a_c, a_b], then [a_b, a_c]. This is thought to be helpful in
// general, but particularly in cases where all children of the $or use the same fields and have the
// same indexes available, as in this example.
extern AtomicBool internalQueryEnumerationPreferLockstepOrEnumeration;

// How many solutions will the enumerator consider at each OR?
extern AtomicInt32 internalQueryEnumerationMaxOrSolutions;

// How many intersections will the enumerator consider at each AND?
extern AtomicInt32 internalQueryEnumerationMaxIntersectPerAnd;

// Do we want to plan each child of the OR independently?
extern AtomicBool internalQueryPlanOrChildrenIndependently;

// How many index scans are we willing to produce in order to obtain a sort order
// during explodeForSort?
extern AtomicInt32 internalQueryMaxScansToExplode;

// Allow the planner to generate covered whole index scans, rather than falling back to a COLLSCAN.
extern AtomicBool internalQueryPlannerGenerateCoveredWholeIndexScans;

// Ignore unknown JSON Schema keywords.
extern AtomicBool internalQueryIgnoreUnknownJSONSchemaKeywords;

//
// Query execution.
//

extern AtomicInt32 internalQueryExecMaxBlockingSortBytes;

// Yield after this many "should yield?" checks.
extern AtomicInt32 internalQueryExecYieldIterations;

// Yield if it's been at least this many milliseconds since we last yielded.
extern AtomicInt32 internalQueryExecYieldPeriodMS;

// Limit the size that we write without yielding to 16MB / 64 (max expected number of indexes)
const int64_t insertVectorMaxBytes = 256 * 1024;

// The number of bytes to buffer at once during a $facet stage.
extern AtomicInt32 internalQueryFacetBufferSizeBytes;

// The maximum size in bytes of the $facet stage's output document.
extern AtomicInt64 internalQueryFacetMaxOutputDocSizeBytes;

extern AtomicInt64 internalLookupStageIntermediateDocumentMaxSizeBytes;

extern AtomicInt32 internalInsertMaxBatchSize;

extern AtomicInt32 internalDocumentSourceCursorBatchSizeBytes;

extern AtomicInt32 internalDocumentSourceLookupCacheSizeBytes;

extern AtomicBool internalQueryProhibitBlockingMergeOnMongoS;

extern AtomicInt32 internalQueryMaxPushBytes;

extern AtomicInt32 internalQueryMaxAddToSetBytes;

extern AtomicInt32 internalQueryMaxRangeBytes;

// The number of bytes after which explain should start truncating portions of its output.
extern AtomicInt32 internalQueryExplainSizeThresholdBytes;
}  // namespace mongo
