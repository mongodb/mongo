// Tests that rooted $OR queries are added to the SBE plan cache.
// @tags: [
//   # This test attempts to perform queries and introspect the server's plan cache entries. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary.
//   assumes_read_preference_unchanged,
//   assumes_read_concern_unchanged,
//   assumes_unsharded_collection,
//   does_not_support_stepdowns,
//   # The SBE plan cache was first enabled in 6.3.
//   requires_fcv_63,
//   assumes_balancer_off,
//   featureFlagSbeFull,
// ]

import {getPlanCacheKeyFromShape, getWinningPlanFromExplain, planHasStage} from "jstests/libs/query/analyze_plan.js";

function getPlanCacheEntries(query, collection, db) {
    const planCacheKey = getPlanCacheKeyFromShape({query, collection, db});
    return coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey}}]).toArray();
}

function assertRootedOrPlan(query, collection, db) {
    const explain = collection.find(query).explain();
    const winningPlan = getWinningPlanFromExplain(explain);
    assert(planHasStage(db, winningPlan, "OR"), explain);
}

const coll = db.sbe_subplan;
coll.drop();

assert.commandWorked(coll.createIndexes([{a: 1, b: -1}, {b: 1}]));
assert.commandWorked(
    coll.insertMany([
        {a: 1, b: 1},
        {a: 1, b: 2},
        {a: 2, b: 2},
        {a: 2, b: 3},
    ]),
);

const query = {
    $or: [{a: 1}, {b: 2}],
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
