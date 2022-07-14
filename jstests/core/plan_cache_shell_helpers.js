// Test the shell helpers which wrap the plan cache commands.
//
// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_unchanged,
//   # This test attempts to perform queries and introspect the server's plan cache entries. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary. If all chunks are moved off of a shard, it can cause the plan cache to
//   # miss commands.
//   assumes_read_preference_unchanged,
//   assumes_unsharded_collection,
//   does_not_support_stepdowns,
// ]
(function() {
'use strict';
load('jstests/aggregation/extras/utils.js');  // For assertArrayEq.
load("jstests/libs/analyze_plan.js");         // For getPlanCacheKeyFromShape.
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.

const isSbeEnabled = checkSBEEnabled(db, ["featureFlagSbeFull"]);
var coll = db.jstests_plan_cache_shell_helpers;
coll.drop();

function assertCacheLength(length) {
    const cacheContents = coll.getPlanCache().list();
    assert.eq(length, cacheContents.length, cacheContents);
}

// Add data and indices.
var n = 200;
for (var i = 0; i < n; i++) {
    assert.commandWorked(coll.insert({a: i, b: -1, c: 1}));
}
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// Populate plan cache.
var queryB = {a: {$gte: 199}, b: -1};
var projectionB = {_id: 0, b: 1};
var sortC = {c: -1};
assert.eq(1, coll.find(queryB, projectionB).sort(sortC).itcount(), 'unexpected document count');
assert.eq(1, coll.find(queryB, projectionB).itcount(), 'unexpected document count');
assert.eq(1, coll.find(queryB).sort(sortC).itcount(), 'unexpected document count');
assert.eq(1, coll.find(queryB).itcount(), 'unexpected document count');
assertCacheLength(4);

//
// PlanCache.getName
//

var planCache = coll.getPlanCache();
assert.eq(coll.getName(), planCache.getName(), 'name of plan cache should match collection');

//
// PlanCache.help
//
planCache.help();

//
// shellPrint
//

print('plan cache:');
print(planCache);

//
// collection.getPlanCache().list
//

var missingCollection = db.jstests_plan_cache_missing;
missingCollection.drop();
// Listing the cache for a non-existing collection is expected to fail by throwing.
assert.throws(() => missingCollection.getPlanCache().list());

// Test that we can use $group and $count with the list() helper.
assert.eq([{_id: null, count: 4}],
          planCache.list([{$group: {_id: null, count: {$sum: 1}}}]),
          planCache.list());
assert.eq([{count: 4}], planCache.list([{$count: "count"}]), planCache.list());

// Test that we can collect descriptions of all the queries that created cache entries using the
// list() helper.
if (isSbeEnabled) {
    assertArrayEq({
        expected: [
            {planCacheKey: getPlanCacheKeyFromShape({query: queryB, collection: coll, db: db})},
            {
                planCacheKey:
                    getPlanCacheKeyFromShape({query: queryB, sort: sortC, collection: coll, db: db})
            },
            {
                planCacheKey: getPlanCacheKeyFromShape(
                    {query: queryB, projection: projectionB, collection: coll, db: db})
            },
            {
                planCacheKey: getPlanCacheKeyFromShape(
                    {query: queryB, projection: projectionB, sort: sortC, collection: coll, db: db})
            },
        ],
        actual: planCache.list([{$project: {planCacheKey: 1}}]),
        extraErrorMsg: planCache.list()
    });
} else {
    assertArrayEq({
        expected: [
            {query: queryB, sort: {}, projection: {}},
            {query: queryB, sort: sortC, projection: {}},
            {query: queryB, sort: {}, projection: projectionB},
            {query: queryB, sort: sortC, projection: projectionB}
        ],
        actual: planCache.list([{$replaceWith: "$createdFromQuery"}]),
        extraErrorMsg: planCache.list()
    });
}

//
// collection.getPlanCache().clearPlansByQuery
//

// should not error on non-existent query shape.
planCache.clearPlansByQuery({unknownfield: 1});
// should error on missing required field query.
assert.throws(function() {
    planCache.clearPlansByQuery();
});

// Invoke with various permutations of required (query) and optional (projection, sort) arguments.
planCache.clearPlansByQuery(queryB, projectionB);
assertCacheLength(3);

planCache.clearPlansByQuery(queryB, undefined, sortC);
assertCacheLength(2);

planCache.clearPlansByQuery(queryB);
assertCacheLength(1);

planCache.clear();
assertCacheLength(0);

// clearPlansByQuery() will also accept a single argument with the query shape object
// as an alternative to specifying the query, sort and projection parameters separately.
// Format of query shape object:
// {
//     query: <query>,
//     projection: <projection>,
//     sort: <sort>
// }

// Repopulate cache
assert.eq(1, coll.find(queryB).sort(sortC).itcount(), 'unexpected document count');

// Clear using query shape object.
planCache.clearPlansByQuery({query: queryB, projection: {}, sort: sortC});
assertCacheLength(0);

// Should not error on missing or extra fields in query shape object.
planCache.clearPlansByQuery({query: queryB});
planCache.clearPlansByQuery(
    {query: queryB, sort: sortC, projection: projectionB, unknown_field: 1});

//
// collection.getPlanCache().clear
//

// Should not error on non-existent collection.
missingCollection.getPlanCache().clear();
// Re-populate plan cache with 1 query shape.
assert.eq(1, coll.find(queryB, projectionB).sort(sortC).itcount(), 'unexpected document count');
assertCacheLength(1);
// Clear cache.
planCache.clear();
assertCacheLength(0);

// Verify that explaining a find command does not write to the plan cache.
planCache.clear();
const explain = coll.find(queryB, projectionB).sort(sortC).explain(true);
assertCacheLength(0);
}());
