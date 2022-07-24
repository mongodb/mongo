// Test clearing of the plan cache, either manually through the planCacheClear command,
// or due to system events such as an index build.
//
// @tags: [
//   # This test attempts to perform queries and introspect/manipulate the server's plan cache
//   # entries. The former operation may be routed to a secondary in the replica set, whereas the
//   # latter must be routed to the primary.
//   assumes_read_concern_unchanged,
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
//   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
//   assumes_balancer_off,
//   assumes_unsharded_collection,
// ]

(function() {
'use strict';

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const coll = db.jstests_plan_cache_clear;
coll.drop();

function numPlanCacheEntries(coll) {
    return coll.aggregate([{$planCacheStats: {}}]).itcount();
}

function dumpPlanCacheState(coll) {
    return coll.aggregate([{$planCacheStats: {}}]).toArray();
}

assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 2, b: 2}));

// We need two indices so that the MultiPlanRunner is executed.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

// Run a query so that an entry is inserted into the cache.
assert.eq(1, coll.find({a: 1, b: 1}).itcount());

// Invalid key should be a no-op.
assert.commandWorked(coll.runCommand('planCacheClear', {query: {unknownfield: 1}}));
assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

// Introduce a second plan cache entry.
assert.eq(0, coll.find({a: 1, b: 1, c: 1}).itcount());
assert.eq(2, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

// Drop one of the two shapes from the cache.
assert.commandWorked(coll.runCommand('planCacheClear', {query: {a: 1, b: 1}}),
                     dumpPlanCacheState(coll));
assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

// Drop the second shape from the cache.
assert.commandWorked(coll.runCommand('planCacheClear', {query: {a: 1, b: 1, c: 1}}),
                     dumpPlanCacheState(coll));
assert.eq(0, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

// planCacheClear can clear $expr queries.
assert.eq(1, coll.find({a: 1, b: 1, $expr: {$eq: ['$a', 1]}}).itcount());
assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));
assert.commandWorked(
    coll.runCommand('planCacheClear', {query: {a: 1, b: 1, $expr: {$eq: ['$a', 1]}}}));
assert.eq(0, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

// planCacheClear fails with an $expr query with an unbound variable.
assert.commandFailed(
    coll.runCommand('planCacheClear', {query: {a: 1, b: 1, $expr: {$eq: ['$a', '$$unbound']}}}));

// Insert two more shapes into the cache.
assert.eq(1, coll.find({a: 1, b: 1}).itcount());
assert.eq(1, coll.find({a: 1, b: 1}, {_id: 0, a: 1}).itcount());
assert.eq(2, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

// Error cases.
assert.commandFailedWithCode(coll.runCommand('planCacheClear', {query: 12345}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(coll.runCommand('planCacheClear', {query: /regex/}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(coll.runCommand('planCacheClear', {query: {a: {$no_such_op: 1}}}),
                             ErrorCodes.BadValue);
// 'sort' parameter is not allowed without 'query' parameter.
assert.commandFailedWithCode(coll.runCommand('planCacheClear', {sort: {a: 1}}),
                             ErrorCodes.BadValue);
// 'projection' parameter is not allowed with 'query' parameter.
assert.commandFailedWithCode(coll.runCommand('planCacheClear', {projection: {_id: 0, a: 1}}),
                             ErrorCodes.BadValue);

// Drop query cache. This clears all cached queries in the collection.
assert.commandWorked(coll.runCommand('planCacheClear'));
assert.eq(0, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

// Clearing the plan cache for a non-existent collection should succeed.
const nonExistentColl = db.plan_cache_clear_nonexistent;
nonExistentColl.drop();
assert.commandWorked(nonExistentColl.runCommand('planCacheClear'));

if (checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    // Plan cache commands should work against the main collection only, not foreignColl
    // collections, when $lookup is pushed down into SBE.
    const foreignColl = db.plan_cache_clear_foreign;
    foreignColl.drop();

    // We need two indices so that the multi-planner is executed.
    assert.commandWorked(foreignColl.createIndex({b: 1}));
    assert.commandWorked(foreignColl.createIndex({b: 1, c: 1}));

    assert.commandWorked(foreignColl.insert([{b: 1}, {b: 3}]));

    const pipeline = [
        {$match: {a: 1}},
        {$lookup: {from: foreignColl.getName(), localField: "a", foreignField: "b", as: "matched"}}
    ];

    // Test case 1: clear plan cache on the main collection.
    //
    // Run a query against the 'foreignColl' and make sure it's cached.
    assert.eq(0, foreignColl.find({b: 1, c: 1}).itcount());
    assert.eq(1, numPlanCacheEntries(foreignColl), dumpPlanCacheState(foreignColl));

    // Run the '$lookup' query and make sure it's cached.
    let results = coll.aggregate(pipeline).toArray();
    assert.eq(3, results.length, results);
    assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

    // Drop query cache on the main collection. This clears all cached queries in the main
    // collection only.
    assert.commandWorked(coll.runCommand("planCacheClear"));
    assert.eq(0, numPlanCacheEntries(coll), dumpPlanCacheState(coll));
    assert.eq(1, numPlanCacheEntries(foreignColl), dumpPlanCacheState(foreignColl));

    // Test case 2: clear plan cache on the foreign collection.
    //
    // Run the '$lookup' query again and make sure it's cached.
    results = coll.aggregate(pipeline).toArray();
    assert.eq(3, results.length, results);
    assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

    // Drop query cache on the foreign collection. Make sure that the plan cache on the main
    // collection is not affected.
    assert.commandWorked(foreignColl.runCommand("planCacheClear"));
    assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));
    assert.eq(0, numPlanCacheEntries(foreignColl), dumpPlanCacheState(foreignColl));

    // Test case 3: clear plan cache on the main collection by query shape.
    //
    // Run a query against the 'foreignColl' and make sure it's cached.
    assert.eq(0, foreignColl.find({b: 1, c: 1}).itcount());
    assert.eq(1, numPlanCacheEntries(foreignColl), dumpPlanCacheState(foreignColl));

    // Run the '$lookup' query and make sure it's cached.
    results = coll.aggregate(pipeline).toArray();
    assert.eq(3, results.length, results);
    assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

    // Drop query cache by the query shape. This clears all cached queries in the main
    // collection only.
    assert.commandWorked(coll.runCommand("planCacheClear", {query: {a: 1}}));
    assert.eq(0, numPlanCacheEntries(coll), dumpPlanCacheState(coll));
    assert.eq(1, numPlanCacheEntries(foreignColl), dumpPlanCacheState(foreignColl));

    // Test case 4: clear plan cache on the foreign collection by (empty) query shape.
    //
    // Run two queries against the 'foreignColl' and make sure they're cached.
    assert.eq(2, foreignColl.find({}).itcount());
    assert.eq(0, foreignColl.find({b: 1, c: 1}).itcount());
    assert.eq(2, numPlanCacheEntries(foreignColl), dumpPlanCacheState(foreignColl));

    // Run the '$lookup' query and make sure it's cached.
    results = coll.aggregate(pipeline).toArray();
    assert.eq(3, results.length, results);
    assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

    // Drop query cache on the foreign collection by the query shape. This clears one cached
    // query in the foreign collection only.
    assert.commandWorked(foreignColl.runCommand("planCacheClear", {query: {}}));
    assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));
    assert.eq(1, numPlanCacheEntries(foreignColl), dumpPlanCacheState(foreignColl));

    // Test case 5: clear by query shape which matches $lookup and non-$lookup queries.
    //
    // Run the query on the main collection whose plan cache key command shape matches the shape of
    // the $lookup query.
    results = coll.aggregate({$match: {a: 1}}).toArray();
    assert.eq(3, results.length, results);
    assert.eq(2, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

    // Run another query on the main collection with a totally different shape.
    results = coll.aggregate({$match: {a: {$in: [1, 2]}}}).toArray();
    assert.eq(4, results.length, results);
    assert.eq(3, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

    // Drop query cache on the main collection by the query shape. This clears two cached queries in
    // the main collection which match the query shape.
    assert.commandWorked(coll.runCommand("planCacheClear", {query: {a: 1}}));
    assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));
    assert.eq(1, numPlanCacheEntries(foreignColl), dumpPlanCacheState(foreignColl));
}

//
// Query Plan Revision
// http://docs.mongodb.org/manual/core/query-plans/#query-plan-revision
// As collections change over time, the query optimizer deletes the query plan and re-evaluates
// after any of the following events:
// - The reIndex rebuilds the index.
// - You add or drop an index.
// - The mongod process restarts.
//

// Make sure the cache is emtpy.
assert.commandWorked(coll.runCommand('planCacheClear'));
assert.eq(0, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

// Case 1: The reIndex rebuilds the index.
// Steps:
//     Populate the cache with 1 entry.
//     Run reIndex on the collection.
//     Confirm that cache is empty.
// (Only standalone mode supports the reIndex command.)
const isMongos = db.adminCommand({isdbgrid: 1}).isdbgrid;
const isStandalone = !isMongos && !db.runCommand({hello: 1}).hasOwnProperty('setName');
if (isStandalone) {
    assert.eq(1, coll.find({a: 1, b: 1}).itcount());
    assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));
    assert.commandWorked(coll.reIndex());
    assert.eq(0, numPlanCacheEntries(coll), dumpPlanCacheState(coll));
}

// Case 2: You add or drop an index.
// Steps:
//     Populate the cache with 1 entry.
//     Add an index.
//     Confirm that cache is empty.
assert.eq(1, coll.find({a: 1, b: 1}).itcount());
assert.eq(1, numPlanCacheEntries(coll), dumpPlanCacheState(coll));
assert.commandWorked(coll.createIndex({b: 1}));
assert.eq(0, numPlanCacheEntries(coll), dumpPlanCacheState(coll));

// Case 3: The mongod process restarts
// Not applicable.
})();
