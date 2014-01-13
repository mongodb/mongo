/**
 * Plan cache commands
 * 
 * Cache-wide Commands
 * - planCacheListQueryShapes
 * - planCacheClear
 *
 * Query-specific Commands
 * - planCacheDrop
 * - planCacheListPlans
 */ 

var t = db.jstests_plan_cache_commands;

t.drop();

t.save({a: 1});

t.ensureIndex({a: 1});

var queryA1 = {a: 1};
var projectionA1 = {_id: 0, a: 1};
var sortA1 = {a: -1};
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');



//
// tests for planCacheListQueryShapes
// Returns a list of query shapes for the queries currently cached in the collection.
//

// Utility function to list query shapes in cache.
function getShapes() {
    var res = t.runCommand('planCacheListQueryShapes');
    print('planCacheListQueryShapes() = ' + tojson(res));
    assert.commandWorked(res, 'planCacheListQueryShapes failed');
    assert(res.hasOwnProperty('shapes'), 'shapes missing from planCacheListQueryShapes result');
    return res.shapes;
    
}
// Attempting to retrieve cache information on non-existent collection
// is an error.
var missingCollection = db.jstests_query_cache_missing;
missingCollection.drop();
assert.commandFailed(missingCollection.runCommand('planCacheListQueryShapes'));

// Retrieve query shapes from the test collection
// Number of shapes should match queries executed by multi-plan runner.
var shapes = getShapes();
assert.eq(1, shapes.length, 'unexpected number of shapes in planCacheListQueryShapes result');
assert.eq({query: queryA1, sort: sortA1, projection: projectionA1}, shapes[0],
          'unexpected query shape returned from planCacheListQueryShapes');



//
// Tests for planCacheDrop
//

// Invalid key should be an error.
assert.commandFailed(t.runCommand('planCacheDrop', {key: 'unknownquery'}));

// Run a new query shape and drop it from the cache
assert.eq(0, t.find({a: 1, b: 1}).itcount(), 'unexpected document count');
assert.eq(2, getShapes().length, 'unexpected cache size after running 2nd query');
assert.commandWorked(t.runCommand('planCacheDrop', {query: {a: 1, b: 1}}));
assert.eq(1, getShapes().length, 'unexpected cache size after dropping 2nd query from cache');



//
// Tests for planCacheListPlans
//

// Utility function to list plans for a query.
function getPlans(query, sort, projection) {
    var key = {query: query, sort: sort, projection: projection};
    var res = t.runCommand('planCacheListPlans', key);
    assert.commandWorked(res, 'planCacheListPlans(' + tojson(key, '', true) + ' failed');
    assert(res.hasOwnProperty('plans'), 'plans missing from planCacheListPlans(' +
           tojson(key, '', true) + ') result');
    return res.plans;
}

// Invalid key should be an error.
assert.commandFailed(t.runCommand('planCacheListPlans', {key: 'unknownquery'}));

// Retrieve plans for valid cache entry.
var plans = getPlans(queryA1, sortA1, projectionA1);
assert.eq(2, plans.length, 'unexpected number of plans cached for query');

// Print every plan
print('planCacheListPlans result:');
for (var i = 0; i < plans.length; i++) {
    print('plan ' + i + ': ' + tojson(plans[i]));
}


//
// Tests for planCacheClear
//

// Drop query cache. This clears all cached queries in the collection.
res = t.runCommand('planCacheClear');
print('planCacheClear() = ' + tojson(res));
assert.commandWorked(res, 'planCacheClear failed');
assert.eq(0, getShapes().length, 'plan cache should be empty after successful planCacheClear()');



//
// Query Plan Revision
// http://docs.mongodb.org/manual/core/query-plans/#query-plan-revision
// As collections change over time, the query optimizer deletes the query plan and re-evaluates
// after any of the following events:
// - The collection receives 1,000 write operations.
// - The reIndex rebuilds the index.
// - You add or drop an index.
// - The mongod process restarts.
//

// Case 1: The collection receives 1,000 write operations.
// Steps:
//     Populate cache. Cache should contain 1 key after running query.
//     Insert 1000 documents.
//     Cache should be cleared.
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
assert.eq(1, getShapes().length, 'plan cache should not be empty after query');
for (var i = 0; i < 1000; i++) {
    t.save({b: i});
}
assert.eq(0, getShapes().length, 'plan cache should be empty after adding 1000 documents.');

// Case 2: The reIndex rebuilds the index.
// Steps:
//     Populate the cache with 1 entry.
//     Run reIndex on the collection.
//     Confirm that cache is empty.
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
assert.eq(1, getShapes().length, 'plan cache should not be empty after query');
res = t.reIndex();
print('reIndex result = ' + tojson(res));
assert.eq(0, getShapes().length, 'plan cache should be empty after reIndex operation');

// Case 3: You add or drop an index.
// Steps:
//     Populate the cache with 1 entry.
//     Add an index.
//     Confirm that cache is empty.
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
assert.eq(1, getShapes().length, 'plan cache should not be empty after query');
t.ensureIndex({b: 1});
assert.eq(0, getShapes().length, 'plan cache should be empty after adding index');

// Case 4: The mongod process restarts
// Not applicable.
