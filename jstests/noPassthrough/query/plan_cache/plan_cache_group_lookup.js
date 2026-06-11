/**
 * Test that plans with $group and $lookup are cached correctly.
 * @tags: [
 *   featureFlagSbeFull
 * ]
 */
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
 * Assert that executing the aggregation 'pipeline' returns exactly one result, and has a
 * corresponding plan cache entry with the desired properties.
 * 'isActive' is true if the cache entry is active.
 * 'fromMultiPlanner' is true if the query part of aggregation has been multi-planned.
 * 'fromPlanCache' is true if the winning plan was retrieved from the plan cache.
 */
function assertCacheUsage({pipeline, fromMultiPlanner, fromPlanCache, isActive}) {
    let profileObj;

    // Using 'assert.soon()' here to skip over transient situations in which a query
    // plan cannot be added to the plan cache.
    assert.soon(() => {
        let results = coll.aggregate(pipeline).toArray();
        assert.eq(results.length, 1, results);

        profileObj = getLatestProfilerEntry(db, {
            op: "command",
            "command.pipeline": {$exists: true},
            ns: coll.getFullName(),
        });
        assert.eq(fromMultiPlanner, !!profileObj.fromMultiPlanner, profileObj);
        return !fromPlanCache || !!profileObj.fromPlanCache;
    });

    const entries = coll.getPlanCache().list();

    assert.eq(fromPlanCache, !!profileObj.fromPlanCache, () => {
        return `Query not served from plan cache.\nProfile: ${tojson(profileObj)}\n` + `Plan cache: ${tojson(entries)}`;
    });

    assert.eq(entries.length, 1, entries);
    const entry = entries[0];
    assert.eq(entry.version, 1, entry);
    assert.eq(entry.isActive, isActive, entry);
    assert.eq(entry.planCacheKey, profileObj.planCacheKey, entry);

    const explain = coll.explain().aggregate(profileObj.command.pipeline);
    const queryPlanner = explain.hasOwnProperty("queryPlanner")
        ? explain.queryPlanner
        : explain.stages[0].$cursor.queryPlanner;
    assert.eq(queryPlanner.planCacheKey, entry.planCacheKey, explain);

    return entry;
}

/**
 * Run the pipeline three times, assert that we have the following plan cache entries.
 *      1. The pipeline runs from the multi-planner, saving an inactive cache entry.
 *      2. The pipeline runs from the multi-planner, activating the cache entry.
 *      3. The pipeline runs from cached solution planner, using the active cache entry.
 */
function testPipelineCaching({pipeline}) {
    const entry = assertCacheUsage({
        pipeline,
        fromMultiPlanner: true,
        fromPlanCache: false,
        isActive: false,
    });

    let nextEntry = assertCacheUsage({
        pipeline,
        fromMultiPlanner: true,
        fromPlanCache: false,
        isActive: true,
    });
    assert.eq(entry.planCacheKey, nextEntry.planCacheKey, {entry, nextEntry});

    nextEntry = assertCacheUsage({
        pipeline,
        fromMultiPlanner: false,
        fromPlanCache: true,
        isActive: true,
    });
    assert.eq(entry.planCacheKey, nextEntry.planCacheKey, {entry, nextEntry});

    return nextEntry;
}

const matchStage = {
    $match: {a: 1},
};
const lookupStage = {
    $lookup: {from: foreignColl.getName(), localField: "a", foreignField: "b", as: "matched"},
};
const groupStage = {
    $group: {_id: "$a", out: {"$sum": 1}},
};

(function testPipelineCombination() {
    setupForeignColl();

    coll.getPlanCache().clear();
    testPipelineCaching({pipeline: [matchStage, lookupStage]});

    coll.getPlanCache().clear();
    testPipelineCaching({pipeline: [matchStage, groupStage]});

    coll.getPlanCache().clear();
    testPipelineCaching({pipeline: [matchStage, lookupStage, groupStage]});

    coll.getPlanCache().clear();
    testPipelineCaching({pipeline: [matchStage, groupStage, lookupStage]});
})();

(function testLookupClassicPlanCacheKey() {
    setupForeignColl({b: 1} /* index */);

    // In classic mode, the plan cache key of $match vs. $match + $lookup should be the same,
    // since the classic cache only considers the query shape.
    coll.getPlanCache().clear();
    let matchEntry = testPipelineCaching({pipeline: [matchStage]});

    coll.getPlanCache().clear();
    let lookupEntry = testPipelineCaching({pipeline: [matchStage, lookupStage]});
    assert.eq(matchEntry.planCacheKey, lookupEntry.planCacheKey, {matchEntry, lookupEntry});
})();

MongoRunner.stopMongod(conn);
