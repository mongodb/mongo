// SERVER-8141 Avoid treating arrays as literals in aggregation pipeline.
(function() {
    'use strict';
    var coll = db.exprs_in_arrays;
    coll.drop();

    assert.writeOK(coll.insert({_id: 0, a: ['foo', 'bar', 'baz'], b: 'bar', c: 'Baz'}));

    // An array of constants should still evaluate to an array of constants.
    var pipeline = [{$project: {_id: 0, d: ['constant', 1]}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{d: ['constant', 1]}]);

    // A field name inside an array should take on the value of that field.
    pipeline = [{$project: {_id: 0, d: ['$b']}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{d: ['bar']}]);

    // An expression inside an array should be evaluated.
    pipeline = [{$project: {_id: 0, d: [{$toLower: 'FoO'}]}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{d: ['foo']}]);

    // Both an expression and a field name inside an array should be evaluated.
    pipeline = [{$project: {_id: 0, d: ['$b', {$toLower: 'FoO'}]}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{d: ['bar', 'foo']}]);

    // A nested array should still be evaluated.
    pipeline = [{$project: {_id: 0, d: ['$b', 'constant', [1, {$toLower: 'FoO'}]]}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{d: ['bar', 'constant', [1, 'foo']]}]);

    // Should still evaluate array elements inside arguments to an expression.
    pipeline = [{$project: {_id: 0, d: {$setIntersection: ['$a', ['$b']]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{d: ['bar']}]);

    pipeline = [{$project: {_id: 0, d: {$setIntersection: ['$a', [{$toLower: 'FoO'}]]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{d: ['foo']}]);

    // Nested arrays.
    pipeline = [{
        $project: {
            _id: 0,
            d: {$setIntersection: [[[1, 'foo', 'bar']], [[1, {$toLower: 'FoO'}, '$b']]]}
        }
    }];
    assert.eq(coll.aggregate(pipeline).toArray(), [{d: [[1, 'foo', 'bar']]}]);

    coll.drop();

    // Should replace missing values with NULL to preserve indices.
    assert.writeOK(coll.insert({_id: 1, x: 1, z: 2}));

    pipeline = [{$project: {_id: 0, coordinate: ['$x', '$y', '$z']}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{coordinate: [1, null, 2]}]);
}());
