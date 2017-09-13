// SERVER-19548: Add $floor, $ceil, and $trunc aggregation expressions.

// For assertErrorCode.
load("jstests/aggregation/extras/utils.js");

(function() {
    "use strict";
    var coll = db.server19548;
    coll.drop();
    // Seed collection so that the pipeline will execute.
    assert.writeOK(coll.insert({}));

    // Helper for testing that op returns expResult.
    function testOp(op, expResult) {
        var pipeline = [{$project: {_id: 0, result: op}}];
        assert.eq(coll.aggregate(pipeline).toArray(), [{result: expResult}]);
    }

    testOp({$ceil: NumberLong(4)}, NumberLong(4));
    testOp({$ceil: NaN}, NaN);
    testOp({$ceil: Infinity}, Infinity);
    testOp({$ceil: -Infinity}, -Infinity);
    testOp({$ceil: null}, null);
    testOp({$ceil: -2.0}, -2.0);
    testOp({$ceil: 0.9}, 1.0);
    testOp({$ceil: -1.2}, -1.0);

    testOp({$floor: NumberLong(4)}, NumberLong(4));
    testOp({$floor: NaN}, NaN);
    testOp({$floor: Infinity}, Infinity);
    testOp({$floor: -Infinity}, -Infinity);
    testOp({$floor: null}, null);
    testOp({$floor: -2.0}, -2.0);
    testOp({$floor: 0.9}, 0.0);
    testOp({$floor: -1.2}, -2.0);

    testOp({$trunc: NumberLong(4)}, NumberLong(4));
    testOp({$trunc: NaN}, NaN);
    testOp({$trunc: Infinity}, Infinity);
    testOp({$trunc: -Infinity}, -Infinity);
    testOp({$trunc: null}, null);
    testOp({$trunc: -2.0}, -2.0);
    testOp({$trunc: 0.9}, 0.0);
    testOp({$trunc: -1.2}, -1.0);

    // More than 1 argument.
    assertErrorCode(coll, [{$project: {a: {$ceil: [1, 2]}}}], 16020);
    assertErrorCode(coll, [{$project: {a: {$floor: [1, 2]}}}], 16020);
    assertErrorCode(coll, [{$project: {a: {$trunc: [1, 2]}}}], 16020);

    // Non-numeric input.
    assertErrorCode(coll, [{$project: {a: {$ceil: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$floor: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$trunc: "string"}}}], 28765);
}());
