/**
 * Tests that planCacheTotalSizeEstimateBytes metric is updated when entries are added or evicted
 * from SBE and Classic Plan Cache entries.
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

if (!checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    jsTest.log("Skipping test because SBE is not fully enabled");
    MongoRunner.stopMongod(conn);
    return;
}

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

const collectionName = "plan_cache_sbe";
const coll = db[collectionName];
coll.drop();

assert.commandWorked(coll.insert({a: 1, b: 1, c: 1}));

// We need some indexes so that the multi-planner is executed.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({c: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

const initialPlanCacheSize = getPlanCacheSize();
// Plan cache must be empty.
assert.eq(0, coll.getPlanCache().list().length);

const sbeQuery = {
    a: 1
};
const classicQuery = {
    a: 1,
    c: 1
};

// Step 1. Insert an entry to SBE Plan Cache.
assert.eq(1, coll.find(sbeQuery).itcount());
assertQueryInPlanCache(coll, sbeQuery);
// Plan Cache must contain exactly 1 entry.
assert.eq(1, coll.getPlanCache().list().length);

// Assert metric is incremented for new cache entry.
const afterSbePlanCacheSize = getPlanCacheSize();
assert.gt(afterSbePlanCacheSize, initialPlanCacheSize);

// Step 2. Insert an entry to Classic Plan Cache.
// Force classic plan cache.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));
assert.eq(1, coll.find(classicQuery).itcount());
assertQueryInPlanCache(coll, classicQuery);
// Plan Cache must contain exactly 2 entries.
assert.eq(2, coll.getPlanCache().list().length);

// Assert metric is incremented for new cache entry.
const afterClassicPlanCacheSize = getPlanCacheSize();
assert.gt(afterClassicPlanCacheSize, afterSbePlanCacheSize);

// Step 3. Remove the entry from Classic Plan Cache.
// Clean up Classic Plan Cache.
assert.commandWorked(db.runCommand({planCacheClear: collectionName, query: classicQuery}));
// Assert metric is decremented back to values before insering classic plan cache entry.
assert.eq(afterSbePlanCacheSize, getPlanCacheSize());

// Step 4. Remove the entry from SBE Plan Cache.
// Move back to SBE plan cache.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));
// Clean up SBE Plan Cache
assert.commandWorked(db.runCommand({planCacheClear: collectionName, query: sbeQuery}));
// Assert metric is decremented back to initial value.
assert.eq(initialPlanCacheSize, getPlanCacheSize());

MongoRunner.stopMongod(conn);
})();
