// Tests that the $planCacheStats aggregation metadata source returns the "shard" and "host" field
// for each plan cache entry when appropriate.
//
// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_unchanged,
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
//   # Plan cache state is node-local and will not get migrated alongside tenant data.
//   tenant_migration_incompatible,
//   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
//   cqf_experimental_incompatible,
// ]
import {getPlanCacheKeyFromExplain} from "jstests/libs/analyze_plan.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db.plan_cache_stats_shard_and_host;
coll.drop();
const planCache = coll.getPlanCache();

// Create a plan cache entry by issuing a query that has two possible indexed plans.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.insert({a: 2, b: 3}));
assert.eq(1, coll.find({a: 2, b: 3}).itcount());

const explain = coll.find({a: 2, b: 3}).explain();
const planCacheKey = getPlanCacheKeyFromExplain(explain, db);

function filterPlanCacheEntriesByKey(planCacheKey, planCacheContents) {
    let filteredPlanCacheEntries = [];
    for (const entry of planCacheContents) {
        if (entry.planCacheKey === planCacheKey) {
            filteredPlanCacheEntries.push(entry);
        }
    }
    return filteredPlanCacheEntries;
}

let planCacheContents = filterPlanCacheEntriesByKey(planCacheKey, planCache.list());

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
    assert.eq(FixtureHelpers.isMongos(db) ||
                  (TestData.hasOwnProperty("testingReplicaSetEndpoint") &&
                   TestData.testingReplicaSetEndpoint),
              entry.hasOwnProperty("shard"),
              entry);
}

// If we group the results by shard or host, then we should only get one plan cache entry for each
// shard/host. As a future improvement, we should return plan cache information from every host in
// every shard. But for now, we use regular host targeting to choose a particular host in each
// shard.
planCacheContents = filterPlanCacheEntriesByKey(
    planCacheKey, planCache.list([{$group: {_id: "$shard", count: {$sum: 1}}}]));

for (const entry of planCacheContents) {
    assert.eq(entry.count, 1, entry);
}

planCacheContents = filterPlanCacheEntriesByKey(
    planCacheKey, planCache.list([{$group: {_id: "$host", count: {$sum: 1}}}]));

for (const entry of planCacheContents) {
    assert.eq(entry.count, 1, entry);
}

// Clear the plan cache and verify that attempting to list the plan cache now returns an empty
// array.
coll.getPlanCache().clear();
assert.eq([], filterPlanCacheEntriesByKey(planCacheKey, planCache.list()));
