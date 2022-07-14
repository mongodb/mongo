/**
 * Test that index filter commands (planCacheSetFilter, planCacheClearFilters) invalidate the
 * corresponding plan cache entries.
 * @tags: [
 *   # This test attempts to perform queries with plan cache filters set up. The index filter
 *   # commands and the queries to which those index filters apply could be routed to different
 *   # nodes.
 *   assumes_read_preference_unchanged,
 *   assumes_read_concern_unchanged,
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getPlanCacheKeyFromShape.
load("jstests/libs/sbe_util.js");      // For checkSBEEnabled.

// For testing convenience this variable is made an integer "1" if SBE is fully enabled, because the
// expected amount of plan cache entries differs between the SBE plan cache and the classic one.
const isSbeEnabled = checkSBEEnabled(db, ["featureFlagSbeFull"]) ? 1 : 0;

const collName = "index_filter_commands_invalidate_plan_cache_entries";
const coll = db[collName];

function initCollection(collection) {
    collection.drop();

    // We need multiple indexes so that the multi-planner is executed.
    assert.commandWorked(collection.createIndex({a: 1}));
    assert.commandWorked(collection.createIndex({b: 1}));
    assert.commandWorked(collection.createIndex({a: 1, b: 1}));
    assert.commandWorked(collection.insert({a: 1, b: 1, c: 1}));
}
initCollection(coll);

function existsInPlanCache(query, sort, projection, planCacheColl) {
    const keyHash = getPlanCacheKeyFromShape(
        {query: query, projection: projection, sort: sort, collection: planCacheColl, db: db});
    const res = planCacheColl.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}])
                    .toArray();

    return res.length > 0;
}

assert.eq(1, coll.find({a: 1}).itcount());
assert(existsInPlanCache({a: 1}, {}, {}, coll));
assert.eq(1, coll.find({a: 1, b: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, coll));
assert.eq(1, coll.find({a: 1, b: 1}).sort({a: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {a: 1}, {}, coll));

assert.eq(coll.aggregate([{$planCacheStats: {}}]).toArray().length, 3);

// This query has same index filter key as the first query "{a: 1}" w/o skip. So when an index
// filter is set/cleared on query {a: 1}, the plan cache entry created for this query should also be
// invalidated.
assert.eq(0, coll.find({a: 1}).skip(1).itcount());

// SBE plan cache key encodes "skip", so there's one more plan cache entry in SBE plan cache. While
// in classic plan cache, queries with only difference in "skip" share the same plan cache entry.
assert.eq(coll.aggregate([{$planCacheStats: {}}]).itcount(), 3 + isSbeEnabled);

assert.commandWorked(
    db.runCommand({planCacheSetFilter: collName, query: {a: 1, b: 1}, indexes: [{a: 1}]}));
assert.eq(coll.aggregate([{$planCacheStats: {}}]).toArray().length, 2 + isSbeEnabled);

// This planCacheSetFilter command will invalidate plan cache entries with filter {a: 1}. There are
// two entries in the SBE plan cache that got invalidated, or one entry in the classic plan cache
// that got invalidated.
assert.commandWorked(
    db.runCommand({planCacheSetFilter: collName, query: {a: 1}, indexes: [{a: 1}]}));
assert.eq(coll.aggregate([{$planCacheStats: {}}]).toArray().length, 1);

// Test that plan cache entries with same query shape but in a different collection won't be cleared
// when an index filter with the same query shape is set/cleared.
const collNameOther = "index_filter_commands_invalidate_plan_cache_entries_other";
const collOther = db[collNameOther];

initCollection(coll);
initCollection(collOther);

assert.eq(1, coll.find({a: 1, b: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, coll));
assert.eq(1, collOther.find({a: 1, b: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, collOther));

// Test that planCacheClearFilters command invalidates corresponding plan cache entries of correct
// collection.
assert.commandWorked(db.runCommand({planCacheClearFilters: collName, query: {a: 1, b: 1}}));
assert(!existsInPlanCache({a: 1, b: 1}, {}, {}, coll));
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, collOther));

// Test planCacheSetFilter command invalidates corresponding plan cache entries of correct
// collection.
assert.eq(1, coll.find({a: 1, b: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, coll));

assert.commandWorked(
    db.runCommand({planCacheSetFilter: collName, query: {a: 1, b: 1}, indexes: [{a: 1}]}));
assert(!existsInPlanCache({a: 1, b: 1}, {}, {}, coll));
assert(existsInPlanCache({a: 1, b: 1}, {}, {}, collOther));
})();
