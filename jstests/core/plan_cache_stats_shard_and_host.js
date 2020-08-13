// Tests that the $planCacheStats aggregation metadata source returns the "shard" and "host" field
// for each plan cache entry when appropriate.
//
// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_unchanged,
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
//   sbe_incompatible,
// ]
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For 'FixtureHelpers'.

const coll = db.plan_cache_stats_shard_and_host;
coll.drop();
const planCache = coll.getPlanCache();

// Create a plan cache entry by issuing a query that has two possible indexed plans.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.insert({a: 2, b: 3}));
assert.eq(1, coll.find({a: 2, b: 3}).itcount());

// List the contents of the plan cache for the collection.
let planCacheContents = planCache.list();

// We expect every shard that has a chunk for the collection to have produced a plan cache entry.
assert.eq(
    FixtureHelpers.numberOfShardsForCollection(coll), planCacheContents.length, planCacheContents);

// Check that the "host" field is present for every plan cache entry.
for (const entry of planCacheContents) {
    assert(entry.hasOwnProperty("host"), entry);
}

// If we're running this command through mongos, then we expect the "shard" field to be present.
// Otherwise, we expect "shard" to be absent. In either case, this should be true for each
// individual plan cache entry.
for (const entry of planCacheContents) {
    assert.eq(FixtureHelpers.isMongos(db), entry.hasOwnProperty("shard"), entry);
}

// If we group the results by shard or host, then we should only get one plan cache entry for each
// shard/host. As a future improvement, we should return plan cache information from every host in
// every shard. But for now, we use regular host targeting to choose a particular host in each
// shard.
planCacheContents = planCache.list([{$group: {_id: "$shard", count: {$sum: 1}}}]);
for (const entry of planCacheContents) {
    assert.eq(entry.count, 1, entry);
}
planCacheContents = planCache.list([{$group: {_id: "$host", count: {$sum: 1}}}]);
for (const entry of planCacheContents) {
    assert.eq(entry.count, 1, entry);
}

// Clear the plan cache and verify that attempting to list the plan cache now returns an empty
// array.
coll.getPlanCache().clear();
assert.eq([], planCache.list());
}());
