/**
 * Test that plans with $group and $lookup lowered to SBE are cached and invalidated correctly.
 * @tags: [
 *   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
 *   cqf_experimental_incompatible,
 *   featureFlagSbeFull
 * ]
 */
import {checkCascadesOptimizerEnabled} from "jstests/libs/optimizer_utils.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());
const colName = jsTestName();
const coll = db.getCollection(colName);
const foreignColl = db.getCollection(colName + "_foreign");

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, a1: 1}));
assert.commandWorked(coll.createIndex({a: -1, a2: 1}));
function setupForeignColl(index) {
    foreignColl.drop();
    assert.commandWorked(foreignColl.insert({b: 1}));
    if (index) {
        assert.commandWorked(foreignColl.createIndex(index));
    }
}
assert.commandWorked(db.setProfilingLevel(2));

/**
 * Assert that the last aggregation command has a corresponding plan cache entry with the
 * desired properties. 'version' is 1 if it's classic cache, 2 if it's SBE cache. 'isActive' is
 * true if the cache entry is active. 'fromMultiPlanner' is true if the query part of
 * aggregation has been multi-planned. 'fromPlanCache' is true if the winning plan was retrieved
 * from the plan cache. 'forcesClassicEngine' is true if the query is forced to use classic
 * engine.
 */
function assertCacheUsage(
    {version, fromMultiPlanner, fromPlanCache, isActive, forcesClassicEngine}) {
    const profileObj = getLatestProfilerEntry(
        db, {op: "command", "command.pipeline": {$exists: true}, ns: coll.getFullName()});
    assert.eq(fromMultiPlanner, !!profileObj.fromMultiPlanner, profileObj);

    assert.eq(fromPlanCache, !!profileObj.fromPlanCache, profileObj);

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
 * Run the pipeline three times, assert that we have the following plan cache entries of
 * "version".
 *      1. The pipeline runs from the multi-planner, saving an inactive cache entry.
 *      2. The pipeline runs from the multi-planner, activating the cache entry.
 *      3. The pipeline runs from cached solution planner, using the active cache entry.
 */
function testLoweredPipeline({pipeline, version, forcesClassicEngine = false}) {
    let results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);
    const entry = assertCacheUsage({
        version: version,
        fromMultiPlanner: true,
        fromPlanCache: false,
        isActive: false,
        forcesClassicEngine
    });

    results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);
    let nextEntry = assertCacheUsage({
        version: version,
        fromMultiPlanner: true,
        fromPlanCache: false,
        isActive: true,
        forcesClassicEngine
    });
    assert.eq(entry.planCacheKey, nextEntry.planCacheKey, {entry, nextEntry});

    results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);
    nextEntry = assertCacheUsage({
        version: version,
        fromMultiPlanner: false,
        fromPlanCache: true,
        isActive: true,
        forcesClassicEngine
    });
    assert.eq(entry.planCacheKey, nextEntry.planCacheKey, {entry, nextEntry});

    return nextEntry;
}

const matchStage = {
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
    const expectedVersion = 2;

    coll.getPlanCache().clear();
    testLoweredPipeline({pipeline: [matchStage, lookupStage], version: expectedVersion});

    coll.getPlanCache().clear();
    testLoweredPipeline({pipeline: [matchStage, groupStage], version: expectedVersion});

    coll.getPlanCache().clear();
    testLoweredPipeline(
        {pipeline: [matchStage, lookupStage, groupStage], version: expectedVersion});

    coll.getPlanCache().clear();
    testLoweredPipeline(
        {pipeline: [matchStage, groupStage, lookupStage], version: expectedVersion});
})();

(function testPartiallyLoweredPipeline() {
    coll.getPlanCache().clear();
    setupForeignColl();
    testLoweredPipeline(
        {pipeline: [matchStage, lookupStage, {$_internalInhibitOptimization: {}}], version: 2});
})();

(function testNonExistentForeignCollectionCache() {
    coll.getPlanCache().clear();
    foreignColl.drop();
    const entryWithoutForeignColl =
        testLoweredPipeline({pipeline: [matchStage, lookupStage], version: 2});

    coll.getPlanCache().clear();
    setupForeignColl();
    const entryWithForeignColl =
        testLoweredPipeline({pipeline: [matchStage, lookupStage], version: 2});

    assert.neq(entryWithoutForeignColl.planCacheKey,
               entryWithForeignColl.planCacheKey,
               {entryWithoutForeignColl, entryWithForeignColl});
    assert.eq(entryWithoutForeignColl.queryHash,
              entryWithForeignColl.queryHash,
              {entryWithoutForeignColl, entryWithForeignColl});
})();

(function testForeignCollectionDropCacheInvalidation() {
    coll.getPlanCache().clear();
    setupForeignColl();
    testLoweredPipeline({pipeline: [matchStage, lookupStage], version: 2});

    foreignColl.drop();
    testLoweredPipeline({pipeline: [matchStage, lookupStage], version: 2});
})();

(function testForeignIndexDropCacheInvalidation() {
    coll.getPlanCache().clear();
    setupForeignColl({b: 1} /* index */);
    testLoweredPipeline({pipeline: [matchStage, lookupStage], version: 2});

    assert.commandWorked(foreignColl.dropIndex({b: 1}));
    testLoweredPipeline({pipeline: [matchStage, lookupStage], version: 2});
})();

(function testForeignIndexBuildCacheInvalidation() {
    coll.getPlanCache().clear();
    setupForeignColl({b: 1} /* index */);
    testLoweredPipeline({pipeline: [matchStage, lookupStage], version: 2});

    assert.commandWorked(foreignColl.createIndex({c: 1}));
    testLoweredPipeline({pipeline: [matchStage, lookupStage], version: 2});
})();

(function testLookupSbeAndClassicPlanCacheKey() {
    setupForeignColl({b: 1} /* index */);

    // When using SBE engine, the plan cache key of $match vs. $match + $lookup should be
    // different.
    coll.getPlanCache().clear();
    let matchEntry = testLoweredPipeline({pipeline: [matchStage], version: 2});

    coll.getPlanCache().clear();
    let lookupEntry = testLoweredPipeline({pipeline: [matchStage, lookupStage], version: 2});
    assert.neq(matchEntry.planCacheKey, lookupEntry.planCacheKey, {matchEntry, lookupEntry});

    // When using classic engine, the plan cache key of $match vs. $match + $lookup should be
    // the same.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));

    coll.getPlanCache().clear();
    matchEntry =
        testLoweredPipeline({pipeline: [matchStage], version: 1, forcesClassicEngine: true});

    coll.getPlanCache().clear();
    lookupEntry = testLoweredPipeline(
        {pipeline: [matchStage, lookupStage], version: 1, forcesClassicEngine: true});
    assert.eq(matchEntry.planCacheKey, lookupEntry.planCacheKey, {matchEntry, lookupEntry});

    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));
})();

MongoRunner.stopMongod(conn);
