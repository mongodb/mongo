// Test the shell helpers which wrap the plan cache commands.
//
// @tags: [
//   # This test attempts to perform queries and introspect the server's plan cache entries. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary.
//   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
//   assumes_balancer_off,
// ]

var t = db.jstests_plan_cache_shell_helpers;
t.drop();

// Utility function to list query shapes in cache.
function getShapes(collection) {
    if (collection == undefined) {
        collection = t;
    }
    var res = collection.runCommand('planCacheListQueryShapes');
    print('planCacheListQueryShapes() = ' + tojson(res));
    assert.commandWorked(res, 'planCacheListQueryShapes failed');
    assert(res.hasOwnProperty('shapes'), 'shapes missing from planCacheListQueryShapes result');
    return res.shapes;
}
// Utility function to list plans for a query.
function getPlans(query, sort, projection) {
    var key = {query: query, sort: sort, projection: projection};
    var res = t.runCommand('planCacheListPlans', key);
    assert.commandWorked(res, 'planCacheListPlans(' + tojson(key, '', true) + ' failed');
    assert(res.hasOwnProperty('plans'),
           'plans missing from planCacheListPlans(' + tojson(key, '', true) + ') result');
    return res.plans;
}

function assertEmptyCache(cache, errMsg) {
    assert.eq(0, cache.listQueryShapes().length, errMsg + '\ncache contained: \n ', +tojson(cache));
}

function assertQueryNotInCache(cache, query, errMsg) {
    assert.eq(0,
              planCache.getPlansByQuery(query).plans.length,
              errMsg + '\ncache contained: \n ',
              +tojson(cache));
}

function assertCacheLength(cache, length, errMsg) {
    assert.eq(length, cache.length, errMsg + '\ncache contained: \n ', +tojson(cache));
}

// Add data an indices.
var n = 200;
for (var i = 0; i < n; i++) {
    t.save({a: i, b: -1, c: 1});
}
t.ensureIndex({a: 1});
t.ensureIndex({b: 1});

// Populate plan cache.
var queryB = {a: {$gte: 199}, b: -1};
var projectionB = {_id: 0, b: 1};
var sortC = {c: -1};
assert.eq(1, t.find(queryB, projectionB).sort(sortC).itcount(), 'unexpected document count');
assert.eq(1, t.find(queryB, projectionB).itcount(), 'unexpected document count');
assert.eq(1, t.find(queryB).sort(sortC).itcount(), 'unexpected document count');
assert.eq(1, t.find(queryB).itcount(), 'unexpected document count');
assertCacheLength(getShapes(), 4, 'unexpected number of query shapes in plan cache');

//
// PlanCache.getName
//

var planCache = t.getPlanCache();
assert.eq(t.getName(), planCache.getName(), 'name of plan cache should match collection');

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
// collection.getPlanCache().listQueryShapes
//

var missingCollection = db.jstests_plan_cache_missing;
missingCollection.drop();
// should return empty array on non-existent collection.
assertEmptyCache(missingCollection.getPlanCache(),
                 'collection.getPlanCache().listQueryShapes() should return empty results ' +
                     'on non-existent collection');
assert.eq(getShapes(),
          planCache.listQueryShapes(),
          'unexpected collection.getPlanCache().listQueryShapes() shell helper result');

//
// collection.getPlanCache().getPlansByQuery
//

// should return empty array on non-existent query shape.
assertQueryNotInCache(planCache,
                      {unknownfield: 1},
                      'collection.getPlanCache().getPlansByQuery() should return empty results ' +
                          'on non-existent collection');
// should error on missing required field query.
assert.throws(function() {
    planCache.getPlansByQuery();
});

// Invoke with various permutations of required (query) and optional (projection, sort) arguments.
assert.eq(getPlans(queryB, sortC, projectionB),
          planCache.getPlansByQuery(queryB, projectionB, sortC).plans,
          'plans from collection.getPlanCache().getPlansByQuery() different from command result');
assert.eq(getPlans(queryB, {}, projectionB),
          planCache.getPlansByQuery(queryB, projectionB).plans,
          'plans from collection.getPlanCache().getPlansByQuery() different from command result');
assert.eq(getPlans(queryB, sortC, {}),
          planCache.getPlansByQuery(queryB, undefined, sortC).plans,
          'plans from collection.getPlanCache().getPlansByQuery() different from command result');
assert.eq(getPlans(queryB, {}, {}),
          planCache.getPlansByQuery(queryB).plans,
          'plans from collection.getPlanCache().getPlansByQuery() different from command result');

// getPlansByQuery() will also accept a single argument with the query shape object
// as an alternative to specifying the query, sort and projection parameters separately.
// Format of query shape object:
// {
//     query: <query>,
//     projection: <projection>,
//     sort: <sort>
// }
var shapeB = {query: queryB, projection: projectionB, sort: sortC};
assert.eq(getPlans(queryB, sortC, projectionB),
          planCache.getPlansByQuery(shapeB).plans,
          'collection.getPlanCache().getPlansByQuery() did not accept query shape object');

// Should return empty array on missing or extra fields in query shape object.
// The entire invalid query shape object will be passed to the command
// as the 'query' component which will result in the server returning an empty
// array of plans.
assertQueryNotInCache(planCache,
                      {query: queryB},
                      'collection.getPlanCache.getPlansByQuery should return empty results on ' +
                          'incomplete query shape');
assertQueryNotInCache(planCache,
                      {query: queryB, sort: sortC, projection: projectionB, unknown_field: 1},
                      'collection.getPlanCache.getPlansByQuery should return empty results on ' +
                          'invalid query shape');

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
assertCacheLength(
    getShapes(),
    3,
    'query shape not dropped after running collection.getPlanCache().clearPlansByQuery()');

planCache.clearPlansByQuery(queryB, undefined, sortC);
assertCacheLength(
    getShapes(),
    2,
    'query shape not dropped after running collection.getPlanCache().clearPlansByQuery()');

planCache.clearPlansByQuery(queryB);
assertCacheLength(
    getShapes(),
    1,
    'query shape not dropped after running collection.getPlanCache().clearPlansByQuery()');

planCache.clear();
assertCacheLength(getShapes(), 0, 'plan cache not empty');

// clearPlansByQuery() will also accept a single argument with the query shape object
// as an alternative to specifying the query, sort and projection parameters separately.
// Format of query shape object:
// {
//     query: <query>,
//     projection: <projection>,
//     sort: <sort>
// }

// Repopulate cache
assert.eq(1, t.find(queryB).sort(sortC).itcount(), 'unexpected document count');

// Clear using query shape object.
planCache.clearPlansByQuery({query: queryB, projection: {}, sort: sortC});
assertCacheLength(
    getShapes(),
    0,
    'plan cache not empty. collection.getPlanCache().clearPlansByQuery did not accept query shape object');

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
assert.eq(1, t.find(queryB, projectionB).sort(sortC).itcount(), 'unexpected document count');
assertCacheLength(getShapes(), 1, 'plan cache should not be empty after running cacheable query');
// Clear cache.
planCache.clear();
assertCacheLength(getShapes(), 0, 'plan cache not empty after clearing');

//
// explain and plan cache
// Running explain should not mutate the plan cache.
//

planCache.clear();

// MultiPlanRunner explain
var multiPlanRunnerExplain = t.find(queryB, projectionB).sort(sortC).explain(true);

print('multi plan runner explain = ' + tojson(multiPlanRunnerExplain));

assertCacheLength(getShapes(), 0, 'explain should not mutate plan cache');
