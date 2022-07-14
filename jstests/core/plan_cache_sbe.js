/**
 * Test that for SBE plans a plan cache entry includes a serialized SBE plan tree, and does not for
 * classic plans.
 *
 * @tags: [
 *   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   # This test attempts to perform queries with plan cache filters set up. The former operation
 *   # may be routed to a secondary in the replica set, whereas the latter must be routed to the
 *   # primary.
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   assumes_unsharded_collection,
 *   # The SBE plan cache was introduced in 6.0.
 *   requires_fcv_60,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const coll = db.plan_cache_sbe;
coll.drop();
const isSbeEnabled = checkSBEEnabled(db, ["featureFlagSbeFull"]);

assert.commandWorked(coll.insert({a: 1, b: 1}));

// Check that a new entry is added to the plan cache even for single plans.
if (isSbeEnabled) {
    assert.eq(1, coll.find({a: 1}).itcount());
    // Validate sbe plan cache stats entry.
    const allStats = coll.aggregate([{$planCacheStats: {}}]).toArray();
    assert.eq(allStats.length, 1, allStats);
    const stats = allStats[0];
    assert(stats.hasOwnProperty("isPinned"), stats);
    assert(stats.isPinned, stats);
    assert(stats.hasOwnProperty("cachedPlan"), stats);
    assert(stats.cachedPlan.hasOwnProperty("slots"), stats);
    assert(stats.cachedPlan.hasOwnProperty("stages"), stats);
    coll.getPlanCache().clear();
}

// We need two indexes so that the multi-planner is executed.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

assert.eq(1, coll.find({a: 1}).itcount());

// Validate plan cache stats entry.
const allStats = coll.aggregate([{$planCacheStats: {}}]).toArray();
assert.eq(allStats.length, 1, allStats);
const stats = allStats[0];
assert(stats.hasOwnProperty("cachedPlan"), stats);

if (isSbeEnabled) {
    assert(stats.cachedPlan.hasOwnProperty("slots"), stats);
    assert(stats.cachedPlan.hasOwnProperty("stages"), stats);
} else {
    assert(!stats.cachedPlan.hasOwnProperty("queryPlan"), stats);
    assert(!stats.cachedPlan.hasOwnProperty("slotBasedPlan"), stats);
}
})();
