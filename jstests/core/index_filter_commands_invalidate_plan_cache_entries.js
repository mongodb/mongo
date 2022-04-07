/**
 * Test that index filter commands (planCacheSetFilter, planCacheClear) invalidate the corresponding
 * plan cache entries.
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

// For testing convenience this variable is made an integer "1" if featureFlagSbePlanCache is on,
// because the expected amount of plan cache entries differs between the two different plan caches.
const isSbePlanCacheEnabled = checkSBEEnabled(db, ["featureFlagSbePlanCache"]) ? 1 : 0;

const collName = "index_filter_commands_invalidate_plan_cache_entries";
const coll = db[collName];
coll.drop();

// We need multiple indexes so that the multi-planner is executed.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: 1, c: 1}));

function existsInPlanCache(query, sort, projection) {
    const keyHash = getPlanCacheKeyFromShape(
        {query: query, projection: projection, sort: sort, collection: coll, db: db});
    const res =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}]).toArray();

    return res.length > 0;
}

assert.eq(1, coll.find({a: 1}).itcount());
assert(existsInPlanCache({a: 1}, {}, {}));
assert.eq(1, coll.find({a: 1, b: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {}, {}));
assert.eq(1, coll.find({a: 1, b: 1}).sort({a: 1}).itcount());
assert(existsInPlanCache({a: 1, b: 1}, {a: 1}));

assert.eq(coll.aggregate([{$planCacheStats: {}}]).toArray().length, 3);

// This query has same index filter key as the first query "{a: 1}" w/o skip. So when an index
// filter is set/cleared on query {a: 1}, the plan cache entry created for this query should also be
// invalidated.
assert.eq(0, coll.find({a: 1}).skip(1).itcount());

// SBE plan cache key encodes "skip", so there's one more plan cache entry in SBE plan cache. While
// in classic plan cache, queries with only difference in "skip" share the same plan cache entry.
assert.eq(coll.aggregate([{$planCacheStats: {}}]).itcount(), 3 + isSbePlanCacheEnabled);

assert.commandWorked(
    db.runCommand({planCacheSetFilter: collName, query: {a: 1, b: 1}, indexes: [{a: 1}]}));
assert.eq(coll.aggregate([{$planCacheStats: {}}]).toArray().length, 2 + isSbePlanCacheEnabled);

// This planCacheSetFilter command will invalidate plan cache entries with filter {a: 1}. There are
// two entries in the SBE plan cache that got invalidated, or one entry in the classic plan cache
// that got invalidated.
assert.commandWorked(
    db.runCommand({planCacheSetFilter: collName, query: {a: 1}, indexes: [{a: 1}]}));
assert.eq(coll.aggregate([{$planCacheStats: {}}]).toArray().length, 1);
})();
