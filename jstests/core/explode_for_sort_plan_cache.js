/**
 * Confirms that explode for sort plans are properly cached and recovered from the plan cache,
 * yielding correct results after the query is auto-parameterized.
 *
 * @tags: [
 *   # Since the plan cache is per-node state, this test assumes that all operations are happening
 *   # against the same mongod.
 *   assumes_read_preference_unchanged,
 *   assumes_read_concern_unchanged,
 *   does_not_support_stepdowns,
 *   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *   assumes_balancer_off,
 *   assumes_unsharded_collection,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");

const coll = db.explode_for_sort_plan_cache;
coll.drop();

// Create two indexes to ensure the multi-planner kicks in and the query plan gets cached.
assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1, d: 1}));

assert.commandWorked(coll.insert({a: 2, b: 3}));

// A helper function to look up a cache entry in the plan cache based on the given filter
// and sort specs.
function getPlanForCacheEntry(query, sort) {
    const keyHash = getPlanCacheKeyFromShape({query: query, sort: sort, collection: coll, db: db});

    const res =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}]).toArray();
    // We expect exactly one matching cache entry.
    assert.eq(1, res.length, () => tojson(coll.aggregate([{$planCacheStats: {}}]).toArray()));
    return res[0];
}

// A helper function to assert that a cache entry doesn't exist in the plan cache based on the
// given filter and sort specs.
function assertCacheEntryDoesNotExist(query, sort) {
    const keyHash = getPlanCacheKeyFromShape({query: query, sort: sort, collection: coll, db: db});
    const res =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}]).toArray();
    assert.eq(0, res.length, () => tojson(coll.aggregate([{$planCacheStats: {}}]).toArray()));
}

let querySpec = {a: {$eq: 2}, b: {$in: [99, 4]}};
const sortSpec = {
    c: 1
};

// TODO SERVER-67576: remove this branch once explode for sort plans are supported by the SBE plan
// cache.
if (checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    // Run the query for the first time and make sure the plan hasn't been cached.
    assert.eq(0, coll.find(querySpec).sort(sortSpec).itcount());
    assertCacheEntryDoesNotExist(querySpec, sortSpec);

    // Run the query again and make sure it's still not cached.
    assert.eq(0, coll.find(querySpec).sort(sortSpec).itcount());
    assertCacheEntryDoesNotExist(querySpec, sortSpec);

    // Run a query that returns one document in the collection, but the plan is still not cached.
    querySpec = {a: {$eq: 2}, b: {$in: [3, 4]}};
    assert.eq(1, coll.find(querySpec).sort(sortSpec).itcount());
    assertCacheEntryDoesNotExist(querySpec, sortSpec);
} else {
    // Run the query for the first time to create an inactive plan cache entry.
    assert.eq(0, coll.find(querySpec).sort(sortSpec).itcount());
    const inactiveEntry = getPlanForCacheEntry(querySpec, sortSpec);
    assert.eq(inactiveEntry.isActive, false, inactiveEntry);

    // Run the query again to activate the cache entry.
    assert.eq(0, coll.find(querySpec).sort(sortSpec).itcount());
    const activeEntry = getPlanForCacheEntry(querySpec, sortSpec);
    assert.eq(activeEntry.isActive, true, activeEntry);
    assert.eq(inactiveEntry.queryHash,
              activeEntry.queryHash,
              `inactive=${tojson(inactiveEntry)}, active=${tojson(activeEntry)}`);
    assert.eq(inactiveEntry.planCacheKey,
              activeEntry.planCacheKey,
              `inactive=${tojson(inactiveEntry)}, active=${tojson(activeEntry)}`);

    // Run a query that reuses the cache entry and should return one document in the collection.
    querySpec = {a: {$eq: 2}, b: {$in: [3, 4]}};
    assert.eq(1, coll.find(querySpec).sort(sortSpec).itcount());
    const reusedEntry = getPlanForCacheEntry(querySpec, sortSpec);
    assert.eq(reusedEntry.isActive, true, reusedEntry);
    assert.eq(activeEntry.queryHash,
              reusedEntry.queryHash,
              `active=${tojson(activeEntry)}, reused=${tojson(reusedEntry)}`);
    assert.eq(activeEntry.planCacheKey,
              reusedEntry.planCacheKey,
              `active=${tojson(activeEntry)}, reused=${tojson(reusedEntry)}`);
}
}());
