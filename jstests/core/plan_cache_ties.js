// SERVER-13675: Plans that tie should not be cached.

var t = db.jstests_plan_cache_ties;
t.drop();

var shapes;

// Generates debug info. Used in the case of test failure.
function dumpPlanCache(shapes) {
    var outstr = '';

    // For each shape in the cache, get the plan cache entry.
    for (var i = 0; i < shapes.length; i++) {
        var shape = shapes[i];
        var cache = t.getPlanCache();
        var plans = cache.getPlansByQuery(shape.query, shape.projection, shape.sort);
        outstr += ('getPlansByQuery() for shape ' + i + ' = ' + tojson(plans) + '\n');
    }

    return outstr;
}

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

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});

for (var i = 0; i < 200; i++) {
    t.save({a: 1, b: 1});
}
t.save({a: 2, b: 2});

// Do a full collection scan to pull the whole collection into memory.
assert.eq(201, t.find().itcount(), 'unexpected number of docs in collection');

// A query with zero results that does not hit EOF should not be cached.
assert.eq(0, t.find({c: 0}).itcount(), 'unexpected count');
shapes = getShapes();
assert.eq(0, shapes.length,
          'unexpected number of query shapes in plan cache\n'
          + dumpPlanCache(shapes));

// A query that hits EOF but still results in a tie should not be cached.
assert.eq(0, t.find({a: 3, b: 3}).itcount(), 'unexpected count');
shapes = getShapes()
assert.eq(0, shapes.length,
          'unexpected number of query shapes in plan cache\n'
          + dumpPlanCache(shapes));

// A query that returns results but leads to a tie should not be cached.
assert.eq(1, t.find({a: 2, b: 2}).itcount(), 'unexpected count');
shapes = getShapes();
assert.eq(0, shapes.length,
          'unexpected number of query shapes in plan cache\n'
          + dumpPlanCache(shapes));

// With a new index and tweaked data, we deliver a new query that should not tie. Check that
// this one gets cached.
t.ensureIndex({a: 1, b: 1});
for (var i = 0; i < 3; i++) {
    t.save({a: 2, b: 1});
    t.save({a: 1, b: 2});
}

assert.eq(1, t.find({a: 2, b: 2}).itcount(), 'unexpected count');
shapes = getShapes();
assert.eq(1, shapes.length,
          'unexpected number of query shapes in plan cache\n'
          + dumpPlanCache(shapes));
