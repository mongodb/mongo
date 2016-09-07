// SERVER-4589: Add $arrayElemAt aggregation expression.

// For assertErrorCode.
load('jstests/aggregation/extras/utils.js');

(function() {
    'use strict';

    var coll = db.agg_array_elem_at_expr;
    coll.drop();

    assert.writeOK(coll.insert({a: [1, 2, 3, 4, 5]}));

    // Normal indexing.
    var pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', 2]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{x: 3}]);

    // Indexing with a float.
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', 1.0]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{x: 2}]);

    // Indexing with a decimal
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', NumberDecimal('2.0')]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{x: 3}]);

    // Negative indexing.
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', -1]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{x: 5}]);
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', -5]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{x: 1}]);

    // Out of bounds positive.
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', 5]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{}]);
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', Math.pow(2, 31) - 1]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{}]);
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', NumberLong(Math.pow(2, 31) - 1)]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{}]);

    // Out of bounds negative.
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', -6]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{}]);
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', -Math.pow(2, 31)]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{}]);
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', NumberLong(-Math.pow(2, 31))]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{}]);

    // Null inputs.
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: ['$a', null]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{x: null}]);
    pipeline = [{$project: {_id: 0, x: {$arrayElemAt: [null, 4]}}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{x: null}]);

    // Error cases.

    // Wrong number of arguments.
    assertErrorCode(coll, [{$project: {x: {$arrayElemAt: [['one', 'arg']]}}}], 16020);

    // First argument is not an array.
    assertErrorCode(coll, [{$project: {x: {$arrayElemAt: ['one', 2]}}}], 28689);

    // Second argument is not numeric.
    assertErrorCode(coll, [{$project: {x: {$arrayElemAt: [[1, 2], '2']}}}], 28690);

    // Second argument is not integral.
    assertErrorCode(coll, [{$project: {x: {$arrayElemAt: [[1, 2], 1.5]}}}], 28691);
    assertErrorCode(coll, [{$project: {x: {$arrayElemAt: [[1, 2], NumberDecimal('1.5')]}}}], 28691);
    assertErrorCode(coll, [{$project: {x: {$arrayElemAt: [[1, 2], Math.pow(2, 32)]}}}], 28691);
    assertErrorCode(coll, [{$project: {x: {$arrayElemAt: [[1, 2], -Math.pow(2, 31) - 1]}}}], 28691);
}());
