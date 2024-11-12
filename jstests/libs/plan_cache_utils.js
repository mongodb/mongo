/**
 * Utility methods for reading planCache counters
 */

import {
    getCachedPlan,
    getEngine,
    getPlanCacheShapeHashFromObject,
    getPlanStage
} from "jstests/libs/analyze_plan.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

export function getPlanCacheSize(db) {
    return db.serverStatus().metrics.query.planCache.totalSizeEstimateBytes;
}

export function getPlanCacheNumEntries(db) {
    return db.serverStatus().metrics.query.planCache.totalQueryShapes;
}

function getPlansForCacheEntry(coll, match) {
    const matchingCacheEntries = coll.getPlanCache().list([{$match: match}]);
    assert.eq(matchingCacheEntries.length,
              1,
              () => tojson(match) + " Contents of cache: " + tojson(coll.getPlanCache().list()));
    return matchingCacheEntries[0];
}

function planHasIxScanStageForIndex(planStats, indexName) {
    const stage = getPlanStage(planStats, "IXSCAN");
    return (stage === null) ? false : indexName === stage.indexName;
}

/**
 * Checks the profiler to ensure that the given latest aggregate command ('pipeline') used
 * the plan cache as expected.  'queryColl' and 'planCacheColl' are separate arguments to
 * support queries on views, where the query runs on 'queryColl' but the plan cache entry is
 * associated with 'planCacheColl.' For non-views, 'planCacheColl' can be omitted.
 */
export function assertCacheUsage({
    queryColl,
    planCacheColl,
    pipeline,
    fromMultiPlanning,
    cacheEntryVersion,
    cacheEntryIsActive,
    cachedIndexName,
    aggOptions = {}
}) {
    if (!planCacheColl) {
        planCacheColl = queryColl;
    }

    const profileObj = getLatestProfilerEntry(
        queryColl.getDB(), {op: {$in: ["command", "getmore"]}, ns: queryColl.getFullName()});
    const planCacheShapeHash = getPlanCacheShapeHashFromObject(profileObj);
    const planCacheKey = profileObj.planCacheKey;
    assert.eq(fromMultiPlanning, !!profileObj.fromMultiPlanner, profileObj);

    const entry = getPlansForCacheEntry(planCacheColl, {planCacheShapeHash: planCacheShapeHash});
    assert.eq(cacheEntryVersion, entry.version, entry);
    assert.eq(cacheEntryIsActive, entry.isActive, entry);

    const explain = queryColl.explain().aggregate(pipeline, aggOptions);

    if (entry.version === "2") {
        // SBE plan cache always tracks "reads."
        assert.eq(entry.worksType, "reads");
    } else if (entry.version == "1") {
        if (getEngine(explain) == "sbe") {
            assert.eq(entry.worksType, "reads");
        } else {
            assert.eq(entry.worksType, "works");
        }
    }

    // If the entry is active, we should have a plan cache key.
    if (entry.isActive) {
        assert(entry.planCacheKey);
    }
    if (planCacheKey) {
        assert.eq(entry.planCacheKey, planCacheKey);
        const explainKey = explain.hasOwnProperty("queryPlanner")
            ? explain.queryPlanner.planCacheKey
            : explain.stages[0].$cursor.queryPlanner.planCacheKey;
        assert.eq(explainKey, entry.planCacheKey, {explain: explain, cacheEntry: entry});
    }

    if (cachedIndexName) {
        if (cacheEntryVersion === 2) {
            assert(entry.cachedPlan.stages.includes(cachedIndexName), entry);
        } else {
            assert(planHasIxScanStageForIndex(getCachedPlan(entry.cachedPlan), cachedIndexName),
                   entry);
        }
    }
    return entry;
}

export function setUpActiveCacheEntry(
    coll, pipeline, cacheEntryVersion, cachedIndexName, assertFn) {
    // For the first run, the query should go through multiplanning and create inactive cache entry.
    assertFn(coll.aggregate(pipeline));
    assertCacheUsage({
        queryColl: coll,
        pipeline,
        fromMultiPlanning: true,
        cacheEntryVersion,
        cacheEntryIsActive: false,
        cachedIndexName
    });

    // After the second run, the inactive cache entry should be promoted to an active entry.
    assertFn(coll.aggregate(pipeline));
    assertCacheUsage({
        queryColl: coll,
        pipeline,
        fromMultiPlanning: true,
        cacheEntryVersion,
        cacheEntryIsActive: true,
        cachedIndexName
    });

    // For the third run, the active cached query should be used.
    assertFn(coll.aggregate(pipeline));
    assertCacheUsage({
        queryColl: coll,
        pipeline,
        fromMultiPlanning: false,
        cacheEntryVersion,
        cacheEntryIsActive: true,
        cachedIndexName
    });
}
