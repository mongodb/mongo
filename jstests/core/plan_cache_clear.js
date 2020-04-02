// Test clearing of the plan cache, either manually through the planCacheClear command,
// or due to system events such as an index build.
//
// @tags: [
//   # This test attempts to perform queries and introspect/manipulate the server's plan cache
//   # entries. The former operation may be routed to a secondary in the replica set, whereas the
//   # latter must be routed to the primary.
//   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
//   assumes_read_preference_unchanged,
//   assumes_read_concern_unchanged,
//   does_not_support_stepdowns,
//   assumes_balancer_off,
//   assumes_unsharded_collection,
//   # Sharding support for $planCacheStats requires all nodes to be binary version 4.4.
//   requires_fcv_44,
// ]

(function() {
const coll = db.jstests_plan_cache_clear;
coll.drop();

function numPlanCacheEntries() {
    return coll.aggregate([{$planCacheStats: {}}]).itcount();
}

function dumpPlanCacheState() {
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
assert.eq(1, numPlanCacheEntries(), dumpPlanCacheState());

// Introduce a second plan cache entry.
assert.eq(0, coll.find({a: 1, b: 1, c: 1}).itcount());
assert.eq(2, numPlanCacheEntries(), dumpPlanCacheState());

// Drop one of the two shapes from the cache.
assert.commandWorked(coll.runCommand('planCacheClear', {query: {a: 1, b: 1}}),
                     dumpPlanCacheState());
assert.eq(1, numPlanCacheEntries(), dumpPlanCacheState());

// Drop the second shape from the cache.
assert.commandWorked(coll.runCommand('planCacheClear', {query: {a: 1, b: 1, c: 1}}),
                     dumpPlanCacheState());
assert.eq(0, numPlanCacheEntries(), dumpPlanCacheState());

// planCacheClear can clear $expr queries.
assert.eq(1, coll.find({a: 1, b: 1, $expr: {$eq: ['$a', 1]}}).itcount());
assert.eq(1, numPlanCacheEntries(), dumpPlanCacheState());
assert.commandWorked(
    coll.runCommand('planCacheClear', {query: {a: 1, b: 1, $expr: {$eq: ['$a', 1]}}}));
assert.eq(0, numPlanCacheEntries(), dumpPlanCacheState());

// planCacheClear fails with an $expr query with an unbound variable.
assert.commandFailed(
    coll.runCommand('planCacheClear', {query: {a: 1, b: 1, $expr: {$eq: ['$a', '$$unbound']}}}));

// Insert two more shapes into the cache.
assert.eq(1, coll.find({a: 1, b: 1}).itcount());
assert.eq(1, coll.find({a: 1, b: 1}, {_id: 0, a: 1}).itcount());
assert.eq(2, numPlanCacheEntries(), dumpPlanCacheState());

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
assert.eq(0, numPlanCacheEntries(), dumpPlanCacheState());

// Clearing the plan cache for a non-existent collection should succeed.
const nonExistentColl = db.plan_cache_clear_nonexistent;
nonExistentColl.drop();
assert.commandWorked(nonExistentColl.runCommand('planCacheClear'));

//
// Query Plan Revision
// http://docs.mongodb.org/manual/core/query-plans/#query-plan-revision
// As collections change over time, the query optimizer deletes the query plan and re-evaluates
// after any of the following events:
// - The reIndex rebuilds the index.
// - You add or drop an index.
// - The mongod process restarts.
//

// Case 1: The reIndex rebuilds the index.
// Steps:
//     Populate the cache with 1 entry.
//     Run reIndex on the collection.
//     Confirm that cache is empty.
// (Only standalone mode supports the reIndex command.)
const isMongos = db.adminCommand({isdbgrid: 1}).isdbgrid;
const isStandalone = !isMongos && !db.runCommand({isMaster: 1}).hasOwnProperty('setName');
if (isStandalone) {
    assert.eq(1, coll.find({a: 1, b: 1}).itcount());
    assert.eq(1, numPlanCacheEntries(), dumpPlanCacheState());
    assert.commandWorked(coll.reIndex());
    assert.eq(0, numPlanCacheEntries(), dumpPlanCacheState());
}

// Case 2: You add or drop an index.
// Steps:
//     Populate the cache with 1 entry.
//     Add an index.
//     Confirm that cache is empty.
assert.eq(1, coll.find({a: 1, b: 1}).itcount());
assert.eq(1, numPlanCacheEntries(), dumpPlanCacheState());
assert.commandWorked(coll.createIndex({b: 1}));
assert.eq(0, numPlanCacheEntries(), dumpPlanCacheState());

// Case 3: The mongod process restarts
// Not applicable.
})();
