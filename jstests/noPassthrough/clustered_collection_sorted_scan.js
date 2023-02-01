/**
 * Tests that clustered collections can be used for sorted scanning without inserting
 * a blocking scan operator.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/clustered_collections/clustered_collection_util.js");

Random.setRandomSeed();

const testConnection =
    MongoRunner.runMongod({setParameter: {supportArbitraryClusterKeyIndex: true}});
const testDb = testConnection.getDB('local');
const collectionSize = 10;
const clusteredCollName = "clustered_index_sorted_scan_coll";
const clusterField = "clusterKey";

let nonClusteredCollName = clusteredCollName + "_nc";

// Generate a clustered collection for the remainder of the testing
assert.commandWorked(testDb.createCollection(
    clusteredCollName, {clusteredIndex: {key: {[clusterField]: 1}, unique: true}}));
let clusteredColl = testDb[clusteredCollName];

// Generate a non-clustered collection for comparison
assert.commandWorked(testDb.createCollection(nonClusteredCollName));
assert.commandWorked(testDb[nonClusteredCollName].createIndex({[clusterField]: 1}, {unique: true}));
let nonClusteredColl = testDb[nonClusteredCollName];

// Put something in the collections so the planner has something to chew on.
for (let i = 0; i < collectionSize; ++i) {
    let a = Math.abs(Random.rand());
    assert.commandWorked(clusteredColl.insert({[clusterField]: i, a: a}));
    assert.commandWorked(nonClusteredColl.insert({[clusterField]: i, a: a}));
}

function runTest(isClustered, hasFilter, hasHint, direction) {
    let tsColl = isClustered ? clusteredColl : nonClusteredColl;

    const filter = hasFilter ? {[clusterField]: {$gt: -1}} : {};
    const sort = {[clusterField]: direction};
    const hint = hasHint ? {[clusterField]: 1} : {};

    let query = tsColl.find(filter).sort(sort).hint(hint);

    function formatParamsAndPlan(plan) {
        let params = {
            isClustered: isClustered ? "true" : "false",
            hasFilter: hasFilter ? "true" : "false",
            hasHint: hasHint ? "true" : "false",
            direction: direction ? "forward" : "backward",
        };

        return tojson(params) + tojson(plan);
    }

    let plan = query.explain();
    if (isClustered) {
        let collScan =
            hasFilter ? getPlanStage(plan, "CLUSTERED_IXSCAN") : getPlanStage(plan, "COLLSCAN");

        assert.neq(collScan, null, "Expected collscan in " + formatParamsAndPlan(plan));
        assert.eq(collScan.direction,
                  (direction > 0) ? "forward" : "backward",
                  "Incorrect scan direction in " + formatParamsAndPlan(plan));
    } else {
        assert(planHasStage(testDb, plan, "FETCH"),
               "Expected fetch in " + formatParamsAndPlan(plan));
    }
    assert(!planHasStage(testDb, plan, "SORT"), "Unexpected sort in " + formatParamsAndPlan(plan));
}

function testCollations(direction) {
    let strCollName = clusteredCollName + "_str";

    // Generate a clustered collection for the remainder of the testing
    assert.commandWorked(testDb.createCollection(
        strCollName, {clusteredIndex: {key: {[clusterField]: 1}, unique: true}}));

    let tsColl = testDb[strCollName];

    // Put something in the collection so the planner has something to chew on.
    for (let i = 0; i < collectionSize; ++i) {
        assert.commandWorked(tsColl.insert({[clusterField]: i.toString(), a: Math.random()}));
    }

    // Run query with Faroese collation, just to choose something unlikely.
    // Because the collations don't match, we can't use the clustered index
    // to provide a sort
    let plan = tsColl.find()
                   .sort({[clusterField]: direction})
                   .collation({locale: "fo", caseLevel: true})
                   .explain();
    assert(planHasStage(testDb, plan, "SORT"), "Expected sort in " + tojson(plan));

    // However, if we can exclude strings, we don't need an explicit sort even
    // if the collations don't match
    plan = tsColl.find({[clusterField]: {$gt: -1}})
               .sort({[clusterField]: direction})
               .collation({locale: "fo", caseLevel: true})
               .explain();
    assert(!planHasStage(testDb, plan, "SORT"), "Unxpected sort in " + tojson(plan));
    tsColl.drop();
}

function testMinMax() {
    // Min and max are only supported on forward collection scans.
    const direction = 1;
    // Min and max should be between 0 and collection size
    const minResult = 5;  // inclusive
    const maxResult = 8;  // not inclusive
    const resultCount = maxResult - minResult;

    let normalCursor = nonClusteredColl.find()
                           .hint({[clusterField]: 1})
                           .min({[clusterField]: minResult})
                           .max({[clusterField]: maxResult})
                           .sort({[clusterField]: direction});
    let normalResult = normalCursor.toArray();
    assert.eq(normalResult.length,
              resultCount,
              tojson(normalResult) + " " + tojson(normalCursor.explain()));

    let clusterCursor = clusteredColl.find()
                            .hint({[clusterField]: 1})
                            .min({[clusterField]: minResult})
                            .max({[clusterField]: maxResult})
                            .sort({[clusterField]: direction});
    let clusterResult = clusterCursor.toArray();
    assert.eq(clusterResult.length,
              resultCount,
              tojson(clusterResult) + " " + tojson(clusterCursor.explain()));

    for (let i = 0; i < clusterResult.length; ++i) {
        assert.eq(clusterResult[i][clusterField], normalResult[i][clusterField]);
    }
}

// Ensure that the plan gets cached correctly
function testPlanCache(direction) {
    clusteredColl.getPlanCache().clear();

    const indexName = "_a";
    assert.commandWorked(clusteredColl.createIndex({a: 1}, {name: indexName}));

    const filter = {a: {$gt: -1}};
    const projection = {_id: 0, [clusterField]: 1};
    const sort = {[clusterField]: direction};

    // Because of the _a index above, we should have two alternatves -- filter via the
    // index then a blocking sort, or filter during a collection scan. Because of the blocking
    // sort and the fact that "a" doesnt actually filter anything, we expect the
    // collection scan to win.
    let plan = clusteredColl.find(filter, projection).sort(sort).explain();
    assert(plan.queryPlanner.rejectedPlans.length > 0, tojson(plan));
    assert(planHasStage(testDb, plan, "COLLSCAN"), "Expected COLLSCAN in " + tojson(plan));

    let nonClusteredResults = nonClusteredColl.find(filter, projection).sort(sort).toArray();
    assert.eq(nonClusteredResults.length, collectionSize);

    // Now run the query and verify that the results are expected. Run it a few times so that the
    // cached plan will be used.
    assert.eq(nonClusteredResults,
              clusteredColl.find(filter, projection).sort(sort).toArray(),
              tojson(plan));
    assert.eq(nonClusteredResults,
              clusteredColl.find(filter, projection).sort(sort).toArray(),
              tojson(plan));
    assert.eq(nonClusteredResults,
              clusteredColl.find(filter, projection).sort(sort).toArray(),
              tojson(plan));

    // Verify that there's a cache entry for this query
    let cacheEntries = clusteredColl.getPlanCache().list();
    let cachedPlan = cacheEntries.find(e => e.queryHash == plan.queryPlanner.queryHash);
    assert.neq(cachedPlan, null, "Plan not in cache");

    assert.commandWorked(clusteredColl.dropIndex(indexName));
}

// Actually run all the tests:
for (let isClustered = 0; isClustered <= 1; isClustered++) {
    for (let hasFilter = 0; hasFilter <= 1; hasFilter++) {
        for (let hasHint = 0; hasHint <= 1; hasHint++) {
            runTest(isClustered, hasFilter, hasHint, /* direction = */ 1);
            runTest(isClustered, hasFilter, hasHint, /* direction = */ -1);
        }
    }
}

testCollations(/* direction = */ 1);
testCollations(/* direction = */ -1);

testMinMax();

testPlanCache(/* direction = */ 1);
testPlanCache(/* direction = */ -1);

// If we're sorting on multiple columns, we still need an explicit sort
let plan = clusteredColl.find().sort({[clusterField]: 1, a: 1}).explain();
assert(planHasStage(testDb, plan, "SORT"), "Expected sort in " + tojson(plan));

clusteredColl.drop();
nonClusteredColl.drop();

MongoRunner.stopMongod(testConnection);
})();
