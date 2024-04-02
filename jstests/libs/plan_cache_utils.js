/**
 * Utility methods for reading planCache counters
 */

import {getCachedPlan, getPlanStage} from "jstests/libs/analyze_plan.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

export function getPlanCacheSize(db) {
    return db.serverStatus().metrics.query.planCache.totalSizeEstimateBytes;
}

export function getPlanCacheNumEntries(db) {
    return db.serverStatus().metrics.query.planCache.totalQueryShapes;
}

function getPlansForCacheEntry(coll, match) {
    const matchingCacheEntries = coll.getPlanCache().list([{$match: match}]);
    assert.eq(matchingCacheEntries.length, 1, coll.getPlanCache().list());
    return matchingCacheEntries[0];
}

function planHasIxScanStageForIndex(planStats, indexName) {
    const stage = getPlanStage(planStats, "IXSCAN");
    return (stage === null) ? false : indexName === stage.indexName;
}

export function assertCacheUsage(coll,
                                 pipeline,
                                 fromMultiPlanning,
                                 cacheEntryVersion,
                                 cacheEntryIsActive,
                                 cachedIndexName,
                                 aggOptions = {}) {
    const profileObj = getLatestProfilerEntry(
        coll.getDB(), {op: {$in: ["command", "getmore"]}, ns: coll.getFullName()});
    const queryHash = profileObj.queryHash;
    const planCacheKey = profileObj.planCacheKey;
    assert.eq(fromMultiPlanning, !!profileObj.fromMultiPlanner, profileObj);

    const entry = getPlansForCacheEntry(coll, {queryHash: queryHash});
    assert.eq(cacheEntryVersion, entry.version, entry);
    assert.eq(cacheEntryIsActive, entry.isActive, entry);

    // If the entry is active, we should have a plan cache key.
    if (entry.isActive) {
        assert(entry.planCacheKey);
    }
    if (planCacheKey) {
        assert.eq(entry.planCacheKey, planCacheKey);
        const explain = coll.explain().aggregate(pipeline, aggOptions);
        const explainKey = explain.hasOwnProperty("queryPlanner")
            ? explain.queryPlanner.planCacheKey
            : explain.stages[0].$cursor.queryPlanner.planCacheKey;
        assert.eq(explainKey, entry.planCacheKey, {explain: explain, cacheEntry: entry});
    }
    if (cacheEntryVersion === 2) {
        assert(entry.cachedPlan.stages.includes(cachedIndexName), entry);
    } else {
        assert(planHasIxScanStageForIndex(getCachedPlan(entry.cachedPlan), cachedIndexName), entry);
    }
}

export function setUpActiveCacheEntry(
    coll, pipeline, cacheEntryVersion, cachedIndexName, assertFn) {
    // For the first run, the query should go through multiplanning and create inactive cache entry.
    assertFn(coll.aggregate(pipeline));
    assertCacheUsage(coll,
                     pipeline,
                     true /*multiPlanning*/,
                     cacheEntryVersion,
                     false /*cacheEntryIsActive*/,
                     cachedIndexName);

    // After the second run, the inactive cache entry should be promoted to an active entry.
    assertFn(coll.aggregate(pipeline));
    assertCacheUsage(coll,
                     pipeline,
                     true /*multiPlanning*/,
                     cacheEntryVersion,
                     true /*cacheEntryIsActive*/,
                     cachedIndexName);

    // For the third run, the active cached query should be used.
    assertFn(coll.aggregate(pipeline));
    assertCacheUsage(coll,
                     pipeline,
                     false /*multiPlanning*/,
                     cacheEntryVersion,
                     true /*cacheEntryIsActive*/,
                     cachedIndexName);
}
