// SERVER-6074: Add $slice aggregation expression.

// For assertErrorCode.
load('jstests/aggregation/extras/utils.js');

(function() {
    'use strict';

    var coll = db.agg_slice_expr;
    coll.drop();

    // Need to have at least one document to ensure the pipeline executes.
    assert.writeOK(coll.insert({}));

    function testSlice(sliceArgs, expArray) {
        var pipeline = [{$project: {_id: 0, slice: {$slice: sliceArgs}}}];
        assert.eq(coll.aggregate(pipeline).toArray(), [{slice: expArray}]);
    }

    // Two argument form.

    testSlice([[0, 1, 2, 3, 4], 2], [0, 1]);
    testSlice([[0, 1, 2, 3, 4], 2.0], [0, 1]);
    // Negative count
    testSlice([[0, 1, 2, 3, 4], -2], [3, 4]);
    testSlice([[0, 1, 2, 3, 4], -2.0], [3, 4]);
    // Zero count.
    testSlice([[0, 1, 2, 3, 4], 0], []);
    // Out of bounds positive.
    testSlice([[0, 1, 2, 3, 4], 10], [0, 1, 2, 3, 4]);
    // Out of bounds negative.
    testSlice([[0, 1, 2, 3, 4], -10], [0, 1, 2, 3, 4]);
    // Null arguments
    testSlice([null, -10], null);
    testSlice([[0, 1, 2, 3, 4], null], null);

    // Three argument form.

    testSlice([[0, 1, 2, 3, 4], 1, 2], [1, 2]);
    testSlice([[0, 1, 2, 3, 4], 1.0, 2.0], [1, 2]);
    // Negative start index.
    testSlice([[0, 1, 2, 3, 4], -3, 2], [2, 3]);
    testSlice([[0, 1, 2, 3, 4], -5, 2], [0, 1]);
    // Slice starts out of bounds.
    testSlice([[0, 1, 2, 3, 4], -10, 2], [0, 1]);
    testSlice([[0, 1, 2, 3, 4], 10, 2], []);
    // Slice ends out of bounds.
    testSlice([[0, 1, 2, 3, 4], 4, 3], [4]);
    testSlice([[0, 1, 2, 3, 4], -1, 3], [4]);
    // Null arguments
    testSlice([[0, 1, 2, 3, 4], -1, null], null);

    // Error cases.

    // Wrong number of arguments.
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2, 3]]}}}], 28667);
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2, 3], 4, 5, 6]}}}], 28667);

    // First argument is not an array.
    assertErrorCode(coll, [{$project: {x: {$slice: ['one', 2]}}}], 28724);

    // Second argument is not numeric.
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2], '2']}}}], 28725);

    // Second argument is not integral.
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2], 1.5]}}}], 28726);
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2], Math.pow(2, 32)]}}}], 28726);
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2], -Math.pow(2, 31) - 1]}}}], 28726);

    // Third argument is not numeric.
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2], 0, '2']}}}], 28727);

    // Third argument is not integral.
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2], 0, 1.5]}}}], 28728);
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2], 0, Math.pow(2, 32)]}}}], 28728);
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2], 0, -Math.pow(2, 31) - 1]}}}], 28728);

    // Third argument is not positive.
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2], 0, 0]}}}], 28729);
    assertErrorCode(coll, [{$project: {x: {$slice: [[1, 2], 0, -1]}}}], 28729);
}());
