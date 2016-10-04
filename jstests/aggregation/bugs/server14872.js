// SERVER-14872: Aggregation expression to concatenate multiple arrays into one

// For assertErrorCode.
load('jstests/aggregation/extras/utils.js');

(function() {
    'use strict';

    var coll = db.agg_concat_arrays_expr;
    coll.drop();

    assert.writeOK(coll.insert({a: [1, 2], b: ['three'], c: [], d: [[3], 4], e: null, str: 'x'}));

    // Basic concatenation.
    var pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a', '$b', '$c']}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{all: [1, 2, 'three']}]);

    // Concatenation with nested arrays.
    pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a', '$d']}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{all: [1, 2, [3], 4]}]);

    // Concatenation with 1 argument.
    pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a']}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{all: [1, 2]}]);

    // Concatenation with no arguments.
    pipeline = [{$project: {_id: 0, all: {$concatArrays: []}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{all: []}]);

    // Any nullish inputs will result in null.
    pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a', '$e']}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{all: null}]);
    pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a', '$f']}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{all: null}]);

    // Error on any non-array, non-null inputs.
    pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a', '$str']}}}];
    assertErrorCode(coll, pipeline, 28664);
}());
