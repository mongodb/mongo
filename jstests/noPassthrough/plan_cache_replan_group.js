/**
 * Test that plans with $group lowered to SBE are cached and replanned as appropriate.
 * @tags: [
 *   requires_profiling,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/log.js");  // For findMatchingLogLine.
load("jstests/libs/profiler.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db.plan_cache_replan_group;
coll.drop();

if (checkSBEEnabled(db, ["featureFlagSbePlanCache"])) {
    jsTest.log("Skipping test because SBE and SBE plan cache are both enabled.");
    MongoRunner.stopMongod(conn);
    return;
}
// The test should have the same caching behavior whether or not $group is being lowered into SBE.

function getPlansForCacheEntry(match) {
    const matchingCacheEntries = coll.getPlanCache().list([{$match: match}]);
    assert.eq(matchingCacheEntries.length, 1, coll.getPlanCache().list());
    return matchingCacheEntries[0];
}

function planHasIxScanStageForKey(planStats, keyPattern) {
    const stage = getPlanStage(planStats, "IXSCAN");
    if (stage === null) {
        return false;
    }

    return bsonWoCompare(keyPattern, stage.keyPattern) == 0;
}

function assertCacheUsage(multiPlanning, activeCacheEntry, cachedIndex) {
    let profileObj = getLatestProfilerEntry(db, {op: "command", ns: coll.getFullName()});
    let queryHash = profileObj.queryHash;
    assert.eq(multiPlanning, !!profileObj.fromMultiPlanner);

    let entry = getPlansForCacheEntry({queryHash: queryHash});
    assert.eq(activeCacheEntry, entry.isActive);
    assert.eq(planHasIxScanStageForKey(getCachedPlan(entry.cachedPlan), cachedIndex), true, entry);
}

assert.commandWorked(db.setProfilingLevel(2));

// Carefully construct a collection so that some queries will do well with an {a: 1} index
// and others with a {b: 1} index.
for (let i = 1000; i < 1100; i++) {
    assert.commandWorked(coll.insert({a: 1, b: i, c: 5}));
    assert.commandWorked(coll.insert({a: 1, b: i, c: 6}));
    assert.commandWorked(coll.insert({a: 1, b: i, c: 7}));
}
for (let i = 1000; i < 1100; i++) {
    assert.commandWorked(coll.insert({a: i, b: 1, c: 5}));
    assert.commandWorked(coll.insert({a: i, b: 1, c: 8}));
    assert.commandWorked(coll.insert({a: i, b: 1, c: 8}));
}
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// This pipeline will be quick with {a: 1} index, and far slower {b: 1} index. With the {a: 1}
// index, the server should only need to examine one document. Using {b: 1}, it will have to
// scan through each document which has 2 as the value of the 'b' field.
const aIndexPipeline = [{$match: {a: 1042, b: 1}}, {$group: {_id: "$c"}}, {$count: "n"}];
// Opposite of 'aIndexQuery'. Should be quick if the {b: 1} index is used, and slower if the
// {a: 1} index is used.
const bIndexPipeline = [{$match: {a: 1, b: 1042}}, {$group: {_id: "$c"}}, {$count: "n"}];

// For the first run, the query should go through multiplanning and create inactive cache entry.
assert.eq(2, coll.aggregate(aIndexPipeline).toArray()[0].n);
assertCacheUsage(true /*multiPlanning*/, false /*activeCacheEntry*/, {a: 1} /*cachedIndex*/);

// After the second run, the inactive cache entry should be promoted to an active entry.
assert.eq(2, coll.aggregate(aIndexPipeline).toArray()[0].n);
assertCacheUsage(true /*multiPlanning*/, true /*activeCacheEntry*/, {a: 1} /*cachedIndex*/);

// For the third run, the active cached query should be used.
assert.eq(2, coll.aggregate(aIndexPipeline).toArray()[0].n);
assertCacheUsage(false /*multiPlanning*/, true /*activeCacheEntry*/, {a: 1} /*cachedIndex*/);

// Now run the other pipeline, which has the same query shape but is faster with a different index.
// It should trigger re-planning of the query.
assert.eq(3, coll.aggregate(bIndexPipeline).toArray()[0].n);
assertCacheUsage(true /*multiPlanning*/, true /*activeCacheEntry*/, {b: 1} /*cachedIndex*/);

// The other pipeline again, The cache should be used now.
assert.eq(3, coll.aggregate(bIndexPipeline).toArray()[0].n);
assertCacheUsage(false /*multiPlanning*/, true /*activeCacheEntry*/, {b: 1} /*cachedIndex*/);

MongoRunner.stopMongod(conn);
}());
