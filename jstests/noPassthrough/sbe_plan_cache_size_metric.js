/**
 * Tests that planCacheTotalSizeEstimateBytes metric increased for SBE and classic plan cache
 * entries.
 *
 * @tags: [
 *   # Needed as the setParameter for ForceClassicEngine was introduced in 5.1.
 *   requires_fcv_51,
 *   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/sbe_util.js");         // For checkSBEEnabled.
load('jstests/libs/fixture_helpers.js');  // For FixtureHelpers.

const conn = MongoRunner.runMongod();
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB("sbe_plan_cache_size_metric");

function getCacheEntriesByQueryHashKey(coll, queryHash) {
    return coll.aggregate([{$planCacheStats: {}}, {$match: {queryHash}}]).toArray();
}

function getQueryHashFromExplain(explainRes) {
    const hash = FixtureHelpers.isMongos(db)
        ? explainRes.queryPlanner.winningPlan.shards[0].queryHash
        : explainRes.queryPlanner.queryHash;
    assert.eq(typeof (hash), "string");
    return hash;
}

function getPlanCacheSize() {
    return db.serverStatus().metrics.query.planCacheTotalSizeEstimateBytes;
}

function assertQueryInPlanCache(coll, query) {
    const explainResult = assert.commandWorked(coll.explain().find(query).finish());
    const queryHash = getQueryHashFromExplain(explainResult, db);
    const planCacheEntries = getCacheEntriesByQueryHashKey(coll, queryHash);
    assert.eq(1, planCacheEntries.length, planCacheEntries);
}

const isSbePlanCacheEnabled = checkSBEEnabled(db, ["featureFlagSbePlanCache"]);
if (isSbePlanCacheEnabled) {
    const coll = db.plan_cache_sbe;
    coll.drop();

    assert.commandWorked(coll.insert({a: 1, b: 1}));

    // We need two indexes so that the multi-planner is executed.
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));

    const initialPlanCacheSize = getPlanCacheSize();
    // Plan cache must be empty.
    assert.eq(0, coll.getPlanCache().list().length);

    const query = {a: 1};

    assert.eq(1, coll.find(query).itcount());
    assertQueryInPlanCache(coll, query);
    // Plan Cache must contain exactly 1 entry.
    assert.eq(1, coll.getPlanCache().list().length);

    // Assert metric is incremented for new cache entry.
    const afterSbePlanCacheSize = getPlanCacheSize();
    assert.gt(afterSbePlanCacheSize, initialPlanCacheSize);

    // Force classic plan cache.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));
    assert.eq(1, coll.find(query).itcount());
    assertQueryInPlanCache(coll, query);
    // Plan Cache must contain exactly 2 entries.
    assert.eq(2, coll.getPlanCache().list().length);

    // Assert metric is incremented for new cache entry.
    const afterClassicPlanCacheSize = getPlanCacheSize();
    assert.gt(afterClassicPlanCacheSize, afterSbePlanCacheSize);
}

MongoRunner.stopMongod(conn);
})();
