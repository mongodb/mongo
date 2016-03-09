// SERVER-18427: Add $log, $log10, $ln, $pow, and $exp aggregation expressions.

// For assertErrorCode.
load('jstests/aggregation/extras/utils.js');

(function() {
    'use strict';
    var coll = db.log_exponential_expressions;
    coll.drop();
    assert.writeOK(coll.insert({_id: 0}));

    // Helper for testing that op returns expResult.
    function testOp(op, expResult) {
        var pipeline = [{$project: {_id: 0, result: op}}];
        assert.eq(coll.aggregate(pipeline).toArray(), [{result: expResult}]);
    }

    // $log, $log10, $ln.

    // Valid input: numeric/null/NaN, base positive and not equal to 1, arg positive.
    testOp({$log: [10, 10]}, 1);
    testOp({$log10: [10]}, 1);
    testOp({$ln: [Math.E]}, 1);
    // All types converted to doubles.
    testOp({$log: [NumberLong("10"), NumberLong("10")]}, 1);
    testOp({$log10: [NumberLong("10")]}, 1);
    testOp({$ln: [NumberLong("1")]}, 0);
    // LLONG_MAX is converted to a double.
    testOp({$log: [NumberLong("9223372036854775807"), 10]}, 18.964889726830812);
    // Null inputs result in null.
    testOp({$log: [null, 10]}, null);
    testOp({$log: [10, null]}, null);
    testOp({$log10: [null]}, null);
    testOp({$ln: [null]}, null);
    // NaN inputs result in NaN.
    testOp({$log: [NaN, 10]}, NaN);
    testOp({$log: [10, NaN]}, NaN);
    testOp({$log10: [NaN]}, NaN);
    testOp({$ln: [NaN]}, NaN);

    // Invalid input: non-numeric/non-null, bases not positive or equal to 1, args not positive.

    // Args/bases must be numeric or null.
    assertErrorCode(coll, [{$project: {log: {$log: ["string", 5]}}}], 28756);
    assertErrorCode(coll, [{$project: {log: {$log: [5, "string"]}}}], 28757);
    assertErrorCode(coll, [{$project: {log10: {$log10: ["string"]}}}], 28765);
    assertErrorCode(coll, [{$project: {ln: {$ln: ["string"]}}}], 28765);
    // Args/bases cannot equal 0.
    assertErrorCode(coll, [{$project: {log: {$log: [0, 5]}}}], 28758);
    assertErrorCode(coll, [{$project: {log: {$log: [5, 0]}}}], 28759);
    assertErrorCode(coll, [{$project: {log10: {$log10: [0]}}}], 28761);
    assertErrorCode(coll, [{$project: {ln: {$ln: [0]}}}], 28766);
    // Args/bases cannot be negative.
    assertErrorCode(coll, [{$project: {log: {$log: [-1, 5]}}}], 28758);
    assertErrorCode(coll, [{$project: {log: {$log: [5, -1]}}}], 28759);
    assertErrorCode(coll, [{$project: {log10: {$log10: [-1]}}}], 28761);
    assertErrorCode(coll, [{$project: {ln: {$ln: [-1]}}}], 28766);
    // Base can't equal 1.
    assertErrorCode(coll, [{$project: {log: {$log: [5, 1]}}}], 28759);

    // $pow, $exp.

    // Valid input - numeric/null/NaN.

    // $pow -- if either input is a double return a double.
    testOp({$pow: [10, 2]}, 100);
    testOp({$pow: [1 / 2, -1]}, 2);
    testOp({$pow: [-2, 2]}, 4);
    testOp({$pow: [NumberInt("2"), 2]}, 4);
    testOp({$pow: [-2, NumberInt("2")]}, 4);

    // If exponent is negative and base not -1, 0, or 1, return a double.
    testOp({$pow: [NumberLong("2"), NumberLong("-1")]}, 1 / 2);
    testOp({$pow: [NumberInt("4"), NumberInt("-1")]}, 1 / 4);
    testOp({$pow: [NumberInt("4"), NumberLong("-1")]}, 1 / 4);
    testOp({$pow: [NumberInt("1"), NumberLong("-2")]}, NumberLong("1"));
    testOp({$pow: [NumberInt("-1"), NumberLong("-2")]}, NumberLong("1"));

    // If result would overflow a long, return a double.
    testOp({$pow: [NumberInt("2"), NumberLong("63")]}, 9223372036854776000);

    // Result would be incorrect if double were returned.
    testOp({$pow: [NumberInt("3"), NumberInt("35")]}, NumberLong("50031545098999707"));

    // Else if either input is a long, return a long.
    testOp({$pow: [NumberInt("-2"), NumberLong("63")]}, NumberLong("-9223372036854775808"));
    testOp({$pow: [NumberInt("4"), NumberLong("2")]}, NumberLong("16"));
    testOp({$pow: [NumberLong("4"), NumberInt("2")]}, NumberLong("16"));
    testOp({$pow: [NumberLong("4"), NumberLong("2")]}, NumberLong("16"));

    // Else return an int if it fits.
    testOp({$pow: [NumberInt("4"), NumberInt("2")]}, 16);

    // $exp always returns doubles, since e is a double.
    testOp({$exp: [NumberInt("-1")]}, 1 / Math.E);
    testOp({$exp: [NumberLong("1")]}, Math.E);
    // Null input results in null.
    testOp({$pow: [null, 2]}, null);
    testOp({$pow: [1 / 2, null]}, null);
    testOp({$exp: [null]}, null);
    // NaN input results in NaN.
    testOp({$pow: [NaN, 2]}, NaN);
    testOp({$pow: [1 / 2, NaN]}, NaN);
    testOp({$exp: [NaN]}, NaN);

    // Invalid inputs - non-numeric/non-null types, or 0 to a negative exponent.
    assertErrorCode(coll, [{$project: {pow: {$pow: [0, NumberLong("-1")]}}}], 28764);
    assertErrorCode(coll, [{$project: {pow: {$pow: ["string", 5]}}}], 28762);
    assertErrorCode(coll, [{$project: {pow: {$pow: [5, "string"]}}}], 28763);
    assertErrorCode(coll, [{$project: {exp: {$exp: ["string"]}}}], 28765);
}());
