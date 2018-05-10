// Test the planCacheListQueryShapes command, which returns a list of query shapes
// for the queries currently cached in the collection.
//
// @tags: [
//   # This test attempts to perform queries with plan cache filters set up. The former operation
//   # may be routed to a secondary in the replica set, whereas the latter must be routed to the
//   # primary.
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
// ]
(function() {
    const t = db.jstests_plan_cache_list_shapes;
    t.drop();

    // Utility function to list query shapes in cache.
    function getShapes(collection) {
        if (collection === undefined) {
            collection = t;
        }
        const res = collection.runCommand('planCacheListQueryShapes');
        print('planCacheListQueryShapes() = ' + tojson(res));
        assert.commandWorked(res, 'planCacheListQueryShapes failed');
        assert(res.hasOwnProperty('shapes'), 'shapes missing from planCacheListQueryShapes result');
        return res.shapes;
    }

    // Attempting to retrieve cache information on non-existent collection is not an error
    // and should return an empty array of query shapes.
    const missingCollection = db.jstests_query_cache_missing;
    missingCollection.drop();
    assert.eq(0,
              getShapes(missingCollection).length,
              'planCacheListQueryShapes should return empty array on non-existent collection');

    assert.commandWorked(t.save({a: 1, b: 1}));
    assert.commandWorked(t.save({a: 1, b: 2}));
    assert.commandWorked(t.save({a: 1, b: 2}));
    assert.commandWorked(t.save({a: 2, b: 2}));

    // We need two indices so that the MultiPlanRunner is executed.
    assert.commandWorked(t.ensureIndex({a: 1}));
    assert.commandWorked(t.ensureIndex({a: 1, b: 1}));

    // Run a query.
    assert.eq(1,
              t.find({a: 1, b: 1}, {_id: 1, a: 1}).sort({a: -1}).itcount(),
              'unexpected document count');

    // We now expect the two indices to be compared and a cache entry to exist.
    // Retrieve query shapes from the test collection
    // Number of shapes should match queries executed by multi-plan runner.
    let shapes = getShapes();
    assert.eq(1, shapes.length, 'unexpected number of shapes in planCacheListQueryShapes result');
    assert.eq({query: {a: 1, b: 1}, sort: {a: -1}, projection: {_id: 1, a: 1}},
              shapes[0],
              'unexpected query shape returned from planCacheListQueryShapes');

    // Running a different query shape should cause another entry to be cached.
    assert.eq(1, t.find({a: 1, b: 1}).itcount(), 'unexpected document count');
    shapes = getShapes();
    assert.eq(2, shapes.length, 'unexpected number of shapes in planCacheListQueryShapes result');

    // Check that queries with different regex options have distinct shapes.

    // Insert some documents with strings so we have something to search for.
    for (let i = 0; i < 5; i++) {
        assert.commandWorked(t.insert({a: 3, s: 'hello world'}));
    }
    assert.commandWorked(t.insert({a: 3, s: 'hElLo wOrLd'}));

    // Run a query with a regex. Also must include 'a' so that the query may use more than one
    // index, and thus, must use the MultiPlanner.
    const regexQuery = {s: {$regex: 'hello world', $options: 'm'}, a: 3};
    assert.eq(5, t.find(regexQuery).itcount());

    assert.eq(
        3, getShapes().length, 'unexpected number of shapes in planCacheListQueryShapes result ');

    // Run the same query, but with different regex options. We expect that this should cause a
    // shape to get added.
    regexQuery.s.$options = 'mi';
    // There is one more result since the query is now case sensitive.
    assert.eq(6, t.find(regexQuery).itcount());
    assert.eq(
        4, getShapes().length, 'unexpected number of shapes in planCacheListQueryShapes result');
})();
