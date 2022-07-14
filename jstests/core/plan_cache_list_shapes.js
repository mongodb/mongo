// Test using the $planCacheStats aggregation metadata source to list all of the query shapes cached
// for a particular collection.
//
// @tags: [
//   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
//   assumes_balancer_off,
//   does_not_support_stepdowns,
//   # This test attempts to perform queries with plan cache filters set up. The former operation
//   # may be routed to a secondary in the replica set, whereas the latter must be routed to the
//   # primary.
//   assumes_read_concern_unchanged,
//   assumes_read_preference_unchanged,
//   assumes_unsharded_collection,
// ]
(function() {
'use strict';
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

if (checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    jsTest.log("Skipping test because SBE is fully enabled.");
    return;
}

const coll = db.jstests_plan_cache_list_shapes;
coll.drop();

function dumpPlanCacheState() {
    return coll.aggregate([{$planCacheStats: {}}]).toArray();
}

// Utility function to list query shapes in cache.
function getCachedQueryShapes() {
    return coll.aggregate([{$planCacheStats: {}}, {$replaceWith: '$createdFromQuery'}]).toArray();
}

assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 2, b: 2}));

// We need two indices so that the MultiPlanRunner is executed.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

// Run a query.
assert.eq(1, coll.find({a: 1, b: 1}, {_id: 1, a: 1}).sort({a: -1}).itcount());

// We now expect the two indices to be compared and a cache entry to exist.  Retrieve query
// shapes from the test collection Number of shapes should match queries executed by multi-plan
// runner.
let shapes = getCachedQueryShapes();
assert.eq(1, shapes.length, dumpPlanCacheState());
assert.eq({query: {a: 1, b: 1}, sort: {a: -1}, projection: {_id: 1, a: 1}}, shapes[0]);

// Running a different query shape should cause another entry to be cached.
assert.eq(1, coll.find({a: 1, b: 1}).itcount());
shapes = dumpPlanCacheState();
assert.eq(2, shapes.length, shapes);

// Check that each shape has a unique queryHash.
assert.neq(shapes[0]["queryHash"], shapes[1]["queryHash"]);

// Check that queries with different regex options have distinct shapes.

// Insert some documents with strings so we have something to search for.
for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({a: 3, s: 'hello world'}));
}
assert.commandWorked(coll.insert({a: 3, s: 'hElLo wOrLd'}));

// Run a query with a regex. Also must include 'a' so that the query may use more than one
// index, and thus, must use the MultiPlanner.
const regexQuery = {
    s: {$regex: 'hello world', $options: 'm'},
    a: 3
};
assert.eq(5, coll.find(regexQuery).itcount());

shapes = getCachedQueryShapes();
assert.eq(3, shapes.length, shapes);

// Run the same query, but with different regex options. We expect that this should cause a
// shape to get added.
regexQuery.s.$options = 'mi';
// There is one more result since the query is now case sensitive.
assert.eq(6, coll.find(regexQuery).itcount());
shapes = getCachedQueryShapes();
assert.eq(4, shapes.length, shapes);
})();
