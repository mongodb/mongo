/**
 * Tests basic functionality of integrating plan cache with lowered $lookup. Currently only the
 * stages below group/lookup get cached in the classic cache.
 */
(function() {
"use strict";

load("jstests/libs/profiler.js");  // For 'getLatestProfilerEntry'.

const conn = MongoRunner.runMongod({setParameter: {featureFlagSBELookupPushdown: true}});
assert.neq(null, conn, "mongod was unable to start up");
const name = "lookup_pushdown";
const foreignCollName = "foreign_lookup_pushdown";
let db = conn.getDB(name);
let coll = db[name];
let foreignColl = db[foreignCollName];

function verifyPlanCache({query, isActive, planCacheKey}) {
    const cacheEntries = coll.aggregate([{$planCacheStats: {}}]).toArray();
    assert.eq(cacheEntries.length, 1);
    const cacheEntry = cacheEntries[0];
    // TODO(SERVER-61507): Convert the assertion to SBE cache once lowered $lookup integrates
    // with SBE plan cache.
    assert.eq(cacheEntry.version, 1);
    assert.docEq(cacheEntry.createdFromQuery.query, query);
    assert.eq(cacheEntry.isActive, isActive);
    if (planCacheKey) {
        assert.eq(cacheEntry.planCacheKey, planCacheKey);
    }
    return cacheEntry;
}

// Create two indices to make sure the query gets multi-planned, so that the query subtree will
// be saved in the classic cache.
assert.commandWorked(coll.createIndexes([{a: 1, b: 1}, {a: 1, c: 1}]));
assert.commandWorked(coll.insert([{a: 1}, {a: 2}]));
assert.commandWorked(foreignColl.insert([{c: 1}, {c: 2}]));
const query = {
    a: {$gt: 1}
};
const pipeline = [
    {$match: query},
    {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}}
];

// First run should create an inactive cache entry.
assert.eq(1, coll.aggregate(pipeline).itcount());
const cacheEntry = verifyPlanCache({query, isActive: false});
const planCacheKey = cacheEntry.planCacheKey;

// Second run should mark the cache entry active.
assert.eq(1, coll.aggregate(pipeline).itcount());
verifyPlanCache({query, planCacheKey, isActive: true});

// Third run should use the active cached entry.
assert.commandWorked(db.setProfilingLevel(2));
assert.eq(1, coll.aggregate(pipeline).itcount());
const profileEntry = getLatestProfilerEntry(db, {});
assert.eq(planCacheKey, profileEntry.planCacheKey);

// Explain output should show the same plan cache key.
const explain = coll.explain().aggregate(pipeline);
assert.eq(planCacheKey, explain.queryPlanner.planCacheKey);

MongoRunner.stopMongod(conn);
}());
