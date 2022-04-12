/**
 * Test that plans with $group and $lookup lowered to SBE are cached and replanned as appropriate.
 * @tags: [
 *   requires_profiling,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/log.js");  // For findMatchingLogLine.
load("jstests/libs/profiler.js");
load("jstests/libs/sbe_util.js");      // For checkSBEEnabled.
load("jstests/libs/analyze_plan.js");  // For 'getAggPlanStages()'

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db.plan_cache_replan_group_lookup;
const foreignCollName = "foreign";
coll.drop();

if (checkSBEEnabled(db, ["featureFlagSbePlanCache"])) {
    jsTest.log("Skipping test because SBE and SBE plan cache are both enabled.");
    MongoRunner.stopMongod(conn);
    return;
}
// The test should have the same caching behavior whether or not aggregation stages are being
// lowered into SBE.

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
    const profileObj = getLatestProfilerEntry(db, {op: "command", ns: coll.getFullName()});
    const queryHash = profileObj.queryHash;
    assert.eq(multiPlanning, !!profileObj.fromMultiPlanner);

    const entry = getPlansForCacheEntry({queryHash: queryHash});
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

function testFn(aIndexPipeline,
                bIndexPipeline,
                setUpFn = undefined,
                tearDownFn = undefined,
                explainFn = undefined) {
    if (setUpFn) {
        setUpFn();
    }

    if (explainFn) {
        explainFn(aIndexPipeline);
        explainFn(bIndexPipeline);
    }

    // For the first run, the query should go through multiplanning and create inactive cache entry.
    assert.eq(2, coll.aggregate(aIndexPipeline).toArray()[0].n);
    assertCacheUsage(true /*multiPlanning*/, false /*activeCacheEntry*/, {a: 1} /*cachedIndex*/);

    // After the second run, the inactive cache entry should be promoted to an active entry.
    assert.eq(2, coll.aggregate(aIndexPipeline).toArray()[0].n);
    assertCacheUsage(true /*multiPlanning*/, true /*activeCacheEntry*/, {a: 1} /*cachedIndex*/);

    // For the third run, the active cached query should be used.
    assert.eq(2, coll.aggregate(aIndexPipeline).toArray()[0].n);
    assertCacheUsage(false /*multiPlanning*/, true /*activeCacheEntry*/, {a: 1} /*cachedIndex*/);

    // Now run the other pipeline, which has the same query shape but is faster with a different
    // index. It should trigger re-planning of the query.
    assert.eq(3, coll.aggregate(bIndexPipeline).toArray()[0].n);
    assertCacheUsage(true /*multiPlanning*/, true /*activeCacheEntry*/, {b: 1} /*cachedIndex*/);

    // The other pipeline again, The cache should be used now.
    assert.eq(3, coll.aggregate(bIndexPipeline).toArray()[0].n);
    assertCacheUsage(false /*multiPlanning*/, true /*activeCacheEntry*/, {b: 1} /*cachedIndex*/);
    if (tearDownFn) {
        tearDownFn();
    }

    coll.getPlanCache().clear();
}

// This pipeline will be quick with {a: 1} index, and far slower {b: 1} index. With the {a: 1}
// index, the server should only need to examine one document. Using {b: 1}, it will have to
// scan through each document which has 2 as the value of the 'b' field.
const aIndexPredicate = [{$match: {a: 1042, b: 1}}];

// Opposite of 'aIndexQuery'. Should be quick if the {b: 1} index is used, and slower if the
// {a: 1} index is used.
const bIndexPredicate = [{$match: {a: 1, b: 1042}}];

// $group tests.
const groupSuffix = [{$group: {_id: "$c"}}, {$count: "n"}];
testFn(aIndexPredicate.concat(groupSuffix), bIndexPredicate.concat(groupSuffix));

// $lookup tests.
const lookupStage =
    [{$lookup: {from: foreignCollName, localField: 'c', foreignField: 'foreignKey', as: 'out'}}];
const aLookup = aIndexPredicate.concat(lookupStage).concat(groupSuffix);
const bLookup = bIndexPredicate.concat(lookupStage).concat(groupSuffix);

function createLookupForeignColl() {
    const foreignColl = db[foreignCollName];

    // Here, the values for 'foreignKey' are expected to match existing values for 'c' in 'coll'.
    assert.commandWorked(foreignColl.insert([{foreignKey: 8}, {foreignKey: 6}]));
}

function dropLookupForeignColl() {
    assert(db[foreignCollName].drop());
}

const lookupPushdownEnabled = checkSBEEnabled(db, ["featureFlagSBELookupPushdown"]);
function verifyCorrectLookupAlgorithmUsed(targetJoinAlgorithm, pipeline, options = {}) {
    if (!lookupPushdownEnabled) {
        return;
    }
    const explain = coll.explain().aggregate(pipeline, options);
    const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");

    // Verify via explain that $lookup was lowered and appropriate $lookup algorithm was chosen.
    assert.eq(
        eqLookupNodes.length, 1, "expected at least one EQ_LOOKUP node; got " + tojson(explain));
    assert.eq(eqLookupNodes[0].strategy, targetJoinAlgorithm);
}

// NLJ.
testFn(aLookup,
       bLookup,
       createLookupForeignColl,
       dropLookupForeignColl,
       (pipeline) =>
           verifyCorrectLookupAlgorithmUsed("NestedLoopJoin", pipeline, {allowDiskUse: false}));

// INLJ.
testFn(aLookup,
       bLookup,
       () => {
           createLookupForeignColl();
           assert.commandWorked(db[foreignCollName].createIndex({foreignKey: 1}));
       },
       dropLookupForeignColl,
       (pipeline) => verifyCorrectLookupAlgorithmUsed("IndexedLoopJoin", pipeline));

// TODO SERVER-64443: Verify that replanning works when HashLookupStage is implemented.

// Verify that changing the plan for the right side does not trigger a replan.
const foreignColl = db[foreignCollName];
coll.drop();
foreignColl.drop();
coll.getPlanCache().clear();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.insert([
    {_id: 0, a: 1, b: 1},
    {_id: 1, a: 1, b: 2},
    {_id: 2, a: 1, b: 3},
    {_id: 3, a: 1, b: 4},
]));

assert.commandWorked(foreignColl.createIndex({c: 1}));
for (let i = -30; i < 30; ++i) {
    assert.commandWorked(foreignColl.insert({_id: i, c: i}));
}

const avoidReplanPipeline = [
    {$match: {a: 1, b: 3}},
    {$lookup: {from: foreignColl.getName(), as: "as", localField: "a", foreignField: "c"}}
];
function runQuery(options = {}) {
    assert.eq([{_id: 2, a: 1, b: 3, as: [{_id: 1, c: 1}]}],
              coll.aggregate(avoidReplanPipeline, options).toArray());
}

// Verify that we are using IndexedLoopJoin.
verifyCorrectLookupAlgorithmUsed("IndexedLoopJoin", avoidReplanPipeline);

runQuery();
assertCacheUsage(true /*multiPlanning*/, false /*activeCacheEntry*/, {b: 1} /*cachedIndex*/);

runQuery();
assertCacheUsage(true /*multiPlanning*/, true /*activeCacheEntry*/, {b: 1} /*cachedIndex*/);

// After dropping the index on the right-hand side, we should NOT replan the cached query. We
// will, however, choose a different join algorithm.
assert.commandWorked(foreignColl.dropIndex({c: 1}));

// Verify that we are now using NestedLoopJoin.
verifyCorrectLookupAlgorithmUsed("NestedLoopJoin", avoidReplanPipeline, {allowDiskUse: false});

runQuery({allowDiskUse: false});
assertCacheUsage(false /*multiPlanning*/, true /*activeCacheEntry*/, {b: 1} /*cachedIndex*/);
runQuery({allowDiskUse: false});
assertCacheUsage(false /*multiPlanning*/, true /*activeCacheEntry*/, {b: 1} /*cachedIndex*/);

// Run with 'allowDiskUse: true'. This should now use HashJoin, and we should still avoid
// replanning the cached query.

verifyCorrectLookupAlgorithmUsed("HashJoin", avoidReplanPipeline, {allowDiskUse: true});

runQuery({allowDiskUse: true});
assertCacheUsage(false /*multiPlanning*/, true /*activeCacheEntry*/, {b: 1} /*cachedIndex*/);
runQuery({allowDiskUse: true});
assertCacheUsage(false /*multiPlanning*/, true /*activeCacheEntry*/, {b: 1} /*cachedIndex*/);
MongoRunner.stopMongod(conn);
}());
