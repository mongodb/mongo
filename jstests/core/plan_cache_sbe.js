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
 *   assumes_unsharded_collection,]
 */
(function() {
"use strict";

const coll = db.plan_cache_sbe;
coll.drop();

// Note that the "getParameter" command is expected to fail in versions of mongod that do not yet
// include the slot-based execution engine. When that happens, however, 'isSBEEnabled' still
// correctly evaluates to false.
const isSBEEnabled = (() => {
    const getParam = db.adminCommand({getParameter: 1, featureFlagSBE: 1});
    return getParam.hasOwnProperty("featureFlagSBE") && getParam.featureFlagSBE.value;
})();

assert.commandWorked(coll.insert({a: 1, b: 1}));

// We need two indexes so that the multi-planner is executed.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

// Run query.
assert.eq(1, coll.find({a: 1}).itcount());

// Validate plan cache stats entry.
const allStats = coll.aggregate([{$planCacheStats: {}}]).toArray();
assert.eq(allStats.length, 1, allStats);
const stats = allStats[0];
assert(stats.hasOwnProperty("cachedPlan"), stats);
assert.eq(stats.cachedPlan.hasOwnProperty("queryPlan"), isSBEEnabled, stats);
assert.eq(stats.cachedPlan.hasOwnProperty("slotBasedPlan"), isSBEEnabled, stats);
})();
