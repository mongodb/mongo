/**
 * Test that plans with $group and $lookup lowered to SBE are cached and invalidated correctly.
 */
(function() {
"use strict";

load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db.plan_cache_pipeline;
const foreignColl = db.plan_cache_pipeline_foreign;

if (!checkSBEEnabled(db)) {
    jsTest.log("Skipping test because SBE is not enabled");
    MongoRunner.stopMongod(conn);
    return;
}

const sbeFullEnabled = checkSBEEnabled(db, ["featureFlagSbeFull"]);

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, a1: 1}));
assert.commandWorked(coll.createIndex({a: 1, a2: 1}));
function setupForeignColl(index) {
    foreignColl.drop();
    assert.commandWorked(foreignColl.insert({b: 1}));
    if (index) {
        assert.commandWorked(foreignColl.createIndex(index));
    }
}
assert.commandWorked(db.setProfilingLevel(2));

/**
 * Assert that the last aggregation command has a corresponding plan cache entry with the desired
 * properties. 'version' is 1 if it's classic cache, 2 if it's SBE cache. 'isActive' is true if the
 * cache entry is active. 'fromMultiPlanner' is true if the query part of aggregation has been
 * multi-planned. 'forcesClassicEngine' is true if the query is forced to use classic engine.
 */
function assertCacheUsage({version, fromMultiPlanner, isActive, forcesClassicEngine = false}) {
    const profileObj = getLatestProfilerEntry(
        db, {op: "command", "command.pipeline": {$exists: true}, ns: coll.getFullName()});
    assert.eq(fromMultiPlanner, !!profileObj.fromMultiPlanner, profileObj);

    const entries = coll.getPlanCache().list();
    assert.eq(entries.length, 1, entries);
    const entry = entries[0];
    assert.eq(entry.version, version, entry);
    assert.eq(entry.isActive, isActive, entry);
    assert.eq(entry.planCacheKey, profileObj.planCacheKey, entry);

    const explain = coll.explain().aggregate(profileObj.command.pipeline);
    const queryPlanner = explain.hasOwnProperty("queryPlanner")
        ? explain.queryPlanner
        : explain.stages[0].$cursor.queryPlanner;
    if (!forcesClassicEngine) {
        assert(queryPlanner.winningPlan.hasOwnProperty("slotBasedPlan"), explain);
    }
    assert.eq(queryPlanner.planCacheKey, entry.planCacheKey, explain);

    return entry;
}

/**
 * Run the pipeline three times, assert that we have the following plan cache entries of "version".
 *      1. The pipeline runs from the multi-planner, saving an inactive cache entry.
 *      2. The pipeline runs from the multi-planner, activating the cache entry.
 *      3. The pipeline runs from cached solution planner, using the active cache entry.
 */
function testLoweredPipeline({pipeline, version, forcesClassicEngine = false}) {
    let results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);
    const entry = assertCacheUsage(
        {version: version, fromMultiPlanner: true, isActive: false, forcesClassicEngine});

    results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);
    let nextEntry = assertCacheUsage(
        {version: version, fromMultiPlanner: true, isActive: true, forcesClassicEngine});
    assert.eq(entry.planCacheKey, nextEntry.planCacheKey, {entry, nextEntry});

    results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);
    nextEntry = assertCacheUsage(
        {version: version, fromMultiPlanner: false, isActive: true, forcesClassicEngine});
    assert.eq(entry.planCacheKey, nextEntry.planCacheKey, {entry, nextEntry});

    return nextEntry;
}

const multiPlanningQueryStage = {
    $match: {a: 1}
};
const lookupStage = {
    $lookup: {from: foreignColl.getName(), localField: "a", foreignField: "b", as: "matched"}
};
const groupStage = {
    $group: {_id: "$a", out: {"$sum": 1}}
};

(function testLoweredPipelineCombination() {
    setupForeignColl();
    const expectedVersion = sbeFullEnabled ? 2 : 1;

    coll.getPlanCache().clear();
    testLoweredPipeline(
        {pipeline: [multiPlanningQueryStage, lookupStage], version: expectedVersion});

    coll.getPlanCache().clear();
    testLoweredPipeline(
        {pipeline: [multiPlanningQueryStage, groupStage], version: expectedVersion});

    coll.getPlanCache().clear();
    testLoweredPipeline(
        {pipeline: [multiPlanningQueryStage, lookupStage, groupStage], version: expectedVersion});

    coll.getPlanCache().clear();
    testLoweredPipeline(
        {pipeline: [multiPlanningQueryStage, groupStage, lookupStage], version: expectedVersion});
})();

(function testPartiallyLoweredPipeline() {
    coll.getPlanCache().clear();
    setupForeignColl();
    testLoweredPipeline({
        pipeline: [multiPlanningQueryStage, lookupStage, {$_internalInhibitOptimization: {}}],
        version: sbeFullEnabled ? 2 : 1
    });
})();

(function testNonExistentForeignCollectionCache() {
    if (!sbeFullEnabled) {
        jsTestLog("Skipping testNonExistentForeignCollectionCache when SBE is not fully enabled");
        return;
    }

    coll.getPlanCache().clear();
    foreignColl.drop();
    const entryWithoutForeignColl =
        testLoweredPipeline({pipeline: [multiPlanningQueryStage, lookupStage], version: 2});

    coll.getPlanCache().clear();
    setupForeignColl();
    const entryWithForeignColl =
        testLoweredPipeline({pipeline: [multiPlanningQueryStage, lookupStage], version: 2});

    assert.neq(entryWithoutForeignColl.planCacheKey,
               entryWithForeignColl.planCacheKey,
               {entryWithoutForeignColl, entryWithForeignColl});
    assert.eq(entryWithoutForeignColl.queryHash,
              entryWithForeignColl.queryHash,
              {entryWithoutForeignColl, entryWithForeignColl});
})();

(function testForeignCollectionDropCacheInvalidation() {
    if (!sbeFullEnabled) {
        jsTestLog(
            "Skipping testForeignCollectionDropCacheInvalidation when SBE is not fully enabled");
        return;
    }

    coll.getPlanCache().clear();
    setupForeignColl();
    testLoweredPipeline({pipeline: [multiPlanningQueryStage, lookupStage], version: 2});

    foreignColl.drop();
    testLoweredPipeline({pipeline: [multiPlanningQueryStage, lookupStage], version: 2});
})();

(function testForeignIndexDropCacheInvalidation() {
    if (!sbeFullEnabled) {
        jsTestLog("Skipping testForeignIndexDropCacheInvalidation when SBE is not fully enabled");
        return;
    }

    coll.getPlanCache().clear();
    setupForeignColl({b: 1} /* index */);
    testLoweredPipeline({pipeline: [multiPlanningQueryStage, lookupStage], version: 2});

    assert.commandWorked(foreignColl.dropIndex({b: 1}));
    testLoweredPipeline({pipeline: [multiPlanningQueryStage, lookupStage], version: 2});
})();

(function testForeignIndexBuildCacheInvalidation() {
    if (!sbeFullEnabled) {
        jsTestLog("Skipping testForeignIndexBuildCacheInvalidation when SBE is not fully enabled");
        return;
    }

    coll.getPlanCache().clear();
    setupForeignColl({b: 1} /* index */);
    testLoweredPipeline({pipeline: [multiPlanningQueryStage, lookupStage], version: 2});

    assert.commandWorked(foreignColl.createIndex({c: 1}));
    testLoweredPipeline({pipeline: [multiPlanningQueryStage, lookupStage], version: 2});
})();

(function testLookupSbeAndClassicPlanCacheKey() {
    if (!sbeFullEnabled) {
        jsTestLog("Skipping testLookupWithClassicPlanCache when SBE is not fully enabled");
        return;
    }

    setupForeignColl({b: 1} /* index */);

    // When using SBE engine, the plan cache key of $match vs. $match + $lookup should be different.
    coll.getPlanCache().clear();
    let matchEntry = testLoweredPipeline({pipeline: [multiPlanningQueryStage], version: 2});

    coll.getPlanCache().clear();
    let lookupEntry =
        testLoweredPipeline({pipeline: [multiPlanningQueryStage, lookupStage], version: 2});
    assert.neq(matchEntry.planCacheKey, lookupEntry.planCacheKey, {matchEntry, lookupEntry});

    // When using classic engine, the plan cache key of $match vs. $match + $lookup should be the
    // same.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));

    coll.getPlanCache().clear();
    matchEntry = testLoweredPipeline(
        {pipeline: [multiPlanningQueryStage], version: 1, forcesClassicEngine: true});

    coll.getPlanCache().clear();
    lookupEntry = testLoweredPipeline(
        {pipeline: [multiPlanningQueryStage, lookupStage], version: 1, forcesClassicEngine: true});
    assert.eq(matchEntry.planCacheKey, lookupEntry.planCacheKey, {matchEntry, lookupEntry});

    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));
})();

MongoRunner.stopMongod(conn);
})();
