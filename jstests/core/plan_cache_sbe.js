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
 * ]
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.
load("jstests/libs/sbe_explain_helpers.js");  // For engineSpecificAssertion.

if (checkSBEEnabled(db, ["featureFlagSbePlanCache"])) {
    jsTest.log("Skipping test because SBE and SBE plan cache are both enabled.");
    return;
}

const coll = db.plan_cache_sbe;
coll.drop();

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
engineSpecificAssertion(!stats.cachedPlan.hasOwnProperty("queryPlan"),
                        stats.cachedPlan.hasOwnProperty("queryPlan"),
                        db,
                        stats);
engineSpecificAssertion(!stats.cachedPlan.hasOwnProperty("slotBasedPlan"),
                        stats.cachedPlan.hasOwnProperty("slotBasedPlan"),
                        db,
                        stats);
})();
