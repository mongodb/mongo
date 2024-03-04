/**
 * Tests for the $planCacheStats aggregation metadata source.
 * @tags: [
 *   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
 *   cqf_experimental_incompatible,
 * ]
 */
import {
    getAggPlanStage,
    getCachedPlan,
    getPlanCacheKeyFromShape,
    getPlanStage,
    getPlanStages,
} from "jstests/libs/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod failed to start up");

const testDb = conn.getDB("test");
const coll = testDb.plan_cache_stats_agg_source;
const isSBEEnabled = checkSbeFullyEnabled(testDb);

function makeMatchForFilteringByShape(query) {
    const keyHash = getPlanCacheKeyFromShape({query: query, collection: coll, db: testDb});
    return {$match: {planCacheKey: keyHash}};
}

// Returns a BSON object representing the plan cache entry for the query shape {a: 1, b: 1}.
function getSingleEntryStats() {
    const cursor =
        coll.aggregate([{$planCacheStats: {}}, makeMatchForFilteringByShape({a: 1, b: 1})]);
    assert(cursor.hasNext());
    const entryStats = cursor.next();
    assert(!cursor.hasNext());
    return entryStats;
}

// Fails when the collection does not exist.
assert.commandFailedWithCode(
    testDb.runCommand({aggregate: coll.getName(), pipeline: [{$planCacheStats: {}}], cursor: {}}),
    50933);

// Create a collection with two indices.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// Should return an empty result set when there are no cache entries yet.
assert.eq(0, coll.aggregate([{$planCacheStats: {}}]).itcount());

assert.commandWorked(coll.insertMany([
    {_id: 0, a: 1, b: 1},
    {_id: 1, a: 1, b: 1, c: 1},
    {_id: 2, a: 1, b: 1, c: 1, d: 1},
]));

// Run three distinct query shapes and check that there are three cache entries.
assert.eq(3, coll.find({a: 1, b: 1}).itcount());
assert.eq(2, coll.find({a: 1, b: 1, c: 1}).itcount());
assert.eq(1, coll.find({a: 1, b: 1, d: 1}).itcount());
assert.eq(3, coll.aggregate([{$planCacheStats: {}}]).itcount());

// We should be able to find particular cache entries by maching on the query from which the
// entry was created.
assert.eq(
    1,
    coll.aggregate([{$planCacheStats: {}}, makeMatchForFilteringByShape({a: 1, b: 1})]).itcount());
assert.eq(1,
          coll.aggregate([{$planCacheStats: {}}, makeMatchForFilteringByShape({a: 1, b: 1, c: 1})])
              .itcount());
assert.eq(1,
          coll.aggregate([{$planCacheStats: {}}, makeMatchForFilteringByShape({a: 1, b: 1, d: 1})])
              .itcount());

// A similar match on a query filter that was never run should turn up nothing.
assert.eq(0,
          coll.aggregate([{$planCacheStats: {}}, makeMatchForFilteringByShape({a: 1, b: 1, e: 1})])
              .itcount());

// Test $group over the plan cache metadata.
assert.eq(1,
          coll.aggregate([{$planCacheStats: {}}, {$group: {_id: "$createdFromQuery.query.a"}}])
              .itcount());

// Explain should show that a $match gets absorbed into the $planCacheStats stage.
let explain = assert.commandWorked(coll.explain().aggregate(
    [{$planCacheStats: {}}, {$match: {"createdFromQuery.query": {a: 1, b: 1}}}]));
assert.eq(explain.stages.length, 1);
const planCacheStatsExplain = getAggPlanStage(explain, "$planCacheStats");
assert.neq(planCacheStatsExplain, null);
assert(planCacheStatsExplain.hasOwnProperty("$planCacheStats"));
assert(planCacheStatsExplain.$planCacheStats.hasOwnProperty("match"));
assert.eq(planCacheStatsExplain.$planCacheStats.match, {"createdFromQuery.query": {a: 1, b: 1}});

// Get the plan cache metadata for a particular query.
let entryStats = getSingleEntryStats();

// Verify that $planCacheStats reports the same 'queryHash' and 'planCacheKey' as explain
// for this query shape.
explain = assert.commandWorked(coll.find({a: 1, b: 1}).explain());
assert.eq(entryStats.queryHash, explain.queryPlanner.queryHash);
assert.eq(entryStats.planCacheKey, explain.queryPlanner.planCacheKey);

// Since the query shape was only run once, the plan cache entry should not be active.
assert.eq(entryStats.isActive, false);

// Sanity check 'works' value.
assert(entryStats.hasOwnProperty("works"));
assert.gt(entryStats.works, 0);

// Verify that the 'timeOfCreation' for the entry is now +/- one day.
const now = new Date();
const yesterday = (new Date()).setDate(now.getDate() - 1);
const tomorrow = (new Date()).setDate(now.getDate() + 1);
assert(entryStats.hasOwnProperty("timeOfCreation"));
assert.gt(entryStats.timeOfCreation, yesterday);
assert.lt(entryStats.timeOfCreation, tomorrow);

assert(entryStats.hasOwnProperty("version"));

assert.eq(false, entryStats.indexFilterSet);

// After creating an index filter on a different query shape, $planCacheStats should still
// report that no index filter is set. Setting a filter clears the cache, so we rerun the query
// associated with the cache entry.
assert.commandWorked(testDb.runCommand(
    {planCacheSetFilter: coll.getName(), query: {a: 1, b: 1, c: 1}, indexes: [{a: 1}, {b: 1}]}));
assert.eq(2, coll.aggregate([{$planCacheStats: {}}]).itcount());
assert.eq(2, coll.find({a: 1, b: 1, c: 1}).itcount());
assert.eq(3, coll.aggregate([{$planCacheStats: {}}]).itcount());
entryStats = getSingleEntryStats();
assert.eq(false, entryStats.indexFilterSet);

// Create an index filter on shape {a: 1, b: 1}, and verify that indexFilterSet is now true.
assert.commandWorked(testDb.runCommand(
    {planCacheSetFilter: coll.getName(), query: {a: 1, b: 1}, indexes: [{a: 1}, {b: 1}]}));
assert.eq(2, coll.aggregate([{$planCacheStats: {}}]).itcount());
assert.eq(3, coll.find({a: 1, b: 1}).itcount());
assert.eq(3, coll.aggregate([{$planCacheStats: {}}]).itcount());
entryStats = getSingleEntryStats();
assert.eq(true, entryStats.indexFilterSet);

if (entryStats["version"] === "1") {
    // Verify that the entry has the expected 'createdFromQuery' field.
    assert(entryStats.hasOwnProperty("createdFromQuery"));
    assert.eq(entryStats.createdFromQuery.query, {a: 1, b: 1});
    assert.eq(entryStats.createdFromQuery.sort, {});
    assert.eq(entryStats.createdFromQuery.projection, {});
    assert(!entryStats.createdFromQuery.hasOwnProperty("collation"));

    // Check that the cached plan is an index scan either on {a: 1} or {b: 1}.
    assert(entryStats.hasOwnProperty("cachedPlan"));
    const ixscanStage = getPlanStage(getCachedPlan(entryStats.cachedPlan), "IXSCAN");
    assert.neq(ixscanStage, null);
    assert(bsonWoCompare(ixscanStage.keyPattern, {a: 1}) === 0 ||
           bsonWoCompare(ixscanStage.keyPattern, {b: 1}) === 0);

    // There should be at least two plans in 'creationExecStats', and each should have at least one
    // index scan.
    assert(entryStats.hasOwnProperty("creationExecStats"));
    assert.gte(entryStats.creationExecStats.length, 2);
    for (let plan of entryStats.creationExecStats) {
        assert(plan.hasOwnProperty("executionStages"));
        // If we are in SBE mode, then explain output format is different for 'creationExecStats'.
        const stages = getPlanStages(plan.executionStages, isSBEEnabled ? "ixseek" : "IXSCAN");
        assert.gt(stages.length, 0);
    }

    // Assert that the entry has an array of at least two scores, and that all scores are greater
    // than 1.
    assert(entryStats.hasOwnProperty("candidatePlanScores"));
    assert.gte(entryStats.candidatePlanScores.length, 2);
    for (let score of entryStats.candidatePlanScores) {
        assert.gt(score, 1);
    }
}

// Should throw an error if $planCacheStats is not first.
assert.throws(
    () => coll.aggregate([{$match: {createdFromQuery: {a: 1, b: 1}}}, {$planCacheStats: {}}]));

// If the plan cache is cleared, then there are no longer any results returned by
// $planCacheStats.
assert.commandWorked(testDb.runCommand({planCacheClear: coll.getName()}));
assert.eq(0, coll.aggregate([{$planCacheStats: {}}]).itcount());

MongoRunner.stopMongod(conn);
