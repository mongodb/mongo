// SERVER-9625 Making accumulators $sum, $min, $max, $avg, $stdDevSamp, and $stdDevPop available as
// expressions.

// For assertErrorCode.
load('jstests/aggregation/extras/utils.js');

(function() {
    'use strict';
    var coll = db.server9625;
    coll.drop();
    assert.writeOK(coll.insert({}));

    // Helper for testing that op returns expResult.
    function testOp(op, expResult) {
        var pipeline = [{$project: {_id: 0, result: op}}];
        assert.eq(coll.aggregate(pipeline).toArray(), [{result: expResult}]);
    }

    // ExpressionFromAccumulators take either a list of arguments or a single array argument.
    testOp({$avg: [1, 2, 3, 4, 5]}, 3);
    testOp({$avg: [[1, 2, 3, 4, 5]]}, 3);
    testOp({$min: [1, 2, 3, 4, 5]}, 1);
    testOp({$min: [[1, 2, 3, 4, 5]]}, 1);
    testOp({$max: [1, 2, 3, 4, 5]}, 5);
    testOp({$max: [[1, 2, 3, 4, 5]]}, 5);
    testOp({$sum: [1, 2, 3, 4, 5]}, 15);
    testOp({$sum: [[1, 2, 3, 4, 5]]}, 15);
    testOp({$stdDevPop: [1, 3]}, 1);
    testOp({$stdDevPop: [[1, 3]]}, 1);
    testOp({$stdDevSamp: [1, 2, 3]}, 1);
    testOp({$stdDevSamp: [[1, 2, 3]]}, 1);

    // Null arguments are ignored.
    testOp({$avg: [1, 2, 3, 4, 5, null]}, 3);
    testOp({$min: [1, 2, 3, 4, 5, null]}, 1);
    testOp({$max: [1, 2, 3, 4, 5, null]}, 5);
    testOp({$sum: [1, 2, 3, 4, 5, null]}, 15);
    testOp({$stdDevPop: [1, 3, null]}, 1);
    testOp({$stdDevSamp: [1, 2, 3, null]}, 1);

    // NaN arguments are processed by all expressions.
    testOp({$avg: [1, 2, 3, 4, 5, NaN]}, NaN);
    testOp({$min: [1, 2, 3, 4, 5, NaN]}, NaN);
    testOp({$max: [1, 2, 3, 4, 5, NaN]}, 5);
    testOp({$sum: [1, 2, 3, 4, 5, NaN]}, NaN);
    testOp({$stdDevPop: [1, 3, NaN]}, NaN);
    testOp({$stdDevSamp: [1, 2, 3, NaN]}, NaN);

    // Use at least one non-constant value in the following tests, to ensure
    // isAssociative() and isCommutative() are called. If all arguments are constant, the
    // optimization will evaluate them all into one, without calling isAssociative() nor
    // isCommutative().
    coll.drop();
    assert.writeOK(coll.insert({"a": 1, "b": 6}));

    // These expressions are associative and commutative so inner expression can be combined with
    // outer.
    testOp({$sum: ["$a", 2, 3, {$sum: [4, 5]}]}, 15);
    testOp({$min: ["$a", 2, 3, {$min: [4, 5]}]}, 1);
    testOp({$max: ["$a", 2, 3, {$max: [4, 5]}]}, 5);

    // These expressions are not associative and commutative so inner expression cannot be combined
    // with outer.
    testOp({$avg: ["$a", 3, {$avg: [4, 6]}]}, 3);
    testOp({$stdDevPop: ["$a", {$stdDevPop: [1, 3]}]}, 0);
    testOp({$stdDevSamp: ["$a", {$stdDevSamp: [1, 2, 3]}]}, 0);

    // If isAssociative() and isCommutative() did not return false when provided a single argument,
    // the single array argument provided to the inner expression would be ignored instead of
    // treated as a list of arguments, and these tests would fail.
    testOp({$sum: ["$a", 2, 3, {$sum: [["$a", 4, 5]]}]}, 16);
    testOp({$min: ["$b", 2, 3, {$min: [["$a", 4, 5]]}]}, 1);
    testOp({$max: ["$a", 2, 3, {$max: [["$b", 4, 5]]}]}, 6);
}());
