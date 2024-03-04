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
//   # Plan cache state is node-local and will not get migrated alongside tenant data.
//   tenant_migration_incompatible,
//   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
//   cqf_experimental_incompatible,
// ]
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getPlanCacheKeyFromShape} from "jstests/libs/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

const isSbeEnabled = checkSbeFullyEnabled(db);
var coll = db.jstests_plan_cache_shell_helpers;
coll.drop();

// Function that enforces the presence (or absence) of the specified query shapes in the plan cache.
function assertCacheContent(expectedShapes) {
    const cacheContents = coll.getPlanCache().list();
    var cacheKeysSet = new Set();
    for (var i = 0; i < cacheContents.length; i++) {
        cacheKeysSet.add(isSbeEnabled ? cacheContents[i].planCacheKey
                                      : tojson(cacheContents[i].createdFromQuery));
    }
    for (const [shape, shouldBeInCache] of expectedShapes) {
        var searchKey = isSbeEnabled ? getPlanCacheKeyFromShape({
            query: shape.query,
            projection: shape.projection,
            sort: shape.sort,
            collection: coll,
            db: db
        })
                                     : tojson(shape);
        assert.eq(cacheKeysSet.has(searchKey),
                  shouldBeInCache,
                  'expected query plan ' + tojson(searchKey) + ' to be ' +
                      (shouldBeInCache ? 'present' : 'absent') + ' in ' + tojson(cacheContents));
    }
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
var queryBprojBSortShape = {query: queryB, sort: sortC, projection: projectionB};
var queryBprojBShape = {query: queryB, sort: {}, projection: projectionB};
var queryBSortShape = {query: queryB, sort: sortC, projection: {}};
var queryBShape = {query: queryB, sort: {}, projection: {}};
assert.eq(1, coll.find(queryB, projectionB).sort(sortC).itcount(), 'unexpected document count');
assert.eq(1, coll.find(queryB, projectionB).itcount(), 'unexpected document count');
assert.eq(1, coll.find(queryB).sort(sortC).itcount(), 'unexpected document count');
assert.eq(1, coll.find(queryB).itcount(), 'unexpected document count');
assertCacheContent([
    [queryBprojBSortShape, true],
    [queryBprojBShape, true],
    [queryBSortShape, true],
    [queryBShape, true]
]);

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
var groupQuery = planCache.list([{$group: {_id: null, count: {$sum: 1}}}]);
assert(groupQuery instanceof Array, planCache.list());
assert.gte(groupQuery[0].count, 4, groupQuery);

var countQuery = planCache.list([{$count: "count"}]);
assert(countQuery instanceof Array, planCache.list());
assert.gte(countQuery[0].count, 4, countQuery);

// Test that we can collect descriptions of all the queries that created cache entries using the
// list() helper.
if (isSbeEnabled) {
    var listQuery = planCache.list([{$project: {planCacheKey: 1}}]);
    assert(listQuery instanceof Array, planCache.list());
    assert.gte(listQuery.length, 4);
} else {
    var listQuery = planCache.list([{$replaceWith: "$createdFromQuery"}]);
    assert(listQuery instanceof Array, planCache.list());
    assert.gte(listQuery.length, 4);
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
assertCacheContent([
    [queryBprojBSortShape, true],
    [queryBprojBShape, false],
    [queryBSortShape, true],
    [queryBShape, true]
]);

planCache.clearPlansByQuery(queryB, undefined, sortC);
assertCacheContent([
    [queryBprojBSortShape, true],
    [queryBprojBShape, false],
    [queryBSortShape, false],
    [queryBShape, true]
]);

planCache.clearPlansByQuery(queryB);
assertCacheContent([
    [queryBprojBSortShape, true],
    [queryBprojBShape, false],
    [queryBSortShape, false],
    [queryBShape, false]
]);

planCache.clear();
assertCacheContent([
    [queryBprojBSortShape, false],
    [queryBprojBShape, false],
    [queryBSortShape, false],
    [queryBShape, false]
]);

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
assertCacheContent([
    [queryBprojBSortShape, false],
    [queryBprojBShape, false],
    [queryBSortShape, false],
    [queryBShape, false]
]);

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
assertCacheContent([
    [queryBprojBSortShape, true],
    [queryBprojBShape, false],
    [queryBSortShape, false],
    [queryBShape, false]
]);
// Clear cache.
planCache.clear();
assertCacheContent([
    [queryBprojBSortShape, false],
    [queryBprojBShape, false],
    [queryBSortShape, false],
    [queryBShape, false]
]);

// Verify that explaining a find command does not write to the plan cache.
planCache.clear();
const explain = coll.find(queryB, projectionB).sort(sortC).explain(true);
assertCacheContent([
    [queryBprojBSortShape, false],
    [queryBprojBShape, false],
    [queryBSortShape, false],
    [queryBShape, false]
]);
