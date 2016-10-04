// Test the planCacheListQueryShapes command, which returns a list of query shapes
// for the queries currently cached in the collection.

var t = db.jstests_plan_cache_list_shapes;
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

// Attempting to retrieve cache information on non-existent collection is not an error
// and should return an empty array of query shapes.
var missingCollection = db.jstests_query_cache_missing;
missingCollection.drop();
assert.eq(0,
          getShapes(missingCollection).length,
          'planCacheListQueryShapes should return empty array on non-existent collection');

t.save({a: 1, b: 1});
t.save({a: 1, b: 2});
t.save({a: 1, b: 2});
t.save({a: 2, b: 2});

// We need two indices so that the MultiPlanRunner is executed.
t.ensureIndex({a: 1});
t.ensureIndex({a: 1, b: 1});

// Run a query.
assert.eq(
    1, t.find({a: 1, b: 1}, {_id: 1, a: 1}).sort({a: -1}).itcount(), 'unexpected document count');

// We now expect the two indices to be compared and a cache entry to exist.
// Retrieve query shapes from the test collection
// Number of shapes should match queries executed by multi-plan runner.
var shapes = getShapes();
assert.eq(1, shapes.length, 'unexpected number of shapes in planCacheListQueryShapes result');
assert.eq({query: {a: 1, b: 1}, sort: {a: -1}, projection: {_id: 1, a: 1}},
          shapes[0],
          'unexpected query shape returned from planCacheListQueryShapes');

// Running a different query shape should cause another entry to be cached.
assert.eq(1, t.find({a: 1, b: 1}).itcount(), 'unexpected document count');
shapes = getShapes();
assert.eq(2, shapes.length, 'unexpected number of shapes in planCacheListQueryShapes result');
