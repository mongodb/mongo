// SERVER-13675: Plans that tie should not be cached.

var t = db.jstests_plan_cache_ties;
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

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});

for (var i = 0; i < 200; i++) {
    t.save({a: 1, b: 1});
}
t.save({a: 2, b: 2});

// A query with zero results that does not hit EOF should not be cached.
assert.eq(0, t.find({c: 0}).itcount(), 'unexpected count');
assert.eq(0, getShapes().length, 'unexpected number of query shapes in plan cache');

// A query that hits EOF but still results in a tie should not be cached.
assert.eq(0, t.find({a: 3, b: 3}).itcount(), 'unexpected count');
assert.eq(0, getShapes().length, 'unexpected number of query shapes in plan cache');

// A query that returns results but leads to a tie should not be cached.
assert.eq(1, t.find({a: 2, b: 2}).itcount(), 'unexpected count');
assert.eq(0, getShapes().length, 'unexpected number of query shapes in plan cache');

// With a new index and tweaked data, we deliver a new query that should not tie. Check that
// this one gets cached.
t.ensureIndex({a: 1, b: 1});
for (var i = 0; i < 3; i++) {
    t.save({a: 2, b: 1});
    t.save({a: 1, b: 2});
}

assert.eq(1, t.find({a: 2, b: 2}).itcount(), 'unexpected count');
assert.eq(1, getShapes().length, 'unexpected number of query shapes in plan cache');
