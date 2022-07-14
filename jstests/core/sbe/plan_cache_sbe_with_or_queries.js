// Tests that rooted $OR queries are added to the SBE plan cache.
// @tags: [
//   # This test attempts to perform queries and introspect the server's plan cache entries. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary.
//   assumes_read_preference_unchanged,
//   assumes_read_concern_unchanged,
//   assumes_unsharded_collection,
//   does_not_support_stepdowns,
//   requires_fcv_60,
// ]

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

if (!checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    jsTest.log("Skip running the test because SBE is not enabled");
    return;
}

function getPlanCacheEntries(query, collection, db) {
    const planCacheKey = getPlanCacheKeyFromShape({query, collection, db});
    return coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey}}]).toArray();
}

function assertRootedOrPlan(query, collection, db) {
    const explain = collection.find(query).explain();
    const winningPlan = getWinningPlan(explain.queryPlanner);
    assert(planHasStage(db, winningPlan, "OR"), explain);
}

const coll = db.sbe_subplan;
coll.drop();

assert.commandWorked(coll.createIndexes([{a: 1, b: -1}, {b: 1}]));
assert.commandWorked(coll.insertMany([{a: 1, b: 1}, {a: 1, b: 2}, {a: 2, b: 2}, {a: 2, b: 3}]));

const query = {
    $or: [{a: 1}, {b: 2}]
};
assert.eq(3, coll.find(query).itcount());

assertRootedOrPlan(query, coll, db);

const planCacheEntries = getPlanCacheEntries(query, coll, db);
// A subplan query adds an entry to the plan cache.
assert.eq(1, planCacheEntries.length, planCacheEntries);

// And this entry must be pinned and active.
assert.eq(true, planCacheEntries[0].isPinned, planCacheEntries);
assert.eq(true, planCacheEntries[0].isActive, planCacheEntries);
// Works is always 0 for pinned plan cache entries.
assert.eq(0, planCacheEntries[0].works, planCacheEntries);
}());
