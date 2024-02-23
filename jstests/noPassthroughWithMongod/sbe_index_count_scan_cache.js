/**
 * Tests the SBE plan cache for COUNT SCAN queries.
 *
 * @tags: [
 *    # This test is specifically verifying the behavior of the SBE plan cache.
 *    featureFlagSbeFull,
 * ]
 */
import {getPlanStages, getWinningPlan} from "jstests/libs/analyze_plan.js";

const testDb = db.getSiblingDB(jsTestName());
assert.commandWorked(testDb.dropDatabase());

const coll = testDb.coll;

assert.commandWorked(coll.insert([
    {a: 1},
    {a: 1, b: 1},
    {a: null, b: 2},
    {b: 2},
    {b: 4},
    {a: {b: 4}},
    {a: [], b: 2},
    {a: [[], 3]},
    {a: {}},
]));

function assertCountScan(pipeline) {
    const explain = coll.explain().aggregate(pipeline);
    let queryPlan;
    if (explain.hasOwnProperty("stages")) {
        queryPlan = getWinningPlan(explain.stages[0].$cursor.queryPlanner);
    } else {
        queryPlan = getWinningPlan(explain.queryPlanner);
    }
    const countScan = getPlanStages(queryPlan, "COUNT_SCAN");
    assert.neq([], countScan, explain);
}

function runTest({index, query, expectedCount, updatedQuery, updatedCount}) {
    assert.commandWorked(coll.createIndex(index));
    coll.getPlanCache().clear();
    assert.eq(0, coll.getPlanCache().list().length);
    const oldHits = testDb.serverStatus().metrics.query.planCache.sbe.hits;

    const pipeline = [{$match: query}, {$count: "count"}];
    assertCountScan(pipeline);

    assert.eq(expectedCount, coll.aggregate(pipeline).toArray()[0].count);
    assert.eq(expectedCount, coll.aggregate(pipeline).toArray()[0].count);
    // Verify that the cache has 1 entry, and has been hit for one time.
    assert.eq(1, coll.getPlanCache().list().length);
    assert.eq(testDb.serverStatus().metrics.query.planCache.sbe.hits, oldHits + 1);
    // Run again with a different value to test the parameterization.
    pipeline[0].$match = updatedQuery;
    assert.eq(updatedCount, coll.aggregate(pipeline).toArray()[0].count);
    // Cache not get updated.
    assert.eq(1, coll.getPlanCache().list().length);
    // Hits stats is incremented.
    assert.eq(testDb.serverStatus().metrics.query.planCache.sbe.hits, oldHits + 2);

    assert.commandWorked(coll.dropIndex(index));
}

runTest({index: {a: 1}, query: {a: 1}, expectedCount: 2, updatedQuery: {a: 3}, updatedCount: 1});
// Test for multiKey and null case.
runTest({
    index: {a: 1, b: 1, _id: 1},
    query: {a: null, b: 2},
    expectedCount: 2,
    updatedQuery: {a: null, b: 4},
    updatedCount: 1
});
